// SPDX-License-Identifier: Apache-2.0
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <bpf/bpf.h>
#include "udpmap.h"

#define CACHE_TTL_S 10

struct port_inode { __u16 port; unsigned long inode; };

static int load_ports(struct port_inode *out, int max)
{
    int n = 0;
    const char *files[] = { "/proc/net/udp", "/proc/net/udp6" };
    for (int f = 0; f < 2; f++) {
        FILE *fp = fopen(files[f], "r");
        if (!fp)
            continue;
        char line[512];
        fgets(line, sizeof(line), fp);   /* header */
        while (n < max && fgets(line, sizeof(line), fp)) {
            char local[64];
            unsigned long inode;
            /* sl local_address rem_address st tx_queue:rx_queue tr:tm->when retrnsmt uid timeout inode */
            if (sscanf(line, "%*d: %63s %*s %*s %*s %*s %*s %*s %*s %lu", local, &inode) != 2)
                continue;
            const char *colon = strrchr(local, ':');
            if (!colon)
                continue;
            out[n].port = (__u16)strtoul(colon + 1, NULL, 16);
            out[n].inode = inode;
            n++;
        }
        fclose(fp);
    }
    return n;
}

static void rebuild(struct udpmap *m)
{
    struct port_inode ports[1024];
    int np = load_ports(ports, 1024);
    m->n_cache = 0;

    __u32 cur, next;
    void *pk = NULL;
    while (m->n_cache < 256 && bpf_map_get_next_key(m->processes_fd, pk, &next) == 0) {
        cur = next;
        pk = &cur;
        struct ros2ebpf_process_info info;
        if (bpf_map_lookup_elem(m->processes_fd, &cur, &info))
            continue;
        char dirpath[64];
        snprintf(dirpath, sizeof(dirpath), "/proc/%u/fd", cur);
        DIR *d = opendir(dirpath);
        if (!d)
            continue;
        struct dirent *de;
        while ((de = readdir(d)) != NULL && m->n_cache < 256) {
            char lpath[384], target[64];
            snprintf(lpath, sizeof(lpath), "%s/%s", dirpath, de->d_name);
            ssize_t l = readlink(lpath, target, sizeof(target) - 1);
            if (l <= 0)
                continue;
            target[l] = '\0';
            unsigned long inode;
            if (sscanf(target, "socket:[%lu]", &inode) != 1)
                continue;
            for (int i = 0; i < np; i++)
                if (ports[i].inode == inode) {
                    m->cache[m->n_cache].port = ports[i].port;
                    m->cache[m->n_cache].pid = cur;
                    memcpy(m->cache[m->n_cache].comm, info.comm, ROS2EBPF_COMM_LEN);
                    m->cache[m->n_cache].comm[ROS2EBPF_COMM_LEN - 1] = '\0';
                    m->n_cache++;
                    break;
                }
        }
        closedir(d);
    }
    m->cache_time = (long)time(NULL);
}

int udpmap_resolve(void *ctx, __u16 lport, __u16 family, char *comm, size_t comm_len, __u32 *pid)
{
    (void)family;
    struct udpmap *m = ctx;
    if ((long)time(NULL) - m->cache_time > CACHE_TTL_S)
        rebuild(m);
    for (int i = 0; i < m->n_cache; i++)
        if (m->cache[i].port == lport) {
            snprintf(comm, comm_len, "%s", m->cache[i].comm);
            *pid = m->cache[i].pid;
            return 0;
        }
    return -1;
}
