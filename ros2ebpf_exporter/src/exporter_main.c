// SPDX-License-Identifier: Apache-2.0
/*
 * ros2ebpf-exporter: standalone reader for the external exporter mode.
 * Opens the pinned maps on every scrape (so it may start before the daemon)
 * and serves /metrics. Needs CAP_BPF (or root) to open pinned maps.
 */
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "ros2ebpf/exporter.h"

struct state {
    const char *pin_root;
    struct ros2ebpf_host_info hi;
    struct ros2ebpf_export_opts opts;
    struct ros2ebpf_expectation *exp;
    size_t n_exp;
    double last_duration;
};

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int render_cb(void *ctx, struct ros2ebpf_buf *out)
{
    struct state *st = ctx;
    double t0 = now_s();
    struct ros2ebpf_maps maps;
    int rc = ros2ebpf_maps_open(&maps, st->pin_root);
    if (rc) {
        fprintf(stderr, "ros2ebpf-exporter: cannot open maps under %s: %s\n", st->pin_root, strerror(-rc));
        return rc;
    }
    struct ros2ebpf_snapshot snap;
    rc = ros2ebpf_snapshot_read(&maps, &snap);
    ros2ebpf_maps_close(&maps);
    if (rc)
        return rc;
    st->opts.scrape_duration_seconds = st->last_duration;
    rc = ros2ebpf_render(&snap, &st->opts, out);
    ros2ebpf_snapshot_free(&snap);
    st->last_duration = now_s() - t0;
    return rc;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --pin-root DIR      pinned maps root (default %s)\n"
            "  --listen ADDR       host:port for /metrics (default %s)\n"
            "  --host NAME         value of the host label (default: kernel hostname)\n"
            "  --robot NAME        add a robot label (normally set by the collector instead)\n"
            "  --expectations F    expectations.yaml to export as ros2_topic_expected_*\n"
            "  --rmw-label         add the rmw label to endpoint series\n"
            "  --once              render once to stdout and exit\n"
            "  --version\n",
            argv0, ROS2EBPF_DEFAULT_PIN_ROOT, ROS2EBPF_DEFAULT_METRICS_LISTEN);
}

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "pin-root", required_argument, NULL, 'p' },
        { "listen", required_argument, NULL, 'l' },
        { "host", required_argument, NULL, 'H' },
        { "robot", required_argument, NULL, 'r' },
        { "expectations", required_argument, NULL, 'e' },
        { "rmw-label", no_argument, NULL, 'm' },
        { "once", no_argument, NULL, '1' },
        { "version", no_argument, NULL, 'V' },
        { "help", no_argument, NULL, 'h' },
        { 0 }
    };
    struct state st = { .pin_root = ROS2EBPF_DEFAULT_PIN_ROOT };
    const char *listen_addr = ROS2EBPF_DEFAULT_METRICS_LISTEN;
    const char *host_override = NULL, *robot = NULL, *exp_path = NULL;
    int once = 0, rmw_label = 0, c;

    while ((c = getopt_long(argc, argv, "p:l:H:r:e:m1Vh", longopts, NULL)) != -1) {
        switch (c) {
        case 'p': st.pin_root = optarg; break;
        case 'l': listen_addr = optarg; break;
        case 'H': host_override = optarg; break;
        case 'r': robot = optarg; break;
        case 'e': exp_path = optarg; break;
        case 'm': rmw_label = 1; break;
        case '1': once = 1; break;
        case 'V': printf("ros2ebpf-exporter %s\n", ROS2EBPF_EXPORTER_VERSION); return 0;
        default: usage(argv[0]); return c == 'h' ? 0 : 2;
        }
    }

    ros2ebpf_host_info_collect(&st.hi);
    if (host_override)
        snprintf(st.hi.host, sizeof(st.hi.host), "%s", host_override);
    snprintf(st.hi.exporter_mode, sizeof(st.hi.exporter_mode), "external");
    if (exp_path) {
        int n = ros2ebpf_expectations_load(exp_path, &st.exp, &st.n_exp);
        if (n < 0)
            fprintf(stderr, "ros2ebpf-exporter: %s: %s\n", exp_path, strerror(-n));
    }
    st.opts = (struct ros2ebpf_export_opts){
        .host = st.hi.host, .robot = robot, .rmw_label = rmw_label, .host_info = &st.hi,
        .expectations = st.exp, .n_expectations = st.n_exp,
        .probes_attached = -1, .probes_configured = -1, .version = ROS2EBPF_EXPORTER_VERSION,
    };

    if (once) {
        struct ros2ebpf_buf out = { 0 };
        int rc = render_cb(&st, &out);
        if (rc)
            return 1;
        fwrite(out.data, 1, out.len, stdout);
        ros2ebpf_buf_free(&out);
        return 0;
    }

    int lfd = ros2ebpf_http_listen(listen_addr);
    if (lfd < 0) {
        fprintf(stderr, "ros2ebpf-exporter: listen %s: %s\n", listen_addr, strerror(-lfd));
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fprintf(stderr, "ros2ebpf-exporter %s: serving http://%s/metrics from %s\n",
            ROS2EBPF_EXPORTER_VERSION, listen_addr, st.pin_root);
    while (!stop) {
        struct pollfd p = { .fd = lfd, .events = POLLIN };
        int r = poll(&p, 1, 1000);
        if (r > 0)
            ros2ebpf_http_serve_one(lfd, render_cb, &st);
        else if (r < 0 && errno != EINTR)
            break;
    }
    close(lfd);
    free(st.exp);
    return 0;
}
