#include "config.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include <unistd.h>

#include "util.h"
#include "server.h"
#include "logo.h"

#define LISTEN_BACKLOG 128
#define PATH_SIZE 1024

struct server_client_t {
  struct server_t * server;
  uv_tcp_t client;
  uv_write_t req;
  uv_buf_t buf;
};

static void server_sigpipe_handler(uv_signal_t * sigpipe_handler, int signum)
{
  struct server_t * server = sigpipe_handler->data;
  log_append(server->log, LOG_WARN, "Caught SIGPIPE: %d", signum);
}

static void server_sigint_handler(uv_signal_t * sigint_handler, int signum)
{
  struct server_t * server = sigint_handler->data;
  log_append(server->log, LOG_DEBUG, "Caught SIGINT: %d", signum);

  server_stop(server);
}

static void server_sigterm_handler(uv_signal_t * sigterm_handler, int signum)
{
  struct server_t * server = sigterm_handler->data;
  log_append(server->log, LOG_DEBUG, "Caught SIGTERM: %d", signum);

  server_stop(server);
}

static void server_stop_continue(struct server_t * server)
{
  if (server->active_workers < 1 && server->active_listeners < 1 && server->active_handlers < 1) {
    log_append(server->log, LOG_TRACE, "Closed server handles...");
    uv_stop(&server->loop);
  } else if (server->active_workers < 1) {
    server_stop(server);
  }
}

static void handler_closed(uv_handle_t * handle)
{
  struct server_t * server = handle->data;

  server->active_handlers--;

  server_stop_continue(server);
}

static void worker_pipe_closed(uv_handle_t * handle)
{
  struct worker_process_t * worker_process = handle->data;
  struct server_t * server = worker_process->server;
  free(worker_process);

  server->active_workers--;

  server_stop_continue(server);
}

static void tcp_listener_closed(uv_handle_t * handle)
{
  struct listen_address_t * addr = handle->data;
  struct server_t * server = addr->data;

  server->active_listeners--;

  server_stop_continue(server);
}

static void process_handle_closed(uv_handle_t * req)
{
  struct worker_process_t * worker = req->data;

  uv_close((uv_handle_t *) &worker->pipe, worker_pipe_closed);
}

static void close_process_handle(uv_process_t * req, int64_t exit_status, int term_signal)
{
  struct worker_process_t * worker = req->data;
  struct server_t * server = worker->server;

  worker->stopped = true;
  log_append(server->log, term_signal == 0 ? LOG_DEBUG : LOG_WARN,
      "Process exited with status %" PRId64 ", signal %d. %zu remaining workers\n",
      exit_status, term_signal, server->active_workers - 1);

  uv_close((uv_handle_t *) req, process_handle_closed);
}

static void client_handle_closed(uv_handle_t * handle)
{
  struct server_client_t * server_client = handle->data;

  free(server_client);
}

void on_write_complete(uv_write_t * req, int status)
{
  struct server_client_t * server_client = req->data;
  struct server_t * server = server_client->server;

  if (status) {
    log_append(server->log, LOG_ERROR, "Error passing file descriptor to worker: %s\n", uv_err_name(status));
  }

  uv_close((uv_handle_t *) &server_client->client, client_handle_closed);
}

static void server_on_new_connection(uv_stream_t * uv_stream, int status)
{
  struct listen_address_t * addr = uv_stream->data;
  struct server_t * server = addr->data;

  if (status == -1) {
    log_append(server->log, LOG_ERROR, "Error getting new connection: %s\n", uv_err_name(status));
    return;
  }

  struct server_client_t * server_client = malloc(sizeof(struct server_client_t));
  server_client->server = server;
  uv_tcp_t * client = &server_client->client;
  uv_tcp_init(&server->loop, client);
  uv_tcp_nodelay(client, true);
  client->data = server_client;

  if (uv_accept(uv_stream, (uv_stream_t *) client) == 0) {

    uv_write_t * write_req = &server_client->req;
    write_req->data = server_client;

    uv_buf_t * buf = &server_client->buf;

    // send the index of the listen_address_t that accepted this
    // connection to the worker
    char index_encoded = addr->index;
    buf->base = &index_encoded;
    buf->len = 1;

    struct worker_process_t * worker = server->workers[server->round_robin_counter];

    log_append(server->log, LOG_DEBUG, "Server %d: Accepted file %d for worker %d\n",
        getpid(), client->io_watcher.fd, worker->req.pid);

    uv_write2(write_req, (uv_stream_t *) &worker->pipe, buf, 1,
        (uv_stream_t *) client, on_write_complete);
    server->round_robin_counter = (server->round_robin_counter + 1) %
      server->config->num_workers;

  } else {
    free(server_client);
    uv_close((uv_handle_t *) client, NULL);
  }
}

static bool setup_workers(struct server_t * server)
{
  size_t path_size = PATH_SIZE;
  char worker_path[PATH_SIZE];
  uv_exepath(worker_path, &path_size);

  // copy the existing arguments, but add "-a" as the second to start the child
  // process as a worker
  char * args[server->config->argc + 2];
  args[0] = worker_path;
  args[1] = "-a"; // run as a worker
  for (int i = 1; i < server->config->argc; i++) {
    args[i+1] = server->config->argv[i];
  }
  args[server->config->argc+1] = NULL;

  int num_workers = server->config->num_workers;

  server->workers = malloc(sizeof(struct worker_process_t *) * num_workers);

  while (num_workers--) {
    struct worker_process_t * worker = calloc(sizeof(struct worker_process_t), 1);
    worker->server = server;
    worker->stopped = false;
    server->workers[num_workers] = worker;
    uv_pipe_init(&server->loop, &worker->pipe, 1);
    worker->pipe.data = worker;

    uv_stdio_container_t child_stdio[3];
    child_stdio[0].flags = UV_CREATE_PIPE | UV_READABLE_PIPE;
    child_stdio[0].data.stream = (uv_stream_t *) &worker->pipe;
    child_stdio[1].flags = UV_INHERIT_FD;
    child_stdio[1].data.fd = 1;
    child_stdio[2].flags = UV_INHERIT_FD;
    child_stdio[2].data.fd = 2;

    worker->options.stdio = child_stdio;
    worker->options.stdio_count = 3;

    worker->options.exit_cb = close_process_handle;
    worker->options.file = args[0];
    worker->options.args = args;

    worker->req.data = worker;

    int uv_error = uv_spawn(&server->loop, &worker->req, &worker->options);
    if (uv_error < 0) {
      log_append(server->log, LOG_FATAL, "Failed to spawn process: %s", uv_err_name(uv_error));
      return false;
    }
    server->active_workers++;
  }
  return true;
}

bool server_run(struct server_t * server)
{
  for (size_t i = 0; i < LOGO_LINES_LENGTH; i++) {
    log_append(server->log, LOG_INFO, (char *) LOGO_LINES[i]);
  }
  log_append(server->log, LOG_INFO, "Server starting");

  if (!setup_workers(server)) {
    return false;
  }

  uv_signal_start(&server->sigpipe_handler, server_sigpipe_handler, SIGPIPE);
  uv_signal_start(&server->sigint_handler, server_sigint_handler, SIGINT);
  uv_signal_start(&server->sigterm_handler, server_sigterm_handler, SIGTERM);

  size_t index = 0;
  struct listen_address_t * curr = server->config->address_list;
  while (curr) {
    struct tcp_list_t * tcp_list = malloc(sizeof(struct tcp_list_t));
    uv_tcp_t * tcp = &tcp_list->uv_server;
    uv_tcp_init(&server->loop, tcp);
    tcp->data = curr;
    curr->data = server;
    curr->index = index;

    tcp_list->next = server->tcp_list;
    server->tcp_list = tcp_list;

    struct sockaddr_in bind_addr;
    int r;
    if ((r = uv_ip4_addr(curr->hostname, curr->port, &bind_addr))) {
      log_append(server->log, LOG_FATAL, "Initializing bind on address %s://%s:%ld failed: %s",
          curr->use_tls ? "https" : "http", curr->hostname, curr->port, uv_err_name(r));
      return false;
    }
    uv_tcp_bind(tcp, (const struct sockaddr *)&bind_addr, 0);
    if ((r = uv_listen((uv_stream_t *) tcp, LISTEN_BACKLOG, server_on_new_connection))) {
      log_append(server->log, LOG_FATAL, "Listening on %s://%s:%ld failed: %s",
          curr->use_tls ? "https" : "http", curr->hostname, curr->port, uv_err_name(r));
      return false;
    } else {
      log_append(server->log, LOG_INFO, "Listening on %s://%s:%ld",
          curr->use_tls ? "https" : "http", curr->hostname, curr->port);
    }
    curr = curr->next;
    index++;
  }
  server->active_listeners = index;

  uv_run(&server->loop, UV_RUN_DEFAULT);

  return true;
}

void server_stop(struct server_t * server)
{
  if (!server->stopping) {
    server->stopping = true;
    log_append(server->log, LOG_INFO, "Server shutting down...");

    uv_signal_stop(&server->sigpipe_handler);
    uv_signal_stop(&server->sigint_handler);
    uv_signal_stop(&server->sigterm_handler);
    uv_close((uv_handle_t *) &server->sigpipe_handler, handler_closed);
    uv_close((uv_handle_t *) &server->sigint_handler, handler_closed);
    uv_close((uv_handle_t *) &server->sigterm_handler, handler_closed);

    struct tcp_list_t * tcp_list = server->tcp_list;
    while (tcp_list) {
      uv_close((uv_handle_t *) &tcp_list->uv_server, tcp_listener_closed);
      tcp_list = tcp_list->next;
    }

    if (server->active_workers > 0) {
      for (size_t i = 0; i < server->config->num_workers; i++) {
        struct worker_process_t * worker = server->workers[i];
        if (!worker->stopped) {
          worker->stopped = true;
          log_append(server->log, LOG_DEBUG, "Killing process: %d...", worker->req.pid);
          uv_process_kill(&worker->req, SIGTERM);
        }
      }
    }

    server_stop_continue(server);
  }
}

void server_init(struct server_t * server, struct server_config_t * config)
{
  uv_loop_init(&server->loop);

  server->tcp_list = NULL;

  server->log = &config->server_log;
  server->data_log = &config->data_log;
  server->config = config;
  server->workers = NULL;

  server->stopping = false;
  server->round_robin_counter = 0;
  server->active_handlers = 0;

  uv_signal_init(&server->loop, &server->sigpipe_handler);
  server->sigpipe_handler.data = server;
  uv_signal_init(&server->loop, &server->sigint_handler);
  server->sigint_handler.data = server;
  uv_signal_init(&server->loop, &server->sigterm_handler);
  server->sigterm_handler.data = server;
  server->active_handlers += 3;

  server->active_listeners = 0;
  server->active_workers = 0;
}

void server_free(struct server_t * server)
{
  uv_loop_close(&server->loop);

  free(server->workers);

  while (server->tcp_list) {
    struct tcp_list_t * tcp_list = server->tcp_list;
    server->tcp_list = server->tcp_list->next;
    free(tcp_list);
  }
}

