// SPDX-License-Identifier: Apache-2.0
/*
 * Prometheus text exposition from a snapshot.
 *
 * Label contract (docs/metrics.md): `host` on everything; `domain_id`,
 * `comm` and `topic`/`service` on endpoint series; no `pid` on counters.
 * Endpoints that share a label set (two publishers of one topic in one
 * process, or two processes with the same comm) are summed, so every
 * emitted series is unique.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ros2ebpf/exporter.h"

struct series_key {
    __u32 kind;
    __u32 domain_id;
    char comm[ROS2EBPF_COMM_LEN];
    char name[ROS2EBPF_NAME_LEN];
    char rmw[ROS2EBPF_RMW_ID_LEN];
};

struct series {
    struct series_key key;
    __u64 count, count2, seq_gaps, seq_lost, period_sum_ns;
    __u64 hist[ROS2EBPF_HIST_BUCKETS];
};

static int cmp_pid(const void *a, const void *b)
{
    const struct ros2ebpf_process_entry *x = a, *y = b;
    return (x->pid > y->pid) - (x->pid < y->pid);
}

static int cmp_series(const void *a, const void *b)
{
    return memcmp(&((const struct series *)a)->key, &((const struct series *)b)->key,
                  sizeof(struct series_key));
}

static const struct ros2ebpf_process_entry *
find_process(const struct ros2ebpf_process_entry *sorted, size_t n, __u32 pid)
{
    struct ros2ebpf_process_entry k = { .pid = pid };
    return bsearch(&k, sorted, n, sizeof(*sorted), cmp_pid);
}

static int rmw_id_known(const char *id)
{
    return strncmp(id, "rmw_", 4) == 0;
}

/* {host="..",robot="..",domain_id="..",comm="..",<name_label>="..",rmw=".."} */
static int labels(struct ros2ebpf_buf *b, const struct ros2ebpf_export_opts *o,
                  const struct series *s, const char *name_label)
{
    int rc = ros2ebpf_buf_append(b, "{host=\"", 7);
    rc = rc ?: ros2ebpf_buf_escape(b, o->host);
    if (o->robot && o->robot[0]) {
        rc = rc ?: ros2ebpf_buf_append(b, "\",robot=\"", 9);
        rc = rc ?: ros2ebpf_buf_escape(b, o->robot);
    }
    rc = rc ?: ros2ebpf_buf_append(b, "\",domain_id=\"", 13);
    if (s->key.domain_id == ROS2EBPF_DOMAIN_UNKNOWN)
        rc = rc ?: ros2ebpf_buf_append(b, "unknown", 7);
    else
        rc = rc ?: ros2ebpf_buf_printf(b, "%u", s->key.domain_id);
    rc = rc ?: ros2ebpf_buf_append(b, "\",comm=\"", 8);
    rc = rc ?: ros2ebpf_buf_escape(b, s->key.comm);
    rc = rc ?: ros2ebpf_buf_printf(b, "\",%s=\"", name_label);
    rc = rc ?: ros2ebpf_buf_escape(b, s->key.name);
    if (o->rmw_label) {
        rc = rc ?: ros2ebpf_buf_append(b, "\",rmw=\"", 7);
        rc = rc ?: ros2ebpf_buf_escape(b, s->key.rmw);
    }
    return rc ?: ros2ebpf_buf_append(b, "\"}", 2);
}

static int host_label_prefix(struct ros2ebpf_buf *b, const struct ros2ebpf_export_opts *o)
{
    int rc = ros2ebpf_buf_append(b, "{host=\"", 7);
    rc = rc ?: ros2ebpf_buf_escape(b, o->host);
    if (o->robot && o->robot[0]) {
        rc = rc ?: ros2ebpf_buf_append(b, "\",robot=\"", 9);
        rc = rc ?: ros2ebpf_buf_escape(b, o->robot);
    }
    return rc ?: ros2ebpf_buf_append(b, "\"}", 2);
}

static int header(struct ros2ebpf_buf *b, const char *name, const char *type, const char *help)
{
    return ros2ebpf_buf_printf(b, "# HELP %s %s\n# TYPE %s %s\n", name, help, name, type);
}

static int counter_block(struct ros2ebpf_buf *b, const struct ros2ebpf_export_opts *o,
                         const struct series *ser, size_t n, __u32 kind, int secondary,
                         const char *metric, const char *help, const char *name_label,
                         const char *extra_label /* e.g. ",side=\"client\"" or NULL */)
{
    int rc = header(b, metric, "counter", help);
    for (size_t i = 0; i < n && !rc; i++) {
        if (ser[i].key.kind != kind)
            continue;
        rc = ros2ebpf_buf_append(b, metric, strlen(metric));
        rc = rc ?: labels(b, o, &ser[i], name_label);
        if (extra_label) {
            b->len--; /* drop the closing brace to append more labels */
            rc = rc ?: ros2ebpf_buf_printf(b, "%s}", extra_label);
        }
        rc = rc ?: ros2ebpf_buf_printf(b, " %llu\n",
                                       (unsigned long long)(secondary ? ser[i].count2 : ser[i].count));
    }
    return rc;
}

static void fmt_seconds(char *out, size_t n, __u64 ns)
{
    snprintf(out, n, "%.9g", (double)ns / 1e9);
}

int ros2ebpf_render(const struct ros2ebpf_snapshot *s, const struct ros2ebpf_export_opts *o,
                    struct ros2ebpf_buf *b)
{
    if (!o || !o->host)
        return -EINVAL;
    int rc = 0;

    /* ---- sort processes, aggregate endpoints into series ---- */
    struct ros2ebpf_process_entry *procs = NULL;
    if (s->n_processes) {
        procs = malloc(s->n_processes * sizeof(*procs));
        if (!procs)
            return -ENOMEM;
        memcpy(procs, s->processes, s->n_processes * sizeof(*procs));
        qsort(procs, s->n_processes, sizeof(*procs), cmp_pid);
    }

    struct series *ser = calloc(s->n_endpoints ? s->n_endpoints : 1, sizeof(*ser));
    if (!ser) {
        free(procs);
        return -ENOMEM;
    }
    size_t n = 0, unresolved = 0, name_failed = 0;
    __u32 unknown_rmw_pids[256];
    size_t n_unknown_rmw = 0;

    for (size_t i = 0; i < s->n_endpoints; i++) {
        const struct ros2ebpf_endpoint_entry *e = &s->endpoints[i];
        if (!e->has_info || (e->stats.flags & ROS2EBPF_F_EXCLUDE))
            continue;
        if (e->info.flags & ROS2EBPF_F_NAME_FAIL) {
            name_failed++;
            continue;
        }
        const struct ros2ebpf_process_entry *p = find_process(procs, s->n_processes, e->key.pid);
        if (!p) {
            unresolved++;         /* identity not resolved yet (or process gone): next scrape */
            continue;
        }
        if (!(e->info.flags & ROS2EBPF_F_ID_FAIL) && !rmw_id_known(e->info.rmw_id)) {
            int seen = 0;
            for (size_t k = 0; k < n_unknown_rmw; k++)
                if (unknown_rmw_pids[k] == e->key.pid) { seen = 1; break; }
            if (!seen && n_unknown_rmw < 256)
                unknown_rmw_pids[n_unknown_rmw++] = e->key.pid;
        }
        struct series *x = &ser[n++];
        memset(x, 0, sizeof(*x));
        x->key.kind = e->info.kind;
        x->key.domain_id = p->info.domain_id;
        memcpy(x->key.comm, p->info.comm, sizeof(x->key.comm));
        x->key.comm[sizeof(x->key.comm) - 1] = '\0';
        memcpy(x->key.name, e->info.name, sizeof(x->key.name));
        x->key.name[sizeof(x->key.name) - 1] = '\0';
        if (o->rmw_label) {
            memcpy(x->key.rmw, e->info.rmw_id, sizeof(x->key.rmw));
            x->key.rmw[sizeof(x->key.rmw) - 1] = '\0';
        }
        x->count = e->stats.count;
        x->count2 = e->stats.count2;
        x->seq_gaps = e->stats.seq_gaps;
        x->seq_lost = e->stats.seq_lost;
        x->period_sum_ns = e->stats.period_sum_ns;
        memcpy(x->hist, e->stats.hist, sizeof(x->hist));
    }
    free(procs);

    if (n > 1) {
        qsort(ser, n, sizeof(*ser), cmp_series);
        size_t w = 0;
        for (size_t r = 1; r < n; r++) {
            if (cmp_series(&ser[w], &ser[r]) == 0) {
                ser[w].count += ser[r].count;
                ser[w].count2 += ser[r].count2;
                ser[w].seq_gaps += ser[r].seq_gaps;
                ser[w].seq_lost += ser[r].seq_lost;
                ser[w].period_sum_ns += ser[r].period_sum_ns;
                for (int k = 0; k < ROS2EBPF_HIST_BUCKETS; k++)
                    ser[w].hist[k] += ser[r].hist[k];
            } else {
                ser[++w] = ser[r];
            }
        }
        n = w + 1;
    }

    /* ---- endpoint counters ---- */
    rc = rc ?: counter_block(b, o, ser, n, ROS2EBPF_KIND_PUBLISHER, 0, "ros2_rmw_publish_total",
                             "Calls to rmw_publish / rmw_publish_loaned_message.", "topic", NULL);

    /* publish period histogram */
    {
        const char *m = "ros2_rmw_publish_period_seconds";
        rc = rc ?: header(b, m, "histogram",
                          "Interval between consecutive publishes of the same publisher.");
        __u32 nb = s->config_ok ? s->config.nbuckets : 0;
        if (nb > ROS2EBPF_HIST_BUCKETS - 1)
            nb = ROS2EBPF_HIST_BUCKETS - 1;
        for (size_t i = 0; i < n && !rc; i++) {
            const struct series *x = &ser[i];
            if (x->key.kind != ROS2EBPF_KIND_PUBLISHER)
                continue;
            __u64 cum = 0;
            for (__u32 k = 0; k <= nb && !rc; k++) {
                cum += x->hist[k];
                char le[32];
                if (k < nb)
                    fmt_seconds(le, sizeof(le), s->config.bounds_ns[k]);
                else
                    snprintf(le, sizeof(le), "+Inf");
                rc = ros2ebpf_buf_printf(b, "%s_bucket", m);
                rc = rc ?: labels(b, o, x, "topic");
                b->len--;
                rc = rc ?: ros2ebpf_buf_printf(b, ",le=\"%s\"} %llu\n", le, (unsigned long long)cum);
            }
            char sum[32];
            fmt_seconds(sum, sizeof(sum), x->period_sum_ns);
            rc = rc ?: ros2ebpf_buf_printf(b, "%s_sum", m);
            rc = rc ?: labels(b, o, x, "topic");
            rc = rc ?: ros2ebpf_buf_printf(b, " %s\n", sum);
            rc = rc ?: ros2ebpf_buf_printf(b, "%s_count", m);
            rc = rc ?: labels(b, o, x, "topic");
            rc = rc ?: ros2ebpf_buf_printf(b, " %llu\n", (unsigned long long)cum);
        }
    }

    rc = rc ?: counter_block(b, o, ser, n, ROS2EBPF_KIND_SUBSCRIPTION, 0, "ros2_rmw_take_total",
                             "rmw_take_with_info calls that returned a message (taken == true).",
                             "topic", NULL);
    {
        rc = rc ?: header(b, "ros2_rmw_take_seq_gap_total", "counter",
                          "Gap events in publication_sequence_number on take (only for seq_gap topics).");
        for (size_t i = 0; i < n && !rc; i++) {
            if (ser[i].key.kind != ROS2EBPF_KIND_SUBSCRIPTION)
                continue;
            rc = ros2ebpf_buf_append(b, "ros2_rmw_take_seq_gap_total", 27);
            rc = rc ?: labels(b, o, &ser[i], "topic");
            rc = rc ?: ros2ebpf_buf_printf(b, " %llu\n", (unsigned long long)ser[i].seq_gaps);
        }
        rc = rc ?: header(b, "ros2_rmw_take_seq_lost_total", "counter",
                          "Messages skipped according to publication_sequence_number gaps.");
        for (size_t i = 0; i < n && !rc; i++) {
            if (ser[i].key.kind != ROS2EBPF_KIND_SUBSCRIPTION)
                continue;
            rc = ros2ebpf_buf_append(b, "ros2_rmw_take_seq_lost_total", 28);
            rc = rc ?: labels(b, o, &ser[i], "topic");
            rc = rc ?: ros2ebpf_buf_printf(b, " %llu\n", (unsigned long long)ser[i].seq_lost);
        }
    }

    rc = rc ?: header(b, "ros2_rmw_service_request_total", "counter",
                      "Service requests: side=client counts rmw_send_request, side=server counts taken rmw_take_request.");
    for (size_t i = 0; i < n && !rc; i++) {
        const struct series *x = &ser[i];
        if (x->key.kind == ROS2EBPF_KIND_CLIENT) {
            rc = ros2ebpf_buf_append(b, "ros2_rmw_service_request_total", 30);
            rc = rc ?: labels(b, o, x, "service");
            b->len--;
            rc = rc ?: ros2ebpf_buf_printf(b, ",side=\"client\"} %llu\n", (unsigned long long)x->count);
        } else if (x->key.kind == ROS2EBPF_KIND_SERVICE) {
            rc = ros2ebpf_buf_append(b, "ros2_rmw_service_request_total", 30);
            rc = rc ?: labels(b, o, x, "service");
            b->len--;
            rc = rc ?: ros2ebpf_buf_printf(b, ",side=\"server\"} %llu\n", (unsigned long long)x->count);
        }
    }
    rc = rc ?: header(b, "ros2_rmw_service_response_total", "counter",
                      "Service responses: side=server counts rmw_send_response, side=client counts taken rmw_take_response.");
    for (size_t i = 0; i < n && !rc; i++) {
        const struct series *x = &ser[i];
        if (x->key.kind == ROS2EBPF_KIND_CLIENT) {
            rc = ros2ebpf_buf_append(b, "ros2_rmw_service_response_total", 31);
            rc = rc ?: labels(b, o, x, "service");
            b->len--;
            rc = rc ?: ros2ebpf_buf_printf(b, ",side=\"client\"} %llu\n", (unsigned long long)x->count2);
        } else if (x->key.kind == ROS2EBPF_KIND_SERVICE) {
            rc = ros2ebpf_buf_append(b, "ros2_rmw_service_response_total", 31);
            rc = rc ?: labels(b, o, x, "service");
            b->len--;
            rc = rc ?: ros2ebpf_buf_printf(b, ",side=\"server\"} %llu\n", (unsigned long long)x->count2);
        }
    }
    free(ser);

    /* ---- UDP drops ---- */
    if (s->udp_ok) {
        rc = rc ?: header(b, "ros2_udp_rcvbuf_drop_total", "counter",
                          "UDP datagrams dropped because the receiving socket queue was full (udp:udp_fail_queue_rcv_skb).");
        for (size_t i = 0; i < s->n_udp && !rc; i++) {
            const struct ros2ebpf_udp_entry *u = &s->udp[i];
            char comm[ROS2EBPF_COMM_LEN] = "unknown";
            __u32 pid = 0;
            if (o->resolve_port)
                o->resolve_port(o->resolve_ctx, u->key.lport, u->key.family, comm, sizeof(comm), &pid);
            rc = ros2ebpf_buf_append(b, "ros2_udp_rcvbuf_drop_total", 26);
            rc = rc ?: host_label_prefix(b, o);
            b->len--;
            rc = rc ?: ros2ebpf_buf_printf(b, ",port=\"%u\",comm=\"", u->key.lport);
            rc = rc ?: ros2ebpf_buf_escape(b, comm);
            rc = rc ?: ros2ebpf_buf_printf(b, "\"} %llu\n", (unsigned long long)u->stats.drops);
        }
    }

    /* ---- expectations (robot-level: no host label) ---- */
    if (o->expectations && o->n_expectations) {
        rc = rc ?: header(b, "ros2_topic_expected_hz", "gauge",
                          "Expected publish rate per topic for this robot (from expectations.yaml).");
        for (size_t i = 0; i < o->n_expectations && !rc; i++) {
            if (o->expectations[i].hz < 0)
                continue;
            rc = ros2ebpf_buf_append(b, "ros2_topic_expected_hz{topic=\"", 30);
            rc = rc ?: ros2ebpf_buf_escape(b, o->expectations[i].topic);
            rc = rc ?: ros2ebpf_buf_printf(b, "\"} %g\n", o->expectations[i].hz);
        }
        rc = rc ?: header(b, "ros2_topic_expected_takers", "gauge",
                          "Expected number of taking endpoints per topic for this robot.");
        for (size_t i = 0; i < o->n_expectations && !rc; i++) {
            if (o->expectations[i].takers < 0)
                continue;
            rc = ros2ebpf_buf_append(b, "ros2_topic_expected_takers{topic=\"", 34);
            rc = rc ?: ros2ebpf_buf_escape(b, o->expectations[i].topic);
            rc = rc ?: ros2ebpf_buf_printf(b, "\"} %d\n", o->expectations[i].takers);
        }
    }

    /* ---- process info ---- */
    rc = rc ?: header(b, "ros2_process_info", "gauge", "One series per process with ROS 2 endpoints seen by the probes.");
    for (size_t i = 0; i < s->n_processes && !rc; i++) {
        const struct ros2ebpf_process_entry *p = &s->processes[i];
        char comm[ROS2EBPF_COMM_LEN], rmw[ROS2EBPF_RMW_ID_LEN], container[ROS2EBPF_CONTAINER_LEN];
        memcpy(comm, p->info.comm, sizeof(comm)); comm[sizeof(comm) - 1] = 0;
        memcpy(rmw, p->info.rmw, sizeof(rmw)); rmw[sizeof(rmw) - 1] = 0;
        if (!rmw[0]) {
            /* RMW_IMPLEMENTATION not set: use the identifier the probe read from an rmw handle */
            for (size_t k = 0; k < s->n_endpoints; k++) {
                const struct ros2ebpf_endpoint_entry *e = &s->endpoints[k];
                if (e->key.pid == p->pid && e->has_info && !(e->info.flags & ROS2EBPF_F_ID_FAIL) && e->info.rmw_id[0]) {
                    memcpy(rmw, e->info.rmw_id, sizeof(rmw));
                    rmw[sizeof(rmw) - 1] = 0;
                    break;
                }
            }
        }
        memcpy(container, p->info.container, sizeof(container)); container[sizeof(container) - 1] = 0;
        rc = ros2ebpf_buf_append(b, "ros2_process_info", 17);
        rc = rc ?: host_label_prefix(b, o);
        b->len--;
        rc = rc ?: ros2ebpf_buf_append(b, ",comm=\"", 7);
        rc = rc ?: ros2ebpf_buf_escape(b, comm);
        rc = rc ?: ros2ebpf_buf_printf(b, "\",pid=\"%u\",domain_id=\"", p->pid);
        if (p->info.domain_id == ROS2EBPF_DOMAIN_UNKNOWN)
            rc = rc ?: ros2ebpf_buf_append(b, "unknown", 7);
        else
            rc = rc ?: ros2ebpf_buf_printf(b, "%u", p->info.domain_id);
        rc = rc ?: ros2ebpf_buf_append(b, "\",rmw=\"", 7);
        rc = rc ?: ros2ebpf_buf_escape(b, rmw);
        rc = rc ?: ros2ebpf_buf_append(b, "\",container=\"", 13);
        rc = rc ?: ros2ebpf_buf_escape(b, container);
        rc = rc ?: ros2ebpf_buf_printf(b, "\",localhost_only=\"%s\"} 1\n",
                                       p->info.localhost_only ? "true" : "false");
    }

    /* ---- host info ---- */
    if (o->host_info) {
        const struct ros2ebpf_host_info *h = o->host_info;
        rc = rc ?: header(b, "ros2ebpf_host_info", "gauge", "Static facts about this host and the daemon configuration.");
        rc = rc ?: ros2ebpf_buf_append(b, "ros2ebpf_host_info", 18);
        rc = rc ?: host_label_prefix(b, o);
        b->len--;
        const char *kv[][2] = {
            { "machine_id", h->machine_id }, { "distro", h->distro }, { "arch", h->arch },
            { "kernel", h->kernel }, { "shim_path", h->shim_path },
            { "offset_table", h->offset_table }, { "exporter_mode", h->exporter_mode },
        };
        for (size_t i = 0; i < sizeof(kv) / sizeof(kv[0]) && !rc; i++) {
            rc = ros2ebpf_buf_printf(b, ",%s=\"", kv[i][0]);
            rc = rc ?: ros2ebpf_buf_escape(b, kv[i][1]);
            rc = rc ?: ros2ebpf_buf_append(b, "\"", 1);
        }
        rc = rc ?: ros2ebpf_buf_append(b, "} 1\n", 4);
    }

    /* ---- self health ---- */
#define SELF_GAUGE(name, type, help, fmt, val) do {                                   \
        rc = rc ?: header(b, name, type, help);                                       \
        rc = rc ?: ros2ebpf_buf_append(b, name, strlen(name));                        \
        rc = rc ?: host_label_prefix(b, o);                                           \
        rc = rc ?: ros2ebpf_buf_printf(b, " " fmt "\n", val);                         \
    } while (0)

    rc = rc ?: header(b, "ros2ebpf_build_info", "gauge", "Exporter library version.");
    rc = rc ?: ros2ebpf_buf_append(b, "ros2ebpf_build_info", 19);
    rc = rc ?: host_label_prefix(b, o);
    b->len--;
    rc = rc ?: ros2ebpf_buf_printf(b, ",version=\"%s\"} 1\n", o->version ? o->version : ROS2EBPF_EXPORTER_VERSION);

    SELF_GAUGE("ros2ebpf_map_abi", "gauge", "Layout version of the pinned maps.", "%u",
               s->config_ok ? s->config.abi : 0);
    if (o->probes_attached >= 0) {
        SELF_GAUGE("ros2ebpf_probes_attached", "gauge", "BPF programs currently attached.", "%d", o->probes_attached);
        SELF_GAUGE("ros2ebpf_probes_configured", "gauge", "BPF programs that the configuration asks for.", "%d", o->probes_configured);
    }
    rc = rc ?: header(b, "ros2ebpf_map_entries", "gauge", "Entries per pinned map at scrape time.");
    {
        const char *names[] = { "endpoints", "processes", "udp_drops" };
        size_t vals[] = { s->n_endpoints, s->n_processes, s->n_udp };
        for (int i = 0; i < 3 && !rc; i++) {
            rc = ros2ebpf_buf_append(b, "ros2ebpf_map_entries", 20);
            rc = rc ?: host_label_prefix(b, o);
            b->len--;
            rc = rc ?: ros2ebpf_buf_printf(b, ",map=\"%s\"} %zu\n", names[i], vals[i]);
        }
    }
    SELF_GAUGE("ros2ebpf_name_read_failures_total", "counter", "Endpoint name reads that failed in the probe (offset table mismatch?).",
               "%llu", (unsigned long long)s->self[ROS2EBPF_SC_NAME_READ_FAIL]);
    SELF_GAUGE("ros2ebpf_id_read_failures_total", "counter", "implementation_identifier reads that failed in the probe.",
               "%llu", (unsigned long long)s->self[ROS2EBPF_SC_ID_READ_FAIL]);
    SELF_GAUGE("ros2ebpf_events_dropped_total", "counter", "Ring buffer events dropped because the buffer was full.",
               "%llu", (unsigned long long)s->self[ROS2EBPF_SC_RINGBUF_FULL]);
    SELF_GAUGE("ros2ebpf_map_full_total", "counter", "Endpoint entries not created because the map was full.",
               "%llu", (unsigned long long)s->self[ROS2EBPF_SC_MAP_FULL]);
    SELF_GAUGE("ros2ebpf_environ_read_failures_total", "counter", "Processes whose /proc/<pid>/environ could not be read (domain_id=unknown).",
               "%llu", (unsigned long long)o->environ_read_failures);
    SELF_GAUGE("ros2ebpf_identity_resolve_failures_total", "counter", "Processes that exited before their identity could be resolved.",
               "%llu", (unsigned long long)o->identity_resolve_failures);
    SELF_GAUGE("ros2ebpf_unknown_rmw_processes", "gauge", "Processes whose rmw handles carry an implementation_identifier not starting with rmw_.",
               "%zu", n_unknown_rmw);
    SELF_GAUGE("ros2ebpf_unresolved_endpoints", "gauge", "Endpoints skipped this scrape because their process identity is not resolved yet.",
               "%zu", unresolved);
    SELF_GAUGE("ros2ebpf_unnamed_endpoints", "gauge", "Endpoints skipped because their name could not be read.",
               "%zu", name_failed);
    SELF_GAUGE("ros2ebpf_snapshot_read_errors", "gauge", "Map lookups that failed while taking this snapshot.",
               "%llu", (unsigned long long)s->read_errors);
    SELF_GAUGE("ros2ebpf_scrape_duration_seconds", "gauge", "Time spent reading maps and rendering the previous scrape.",
               "%g", o->scrape_duration_seconds);
#undef SELF_GAUGE
    return rc;
}
