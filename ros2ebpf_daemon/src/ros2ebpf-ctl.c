// SPDX-License-Identifier: Apache-2.0
/*
 * ros2ebpf-ctl: thin JSON-RPC client for the daemon control socket. Used by
 * `ros2 ebpf` over ssh, by container readiness probes, and for scripting on
 * hosts without ROS.
 *
 *   ros2ebpf-ctl [-s SOCKET] status [-q]
 *   ros2ebpf-ctl [-s SOCKET] enable publish
 *   ros2ebpf-ctl [-s SOCKET] <method> ['{"json":"params"}']
 *   ros2ebpf-ctl [-s SOCKET] rpc          # forward JSON-RPC lines from stdin
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <json-c/json.h>

static int connect_socket(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa))) {
        int err = -errno;
        close(fd);
        return err;
    }
    return fd;
}

static int send_line(int fd, const char *line)
{
    size_t n = strlen(line);
    while (n) {
        ssize_t w = write(fd, line, n);
        if (w < 0)
            return -errno;
        line += w;
        n -= (size_t)w;
    }
    return write(fd, "\n", 1) == 1 ? 0 : -errno;
}

static char *read_line(int fd)
{
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf)
        return NULL;
    for (;;) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *p = realloc(buf, cap);
            if (!p) {
                free(buf);
                return NULL;
            }
            buf = p;
        }
        ssize_t r = read(fd, buf + len, 1);
        if (r <= 0)
            break;
        if (buf[len] == '\n')
            break;
        len++;
    }
    buf[len] = '\0';
    return buf;
}

int main(int argc, char **argv)
{
    const char *sock = "/run/ros2ebpf/control.sock";
    int quiet = 0, i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc)
            sock = argv[++i];
        else if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet"))
            quiet = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr, "usage: %s [-s SOCKET] [-q] <status|enable|disable|reload|snapshot|config|metrics|discover|rpc> [family|json]\n", argv[0]);
            return 0;
        } else
            break;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: %s [-s SOCKET] [-q] <method> [args]\n", argv[0]);
        return 2;
    }
    const char *method = argv[i++];
    const char *env_sock = getenv("ROS2EBPF_SOCKET");
    if (env_sock && !strcmp(sock, "/run/ros2ebpf/control.sock"))
        sock = env_sock;

    int fd = connect_socket(sock);
    if (fd < 0) {
        fprintf(stderr, "ros2ebpf-ctl: cannot connect to %s: %s (is ros2ebpfd running?)\n", sock, strerror(-fd));
        return 3;
    }

    if (!strcmp(method, "rpc")) {
        char line[65536];
        while (fgets(line, sizeof(line), stdin)) {
            line[strcspn(line, "\n")] = '\0';
            if (!line[0])
                continue;
            if (send_line(fd, line))
                break;
            char *resp = read_line(fd);
            if (!resp)
                break;
            printf("%s\n", resp);
            fflush(stdout);
            free(resp);
        }
        close(fd);
        return 0;
    }

    struct json_object *req = json_object_new_object();
    json_object_object_add(req, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(req, "id", json_object_new_int(1));
    json_object_object_add(req, "method", json_object_new_string(method));
    if (i < argc) {
        struct json_object *params = json_tokener_parse(argv[i]);
        if (!params) {
            params = json_object_new_object();
            json_object_object_add(params, "family", json_object_new_string(argv[i]));
        }
        json_object_object_add(req, "params", params);
    }
    int rc = send_line(fd, json_object_to_json_string_ext(req, JSON_C_TO_STRING_PLAIN));
    json_object_put(req);
    if (rc) {
        fprintf(stderr, "ros2ebpf-ctl: send: %s\n", strerror(-rc));
        return 3;
    }
    char *line = read_line(fd);
    close(fd);
    if (!line || !line[0]) {
        fprintf(stderr, "ros2ebpf-ctl: no response\n");
        return 3;
    }
    struct json_object *resp = json_tokener_parse(line);
    free(line);
    if (!resp) {
        fprintf(stderr, "ros2ebpf-ctl: bad response\n");
        return 3;
    }
    struct json_object *err = NULL, *result = NULL;
    int exit_code = 0;
    if (json_object_object_get_ex(resp, "error", &err)) {
        fprintf(stderr, "error: %s\n", json_object_to_json_string(err));
        exit_code = 1;
    } else if (json_object_object_get_ex(resp, "result", &result)) {
        struct json_object *text = NULL, *healthy = NULL;
        if (!strcmp(method, "status") && json_object_object_get_ex(result, "healthy", &healthy))
            exit_code = json_object_get_boolean(healthy) ? 0 : 1;
        if (!quiet) {
            if (!strcmp(method, "metrics") && json_object_object_get_ex(result, "text", &text))
                fputs(json_object_get_string(text), stdout);
            else
                printf("%s\n", json_object_to_json_string_ext(result, JSON_C_TO_STRING_PRETTY));
        }
    }
    json_object_put(resp);
    return exit_code;
}
