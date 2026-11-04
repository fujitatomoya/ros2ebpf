/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ROS2EBPF_IDENTITY_H
#define ROS2EBPF_IDENTITY_H
#include "config.h"
#include "ros2ebpf/maps.h"

struct identity {
    const struct config *cfg;
    int processes_fd, endpoints_fd, info_fd, seen_fd;
    __u64 environ_failures;
    __u64 resolve_failures;
    __u64 resolved;
};

/* Read /proc/<pid>/{comm,environ,cgroup,stat} and write the processes map. */
int identity_resolve_pid(struct identity *id, __u32 pid);
/* Delete every entry belonging to pid from endpoints, endpoint_info, processes, seen_pids. */
int identity_sweep_pid(struct identity *id, __u32 pid);
/* Periodic safety net: sweep dead pids, resolve pids seen by BPF but not yet resolved. */
void identity_gc(struct identity *id);
/* Fill `domains` with distinct domain ids in the processes map; returns count. */
int identity_domains_seen(struct identity *id, __u32 *domains, int max);

#endif
