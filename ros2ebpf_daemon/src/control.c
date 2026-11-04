// SPDX-License-Identifier: Apache-2.0
/*
 * JSON-RPC 2.0 over a unix socket (and optionally HTTP). One request per
 * line, one response per line. The socket is group-accessible (0660, group
 * `ros2ebpf` when it exists) so `ros2 ebpf` can run unprivileged.
 */
#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "control.h"
#include "log.h"
#include "ros2ebpf/exporter.h"

static int mkdir_parent(const char *path)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
        return 0;
    *slash = '\0';
    if (mkdir(tmp, 0755) && errno != EEXIST)
        return -errno;
    return 0;
}

int control_init(struct control *c, const char *socket_path, const char *http_addr,
                 control_handler h, void *ctx)
{
    memset(c, 0, sizeof(*c));
    c->unix_fd = c->http_fd = -1;
    c->handler = h;
    c->ctx = ctx;
    snprintf(c->socket_path, sizeof(c->socket_path), "%s", socket_path);

    int rc = mkdir_parent(socket_path);
    if (rc) {
        log_error("control: cannot create directory for %s: %s", socket_path, strerror(-rc));
        return rc;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", socket_path);
    unlink(socket_path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) || listen(fd, 8)) {
        rc = -errno;
        close(fd);
        log_error("control: bind %s: %s", socket_path, strerror(-rc));
        return rc;
    }
    struct group *g = getgrnam("ros2ebpf");
    if (g)
        chown(socket_path, (uid_t)-1, g->gr_gid);
    chmod(socket_path, 0660);
    c->unix_fd = fd;
    log_info("control socket at %s%s", socket_path, g ? " (group ros2ebpf)" : "");

    if (http_addr && http_addr[0]) {
        int hfd = ros2ebpf_http_listen(http_addr);
        if (hfd < 0) {
            log_error("control: http listen %s: %s", http_addr, strerror(-hfd));
            return hfd;
        }
        c->http_fd = hfd;
        log_info("control http at %s (no authentication: firewall it)", http_addr);
    }
    return 0;
}

void control_close(struct control *c)
{
    if (c->unix_fd >= 0) {
        close(c->unix_fd);
        unlink(c->socket_path);
    }
    if (c->http_fd >= 0)
        close(c->http_fd);
    c->unix_fd = c->http_fd = -1;
}

static struct json_object *error_response(struct json_object *id, int code, const char *msg)
{
    struct json_object *resp = json_object_new_object();
    json_object_object_add(resp, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(resp, "id", id ? json_object_get(id) : NULL);
    struct json_object *err = json_object_new_object();
    json_object_object_add(err, "code", json_object_new_int(code));
    json_object_object_add(err, "message", json_object_new_string(msg));
    json_object_object_add(resp, "error", err);
    return resp;
}

struct json_object *control_dispatch(struct control *c, struct json_object *req)
{
    struct json_object *id = NULL, *method = NULL, *params = NULL;
    json_object_object_get_ex(req, "id", &id);
    if (!json_object_object_get_ex(req, "method", &method) || !json_object_is_type(method, json_type_string))
        return error_response(id, -32600, "invalid request: missing method");
    json_object_object_get_ex(req, "params", &params);

    struct json_object *result = NULL;
    char errmsg[256] = "";
    int rc = c->handler(c->ctx, json_object_get_string(method), params, &result, errmsg, sizeof(errmsg));
    if (rc)
        return error_response(id, rc, errmsg[0] ? errmsg : "error");
    struct json_object *resp = json_object_new_object();
    json_object_object_add(resp, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(resp, "id", id ? json_object_get(id) : NULL);
    json_object_object_add(resp, "result", result ? result : json_object_new_object());
    return resp;
}

static int write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void handle_line(struct control *c, int fd, char *line)
{
    struct json_object *req = json_tokener_parse(line);
    struct json_object *resp;
    if (!req)
        resp = error_response(NULL, -32700, "parse error");
    else
        resp = control_dispatch(c, req);
    const char *text = json_object_to_json_string_ext(resp, JSON_C_TO_STRING_PLAIN);
    write_all(fd, text, strlen(text));
    write_all(fd, "\n", 1);
    json_object_put(resp);
    if (req)
        json_object_put(req);
}

int control_serve_unix(struct control *c)
{
    int fd = accept4(c->unix_fd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0)
        return -errno;
    struct timeval tv = { .tv_sec = 5 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(fd);
        return -ENOMEM;
    }
    for (;;) {
        if (len == cap - 1) {
            /* oversized request without newline */
            handle_line(c, fd, (char *)"{}");
            break;
        }
        ssize_t r = recv(fd, buf + len, cap - 1 - len, 0);
        if (r <= 0)
            break;
        len += (size_t)r;
        buf[len] = '\0';
        char *start = buf, *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            if (*start)
                handle_line(c, fd, start);
            start = nl + 1;
        }
        len = strlen(start);
        memmove(buf, start, len + 1);
    }
    free(buf);
    close(fd);
    return 0;
}

int control_serve_http(struct control *c)
{
    int fd = accept4(c->http_fd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0)
        return -errno;
    struct timeval tv = { .tv_sec = 5 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char req[65536];
    size_t got = 0;
    size_t content_length = 0, header_end = 0;
    while (got < sizeof(req) - 1) {
        ssize_t r = recv(fd, req + got, sizeof(req) - 1 - got, 0);
        if (r <= 0)
            break;
        got += (size_t)r;
        req[got] = '\0';
        char *he = strstr(req, "\r\n\r\n");
        if (he) {
            header_end = (size_t)(he - req) + 4;
            const char *cl = strcasestr(req, "content-length:");
            if (cl)
                content_length = strtoul(cl + 15, NULL, 10);
            if (got >= header_end + content_length)
                break;
        }
    }
    req[got] = '\0';
    char method[8] = "", path[256] = "";
    sscanf(req, "%7s %255s", method, path);

    struct json_object *resp = NULL;
    int status = 200;
    if (!strcmp(method, "POST") && !strcmp(path, "/rpc") && header_end) {
        struct json_object *r = json_tokener_parse(req + header_end);
        resp = r ? control_dispatch(c, r) : error_response(NULL, -32700, "parse error");
        if (r)
            json_object_put(r);
    } else if (!strcmp(method, "GET") && !strcmp(path, "/status")) {
        struct json_object *r = json_object_new_object();
        json_object_object_add(r, "method", json_object_new_string("status"));
        json_object_object_add(r, "id", json_object_new_int(1));
        resp = control_dispatch(c, r);
        json_object_put(r);
    } else {
        status = 404;
        resp = error_response(NULL, -32601, "use POST /rpc or GET /status");
    }
    const char *body = json_object_to_json_string_ext(resp, JSON_C_TO_STRING_PLAIN);
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n", status, status == 200 ? "OK" : "Not Found", strlen(body) + 1);
    write_all(fd, head, (size_t)n);
    write_all(fd, body, strlen(body));
    write_all(fd, "\n", 1);
    json_object_put(resp);
    close(fd);
    return 0;
}
