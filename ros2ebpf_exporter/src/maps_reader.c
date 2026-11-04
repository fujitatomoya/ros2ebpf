// SPDX-License-Identifier: Apache-2.0
/*
 * Opens the pinned maps and copies them into a snapshot. Only the pinned
 * path contract from maps.h is used, so this works for the daemon (which
 * also owns the maps through its skeleton) and for an external reader.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <bpf/bpf.h>
#include "ros2ebpf/exporter.h"

static const char *map_names[ROS2EBPF_MI_COUNT] = {
    [ROS2EBPF_MI_CONFIG]        = ROS2EBPF_MAP_CONFIG,
    [ROS2EBPF_MI_ENDPOINTS]     = ROS2EBPF_MAP_ENDPOINTS,
    [ROS2EBPF_MI_ENDPOINT_INFO] = ROS2EBPF_MAP_ENDPOINT_INFO,
    [ROS2EBPF_MI_PROCESSES]     = ROS2EBPF_MAP_PROCESSES,
    [ROS2EBPF_MI_SELF]          = ROS2EBPF_MAP_SELF,
    [ROS2EBPF_MI_UDP_DROPS]     = ROS2EBPF_MAP_UDP_DROPS,
};

int ros2ebpf_maps_open(struct ros2ebpf_maps *m, const char *pin_root)
{
    for (int i = 0; i < ROS2EBPF_MI_COUNT; i++)
        m->fd[i] = -1;
    for (int i = 0; i < ROS2EBPF_MI_COUNT; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", pin_root, map_names[i]);
        int fd = bpf_obj_get(path);
        if (fd < 0) {
            int err = -errno;
            if (i == ROS2EBPF_MI_UDP_DROPS)
                continue;
            ros2ebpf_maps_close(m);
            return err;
        }
        m->fd[i] = fd;
    }
    return 0;
}

void ros2ebpf_maps_close(struct ros2ebpf_maps *m)
{
    for (int i = 0; i < ROS2EBPF_MI_COUNT; i++) {
        if (m->fd[i] >= 0)
            close(m->fd[i]);
        m->fd[i] = -1;
    }
}

/* Generic hash-map dump: grows `*out` (element size `elem`) by calling `store`. */
static int dump_hash(int fd, size_t key_size, size_t val_size,
                     void **out, size_t *n, size_t elem,
                     void (*store)(void *dst, const void *key, const void *val),
                     __u64 *errors)
{
    unsigned char *key = calloc(1, key_size), *next = calloc(1, key_size);
    unsigned char *val = calloc(1, val_size);
    size_t cap = 0;
    int rc = 0;
    if (!key || !next || !val) {
        rc = -ENOMEM;
        goto out;
    }
    const void *prev = NULL;
    while (bpf_map_get_next_key(fd, prev, next) == 0) {
        memcpy(key, next, key_size);
        prev = key;
        if (bpf_map_lookup_elem(fd, key, val) != 0) {
            (*errors)++;            /* deleted between get_next_key and lookup */
            continue;
        }
        if (*n == cap) {
            cap = cap ? cap * 2 : 64;
            void *p = realloc(*out, cap * elem);
            if (!p) {
                rc = -ENOMEM;
                goto out;
            }
            *out = p;
        }
        store((unsigned char *)*out + (*n) * elem, key, val);
        (*n)++;
    }
out:
    free(key);
    free(next);
    free(val);
    return rc;
}

static void store_endpoint(void *dst, const void *key, const void *val)
{
    struct ros2ebpf_endpoint_entry *e = dst;
    memset(e, 0, sizeof(*e));
    memcpy(&e->key, key, sizeof(e->key));
    memcpy(&e->stats, val, sizeof(e->stats));
}

static void store_process(void *dst, const void *key, const void *val)
{
    struct ros2ebpf_process_entry *e = dst;
    memcpy(&e->pid, key, sizeof(e->pid));
    memcpy(&e->info, val, sizeof(e->info));
}

static void store_udp(void *dst, const void *key, const void *val)
{
    struct ros2ebpf_udp_entry *e = dst;
    memcpy(&e->key, key, sizeof(e->key));
    memcpy(&e->stats, val, sizeof(e->stats));
}

int ros2ebpf_snapshot_read(const struct ros2ebpf_maps *m, struct ros2ebpf_snapshot *s)
{
    memset(s, 0, sizeof(*s));
    __u32 zero = 0;
    if (bpf_map_lookup_elem(m->fd[ROS2EBPF_MI_CONFIG], &zero, &s->config) == 0)
        s->config_ok = 1;

    for (__u32 i = 0; i < ROS2EBPF_SC_COUNT; i++)
        bpf_map_lookup_elem(m->fd[ROS2EBPF_MI_SELF], &i, &s->self[i]);

    int rc = dump_hash(m->fd[ROS2EBPF_MI_ENDPOINTS],
                       sizeof(struct ros2ebpf_endpoint_key), sizeof(struct ros2ebpf_endpoint_stats),
                       (void **)&s->endpoints, &s->n_endpoints,
                       sizeof(struct ros2ebpf_endpoint_entry), store_endpoint, &s->read_errors);
    if (rc)
        goto fail;
    for (size_t i = 0; i < s->n_endpoints; i++) {
        struct ros2ebpf_endpoint_entry *e = &s->endpoints[i];
        if (bpf_map_lookup_elem(m->fd[ROS2EBPF_MI_ENDPOINT_INFO], &e->key, &e->info) == 0)
            e->has_info = 1;
        else
            s->read_errors++;
    }

    rc = dump_hash(m->fd[ROS2EBPF_MI_PROCESSES], sizeof(__u32), sizeof(struct ros2ebpf_process_info),
                   (void **)&s->processes, &s->n_processes,
                   sizeof(struct ros2ebpf_process_entry), store_process, &s->read_errors);
    if (rc)
        goto fail;

    if (m->fd[ROS2EBPF_MI_UDP_DROPS] >= 0) {
        rc = dump_hash(m->fd[ROS2EBPF_MI_UDP_DROPS], sizeof(struct ros2ebpf_udp_key),
                       sizeof(struct ros2ebpf_udp_stats), (void **)&s->udp, &s->n_udp,
                       sizeof(struct ros2ebpf_udp_entry), store_udp, &s->read_errors);
        if (rc)
            goto fail;
        s->udp_ok = 1;
    }
    return 0;
fail:
    ros2ebpf_snapshot_free(s);
    return rc;
}

void ros2ebpf_snapshot_free(struct ros2ebpf_snapshot *s)
{
    free(s->endpoints);
    free(s->processes);
    free(s->udp);
    s->endpoints = NULL;
    s->processes = NULL;
    s->udp = NULL;
    s->n_endpoints = s->n_processes = s->n_udp = 0;
}
