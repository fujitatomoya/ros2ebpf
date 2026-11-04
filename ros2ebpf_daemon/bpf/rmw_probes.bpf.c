// SPDX-License-Identifier: Apache-2.0
/*
 * ros2ebpf: uprobes on librmw_implementation.so.
 *
 * Every rmw_* entry point receives a public rmw handle as its first argument
 * (rmw_publisher_t, rmw_subscription_t, rmw_client_t, rmw_service_t). The
 * handle starts with `const char *implementation_identifier`, followed by
 * `void *data`, followed by the name pointer; the name offset comes from the
 * `config` map so a per-(distro, arch) table in the daemon decides it.
 *
 * Hot path: one hash lookup + atomic add. Cold path (first sight of a
 * handle): read the identifier and the name from user memory, decide the
 * filter flags, create the entries, and tell the daemon about an unseen pid.
 */
#include "vmlinux_min.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "ros2ebpf/maps.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

#ifndef BPF_UPROBE
#define BPF_UPROBE BPF_KPROBE
#endif
#ifndef BPF_URETPROBE
#define BPF_URETPROBE BPF_KRETPROBE
#endif

/* ---- maps (pinned ones form the exporter contract, see maps.h) ---------- */

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct ros2ebpf_config);
    __uint(pinning, LIBBPF_PIN_BY_NAME);
} config SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, ROS2EBPF_MAX_ENDPOINTS);
    __type(key, struct ros2ebpf_endpoint_key);
    __type(value, struct ros2ebpf_endpoint_stats);
    __uint(pinning, LIBBPF_PIN_BY_NAME);
} endpoints SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, ROS2EBPF_MAX_ENDPOINTS);
    __type(key, struct ros2ebpf_endpoint_key);
    __type(value, struct ros2ebpf_endpoint_info);
    __uint(pinning, LIBBPF_PIN_BY_NAME);
} endpoint_info SEC(".maps");

/* Written by the daemon, read by the exporter. BPF never touches it. */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, ROS2EBPF_MAX_PIDS);
    __type(key, __u32);
    __type(value, struct ros2ebpf_process_info);
    __uint(pinning, LIBBPF_PIN_BY_NAME);
} processes SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, ROS2EBPF_MAX_FILTERS);
    __type(key, __u64);                 /* ros2ebpf_name_hash(name) */
    __type(value, __u32);               /* ROS2EBPF_F_* */
    __uint(pinning, LIBBPF_PIN_BY_NAME);
} name_filter SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);
    __uint(pinning, LIBBPF_PIN_BY_NAME);
} events SEC(".maps");

/* pids already announced through `events`; pinned so a daemon restart does
 * not re-announce every process (their identity is still in `processes`). */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, ROS2EBPF_MAX_PIDS);
    __type(key, __u32);
    __type(value, __u8);
    __uint(pinning, LIBBPF_PIN_BY_NAME);
} seen_pids SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, ROS2EBPF_SC_COUNT);
    __type(key, __u32);
    __type(value, __u64);
    __uint(pinning, LIBBPF_PIN_BY_NAME);
} self_counters SEC(".maps");

static __always_inline void self_inc(__u32 idx)
{
    __u64 *v = bpf_map_lookup_elem(&self_counters, &idx);
    if (v)
        __sync_fetch_and_add(v, 1);
}

/* entry -> return scratch for the take_* uretprobes, keyed by pid_tgid */
struct take_args {
    __u64 handle;
    __u64 taken_ptr;
    __u64 info_ptr;
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 8192);
    __type(key, __u64);
    __type(value, struct take_args);
} take_args SEC(".maps");

/* ---- helpers ------------------------------------------------------------ */

static __always_inline __u32 cur_pid(void)
{
    return (__u32)(bpf_get_current_pid_tgid() >> 32);
}

static __always_inline void announce_pid(__u32 pid, __u64 now)
{
    __u8 one = 1;
    /* BPF_NOEXIST makes the first caller win; others see -EEXIST and skip */
    if (bpf_map_update_elem(&seen_pids, &pid, &one, BPF_NOEXIST) != 0)
        return;
    struct ros2ebpf_event *ev = bpf_ringbuf_reserve(&events, sizeof(*ev), 0);
    if (!ev) {
        self_inc(ROS2EBPF_SC_RINGBUF_FULL);
        return;
    }
    ev->type = ROS2EBPF_EV_NEW_PID;
    ev->pid = pid;
    ev->ts_ns = now;
    bpf_ringbuf_submit(ev, 0);
}

static __always_inline __u32 name_offset(const struct ros2ebpf_config *cfg, __u8 kind)
{
    switch (kind) {
    case ROS2EBPF_KIND_PUBLISHER:    return cfg->name_off_pub;
    case ROS2EBPF_KIND_SUBSCRIPTION: return cfg->name_off_sub;
    case ROS2EBPF_KIND_CLIENT:       return cfg->name_off_client;
    case ROS2EBPF_KIND_SERVICE:      return cfg->name_off_service;
    }
    return 0;
}

/* Cold path. Returns the stats entry or NULL. */
static __noinline struct ros2ebpf_endpoint_stats *
first_sight(struct ros2ebpf_endpoint_key *key, __u8 kind, __u64 now)
{
    __u32 zero = 0;
    const struct ros2ebpf_config *cfg = bpf_map_lookup_elem(&config, &zero);
    if (!cfg)
        return NULL;

    struct ros2ebpf_endpoint_info info = {};
    __u32 flags = 0;
    const char *ptr = NULL;

    info.kind = kind;
    info.first_seen_ns = now;

    /* implementation_identifier is always the first field of an rmw handle */
    if (bpf_probe_read_user(&ptr, sizeof(ptr), (void *)key->handle) != 0 ||
        bpf_probe_read_user_str(info.rmw_id, sizeof(info.rmw_id), ptr) < 0) {
        flags |= ROS2EBPF_F_ID_FAIL;
        self_inc(ROS2EBPF_SC_ID_READ_FAIL);
    }

    ptr = NULL;
    if (bpf_probe_read_user(&ptr, sizeof(ptr),
                            (void *)(key->handle + name_offset(cfg, kind))) != 0 ||
        bpf_probe_read_user_str(info.name, sizeof(info.name), ptr) < 0) {
        flags |= ROS2EBPF_F_NAME_FAIL;
        self_inc(ROS2EBPF_SC_NAME_READ_FAIL);
    }

    if (!(flags & ROS2EBPF_F_NAME_FAIL)) {
        __u64 h = ros2ebpf_name_hash(info.name, sizeof(info.name));
        const __u32 *f = bpf_map_lookup_elem(&name_filter, &h);
        flags |= f ? *f : cfg->default_flags;
    }
    info.flags = (__u8)flags;

    bpf_map_update_elem(&endpoint_info, key, &info, BPF_NOEXIST);

    struct ros2ebpf_endpoint_stats fresh = {};
    fresh.flags = flags;
    fresh.kind = kind;
    if (bpf_map_update_elem(&endpoints, key, &fresh, BPF_NOEXIST) != 0) {
        struct ros2ebpf_endpoint_stats *existing = bpf_map_lookup_elem(&endpoints, key);
        if (!existing)
            self_inc(ROS2EBPF_SC_MAP_FULL);
        return existing;
    }

    announce_pid(key->pid, now);
    return bpf_map_lookup_elem(&endpoints, key);
}

static __always_inline struct ros2ebpf_endpoint_stats *
lookup_or_create(__u64 handle, __u8 kind, __u64 now)
{
    struct ros2ebpf_endpoint_key key = {};
    key.pid = cur_pid();
    key.handle = handle;
    struct ros2ebpf_endpoint_stats *st = bpf_map_lookup_elem(&endpoints, &key);
    if (!st)
        st = first_sight(&key, kind, now);
    if (!st || (st->flags & ROS2EBPF_F_EXCLUDE))
        return NULL;
    return st;
}

static __always_inline void record_interval(struct ros2ebpf_endpoint_stats *st, __u64 now)
{
    __u64 last = st->last_ns;
    st->last_ns = now;
    if (!last || now <= last)
        return;
    __u64 dt = now - last;
    __sync_fetch_and_add(&st->period_sum_ns, dt);

    __u32 zero = 0;
    const struct ros2ebpf_config *cfg = bpf_map_lookup_elem(&config, &zero);
    if (!cfg)
        return;
    __u32 n = cfg->nbuckets;
    if (n > ROS2EBPF_HIST_BUCKETS - 1)
        n = ROS2EBPF_HIST_BUCKETS - 1;
    __u32 b = n; /* +Inf */
    for (__u32 i = 0; i < ROS2EBPF_HIST_BUCKETS - 1; i++) {
        if (i >= n)
            break;
        if (dt <= cfg->bounds_ns[i]) {
            b = i;
            break;
        }
    }
    __sync_fetch_and_add(&st->hist[b & (ROS2EBPF_HIST_BUCKETS - 1)], 1);
}

/* ---- publishers --------------------------------------------------------- */

static __always_inline int on_publish(struct pt_regs *ctx)
{
    __u64 now = bpf_ktime_get_ns();
    struct ros2ebpf_endpoint_stats *st =
        lookup_or_create((__u64)PT_REGS_PARM1(ctx), ROS2EBPF_KIND_PUBLISHER, now);
    if (!st)
        return 0;
    __sync_fetch_and_add(&st->count, 1);
    record_interval(st, now);
    return 0;
}

SEC("uprobe/rmw_publish")
int BPF_UPROBE(rmw_publish_enter) { return on_publish(ctx); }

SEC("uprobe/rmw_publish_loaned_message")
int BPF_UPROBE(rmw_publish_loaned_enter) { return on_publish(ctx); }

/* ---- takes (uretprobe: only count when taken == true) ------------------- */

static __always_inline int save_take_args(__u64 handle, __u64 taken_ptr, __u64 info_ptr)
{
    __u64 tid = bpf_get_current_pid_tgid();
    struct take_args a = { .handle = handle, .taken_ptr = taken_ptr, .info_ptr = info_ptr };
    bpf_map_update_elem(&take_args, &tid, &a, BPF_ANY);
    return 0;
}

/* rmw_take_with_info(sub, msg, bool *taken, rmw_message_info_t *info, alloc) */
SEC("uprobe/rmw_take_with_info")
int BPF_UPROBE(rmw_take_with_info_enter)
{
    return save_take_args((__u64)PT_REGS_PARM1(ctx), (__u64)PT_REGS_PARM3(ctx),
                          (__u64)PT_REGS_PARM4(ctx));
}

static __always_inline int on_take_return(struct pt_regs *ctx, __u8 kind, int secondary)
{
    __u64 tid = bpf_get_current_pid_tgid();
    struct take_args *a = bpf_map_lookup_elem(&take_args, &tid);
    if (!a)
        return 0;
    struct take_args args = *a;
    bpf_map_delete_elem(&take_args, &tid);

    if ((int)PT_REGS_RC(ctx) != 0)           /* RMW_RET_OK == 0 */
        return 0;
    bool taken = false;
    if (bpf_probe_read_user(&taken, sizeof(taken), (void *)args.taken_ptr) != 0 || !taken)
        return 0;

    __u64 now = bpf_ktime_get_ns();
    struct ros2ebpf_endpoint_stats *st = lookup_or_create(args.handle, kind, now);
    if (!st)
        return 0;
    __sync_fetch_and_add(secondary ? &st->count2 : &st->count, 1);

    if (kind == ROS2EBPF_KIND_SUBSCRIPTION && (st->flags & ROS2EBPF_F_SEQ_GAP) && args.info_ptr) {
        __u32 zero = 0;
        const struct ros2ebpf_config *cfg = bpf_map_lookup_elem(&config, &zero);
        if (!cfg)
            return 0;
        __u64 seq = 0;
        if (bpf_probe_read_user(&seq, sizeof(seq),
                                (void *)(args.info_ptr + cfg->msginfo_off_pubseq)) != 0)
            return 0;
        if (seq == 0xffffffffffffffffull)     /* RMW_MESSAGE_INFO_SEQUENCE_NUMBER_UNSUPPORTED */
            return 0;
        __u64 last = st->last_seq;
        st->last_seq = seq;
        if (last && seq > last + 1) {
            __sync_fetch_and_add(&st->seq_gaps, 1);
            __sync_fetch_and_add(&st->seq_lost, seq - last - 1);
        }
    }
    return 0;
}

SEC("uretprobe/rmw_take_with_info")
int BPF_URETPROBE(rmw_take_with_info_exit)
{
    return on_take_return(ctx, ROS2EBPF_KIND_SUBSCRIPTION, 0);
}

/* ---- services ----------------------------------------------------------- */

/* rmw_send_request(client, request, sequence_id) */
SEC("uprobe/rmw_send_request")
int BPF_UPROBE(rmw_send_request_enter)
{
    __u64 now = bpf_ktime_get_ns();
    struct ros2ebpf_endpoint_stats *st =
        lookup_or_create((__u64)PT_REGS_PARM1(ctx), ROS2EBPF_KIND_CLIENT, now);
    if (st)
        __sync_fetch_and_add(&st->count, 1);
    return 0;
}

/* rmw_take_response(client, request_header, response, bool *taken) */
SEC("uprobe/rmw_take_response")
int BPF_UPROBE(rmw_take_response_enter)
{
    return save_take_args((__u64)PT_REGS_PARM1(ctx), (__u64)PT_REGS_PARM4(ctx), 0);
}

SEC("uretprobe/rmw_take_response")
int BPF_URETPROBE(rmw_take_response_exit)
{
    return on_take_return(ctx, ROS2EBPF_KIND_CLIENT, 1);
}

/* rmw_take_request(service, request_header, request, bool *taken) */
SEC("uprobe/rmw_take_request")
int BPF_UPROBE(rmw_take_request_enter)
{
    return save_take_args((__u64)PT_REGS_PARM1(ctx), (__u64)PT_REGS_PARM4(ctx), 0);
}

SEC("uretprobe/rmw_take_request")
int BPF_URETPROBE(rmw_take_request_exit)
{
    return on_take_return(ctx, ROS2EBPF_KIND_SERVICE, 0);
}

/* rmw_send_response(service, request_header, response) */
SEC("uprobe/rmw_send_response")
int BPF_UPROBE(rmw_send_response_enter)
{
    __u64 now = bpf_ktime_get_ns();
    struct ros2ebpf_endpoint_stats *st =
        lookup_or_create((__u64)PT_REGS_PARM1(ctx), ROS2EBPF_KIND_SERVICE, now);
    if (st)
        __sync_fetch_and_add(&st->count2, 1);
    return 0;
}

/* ---- cleanup ------------------------------------------------------------ */

/* rmw_destroy_<kind>(rmw_node_t *node, <handle> *h): the handle is the 2nd argument */
static __always_inline int on_destroy(struct pt_regs *ctx)
{
    struct ros2ebpf_endpoint_key key = {};
    key.pid = cur_pid();
    key.handle = (__u64)PT_REGS_PARM2(ctx);
    bpf_map_delete_elem(&endpoints, &key);
    bpf_map_delete_elem(&endpoint_info, &key);
    return 0;
}

SEC("uprobe/rmw_destroy_publisher")
int BPF_UPROBE(rmw_destroy_publisher_enter) { return on_destroy(ctx); }
SEC("uprobe/rmw_destroy_subscription")
int BPF_UPROBE(rmw_destroy_subscription_enter) { return on_destroy(ctx); }
SEC("uprobe/rmw_destroy_client")
int BPF_UPROBE(rmw_destroy_client_enter) { return on_destroy(ctx); }
SEC("uprobe/rmw_destroy_service")
int BPF_UPROBE(rmw_destroy_service_enter) { return on_destroy(ctx); }

/* Group leader exit: the daemon sweeps every key of this pid out of the maps
 * (iterating a hash map by pid is cheap in user space and free here). */
SEC("tp_btf/sched_process_exit")
int BPF_PROG(sched_process_exit, struct task_struct *p)
{
    __u64 id = bpf_get_current_pid_tgid();
    __u32 tgid = id >> 32, tid = (__u32)id;
    if (tgid != tid)
        return 0;
    if (bpf_map_delete_elem(&seen_pids, &tgid) != 0)
        return 0;                            /* never produced an endpoint hit */
    struct ros2ebpf_event *ev = bpf_ringbuf_reserve(&events, sizeof(*ev), 0);
    if (!ev) {
        self_inc(ROS2EBPF_SC_RINGBUF_FULL);
        return 0;
    }
    ev->type = ROS2EBPF_EV_EXIT_PID;
    ev->pid = tgid;
    ev->ts_ns = bpf_ktime_get_ns();
    bpf_ringbuf_submit(ev, 0);
    return 0;
}
