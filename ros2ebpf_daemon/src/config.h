/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ROS2EBPF_CONFIG_H
#define ROS2EBPF_CONFIG_H
#include <stddef.h>
#include "ros2ebpf/maps.h"

#define CFG_MAX_SHIMS   8
#define CFG_MAX_NAMES   256
#define CFG_MAX_BUCKETS (ROS2EBPF_HIST_BUCKETS - 1)
#define CFG_DEFAULT_PATH "/etc/ros2ebpf/config.yaml"

struct cfg_shim {
    char distro[32];          /* "" = derive from path (/opt/ros/<distro>/...) */
    char arch[16];            /* "" = uname -m */
    char path[512];
};

struct config {
    char path[512];           /* file this was loaded from */
    char host[256];
    char robot[64];
    struct cfg_shim shims[CFG_MAX_SHIMS];
    int n_shims;
    int discover_shims;       /* scan /proc/<pid>/maps for shims in containers */
    int read_environ;
    int default_domain_id;
    int probe_publish, probe_take, probe_services, probe_udp_drops;
    int seq_gap_all;          /* probes.seq_gap: all */
    char seq_gap[CFG_MAX_NAMES][ROS2EBPF_NAME_LEN];
    int n_seq_gap;
    char exclude[CFG_MAX_NAMES][ROS2EBPF_NAME_LEN];
    int n_exclude;
    double buckets[CFG_MAX_BUCKETS];
    int n_buckets;
    char exporter_mode[16];   /* inprocess | external */
    char exporter_listen[64];
    int rmw_label;
    char control_socket[256];
    char control_http[64];
    char expectations_file[512];
    char pin_root[256];
    char offset_table[64];    /* "" = auto from shim distro/arch; else "distro/arch" override */
    int log_level;
};

void config_defaults(struct config *c);
/* Returns 0, or -errno / -1 with a message on stderr. */
int  config_load(struct config *c, const char *path);
/* Diff of keys that cannot change at runtime (shims, pin_root, listen addresses, socket). */
int  config_runtime_immutable_changed(const struct config *old, const struct config *new);

#endif
