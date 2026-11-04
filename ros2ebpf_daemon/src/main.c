// SPDX-License-Identifier: Apache-2.0
/*
 * ros2ebpfd: loads the BPF programs, attaches them to librmw_implementation.so,
 * resolves process identity, answers the control socket and (by default)
 * serves /metrics in-process through the exporter library.
 *
 * Single-threaded epoll loop over: BPF ring buffer, control socket(s),
 * metrics listener, signalfd (SIGHUP reload, SIGTERM/SIGINT stop), timerfd
 * (shim discovery, identity garbage collection).
 */
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <bpf/bpf.h>
#include <json-c/json.h>
#include "config.h"
#include "control.h"
#include "identity.h"
#include "log.h"
#include "probes.h"
#include "udpmap.h"
#include "ros2ebpf/exporter.h"

#ifndef ROS2EBPFD_VERSION
#define ROS2EBPFD_VERSION "0.1.0"
#endif

int g_log_level = LOG_INFO;

struct daemon {
    struct config cfg;
    struct probes probes;
    struct identity ident;
    struct udpmap udpmap;
    struct control ctl;
    struct ros2ebpf_host_info hi;
    struct ros2ebpf_expectation *exp;
    size_t n_exp;
    char host_arch[32];
    int metrics_fd;
    double last_scrape_s;
    __u64 scrapes;
    time_t started;
    int stop;
};

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---- BPF events ------------------------------------------------------------ */

static int on_event(void *ctx, void *data, size_t len)
{
    struct daemon *d = ctx;
    if (len < sizeof(struct ros2ebpf_event))
        return 0;
    const struct ros2ebpf_event *ev = data;
    if (ev->type == ROS2EBPF_EV_NEW_PID)
        identity_resolve_pid(&d->ident, ev->pid);
    else if (ev->type == ROS2EBPF_EV_EXIT_PID)
        identity_sweep_pid(&d->ident, ev->pid);
    return 0;
}

/* ---- metrics --------------------------------------------------------------- */

static void fill_opts(struct daemon *d, struct ros2ebpf_export_opts *o)
{
    memset(o, 0, sizeof(*o));
    o->host = d->hi.host;
    o->robot = d->cfg.robot[0] ? d->cfg.robot : NULL;
    o->rmw_label = d->cfg.rmw_label;
    o->host_info = &d->hi;
    o->expectations = d->exp;
    o->n_expectations = d->n_exp;
    o->probes_attached = probes_total_attached(&d->probes);
    o->probes_configured = probes_total_expected(&d->probes);
    o->environ_read_failures = d->ident.environ_failures;
    o->identity_resolve_failures = d->ident.resolve_failures;
    o->scrape_duration_seconds = d->last_scrape_s;
    o->version = ROS2EBPFD_VERSION;
    o->resolve_port = udpmap_resolve;
    o->resolve_ctx = &d->udpmap;
}

static int render_metrics(void *ctx, struct ros2ebpf_buf *out)
{
    struct daemon *d = ctx;
    double t0 = mono_s();
    struct ros2ebpf_maps maps;
    probes_fill_maps(&d->probes, &maps);
    struct ros2ebpf_snapshot snap;
    int rc = ros2ebpf_snapshot_read(&maps, &snap);
    if (rc) {
        log_warn("snapshot: %s", strerror(-rc));
        return rc;
    }
    struct ros2ebpf_export_opts o;
    fill_opts(d, &o);
    rc = ros2ebpf_render(&snap, &o, out);
    ros2ebpf_snapshot_free(&snap);
    d->last_scrape_s = mono_s() - t0;
    d->scrapes++;
    return rc;
}

/* ---- reload ---------------------------------------------------------------- */

static void load_expectations(struct daemon *d)
{
    free(d->exp);
    d->exp = NULL;
    d->n_exp = 0;
    if (!d->cfg.expectations_file[0])
        return;
    int n = ros2ebpf_expectations_load(d->cfg.expectations_file, &d->exp, &d->n_exp);
    if (n < 0) {
        if (n != -ENOENT)
            log_warn("expectations %s: %s", d->cfg.expectations_file, strerror(-n));
        else
            log_debug("expectations %s: not present", d->cfg.expectations_file);
    } else {
        log_info("expectations: %d topics from %s", n, d->cfg.expectations_file);
    }
}

static void sync_families(struct daemon *d)
{
    struct probes *p = &d->probes;
    p->configured[FAM_PUBLISH] = d->cfg.probe_publish;
    p->configured[FAM_TAKE] = d->cfg.probe_take;
    p->configured[FAM_SERVICES] = d->cfg.probe_services;
    p->configured[FAM_UDP_DROPS] = d->cfg.probe_udp_drops;
    for (int f = 0; f < FAM_COUNT; f++) {
        if (p->configured[f] && !p->enabled[f])
            probes_enable(p, f);
        else if (!p->configured[f] && p->enabled[f])
            probes_disable(p, f);
    }
}

static int do_reload(struct daemon *d, char *msg, size_t n)
{
    struct config fresh;
    config_defaults(&fresh);
    int rc = config_load(&fresh, d->cfg.path);
    if (rc) {
        snprintf(msg, n, "config %.400s has errors; keeping the running configuration", d->cfg.path);
        log_error("%s", msg);
        return -1;
    }
    int immutable = config_runtime_immutable_changed(&d->cfg, &fresh);
    if (immutable) {
        /* keep the runtime-bound values from the running config */
        memcpy(fresh.shims, d->cfg.shims, sizeof(fresh.shims));
        fresh.n_shims = d->cfg.n_shims;
        snprintf(fresh.pin_root, sizeof(fresh.pin_root), "%s", d->cfg.pin_root);
        snprintf(fresh.exporter_listen, sizeof(fresh.exporter_listen), "%s", d->cfg.exporter_listen);
        snprintf(fresh.exporter_mode, sizeof(fresh.exporter_mode), "%s", d->cfg.exporter_mode);
        snprintf(fresh.control_socket, sizeof(fresh.control_socket), "%s", d->cfg.control_socket);
        snprintf(fresh.control_http, sizeof(fresh.control_http), "%s", d->cfg.control_http);
        snprintf(fresh.host, sizeof(fresh.host), "%s", d->cfg.host);
    }
    d->cfg = fresh;
    g_log_level = d->cfg.log_level;
    probes_write_config(&d->probes, &d->cfg);
    probes_write_filters(&d->probes, &d->cfg);
    sync_families(d);
    load_expectations(d);
    snprintf(msg, n, "reloaded %.400s%s", d->cfg.path,
             immutable ? " (shims/listen/socket/host changes need a restart)" : "");
    log_info("%s", msg);
    return 0;
}

/* ---- control handlers ------------------------------------------------------ */

static struct json_object *jstr(const char *s) { return json_object_new_string(s ? s : ""); }

static struct json_object *status_json(struct daemon *d)
{
    struct probes *p = &d->probes;
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "version", jstr(ROS2EBPFD_VERSION));
    json_object_object_add(o, "pid", json_object_new_int(getpid()));
    json_object_object_add(o, "uptime_seconds", json_object_new_int64((int64_t)(time(NULL) - d->started)));
    json_object_object_add(o, "host", jstr(d->hi.host));
    json_object_object_add(o, "machine_id", jstr(d->hi.machine_id));
    json_object_object_add(o, "robot", jstr(d->cfg.robot));
    json_object_object_add(o, "distro", jstr(d->hi.distro));
    json_object_object_add(o, "arch", jstr(d->hi.arch));
    json_object_object_add(o, "kernel", jstr(d->hi.kernel));
    json_object_object_add(o, "config_path", jstr(d->cfg.path));
    json_object_object_add(o, "pin_root", jstr(d->cfg.pin_root));
    json_object_object_add(o, "offset_table", jstr(p->offset_table));
    json_object_object_add(o, "maps_recreated_at_start", json_object_new_boolean(p->wiped));

    struct json_object *shims = json_object_new_array();
    for (int i = 0; i < p->n_shims; i++) {
        struct json_object *s = json_object_new_object();
        json_object_object_add(s, "path", jstr(p->shims[i].path));
        json_object_object_add(s, "distro", jstr(p->shims[i].distro));
        json_object_object_add(s, "arch", jstr(p->shims[i].arch));
        json_object_object_add(s, "source", jstr(p->shims[i].from_config ? "config" : "discovered"));
        json_object_object_add(s, "inode", json_object_new_int64((int64_t)p->shims[i].ino));
        json_object_object_add(s, "attach_failed", json_object_new_boolean(p->shims[i].attach_failed));
        json_object_array_add(shims, s);
    }
    json_object_object_add(o, "shims", shims);

    int healthy = p->n_shims > 0 || !(d->cfg.probe_publish || d->cfg.probe_take || d->cfg.probe_services);
    struct json_object *probes = json_object_new_object();
    for (int f = 0; f < FAM_COUNT; f++) {
        struct json_object *fo = json_object_new_object();
        int att = probes_links_attached(p, f), exp = p->enabled[f] ? probes_links_expected(p, f) : 0;
        json_object_object_add(fo, "configured", json_object_new_boolean(p->configured[f]));
        json_object_object_add(fo, "enabled", json_object_new_boolean(p->enabled[f]));
        json_object_object_add(fo, "attached", json_object_new_int(att));
        json_object_object_add(fo, "expected", json_object_new_int(exp));
        json_object_object_add(probes, family_names[f], fo);
        if (p->configured[f] && (!p->enabled[f] || att < exp))
            healthy = 0;
    }
    json_object_object_add(o, "probes", probes);
    json_object_object_add(o, "probes_attached", json_object_new_int(probes_total_attached(p)));
    json_object_object_add(o, "probes_expected", json_object_new_int(probes_total_expected(p)));

    struct json_object *ex = json_object_new_object();
    json_object_object_add(ex, "mode", jstr(d->cfg.exporter_mode));
    json_object_object_add(ex, "listen", jstr(d->cfg.exporter_listen));
    json_object_object_add(ex, "serving", json_object_new_boolean(d->metrics_fd >= 0));
    json_object_object_add(ex, "scrapes", json_object_new_int64((int64_t)d->scrapes));
    json_object_object_add(ex, "last_scrape_duration_seconds", json_object_new_double(d->last_scrape_s));
    json_object_object_add(ex, "expectations_file", jstr(d->cfg.expectations_file));
    json_object_object_add(ex, "expectations", json_object_new_int((int)d->n_exp));
    json_object_object_add(o, "exporter", ex);

    struct json_object *ctl = json_object_new_object();
    json_object_object_add(ctl, "socket", jstr(d->cfg.control_socket));
    json_object_object_add(ctl, "http", jstr(d->cfg.control_http));
    json_object_object_add(o, "control", ctl);

    struct ros2ebpf_maps maps;
    probes_fill_maps(p, &maps);
    struct ros2ebpf_snapshot snap;
    struct json_object *mo = json_object_new_object();
    struct json_object *sc = json_object_new_object();
    if (ros2ebpf_snapshot_read(&maps, &snap) == 0) {
        json_object_object_add(mo, "endpoints", json_object_new_int64((int64_t)snap.n_endpoints));
        json_object_object_add(mo, "processes", json_object_new_int64((int64_t)snap.n_processes));
        json_object_object_add(mo, "udp_drops", json_object_new_int64((int64_t)snap.n_udp));
        json_object_object_add(mo, "abi", json_object_new_int((int)snap.config.abi));
        json_object_object_add(sc, "name_read_failures", json_object_new_int64((int64_t)snap.self[ROS2EBPF_SC_NAME_READ_FAIL]));
        json_object_object_add(sc, "id_read_failures", json_object_new_int64((int64_t)snap.self[ROS2EBPF_SC_ID_READ_FAIL]));
        json_object_object_add(sc, "events_dropped", json_object_new_int64((int64_t)snap.self[ROS2EBPF_SC_RINGBUF_FULL]));
        json_object_object_add(sc, "map_full", json_object_new_int64((int64_t)snap.self[ROS2EBPF_SC_MAP_FULL]));
        ros2ebpf_snapshot_free(&snap);
    }
    json_object_object_add(o, "maps", mo);
    json_object_object_add(o, "self_counters", sc);

    struct json_object *id = json_object_new_object();
    __u32 domains[64];
    int nd = identity_domains_seen(&d->ident, domains, 64);
    struct json_object *da = json_object_new_array();
    for (int i = 0; i < nd; i++)
        json_object_array_add(da, domains[i] == ROS2EBPF_DOMAIN_UNKNOWN ? jstr("unknown")
                                                                       : json_object_new_int((int)domains[i]));
    json_object_object_add(id, "domains_seen", da);
    json_object_object_add(id, "read_environ", json_object_new_boolean(d->cfg.read_environ));
    json_object_object_add(id, "resolved", json_object_new_int64((int64_t)d->ident.resolved));
    json_object_object_add(id, "environ_read_failures", json_object_new_int64((int64_t)d->ident.environ_failures));
    json_object_object_add(id, "resolve_failures", json_object_new_int64((int64_t)d->ident.resolve_failures));
    json_object_object_add(o, "identity", id);
    json_object_object_add(o, "healthy", json_object_new_boolean(healthy));
    return o;
}

static const char *kind_name(__u32 k)
{
    switch (k) {
    case ROS2EBPF_KIND_PUBLISHER: return "publisher";
    case ROS2EBPF_KIND_SUBSCRIPTION: return "subscription";
    case ROS2EBPF_KIND_CLIENT: return "client";
    case ROS2EBPF_KIND_SERVICE: return "service";
    }
    return "?";
}

static struct json_object *snapshot_json(struct daemon *d)
{
    struct ros2ebpf_maps maps;
    probes_fill_maps(&d->probes, &maps);
    struct ros2ebpf_snapshot snap;
    struct json_object *o = json_object_new_object();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    json_object_object_add(o, "ts_ns", json_object_new_int64((int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec));
    struct json_object *arr = json_object_new_array();
    if (ros2ebpf_snapshot_read(&maps, &snap) == 0) {
        for (size_t i = 0; i < snap.n_endpoints; i++) {
            const struct ros2ebpf_endpoint_entry *e = &snap.endpoints[i];
            if (!e->has_info)
                continue;
            struct json_object *x = json_object_new_object();
            json_object_object_add(x, "pid", json_object_new_int((int)e->key.pid));
            json_object_object_add(x, "handle", json_object_new_int64((int64_t)e->key.handle));
            json_object_object_add(x, "kind", jstr(kind_name(e->info.kind)));
            char name[ROS2EBPF_NAME_LEN], rmw[ROS2EBPF_RMW_ID_LEN];
            memcpy(name, e->info.name, sizeof(name)); name[sizeof(name) - 1] = 0;
            memcpy(rmw, e->info.rmw_id, sizeof(rmw)); rmw[sizeof(rmw) - 1] = 0;
            json_object_object_add(x, "name", jstr(name));
            json_object_object_add(x, "rmw", jstr(rmw));
            json_object_object_add(x, "flags", json_object_new_int(e->info.flags));
            json_object_object_add(x, "count", json_object_new_int64((int64_t)e->stats.count));
            json_object_object_add(x, "count2", json_object_new_int64((int64_t)e->stats.count2));
            json_object_object_add(x, "seq_gaps", json_object_new_int64((int64_t)e->stats.seq_gaps));
            json_object_object_add(x, "seq_lost", json_object_new_int64((int64_t)e->stats.seq_lost));
            json_object_object_add(x, "period_sum_ns", json_object_new_int64((int64_t)e->stats.period_sum_ns));
            const char *comm = "";
            __u32 domain = ROS2EBPF_DOMAIN_UNKNOWN;
            char commbuf[ROS2EBPF_COMM_LEN] = "";
            for (size_t k = 0; k < snap.n_processes; k++)
                if (snap.processes[k].pid == e->key.pid) {
                    memcpy(commbuf, snap.processes[k].info.comm, sizeof(commbuf));
                    commbuf[sizeof(commbuf) - 1] = 0;
                    comm = commbuf;
                    domain = snap.processes[k].info.domain_id;
                    break;
                }
            json_object_object_add(x, "comm", jstr(comm));
            json_object_object_add(x, "domain_id", domain == ROS2EBPF_DOMAIN_UNKNOWN ? jstr("unknown")
                                                                                    : json_object_new_int((int)domain));
            json_object_array_add(arr, x);
        }
        ros2ebpf_snapshot_free(&snap);
    }
    json_object_object_add(o, "endpoints", arr);
    return o;
}

static struct json_object *config_json(struct daemon *d)
{
    const struct config *c = &d->cfg;
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "path", jstr(c->path));
    json_object_object_add(o, "host", jstr(c->host));
    json_object_object_add(o, "robot", jstr(c->robot));
    struct json_object *shims = json_object_new_array();
    for (int i = 0; i < c->n_shims; i++) {
        struct json_object *s = json_object_new_object();
        json_object_object_add(s, "distro", jstr(c->shims[i].distro));
        json_object_object_add(s, "arch", jstr(c->shims[i].arch));
        json_object_object_add(s, "path", jstr(c->shims[i].path));
        json_object_array_add(shims, s);
    }
    json_object_object_add(o, "shims", shims);
    json_object_object_add(o, "discover_shims", json_object_new_boolean(c->discover_shims));
    struct json_object *idn = json_object_new_object();
    json_object_object_add(idn, "read_environ", json_object_new_boolean(c->read_environ));
    json_object_object_add(idn, "default_domain_id", json_object_new_int(c->default_domain_id));
    json_object_object_add(o, "identity", idn);
    struct json_object *pr = json_object_new_object();
    json_object_object_add(pr, "publish", json_object_new_boolean(c->probe_publish));
    json_object_object_add(pr, "take", json_object_new_boolean(c->probe_take));
    json_object_object_add(pr, "services", json_object_new_boolean(c->probe_services));
    json_object_object_add(pr, "udp_drops", json_object_new_boolean(c->probe_udp_drops));
    struct json_object *sg = json_object_new_array();
    for (int i = 0; i < c->n_seq_gap; i++)
        json_object_array_add(sg, jstr(c->seq_gap[i]));
    json_object_object_add(pr, "seq_gap", c->seq_gap_all ? jstr("all") : sg);
    if (c->seq_gap_all)
        json_object_put(sg);
    json_object_object_add(o, "probes", pr);
    struct json_object *tp = json_object_new_object();
    struct json_object *ex = json_object_new_array();
    for (int i = 0; i < c->n_exclude; i++)
        json_object_array_add(ex, jstr(c->exclude[i]));
    json_object_object_add(tp, "exclude", ex);
    json_object_object_add(o, "topics", tp);
    struct json_object *hb = json_object_new_array();
    for (int i = 0; i < c->n_buckets; i++)
        json_object_array_add(hb, json_object_new_double(c->buckets[i]));
    json_object_object_add(o, "histogram_buckets_seconds", hb);
    struct json_object *exo = json_object_new_object();
    json_object_object_add(exo, "mode", jstr(c->exporter_mode));
    json_object_object_add(exo, "listen", jstr(c->exporter_listen));
    json_object_object_add(exo, "rmw_label", json_object_new_boolean(c->rmw_label));
    json_object_object_add(o, "exporter", exo);
    struct json_object *co = json_object_new_object();
    json_object_object_add(co, "socket", jstr(c->control_socket));
    json_object_object_add(co, "http", jstr(c->control_http));
    json_object_object_add(o, "control", co);
    json_object_object_add(o, "expectations_file", jstr(c->expectations_file));
    json_object_object_add(o, "pin_root", jstr(c->pin_root));
    json_object_object_add(o, "offset_table", jstr(c->offset_table));
    return o;
}

static int handle_rpc(void *ctx, const char *method, struct json_object *params,
                      struct json_object **result, char *errmsg, size_t errlen)
{
    struct daemon *d = ctx;
    if (!strcmp(method, "status")) {
        *result = status_json(d);
        return 0;
    }
    if (!strcmp(method, "enable") || !strcmp(method, "disable")) {
        struct json_object *fam = NULL;
        if (!params || !json_object_object_get_ex(params, "family", &fam) || !json_object_is_type(fam, json_type_string)) {
            snprintf(errmsg, errlen, "params.family required (publish|take|services|udp_drops)");
            return -32602;
        }
        int f = family_from_name(json_object_get_string(fam));
        if (f < 0) {
            snprintf(errmsg, errlen, "unknown family '%s'", json_object_get_string(fam));
            return -32602;
        }
        int rc = 0;
        if (!strcmp(method, "enable")) {
            d->probes.configured[f] = 1;
            rc = probes_enable(&d->probes, f);
        } else {
            d->probes.configured[f] = 0;
            probes_disable(&d->probes, f);
        }
        *result = json_object_new_object();
        json_object_object_add(*result, "family", jstr(family_names[f]));
        json_object_object_add(*result, "enabled", json_object_new_boolean(d->probes.enabled[f]));
        json_object_object_add(*result, "attached", json_object_new_int(probes_links_attached(&d->probes, f)));
        json_object_object_add(*result, "expected", json_object_new_int(probes_links_expected(&d->probes, f)));
        if (rc)
            json_object_object_add(*result, "warning", jstr("some links failed to attach; see daemon log"));
        return 0;
    }
    if (!strcmp(method, "reload")) {
        char msg[512];
        int rc = do_reload(d, msg, sizeof(msg));
        if (rc) {
            snprintf(errmsg, errlen, "%s", msg);
            return -32000;
        }
        *result = json_object_new_object();
        json_object_object_add(*result, "message", jstr(msg));
        return 0;
    }
    if (!strcmp(method, "snapshot")) {
        *result = snapshot_json(d);
        return 0;
    }
    if (!strcmp(method, "config")) {
        *result = config_json(d);
        return 0;
    }
    if (!strcmp(method, "metrics")) {
        struct ros2ebpf_buf out = { 0 };
        int rc = render_metrics(d, &out);
        if (rc) {
            snprintf(errmsg, errlen, "render failed: %s", strerror(-rc));
            return -32000;
        }
        *result = json_object_new_object();
        json_object_object_add(*result, "text", json_object_new_string_len(out.data, (int)out.len));
        ros2ebpf_buf_free(&out);
        return 0;
    }
    if (!strcmp(method, "discover")) {
        int n = d->cfg.discover_shims ? probes_discover_shims(&d->probes, d->host_arch) : 0;
        *result = json_object_new_object();
        json_object_object_add(*result, "new_shims", json_object_new_int(n));
        return 0;
    }
    snprintf(errmsg, errlen, "unknown method '%s'", method);
    return -32601;
}

/* ---- main ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [-c /etc/ros2ebpf/config.yaml] [--check] [--log-level L] [--version]\n"
            "  --check       validate the configuration and the shims, then exit\n", argv0);
}

static void bump_memlock(void)
{
    struct rlimit r = { RLIM_INFINITY, RLIM_INFINITY };
    setrlimit(RLIMIT_MEMLOCK, &r);     /* harmless on >= 5.11 where BPF uses memcg */
}

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "config", required_argument, NULL, 'c' },
        { "check", no_argument, NULL, 'k' },
        { "log-level", required_argument, NULL, 'l' },
        { "version", no_argument, NULL, 'V' },
        { "help", no_argument, NULL, 'h' },
        { 0 }
    };
    const char *cfg_path = CFG_DEFAULT_PATH;
    const char *log_level = NULL;
    int check = 0, c;
    while ((c = getopt_long(argc, argv, "c:kl:Vh", longopts, NULL)) != -1) {
        switch (c) {
        case 'c': cfg_path = optarg; break;
        case 'k': check = 1; break;
        case 'l': log_level = optarg; break;
        case 'V': printf("ros2ebpfd %s (exporter %s, map ABI %d)\n", ROS2EBPFD_VERSION, ROS2EBPF_EXPORTER_VERSION, ROS2EBPF_MAP_ABI); return 0;
        default: usage(argv[0]); return c == 'h' ? 0 : 2;
        }
    }

    static struct daemon d;
    memset(&d, 0, sizeof(d));
    d.metrics_fd = -1;
    d.started = time(NULL);
    config_defaults(&d.cfg);
    if (config_load(&d.cfg, cfg_path))
        return 2;
    g_log_level = d.cfg.log_level;
    if (log_level) {
        if (!strcmp(log_level, "debug")) g_log_level = LOG_DEBUG;
        else if (!strcmp(log_level, "info")) g_log_level = LOG_INFO;
        else if (!strcmp(log_level, "warn")) g_log_level = LOG_WARN;
        else if (!strcmp(log_level, "error")) g_log_level = LOG_ERROR;
    }

    ros2ebpf_host_info_collect(&d.hi);
    const char *env_host = getenv("ROS2EBPF_HOST");
    if (d.cfg.host[0])
        snprintf(d.hi.host, sizeof(d.hi.host), "%s", d.cfg.host);
    else if (env_host && env_host[0])
        snprintf(d.hi.host, sizeof(d.hi.host), "%s", env_host);
    const char *env_robot = getenv("ROS2EBPF_ROBOT");
    if (!d.cfg.robot[0] && env_robot && env_robot[0])
        snprintf(d.cfg.robot, sizeof(d.cfg.robot), "%s", env_robot);
    snprintf(d.hi.robot, sizeof(d.hi.robot), "%s", d.cfg.robot);
    snprintf(d.hi.exporter_mode, sizeof(d.hi.exporter_mode), "%s", d.cfg.exporter_mode);
    snprintf(d.host_arch, sizeof(d.host_arch), "%s", d.hi.arch);

    if (check) {
        int bad = 0;
        printf("config: %s ok\n", d.cfg.path);
        for (int i = 0; i < d.cfg.n_shims; i++) {
            char missing[512];
            if (offsets_check_shim(d.cfg.shims[i].path, missing, sizeof(missing))) {
                printf("shim %s: FAIL (%s)\n", d.cfg.shims[i].path, missing);
                bad = 1;
            } else {
                printf("shim %s: ok\n", d.cfg.shims[i].path);
            }
        }
        if (access("/sys/kernel/btf/vmlinux", R_OK))
            printf("btf: /sys/kernel/btf/vmlinux not readable (CO-RE will fail)\n"), bad = 1;
        else
            printf("btf: ok\n");
        return bad ? 1 : 0;
    }

    log_info("ros2ebpfd %s starting: host=%s machine_id=%s distro=%s arch=%s kernel=%s",
             ROS2EBPFD_VERSION, d.hi.host, d.hi.machine_id, d.hi.distro, d.hi.arch, d.hi.kernel);
    bump_memlock();

    int rc = probes_open_load(&d.probes, &d.cfg, on_event, &d);
    if (rc)
        return 1;
    snprintf(d.hi.offset_table, sizeof(d.hi.offset_table), "%s", d.probes.offset_table);

    d.ident.cfg = &d.cfg;
    struct ros2ebpf_maps m;
    probes_fill_maps(&d.probes, &m);
    d.ident.processes_fd = m.fd[ROS2EBPF_MI_PROCESSES];
    d.ident.endpoints_fd = m.fd[ROS2EBPF_MI_ENDPOINTS];
    d.ident.info_fd = m.fd[ROS2EBPF_MI_ENDPOINT_INFO];
    d.ident.seen_fd = probes_seen_fd(&d.probes);
    d.udpmap.processes_fd = m.fd[ROS2EBPF_MI_PROCESSES];

    probes_add_config_shims(&d.probes, &d.cfg, d.host_arch);
    if (d.cfg.discover_shims)
        probes_discover_shims(&d.probes, d.host_arch);
    snprintf(d.hi.offset_table, sizeof(d.hi.offset_table), "%s", d.probes.offset_table);
    if (d.probes.n_shims)
        snprintf(d.hi.shim_path, sizeof(d.hi.shim_path), "%s", d.probes.shims[0].path);
    else
        log_warn("no librmw_implementation.so found yet (config shims: %d, discovery: %s); "
                 "will keep scanning /proc for ROS 2 processes", d.cfg.n_shims,
                 d.cfg.discover_shims ? "on" : "off");

    probes_write_config(&d.probes, &d.cfg);
    probes_write_filters(&d.probes, &d.cfg);
    sync_families(&d);
    identity_gc(&d.ident);
    load_expectations(&d);

    if (!strcmp(d.cfg.exporter_mode, "inprocess")) {
        d.metrics_fd = ros2ebpf_http_listen(d.cfg.exporter_listen);
        if (d.metrics_fd < 0) {
            log_error("metrics listen %s: %s", d.cfg.exporter_listen, strerror(-d.metrics_fd));
            return 1;
        }
        log_info("metrics at http://%s/metrics", d.cfg.exporter_listen);
    } else {
        log_info("exporter mode external: run ros2ebpf-exporter --pin-root %s", d.cfg.pin_root);
    }

    if (control_init(&d.ctl, d.cfg.control_socket, d.cfg.control_http, handle_rpc, &d))
        return 1;

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    struct itimerspec its = { { 5, 0 }, { 5, 0 } };
    timerfd_settime(tfd, 0, &its, NULL);

    int ep = epoll_create1(EPOLL_CLOEXEC);
    enum { T_RB = 1, T_CTL, T_HTTPCTL, T_METRICS, T_SIG, T_TIMER };
    struct epoll_event ev;
#define ADD(fd_, tag) do { if ((fd_) >= 0) { ev.events = EPOLLIN; ev.data.u32 = (tag); epoll_ctl(ep, EPOLL_CTL_ADD, (fd_), &ev); } } while (0)
    ADD(ring_buffer__epoll_fd(d.probes.rb), T_RB);
    ADD(d.ctl.unix_fd, T_CTL);
    ADD(d.ctl.http_fd, T_HTTPCTL);
    ADD(d.metrics_fd, T_METRICS);
    ADD(sfd, T_SIG);
    ADD(tfd, T_TIMER);
#undef ADD

    log_info("ready: %d/%d probe links attached, %d shim(s), offset table %s",
             probes_total_attached(&d.probes), probes_total_expected(&d.probes), d.probes.n_shims,
             d.probes.offset_table[0] ? d.probes.offset_table : "-");

    unsigned ticks = 0;
    while (!d.stop) {
        struct epoll_event evs[8];
        int n = epoll_wait(ep, evs, 8, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            log_error("epoll_wait: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < n; i++) {
            switch (evs[i].data.u32) {
            case T_RB:
                ring_buffer__consume(d.probes.rb);
                break;
            case T_CTL:
                control_serve_unix(&d.ctl);
                break;
            case T_HTTPCTL:
                control_serve_http(&d.ctl);
                break;
            case T_METRICS:
                ros2ebpf_http_serve_one(d.metrics_fd, render_metrics, &d);
                break;
            case T_SIG: {
                struct signalfd_siginfo si;
                while (read(sfd, &si, sizeof(si)) == sizeof(si)) {
                    if (si.ssi_signo == SIGHUP) {
                        char msg[512];
                        do_reload(&d, msg, sizeof(msg));
                    } else {
                        log_info("signal %d: stopping", si.ssi_signo);
                        d.stop = 1;
                    }
                }
                break;
            }
            case T_TIMER: {
                __u64 exp;
                read(tfd, &exp, sizeof(exp));
                ticks++;
                if (d.cfg.discover_shims)
                    probes_discover_shims(&d.probes, d.host_arch);
                if (ticks % 6 == 0)                 /* every 30 s */
                    identity_gc(&d.ident);
                break;
            }
            }
        }
    }

    control_close(&d.ctl);
    if (d.metrics_fd >= 0)
        close(d.metrics_fd);
    probes_close(&d.probes);          /* links go away; pinned maps stay */
    free(d.exp);
    log_info("stopped (maps remain pinned under %s)", d.cfg.pin_root);
    return 0;
}
