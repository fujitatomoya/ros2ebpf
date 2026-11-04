/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ros2ebpf exporter library.
 *
 * Turns the pinned BPF maps (see maps.h) into Prometheus text exposition.
 * The library never computes a rate: counters are exported as they are and
 * everything derived is left to the server.
 *
 * Two users: ros2ebpfd links it and serves /metrics in-process (default),
 * and the standalone `ros2ebpf-exporter` binary reads the pinned maps from
 * bpffs for the external mode.
 */
#ifndef ROS2EBPF_EXPORTER_H
#define ROS2EBPF_EXPORTER_H

#include <stddef.h>
#include <stdint.h>
#include "ros2ebpf/maps.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROS2EBPF_EXPORTER_VERSION "0.1.0"
#define ROS2EBPF_DEFAULT_METRICS_LISTEN "0.0.0.0:9477"

/* ---- growable text buffer ---------------------------------------------- */

struct ros2ebpf_buf {
    char *data;
    size_t len;
    size_t cap;
};

int  ros2ebpf_buf_printf(struct ros2ebpf_buf *b, const char *fmt, ...)
     __attribute__((format(printf, 2, 3)));
int  ros2ebpf_buf_append(struct ros2ebpf_buf *b, const char *s, size_t n);
void ros2ebpf_buf_free(struct ros2ebpf_buf *b);

/* Append a Prometheus label value with \\ \" \n escaped (no surrounding quotes). */
int  ros2ebpf_buf_escape(struct ros2ebpf_buf *b, const char *s);

/* ---- pinned maps ------------------------------------------------------- */

enum ros2ebpf_map_idx {
    ROS2EBPF_MI_CONFIG = 0,
    ROS2EBPF_MI_ENDPOINTS,
    ROS2EBPF_MI_ENDPOINT_INFO,
    ROS2EBPF_MI_PROCESSES,
    ROS2EBPF_MI_SELF,
    ROS2EBPF_MI_UDP_DROPS,      /* optional: -1 when the udp_drops family is not loaded */
    ROS2EBPF_MI_COUNT
};

struct ros2ebpf_maps {
    int fd[ROS2EBPF_MI_COUNT];
};

/* Opens <pin_root>/<name> for every map. Returns 0, or -errno of the first
 * mandatory map that could not be opened (udp_drops is optional). */
int  ros2ebpf_maps_open(struct ros2ebpf_maps *m, const char *pin_root);
void ros2ebpf_maps_close(struct ros2ebpf_maps *m);

/* ---- snapshot: a user-space copy of the maps taken at scrape time ------ */

struct ros2ebpf_endpoint_entry {
    struct ros2ebpf_endpoint_key   key;
    struct ros2ebpf_endpoint_stats stats;
    struct ros2ebpf_endpoint_info  info;
    int has_info;
};

struct ros2ebpf_process_entry {
    __u32 pid;
    struct ros2ebpf_process_info info;
};

struct ros2ebpf_udp_entry {
    struct ros2ebpf_udp_key   key;
    struct ros2ebpf_udp_stats stats;
};

struct ros2ebpf_snapshot {
    struct ros2ebpf_config config;
    int config_ok;
    struct ros2ebpf_endpoint_entry *endpoints;
    size_t n_endpoints;
    struct ros2ebpf_process_entry *processes;
    size_t n_processes;
    struct ros2ebpf_udp_entry *udp;
    size_t n_udp;
    int udp_ok;                           /* udp_drops map was readable */
    __u64 self[ROS2EBPF_SC_COUNT];
    __u64 read_errors;                    /* lookups that failed mid-iteration */
};

int  ros2ebpf_snapshot_read(const struct ros2ebpf_maps *m, struct ros2ebpf_snapshot *s);
void ros2ebpf_snapshot_free(struct ros2ebpf_snapshot *s);

/* ---- host facts and expectations --------------------------------------- */

struct ros2ebpf_host_info {
    char host[256];
    char machine_id[64];
    char distro[64];
    char arch[32];
    char kernel[128];
    char shim_path[512];
    char offset_table[64];
    char exporter_mode[16];               /* "inprocess" | "external" */
    char robot[64];                       /* optional fallback label */
};

/* Fills host (kernel hostname), machine_id, distro, arch and kernel from the
 * OS; leaves the other fields untouched. */
void ros2ebpf_host_info_collect(struct ros2ebpf_host_info *hi);

struct ros2ebpf_expectation {
    char topic[ROS2EBPF_NAME_LEN];
    double hz;                            /* < 0: not set */
    int takers;                           /* < 0: not set */
};

/* Parses `expectations.yaml`: either `/topic: 10` or
 * `/topic: {hz: 10, takers: 2}` per line. Returns number of entries, or -errno. */
int ros2ebpf_expectations_load(const char *path, struct ros2ebpf_expectation **out, size_t *n);

/* ---- rendering --------------------------------------------------------- */

/* Resolve a UDP local port to the owning process; return 0 and fill comm/pid,
 * or non-zero when unknown. Called once per udp_drops entry per scrape. */
typedef int (*ros2ebpf_port_resolver)(void *ctx, __u16 lport, __u16 family,
                                      char *comm, size_t comm_len, __u32 *pid);

struct ros2ebpf_export_opts {
    const char *host;                     /* required: `host` label on every series */
    const char *robot;                    /* optional fallback label; NULL/"" omits it */
    int rmw_label;                        /* add `rmw` to endpoint series (cardinality!) */
    const struct ros2ebpf_host_info *host_info;        /* ros2ebpf_host_info series; may be NULL */
    const struct ros2ebpf_expectation *expectations;   /* may be NULL */
    size_t n_expectations;
    /* daemon self-health (the standalone exporter passes zeros / -1) */
    int probes_attached;                  /* -1: unknown (external exporter) */
    int probes_configured;
    __u64 environ_read_failures;
    __u64 identity_resolve_failures;
    double scrape_duration_seconds;       /* previous scrape; 0 if unknown */
    const char *version;
    ros2ebpf_port_resolver resolve_port;
    void *resolve_ctx;
};

/* Appends the full exposition to `out`. Returns 0 or -errno. */
int ros2ebpf_render(const struct ros2ebpf_snapshot *s,
                    const struct ros2ebpf_export_opts *o,
                    struct ros2ebpf_buf *out);

/* ---- minimal HTTP/1.1 server for GET /metrics -------------------------- */

typedef int (*ros2ebpf_http_render_fn)(void *ctx, struct ros2ebpf_buf *out);

/* Binds and listens on "host:port" ("[v6]:port" or ":port" also accepted).
 * Returns the listening fd (non-blocking) or -errno. */
int ros2ebpf_http_listen(const char *addr);

/* Accepts one pending connection on `listen_fd`, serves /metrics (via `fn`),
 * /healthz and /, then closes it. Call when `listen_fd` is readable.
 * Returns 0, or -errno if nothing could be accepted. */
int ros2ebpf_http_serve_one(int listen_fd, ros2ebpf_http_render_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif
#endif /* ROS2EBPF_EXPORTER_H */
