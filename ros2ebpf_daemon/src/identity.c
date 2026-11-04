// SPDX-License-Identifier: Apache-2.0
/*
 * Per-process identity, resolved once per pid off the hot path. Everything
 * here is best effort: a process may exit while we read /proc, environ may
 * be unreadable without CAP_SYS_PTRACE, cgroup layouts differ per runtime.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <bpf/bpf.h>
#include "identity.h"
#include "log.h"

static int read_file(const char *path, char *buf, size_t n, size_t *len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    size_t got = 0;
    while (got < n - 1) {
        ssize_t r = read(fd, buf + got, n - 1 - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            int err = -errno;
            close(fd);
            return err;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = '\0';
    *len = got;
    return 0;
}

static void container_from_cgroup(const char *cgroup, char *out, size_t n)
{
    out[0] = '\0';
    /* use the last line (cgroup v2 has one: "0::/path") */
    const char *best = NULL;
    for (const char *line = cgroup; line && *line; ) {
        const char *nl = strchr(line, '\n');
        const char *path = strrchr(line, ':');
        if (!path)
            break;
        best = path + 1;
        if (nl && nl < path) {
            /* ':' came from a later line; walk line by line instead */
            best = NULL;
            break;
        }
        line = nl ? nl + 1 : NULL;
        /* prefer the first line containing a container marker */
        if (strstr(path, "docker") || strstr(path, "containerd") || strstr(path, "crio") || strstr(path, "podman") ||
            strstr(path, "kubepods") || strstr(path, "libpod"))
            break;
    }
    if (!best)
        return;
    char path[512];
    snprintf(path, sizeof(path), "%.*s", (int)strcspn(best, "\n"), best);

    static const char *markers[] = { "docker-", "cri-containerd-", "crio-", "libpod-", "/docker/", "/podman/", "/libpod-" };
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        const char *m = strstr(path, markers[i]);
        if (!m)
            continue;
        m += strlen(markers[i]);
        size_t l = 0;
        while (isxdigit((unsigned char)m[l]))
            l++;
        if (l >= 12) {
            snprintf(out, n, "%.12s", m);
            return;
        }
    }
    if (!strcmp(path, "/") || !strncmp(path, "/user.slice", 11) || !strncmp(path, "/system.slice", 13) ||
        !strncmp(path, "/init.scope", 11))
        return;
    const char *last = strrchr(path, '/');
    if (last && last[1])
        snprintf(out, n, "%s", last + 1);
}

int identity_resolve_pid(struct identity *id, __u32 pid)
{
    struct ros2ebpf_process_info info;
    memset(&info, 0, sizeof(info));
    info.domain_id = (__u32)id->cfg->default_domain_id;

    char path[64], buf[65536];
    size_t len;

    snprintf(path, sizeof(path), "/proc/%u/comm", pid);
    if (read_file(path, buf, sizeof(buf), &len)) {
        id->resolve_failures++;
        return -ESRCH;            /* gone already */
    }
    buf[strcspn(buf, "\n")] = '\0';
    snprintf(info.comm, sizeof(info.comm), "%.15s", buf);

    snprintf(path, sizeof(path), "/proc/%u/stat", pid);
    if (read_file(path, buf, sizeof(buf), &len) == 0) {
        /* field 22 (starttime) comes after the parenthesised comm */
        const char *p = strrchr(buf, ')');
        unsigned long long start = 0;
        if (p) {
            int field = 2;
            for (p++; *p && field < 22; p++)
                if (*p == ' ')
                    field++;
            start = strtoull(p, NULL, 10);
        }
        long hz = sysconf(_SC_CLK_TCK);
        info.start_ns = hz > 0 ? start * (1000000000ull / (unsigned long long)hz) : 0;
    }

    if (id->cfg->read_environ) {
        snprintf(path, sizeof(path), "/proc/%u/environ", pid);
        if (read_file(path, buf, sizeof(buf), &len) == 0 && len > 0) {
            info.environ_ok = 1;
            for (size_t i = 0; i < len; ) {
                const char *kv = buf + i;
                size_t l = strnlen(kv, len - i);
                if (!strncmp(kv, "ROS_DOMAIN_ID=", 14))
                    info.domain_id = (__u32)atoi(kv + 14);
                else if (!strncmp(kv, "RMW_IMPLEMENTATION=", 19))
                    snprintf(info.rmw, sizeof(info.rmw), "%s", kv + 19);
                else if (!strncmp(kv, "ROS_LOCALHOST_ONLY=", 19))
                    info.localhost_only = atoi(kv + 19) != 0;
                i += l + 1;
            }
        } else {
            info.domain_id = ROS2EBPF_DOMAIN_UNKNOWN;
            id->environ_failures++;
            log_debug("pid %u (%s): environ unreadable (need CAP_SYS_PTRACE?)", pid, info.comm);
        }
    } else {
        info.domain_id = ROS2EBPF_DOMAIN_UNKNOWN;
    }

    snprintf(path, sizeof(path), "/proc/%u/cgroup", pid);
    if (read_file(path, buf, sizeof(buf), &len) == 0)
        container_from_cgroup(buf, info.container, sizeof(info.container));

    int rc = bpf_map_update_elem(id->processes_fd, &pid, &info, BPF_ANY);
    if (rc) {
        log_warn("processes map update for pid %u: %s", pid, strerror(errno));
        return -errno;
    }
    id->resolved++;
    log_info("pid %u: comm=%s domain_id=%s%u rmw=%s container=%s", pid, info.comm,
             info.domain_id == ROS2EBPF_DOMAIN_UNKNOWN ? "unknown" : "", 
             info.domain_id == ROS2EBPF_DOMAIN_UNKNOWN ? 0 : info.domain_id,
             info.rmw[0] ? info.rmw : "-", info.container[0] ? info.container : "-");
    return 0;
}

static int sweep_endpoint_map(int fd, __u32 pid)
{
    struct ros2ebpf_endpoint_key keys[256];
    int deleted = 0;
    for (;;) {
        /* collect a batch, then delete: deleting while iterating restarts the walk */
        int n = 0;
        struct ros2ebpf_endpoint_key cur, next;
        void *pk = NULL;
        while (n < 256 && bpf_map_get_next_key(fd, pk, &next) == 0) {
            cur = next;
            pk = &cur;
            if (cur.pid == pid)
                keys[n++] = cur;
        }
        if (n == 0)
            break;
        for (int i = 0; i < n; i++)
            if (bpf_map_delete_elem(fd, &keys[i]) == 0)
                deleted++;
        if (n < 256)
            break;
    }
    return deleted;
}

int identity_sweep_pid(struct identity *id, __u32 pid)
{
    int n = sweep_endpoint_map(id->endpoints_fd, pid);
    sweep_endpoint_map(id->info_fd, pid);
    bpf_map_delete_elem(id->processes_fd, &pid);
    bpf_map_delete_elem(id->seen_fd, &pid);
    log_info("pid %u exited: removed %d endpoints", pid, n);
    return n;
}

static int pid_alive(__u32 pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%u", pid);
    return access(path, F_OK) == 0;
}

void identity_gc(struct identity *id)
{
    __u32 pids[ROS2EBPF_MAX_PIDS];
    int n = 0;
    __u32 cur, next;
    void *pk = NULL;
    while (n < ROS2EBPF_MAX_PIDS && bpf_map_get_next_key(id->seen_fd, pk, &next) == 0) {
        cur = next;
        pk = &cur;
        pids[n++] = cur;
    }
    for (int i = 0; i < n; i++) {
        if (!pid_alive(pids[i])) {
            identity_sweep_pid(id, pids[i]);
            continue;
        }
        struct ros2ebpf_process_info info;
        if (bpf_map_lookup_elem(id->processes_fd, &pids[i], &info) != 0)
            identity_resolve_pid(id, pids[i]);
    }
    /* processes without a seen_pids entry (e.g. after a map wipe) */
    n = 0;
    pk = NULL;
    while (n < ROS2EBPF_MAX_PIDS && bpf_map_get_next_key(id->processes_fd, pk, &next) == 0) {
        cur = next;
        pk = &cur;
        pids[n++] = cur;
    }
    for (int i = 0; i < n; i++)
        if (!pid_alive(pids[i]))
            identity_sweep_pid(id, pids[i]);
}

int identity_domains_seen(struct identity *id, __u32 *domains, int max)
{
    int n = 0;
    __u32 cur, next;
    void *pk = NULL;
    while (bpf_map_get_next_key(id->processes_fd, pk, &next) == 0) {
        cur = next;
        pk = &cur;
        struct ros2ebpf_process_info info;
        if (bpf_map_lookup_elem(id->processes_fd, &cur, &info))
            continue;
        int seen = 0;
        for (int i = 0; i < n; i++)
            if (domains[i] == info.domain_id)
                seen = 1;
        if (!seen && n < max)
            domains[n++] = info.domain_id;
    }
    return n;
}
