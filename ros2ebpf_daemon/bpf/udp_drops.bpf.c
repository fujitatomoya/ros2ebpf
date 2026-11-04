// SPDX-License-Identifier: Apache-2.0
/*
 * ros2ebpf: UDP receive-queue drops, keyed by the receiving socket's local
 * port. The BTF-typed tracepoint hands us the socket on every kernel
 * (older: (rc, sk); 6.11+: (rc, sk, skb)), and needs no tracefs mount, so it
 * works from a container. It fires in softirq context: attribution to a
 * process is done by the daemon from /proc/net/udp{,6} at scrape time.
 */
#include "vmlinux_min.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "ros2ebpf/maps.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, ROS2EBPF_MAX_PORTS);
    __type(key, struct ros2ebpf_udp_key);
    __type(value, struct ros2ebpf_udp_stats);
    __uint(pinning, LIBBPF_PIN_BY_NAME);
} udp_drops SEC(".maps");

SEC("tp_btf/udp_fail_queue_rcv_skb")
int BPF_PROG(udp_fail_queue_rcv_skb, int rc, struct sock *sk)
{
    if (!sk)
        return 0;
    struct ros2ebpf_udp_key key = {};
    key.lport = BPF_CORE_READ(sk, __sk_common.skc_num);
    key.family = BPF_CORE_READ(sk, __sk_common.skc_family);

    struct ros2ebpf_udp_stats *st = bpf_map_lookup_elem(&udp_drops, &key);
    if (st) {
        __sync_fetch_and_add(&st->drops, 1);
        return 0;
    }
    struct ros2ebpf_udp_stats fresh = { .drops = 1 };
    bpf_map_update_elem(&udp_drops, &key, &fresh, BPF_NOEXIST);
    return 0;
}
