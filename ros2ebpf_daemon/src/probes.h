/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ROS2EBPF_PROBES_H
#define ROS2EBPF_PROBES_H
#include <sys/types.h>
#include <bpf/libbpf.h>
#include "config.h"
#include "offsets.h"
#include "ros2ebpf/exporter.h"

enum family { FAM_PUBLISH = 0, FAM_TAKE, FAM_SERVICES, FAM_UDP_DROPS, FAM_COUNT };
extern const char *family_names[FAM_COUNT];
int family_from_name(const char *s);

#define MAX_SHIMS 64

/* One librmw_implementation.so the uprobes are (to be) attached to. The kernel
 * keys uprobes by inode, so (dev, ino) identifies a shim across containers. */
struct shim {
    char path[512];           /* path used for attach; may be /proc/<pid>/root/... */
    dev_t dev;
    ino_t ino;
    char distro[32];
    char arch[16];
    const struct offset_entry *off;
    int from_config;          /* 1: static entry; 0: discovered through /proc/<pid>/maps */
    int attach_failed;        /* last attach attempt failed (retry on next scan) */
};

struct fam_links {
    struct bpf_link **links;
    int n;
};

struct probes {
    struct rmw_probes_bpf *rmw;
    struct udp_drops_bpf *udp;
    struct ring_buffer *rb;
    char pin_root[256];
    struct shim shims[MAX_SHIMS];
    int n_shims;
    const struct offset_entry *active_off;   /* offsets written to the config map */
    char offset_table[64];                   /* "distro/arch" label */
    int configured[FAM_COUNT];               /* family requested by config */
    int enabled[FAM_COUNT];                  /* family currently enabled (links exist) */
    struct fam_links links[FAM_COUNT];
    struct fam_links cleanup;                /* rmw_destroy_* uprobes, per shim */
    struct bpf_link *exit_link;              /* tp_btf sched_process_exit */
    int wiped;                               /* pinned maps were recreated at startup */
};

int  probes_open_load(struct probes *p, const struct config *cfg, ring_buffer_sample_fn cb, void *ctx);
void probes_close(struct probes *p);

/* Static shims from config: resolve distro/arch/offsets and register. */
int  probes_add_config_shims(struct probes *p, const struct config *cfg, const char *host_arch);
/* Scan /proc/<pid>/maps for shims not yet known (containers); attach enabled
 * families to new ones. Returns number of newly attached shims. */
int  probes_discover_shims(struct probes *p, const char *host_arch);

int  probes_enable(struct probes *p, int fam);     /* attach all programs of a family on every shim */
void probes_disable(struct probes *p, int fam);
int  probes_links_expected(const struct probes *p, int fam);
int  probes_links_attached(const struct probes *p, int fam);
int  probes_total_attached(const struct probes *p);
int  probes_total_expected(const struct probes *p);

int  probes_write_config(struct probes *p, const struct config *cfg);
/* (Re)write the name filter map and re-flag existing endpoints. */
int  probes_write_filters(struct probes *p, const struct config *cfg);

/* Map fds in exporter order, for in-process scrapes without reopening pins. */
void probes_fill_maps(const struct probes *p, struct ros2ebpf_maps *m);
int  probes_seen_fd(const struct probes *p);

#endif
