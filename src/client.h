#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include "config.h"

#include <uv.h>

#include "util/log.h"
#include "util/blocking_queue.h"
#include "util/atomic_int.h"

#include "http/http.h"
#include "tls.h"
#include "worker.h"
#include "plugin.h"

struct client_t {

  struct client_t * prev;
  struct client_t * next;

  struct log_context_t * log;
  struct log_context_t * data_log;

  uv_tcp_t tcp;

  struct worker_t * worker;

  tls_client_ctx_t * tls_ctx;

  struct plugin_invoker_t * plugin_invoker;

  http_connection_t * connection;

  bool closing;
  bool eof;

  size_t id;
  bool closed;
  uv_shutdown_t shutdown_req;
  size_t pending_writes;

  bool selected_protocol;

};

bool client_init(struct client_t * client, struct worker_t * worker);

bool client_free(struct client_t * client);

#endif
