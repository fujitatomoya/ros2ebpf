/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ROS2EBPF_CONTROL_H
#define ROS2EBPF_CONTROL_H
#include <stddef.h>
#include <json-c/json.h>

/* Returns 0 and sets *result (owned by caller), or a JSON-RPC error code
 * (-32601 unknown method, -32602 bad params, -32000 server error) with errmsg. */
typedef int (*control_handler)(void *ctx, const char *method, struct json_object *params,
                               struct json_object **result, char *errmsg, size_t errlen);

struct control {
    int unix_fd;
    int http_fd;
    char socket_path[256];
    control_handler handler;
    void *ctx;
};

int  control_init(struct control *c, const char *socket_path, const char *http_addr,
                  control_handler h, void *ctx);
void control_close(struct control *c);
/* Accept one connection and serve newline-delimited JSON-RPC until EOF. */
int  control_serve_unix(struct control *c);
/* Accept one connection and serve POST /rpc (JSON-RPC body) or GET /status. */
int  control_serve_http(struct control *c);
/* Dispatch one request object; returns a response object (caller frees). */
struct json_object *control_dispatch(struct control *c, struct json_object *req);

#endif
