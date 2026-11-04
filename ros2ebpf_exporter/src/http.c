// SPDX-License-Identifier: Apache-2.0
/* Minimal HTTP/1.1 responder: one request per connection, Connection: close. */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "ros2ebpf/exporter.h"

int ros2ebpf_http_listen(const char *addr)
{
    char host[256] = "", port[16] = "";
    const char *colon;
    if (addr[0] == '[') {
        const char *end = strchr(addr, ']');
        if (!end || end[1] != ':')
            return -EINVAL;
        snprintf(host, sizeof(host), "%.*s", (int)(end - addr - 1), addr + 1);
        snprintf(port, sizeof(port), "%s", end + 2);
    } else if ((colon = strrchr(addr, ':')) != NULL) {
        snprintf(host, sizeof(host), "%.*s", (int)(colon - addr), addr);
        snprintf(port, sizeof(port), "%s", colon + 1);
    } else {
        snprintf(port, sizeof(port), "%s", addr);
    }

    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE };
    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host[0] ? host : NULL, port, &hints, &res);
    if (gai != 0)
        return -EINVAL;
    int fd = -1, err = -EADDRNOTAVAIL;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) {
            err = -errno;
            continue;
        }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 16) == 0)
            break;
        err = -errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd >= 0 ? fd : err;
}

static int send_all(int fd, const char *p, size_t n)
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

static int respond(int fd, int status, const char *reason, const char *ctype,
                   const char *body, size_t body_len)
{
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n", status, reason, ctype, body_len);
    int rc = send_all(fd, head, (size_t)n);
    return rc ?: send_all(fd, body, body_len);
}

int ros2ebpf_http_serve_one(int listen_fd, ros2ebpf_http_render_fn fn, void *ctx)
{
    int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0)
        return -errno;
    struct timeval tv = { .tv_sec = 5 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char req[4096];
    size_t got = 0;
    while (got < sizeof(req) - 1) {
        ssize_t r = recv(fd, req + got, sizeof(req) - 1 - got, 0);
        if (r <= 0)
            break;
        got += (size_t)r;
        req[got] = '\0';
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n"))
            break;
    }
    req[got] = '\0';

    char method[8] = "", path[512] = "";
    sscanf(req, "%7s %511s", method, path);
    char *q = strchr(path, '?');
    if (q)
        *q = '\0';

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        respond(fd, 405, "Method Not Allowed", "text/plain", "method not allowed\n", 19);
    } else if (strcmp(path, "/metrics") == 0) {
        struct ros2ebpf_buf out = { 0 };
        int rc = fn(ctx, &out);
        if (rc)
            respond(fd, 503, "Service Unavailable", "text/plain", "maps unavailable\n", 17);
        else
            respond(fd, 200, "OK", "text/plain; version=0.0.4; charset=utf-8",
                    out.data ? out.data : "", out.len);
        ros2ebpf_buf_free(&out);
    } else if (strcmp(path, "/healthz") == 0) {
        respond(fd, 200, "OK", "text/plain", "ok\n", 3);
    } else if (strcmp(path, "/") == 0) {
        static const char body[] =
            "<html><head><title>ros2ebpf</title></head><body><h1>ros2ebpf</h1>"
            "<p><a href=\"/metrics\">/metrics</a></p></body></html>\n";
        respond(fd, 200, "OK", "text/html", body, sizeof(body) - 1);
    } else {
        respond(fd, 404, "Not Found", "text/plain", "not found\n", 10);
    }
    close(fd);
    return 0;
}
