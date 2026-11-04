/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ros2ebpf map contract.
 *
 * This header is the only thing the exporter and the daemon share about the
 * data in the kernel. It is included from BPF C (compiled with clang -target
 * bpf) and from user-space C, so it must stay free of libc types and of
 * anything that is not plain fixed-width integers and arrays.
 *
 * Every map below is pinned by name under the pin root
 * (default /sys/fs/bpf/ros2ebpf). A separate reader opens them with
 * bpf_obj_get(<root>/<name>) and must check ROS2EBPF_MAP_ABI against the
 * `abi` field of the `config` map entry before interpreting anything else.
 */
#ifndef ROS2EBPF_MAPS_H
#define ROS2EBPF_MAPS_H

#ifndef __BPF__
#include <linux/types.h>   /* __u8 .. __u64; in BPF they come from vmlinux_min.h */
#endif

/* Bump whenever a struct or map in this file changes layout. */
#define ROS2EBPF_MAP_ABI 1

#define ROS2EBPF_NAME_LEN      128   /* topic / service name, truncated */
#define ROS2EBPF_RMW_ID_LEN     32   /* implementation_identifier, truncated */
#define ROS2EBPF_COMM_LEN       16   /* TASK_COMM_LEN */
#define ROS2EBPF_CONTAINER_LEN  64
#define ROS2EBPF_HIST_BUCKETS   16   /* bounded buckets + 1 overflow (+Inf) */
#define ROS2EBPF_MAX_ENDPOINTS  65536
#define ROS2EBPF_MAX_PIDS       4096
#define ROS2EBPF_MAX_FILTERS    1024
#define ROS2EBPF_MAX_PORTS      4096

#define ROS2EBPF_DOMAIN_UNKNOWN 0xffffffffu

/* Endpoint kinds, stored in ros2ebpf_endpoint_info.kind */
enum ros2ebpf_kind {
    ROS2EBPF_KIND_PUBLISHER    = 1,
    ROS2EBPF_KIND_SUBSCRIPTION = 2,
    ROS2EBPF_KIND_CLIENT       = 3,
    ROS2EBPF_KIND_SERVICE      = 4,
};

/* Flags, stored in ros2ebpf_endpoint_info.flags and in the name filter map */
#define ROS2EBPF_F_EXCLUDE    (1u << 0)  /* do not count; keep a tombstone so the hot path is one lookup */
#define ROS2EBPF_F_SEQ_GAP    (1u << 1)  /* track publication_sequence_number gaps on take */
#define ROS2EBPF_F_NAME_FAIL  (1u << 2)  /* bpf_probe_read_user_str of the name failed */
#define ROS2EBPF_F_ID_FAIL    (1u << 3)  /* reading implementation_identifier failed */

/* Key of the `endpoints` and `endpoint_info` maps. */
struct ros2ebpf_endpoint_key {
    __u32 pid;        /* tgid */
    __u32 pad;
    __u64 handle;     /* address of rmw_publisher_t / rmw_subscription_t / ... in the process */
};

/* Per-endpoint hot counters. One entry per (pid, handle). Updated with atomic adds. */
struct ros2ebpf_endpoint_stats {
    __u32 flags;                              /* copy of endpoint_info.flags so the hot path needs one lookup */
    __u32 kind;                               /* copy of endpoint_info.kind */
    __u64 count;                              /* publishes / takes; client: requests sent; service: requests taken */
    __u64 count2;                             /* client: responses taken; service: responses sent; else 0 */
    __u64 last_ns;                            /* bpf_ktime_get_ns() of the previous hit (publishers) */
    __u64 last_seq;                           /* last publication_sequence_number seen (subscriptions) */
    __u64 seq_gaps;                           /* number of gap events (not messages lost) */
    __u64 seq_lost;                           /* sum of (seq - last_seq - 1) over gap events */
    __u64 period_sum_ns;                      /* sum of inter-hit intervals (histogram _sum) */
    __u64 hist[ROS2EBPF_HIST_BUCKETS];        /* inter-hit interval histogram, cumulative in exporter */
};

/* Per-endpoint cold facts, written once at first sight. */
struct ros2ebpf_endpoint_info {
    __u8  kind;                               /* enum ros2ebpf_kind */
    __u8  flags;                              /* ROS2EBPF_F_* */
    __u16 pad;
    __u32 pad2;
    __u64 first_seen_ns;
    char  name[ROS2EBPF_NAME_LEN];            /* topic_name / service_name as rmw sees it */
    char  rmw_id[ROS2EBPF_RMW_ID_LEN];        /* implementation_identifier (offset 0 of every rmw handle) */
};

/* Per-process identity, resolved by the daemon from /proc and pinned for the exporter. */
struct ros2ebpf_process_info {
    __u32 domain_id;                          /* ROS2EBPF_DOMAIN_UNKNOWN if environ unreadable */
    __u8  localhost_only;
    __u8  environ_ok;
    __u16 pad;
    __u64 start_ns;
    char  comm[ROS2EBPF_COMM_LEN];
    char  rmw[ROS2EBPF_RMW_ID_LEN];           /* RMW_IMPLEMENTATION or "" */
    char  container[ROS2EBPF_CONTAINER_LEN];  /* from the cgroup path, "" if none */
};

/* Single entry of the `config` ARRAY map, written by the daemon before attach. */
struct ros2ebpf_config {
    __u32 abi;                                /* ROS2EBPF_MAP_ABI */
    __u32 nbuckets;                           /* number of finite histogram bounds, <= ROS2EBPF_HIST_BUCKETS - 1 */
    __u64 bounds_ns[ROS2EBPF_HIST_BUCKETS];   /* ascending upper bounds in ns; bucket nbuckets is +Inf */
    __u32 name_off_pub;                       /* offsetof(rmw_publisher_t, topic_name) */
    __u32 name_off_sub;                       /* offsetof(rmw_subscription_t, topic_name) */
    __u32 name_off_client;                    /* offsetof(rmw_client_t, service_name) */
    __u32 name_off_service;                   /* offsetof(rmw_service_t, service_name) */
    __u32 msginfo_off_pubseq;                 /* offsetof(rmw_message_info_t, publication_sequence_number) */
    __u32 default_flags;                      /* flags for names absent from `name_filter` (ROS2EBPF_F_SEQ_GAP or 0) */
};

/* Events pushed to the `events` ring buffer. */
enum ros2ebpf_event_type {
    ROS2EBPF_EV_NEW_PID  = 1,                 /* first endpoint hit by this pid: daemon resolves identity */
    ROS2EBPF_EV_EXIT_PID = 2,                 /* sched_process_exit for a group leader: daemon drops its keys */
};

struct ros2ebpf_event {
    __u32 type;
    __u32 pid;
    __u64 ts_ns;
};

/* UDP receive-queue drops keyed by local port (the receiving socket's port). */
struct ros2ebpf_udp_key {
    __u16 lport;
    __u16 family;
    __u32 pad;
};

struct ros2ebpf_udp_stats {
    __u64 drops;
};

/* Indices into the `self_counters` ARRAY map (u64 values, monotonic). */
enum ros2ebpf_self_counter {
    ROS2EBPF_SC_NAME_READ_FAIL = 0,           /* bpf_probe_read_user_str(name) failed */
    ROS2EBPF_SC_ID_READ_FAIL   = 1,           /* implementation_identifier unreadable */
    ROS2EBPF_SC_RINGBUF_FULL   = 2,           /* event dropped: ring buffer full */
    ROS2EBPF_SC_MAP_FULL       = 3,           /* endpoint entry not created: map full */
    ROS2EBPF_SC_COUNT          = 8,
};

/* Pinned map names (also the file names under the pin root). */
#define ROS2EBPF_MAP_CONFIG      "config"
#define ROS2EBPF_MAP_ENDPOINTS   "endpoints"
#define ROS2EBPF_MAP_ENDPOINT_INFO "endpoint_info"
#define ROS2EBPF_MAP_PROCESSES   "processes"
#define ROS2EBPF_MAP_NAME_FILTER "name_filter"
#define ROS2EBPF_MAP_UDP_DROPS   "udp_drops"
#define ROS2EBPF_MAP_EVENTS      "events"
#define ROS2EBPF_MAP_SEEN_PIDS   "seen_pids"
#define ROS2EBPF_MAP_SELF        "self_counters"

#define ROS2EBPF_DEFAULT_PIN_ROOT "/sys/fs/bpf/ros2ebpf"

/*
 * FNV-1a over a NUL-terminated name, used for the `name_filter` map key.
 * The same function runs in BPF (bounded loop) and in the daemon.
 */
static inline __u64 ros2ebpf_name_hash(const char *s, __u32 maxlen)
{
    __u64 h = 0xcbf29ce484222325ull;
    __u32 i;
    for (i = 0; i < maxlen; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0)
            break;
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

#endif /* ROS2EBPF_MAPS_H */
