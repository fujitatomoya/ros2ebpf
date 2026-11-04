/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ROS2EBPF_UDPMAP_H
#define ROS2EBPF_UDPMAP_H
#include <stddef.h>
#include "ros2ebpf/maps.h"

/* Resolves a UDP local port to the ROS process owning the socket, searching
 * only pids known to the processes map. Results are cached for a few seconds. */
struct udpmap {
    int processes_fd;
    struct { __u16 port; __u32 pid; char comm[ROS2EBPF_COMM_LEN]; } cache[256];
    int n_cache;
    long cache_time;
};

int udpmap_resolve(void *ctx, __u16 lport, __u16 family, char *comm, size_t comm_len, __u32 *pid);

#endif
