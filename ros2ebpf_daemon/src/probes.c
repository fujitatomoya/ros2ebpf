// SPDX-License-Identifier: Apache-2.0
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include "rmw_probes.skel.h"
#include "udp_drops.skel.h"
#include "probes.h"
#include "log.h"

const char *family_names[FAM_COUNT] = { "publish", "take", "services", "udp_drops" };

int family_from_name(const char *s)
{
    for (int i = 0; i < FAM_COUNT; i++)
        if (!strcmp(s, family_names[i]))
            return i;
    return -1;
}

struct prog_def {
    int fam;
    const char *prog;     /* BPF program name */
    const char *sym;      /* symbol in the shim */
    int retprobe;
};

static const struct prog_def prog_defs[] = {
    { FAM_PUBLISH,  "rmw_publish_enter",         "rmw_publish",                0 },
    { FAM_PUBLISH,  "rmw_publish_loaned_enter",  "rmw_publish_loaned_message", 0 },
    { FAM_TAKE,     "rmw_take_with_info_enter",  "rmw_take_with_info",         0 },
    { FAM_TAKE,     "rmw_take_with_info_exit",   "rmw_take_with_info",         1 },
    { FAM_SERVICES, "rmw_send_request_enter",    "rmw_send_request",           0 },
    { FAM_SERVICES, "rmw_take_response_enter",   "rmw_take_response",          0 },
    { FAM_SERVICES, "rmw_take_response_exit",    "rmw_take_response",          1 },
    { FAM_SERVICES, "rmw_take_request_enter",    "rmw_take_request",           0 },
    { FAM_SERVICES, "rmw_take_request_exit",     "rmw_take_request",           1 },
    { FAM_SERVICES, "rmw_send_response_enter",   "rmw_send_response",          0 },
};
#define N_PROG_DEFS (sizeof(prog_defs) / sizeof(prog_defs[0]))

static const struct prog_def cleanup_defs[] = {
    { -1, "rmw_destroy_publisher_enter",    "rmw_destroy_publisher",    0 },
    { -1, "rmw_destroy_subscription_enter", "rmw_destroy_subscription", 0 },
    { -1, "rmw_destroy_client_enter",       "rmw_destroy_client",       0 },
    { -1, "rmw_destroy_service_enter",      "rmw_destroy_service",      0 },
};
#define N_CLEANUP_DEFS (sizeof(cleanup_defs) / sizeof(cleanup_defs[0]))

static const char *pinned_names[] = {
    ROS2EBPF_MAP_CONFIG, ROS2EBPF_MAP_ENDPOINTS, ROS2EBPF_MAP_ENDPOINT_INFO, ROS2EBPF_MAP_PROCESSES,
    ROS2EBPF_MAP_NAME_FILTER, ROS2EBPF_MAP_EVENTS, ROS2EBPF_MAP_SEEN_PIDS, ROS2EBPF_MAP_SELF,
    ROS2EBPF_MAP_UDP_DROPS,
};

static void wipe_pins(const char *root)
{
    for (size_t i = 0; i < sizeof(pinned_names) / sizeof(pinned_names[0]); i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", root, pinned_names[i]);
        unlink(path);
    }
}

static int mkdir_p(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) && errno != EEXIST)
                return -errno;
            *p = '/';
        }
    if (mkdir(tmp, 0755) && errno != EEXIST)
        return -errno;
    return 0;
}

static int libbpf_print(enum libbpf_print_level level, const char *fmt, va_list args)
{
    if (level == LIBBPF_DEBUG && g_log_level < LOG_DEBUG)
        return 0;
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    size_t l = strlen(buf);
    if (l && buf[l - 1] == '\n')
        buf[l - 1] = '\0';
    if (level == LIBBPF_WARN)
        log_warn("libbpf: %s", buf);
    else if (level == LIBBPF_INFO)
        log_info("libbpf: %s", buf);
    else
        log_debug("libbpf: %s", buf);
    return 0;
}

static int open_load(struct probes *p)
{
    LIBBPF_OPTS(bpf_object_open_opts, opts, .pin_root_path = p->pin_root);
    p->rmw = rmw_probes_bpf__open_opts(&opts);
    if (!p->rmw)
        return -errno ? -errno : -EINVAL;
    int rc = rmw_probes_bpf__load(p->rmw);
    if (rc) {
        rmw_probes_bpf__destroy(p->rmw);
        p->rmw = NULL;
        return rc;
    }
    p->udp = udp_drops_bpf__open_opts(&opts);
    if (!p->udp)
        return -errno ? -errno : -EINVAL;
    rc = udp_drops_bpf__load(p->udp);
    if (rc) {
        udp_drops_bpf__destroy(p->udp);
        p->udp = NULL;
        return rc;
    }
    return 0;
}

int probes_open_load(struct probes *p, const struct config *cfg, ring_buffer_sample_fn cb, void *ctx)
{
    memset(p, 0, sizeof(*p));
    snprintf(p->pin_root, sizeof(p->pin_root), "%s", cfg->pin_root);
    libbpf_set_print(libbpf_print);
    libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

    int rc = mkdir_p(p->pin_root);
    if (rc) {
        log_error("cannot create pin root %s: %s (is bpffs mounted on /sys/fs/bpf?)", p->pin_root, strerror(-rc));
        return rc;
    }

    rc = open_load(p);
    if (rc == 0) {
        /* ABI check against whatever was pinned before us */
        __u32 zero = 0;
        struct ros2ebpf_config c = { 0 };
        if (bpf_map__lookup_elem(p->rmw->maps.config, &zero, sizeof(zero), &c, sizeof(c), 0) == 0 &&
            c.abi != 0 && c.abi != ROS2EBPF_MAP_ABI) {
            log_warn("pinned maps have ABI %u, this daemon uses %u: recreating (counters reset)", c.abi, ROS2EBPF_MAP_ABI);
            rc = -EINVAL;
        }
    }
    if (rc) {
        if (p->rmw) rmw_probes_bpf__destroy(p->rmw);
        if (p->udp) udp_drops_bpf__destroy(p->udp);
        p->rmw = NULL;
        p->udp = NULL;
        log_warn("loading with existing pinned maps failed (%s); recreating them under %s",
                 strerror(-rc), p->pin_root);
        wipe_pins(p->pin_root);
        p->wiped = 1;
        rc = open_load(p);
        if (rc) {
            log_error("failed to load BPF objects: %s", strerror(-rc));
            return rc;
        }
    }

    p->rb = ring_buffer__new(bpf_map__fd(p->rmw->maps.events), cb, ctx, NULL);
    if (!p->rb) {
        rc = -errno;
        log_error("ring_buffer__new: %s", strerror(-rc));
        probes_close(p);
        return rc;
    }
    p->configured[FAM_PUBLISH] = cfg->probe_publish;
    p->configured[FAM_TAKE] = cfg->probe_take;
    p->configured[FAM_SERVICES] = cfg->probe_services;
    p->configured[FAM_UDP_DROPS] = cfg->probe_udp_drops;
    return 0;
}

static void free_links(struct fam_links *fl)
{
    for (int i = 0; i < fl->n; i++)
        bpf_link__destroy(fl->links[i]);
    free(fl->links);
    fl->links = NULL;
    fl->n = 0;
}

void probes_close(struct probes *p)
{
    for (int f = 0; f < FAM_COUNT; f++)
        free_links(&p->links[f]);
    free_links(&p->cleanup);
    if (p->exit_link)
        bpf_link__destroy(p->exit_link);
    p->exit_link = NULL;
    if (p->rb)
        ring_buffer__free(p->rb);
    if (p->rmw)
        rmw_probes_bpf__destroy(p->rmw);
    if (p->udp)
        udp_drops_bpf__destroy(p->udp);
    p->rb = NULL;
    p->rmw = NULL;
    p->udp = NULL;
}

/* ---- shims ---------------------------------------------------------------- */

static struct shim *find_shim(struct probes *p, dev_t dev, ino_t ino)
{
    for (int i = 0; i < p->n_shims; i++)
        if (p->shims[i].dev == dev && p->shims[i].ino == ino)
            return &p->shims[i];
    return NULL;
}

static int find_rejected(const struct probes *p, dev_t dev, ino_t ino)
{
    for (int i = 0; i < p->n_rejected; i++)
        if (p->rejected[i].dev == dev && p->rejected[i].ino == ino)
            return 1;
    return 0;
}

/* ros2ebpf follows ROS 2 Rolling only (#15). A shim under /opt/ros/<distro>/ names its
 * distro and anything else (the colcon install/ of a source build) is Rolling; a shim of
 * another distro is refused. `seen_path` is the path as the process sees it: for a
 * discovered shim the in-container path, not /proc/<pid>/root/<path>. */
static int resolve_shim_offsets(struct shim *s, const char *seen_path, const char *host_arch)
{
    if (offsets_distro_from_path(seen_path, s->distro, sizeof(s->distro)) != 0)
        snprintf(s->distro, sizeof(s->distro), "rolling");
    if (!s->arch[0])
        snprintf(s->arch, sizeof(s->arch), "%s", host_arch);
    s->off = offsets_lookup(s->distro, s->arch);
    if (!s->off) {
        log_error("shim %s: ROS 2 %s on %s is not supported (ros2ebpf follows Rolling only, "
                  "on x86_64 and aarch64); skipping it", s->path, s->distro, s->arch);
        return -1;
    }
    return 0;
}

static int attach_shim_family(struct probes *p, struct shim *s, int fam);
static int attach_shim_cleanup(struct probes *p, struct shim *s);

static int register_shim(struct probes *p, const struct shim *in)
{
    if (p->n_shims >= MAX_SHIMS) {
        log_warn("too many shims (max %d), ignoring %s", MAX_SHIMS, in->path);
        return -ENOSPC;
    }
    if (p->active_off && (p->active_off->name_off != in->off->name_off ||
                          p->active_off->msginfo_pubseq != in->off->msginfo_pubseq)) {
        log_error("shim %s (%s/%s) has different struct offsets than the active table %s; "
                  "mixed layouts on one host are not supported, skipping it",
                  in->path, in->distro, in->arch, p->offset_table);
        return -EINVAL;
    }
    p->shims[p->n_shims] = *in;
    struct shim *s = &p->shims[p->n_shims++];
    if (!p->active_off) {
        p->active_off = s->off;
        snprintf(p->offset_table, sizeof(p->offset_table), "%s/%s", s->distro, s->arch);
    }
    log_info("shim %s: distro=%s arch=%s dev=%u:%u ino=%llu (%s)", s->path, s->distro, s->arch,
             major(s->dev), minor(s->dev), (unsigned long long)s->ino,
             s->from_config ? "config" : "discovered");
    /* attach whatever is currently enabled */
    int any = 0;
    for (int f = 0; f < FAM_UDP_DROPS; f++)
        if (p->enabled[f]) {
            if (attach_shim_family(p, s, f) == 0)
                any = 1;
        }
    if (any)
        attach_shim_cleanup(p, s);
    return 0;
}

int probes_add_config_shims(struct probes *p, const struct config *cfg, const char *host_arch)
{
    int added = 0;
    for (int i = 0; i < cfg->n_shims; i++) {
        struct shim s = { .from_config = 1 };
        snprintf(s.path, sizeof(s.path), "%s", cfg->shims[i].path);
        snprintf(s.arch, sizeof(s.arch), "%s", cfg->shims[i].arch);
        struct stat st;
        if (stat(s.path, &st)) {
            log_error("shim %s: %s", s.path, strerror(errno));
            continue;
        }
        char missing[512];
        if (offsets_check_shim(s.path, missing, sizeof(missing))) {
            log_error("shim %s is not a usable librmw_implementation.so: missing %s", s.path, missing);
            continue;
        }
        s.dev = st.st_dev;
        s.ino = st.st_ino;
        if (find_shim(p, s.dev, s.ino))
            continue;
        if (resolve_shim_offsets(&s, s.path, host_arch))
            continue;
        if (register_shim(p, &s) == 0)
            added++;
    }
    return added;
}

int probes_discover_shims(struct probes *p, const char *host_arch)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    int added = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] < '1' || de->d_name[0] > '9')
            continue;
        char mpath[64];
        snprintf(mpath, sizeof(mpath), "/proc/%.32s/maps", de->d_name);
        FILE *f = fopen(mpath, "r");
        if (!f)
            continue;
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            char *file = strstr(line, " /");
            if (!file)
                continue;
            file++;
            size_t l = strlen(file);
            while (l && (file[l - 1] == '\n' || file[l - 1] == ' '))
                file[--l] = '\0';
            const char *base = strrchr(file, '/');
            if (!base || strcmp(base + 1, "librmw_implementation.so"))
                continue;
            unsigned long start, end, off, maj, min, ino;
            char perms[8];
            if (sscanf(line, "%lx-%lx %7s %lx %lx:%lx %lu", &start, &end, perms, &off, &maj, &min, &ino) != 7)
                continue;
            dev_t dev = makedev((unsigned)maj, (unsigned)min);
            if (find_shim(p, dev, (ino_t)ino) || find_rejected(p, dev, (ino_t)ino))
                break;
            struct shim s = { .from_config = 0, .dev = dev, .ino = (ino_t)ino };
            snprintf(s.path, sizeof(s.path), "/proc/%.32s/root%.440s", de->d_name, file);
            char missing[512];
            if (offsets_check_shim(s.path, missing, sizeof(missing))) {
                log_debug("discovered %s but it lacks symbols (%s); ignoring", s.path, missing);
                break;
            }
            if (resolve_shim_offsets(&s, file, host_arch)) {
                if (p->n_rejected < MAX_SHIMS) {
                    p->rejected[p->n_rejected].dev = dev;
                    p->rejected[p->n_rejected].ino = (ino_t)ino;
                    p->n_rejected++;
                }
                break;
            }
            if (register_shim(p, &s) == 0)
                added++;
            break;
        }
        fclose(f);
    }
    closedir(d);
    return added;
}

/* ---- attach / detach ------------------------------------------------------ */

static int push_link(struct fam_links *fl, struct bpf_link *l)
{
    struct bpf_link **n = realloc(fl->links, (size_t)(fl->n + 1) * sizeof(*n));
    if (!n)
        return -ENOMEM;
    fl->links = n;
    fl->links[fl->n++] = l;
    return 0;
}

static int attach_one(struct probes *p, struct fam_links *fl, const struct shim *s, const struct prog_def *d)
{
    struct bpf_program *prog = bpf_object__find_program_by_name(p->rmw->obj, d->prog);
    if (!prog) {
        log_error("BPF program %s not found in object", d->prog);
        return -ENOENT;
    }
    LIBBPF_OPTS(bpf_uprobe_opts, uopts, .func_name = d->sym, .retprobe = d->retprobe);
    struct bpf_link *l = bpf_program__attach_uprobe_opts(prog, -1, s->path, 0, &uopts);
    if (!l) {
        int err = -errno;
        log_error("attach %s%s on %s:%s failed: %s", d->retprobe ? "uretprobe" : "uprobe",
                  "", s->path, d->sym, strerror(-err));
        return err;
    }
    return push_link(fl, l);
}

static int attach_shim_family(struct probes *p, struct shim *s, int fam)
{
    int rc = 0;
    for (size_t i = 0; i < N_PROG_DEFS; i++) {
        if (prog_defs[i].fam != fam)
            continue;
        int r = attach_one(p, &p->links[fam], s, &prog_defs[i]);
        if (r)
            rc = r;
    }
    s->attach_failed = rc != 0;
    return rc;
}

static int attach_shim_cleanup(struct probes *p, struct shim *s)
{
    int rc = 0;
    for (size_t i = 0; i < N_CLEANUP_DEFS; i++) {
        int r = attach_one(p, &p->cleanup, s, &cleanup_defs[i]);
        if (r)
            rc = r;
    }
    return rc;
}

static int ensure_cleanup_attached(struct probes *p)
{
    int rc = 0;
    if (!p->exit_link) {
        p->exit_link = bpf_program__attach(p->rmw->progs.sched_process_exit);
        if (!p->exit_link) {
            rc = -errno;
            log_error("attach tp_btf sched_process_exit failed: %s (stale pids are swept by the periodic GC instead)",
                      strerror(-rc));
        }
    }
    if (p->cleanup.n == 0)
        for (int i = 0; i < p->n_shims; i++)
            attach_shim_cleanup(p, &p->shims[i]);
    return rc;
}

int probes_enable(struct probes *p, int fam)
{
    if (fam < 0 || fam >= FAM_COUNT)
        return -EINVAL;
    if (p->enabled[fam])
        return 0;
    if (fam == FAM_UDP_DROPS) {
        struct bpf_link *l = bpf_program__attach(p->udp->progs.udp_fail_queue_rcv_skb);
        if (!l) {
            int err = -errno;
            log_error("attach tp_btf udp_fail_queue_rcv_skb failed: %s", strerror(-err));
            return err;
        }
        push_link(&p->links[fam], l);
        p->enabled[fam] = 1;
        log_info("probe family udp_drops enabled");
        return 0;
    }
    int rc = 0;
    for (int i = 0; i < p->n_shims; i++) {
        int r = attach_shim_family(p, &p->shims[i], fam);
        if (r)
            rc = r;
    }
    p->enabled[fam] = 1;
    ensure_cleanup_attached(p);
    log_info("probe family %s enabled: %d/%d links", family_names[fam],
             probes_links_attached(p, fam), probes_links_expected(p, fam));
    return rc;
}

void probes_disable(struct probes *p, int fam)
{
    if (fam < 0 || fam >= FAM_COUNT || !p->enabled[fam])
        return;
    free_links(&p->links[fam]);
    p->enabled[fam] = 0;
    log_info("probe family %s disabled (counters kept)", family_names[fam]);
    if (!p->enabled[FAM_PUBLISH] && !p->enabled[FAM_TAKE] && !p->enabled[FAM_SERVICES]) {
        free_links(&p->cleanup);
        if (p->exit_link) {
            bpf_link__destroy(p->exit_link);
            p->exit_link = NULL;
        }
    }
}

int probes_links_expected(const struct probes *p, int fam)
{
    if (fam == FAM_UDP_DROPS)
        return 1;
    int per_shim = 0;
    for (size_t i = 0; i < N_PROG_DEFS; i++)
        if (prog_defs[i].fam == fam)
            per_shim++;
    return per_shim * p->n_shims;
}

int probes_links_attached(const struct probes *p, int fam)
{
    return p->links[fam].n;
}

int probes_total_attached(const struct probes *p)
{
    int n = p->cleanup.n + (p->exit_link ? 1 : 0);
    for (int f = 0; f < FAM_COUNT; f++)
        n += p->links[f].n;
    return n;
}

int probes_total_expected(const struct probes *p)
{
    int n = 0, any = 0;
    for (int f = 0; f < FAM_COUNT; f++)
        if (p->configured[f]) {
            n += probes_links_expected(p, f);
            if (f != FAM_UDP_DROPS)
                any = 1;
        }
    if (any)
        n += 1 + (int)N_CLEANUP_DEFS * p->n_shims;
    return n;
}

/* ---- config and filter maps ----------------------------------------------- */

int probes_write_config(struct probes *p, const struct config *cfg)
{
    struct ros2ebpf_config c = { 0 };
    c.abi = ROS2EBPF_MAP_ABI;
    c.nbuckets = (__u32)cfg->n_buckets;
    for (int i = 0; i < cfg->n_buckets; i++)
        c.bounds_ns[i] = (__u64)(cfg->buckets[i] * 1e9 + 0.5);
    const struct offset_entry *off = p->active_off ? p->active_off : offsets_host_default();
    offsets_apply(off, &c);
    c.default_flags = cfg->seq_gap_all ? ROS2EBPF_F_SEQ_GAP : 0;
    __u32 zero = 0;
    int rc = bpf_map__update_elem(p->rmw->maps.config, &zero, sizeof(zero), &c, sizeof(c), BPF_ANY);
    if (rc)
        log_error("writing config map: %s", strerror(-rc));
    return rc;
}

static __u32 flags_for_name(const struct config *cfg, const char *name)
{
    __u32 flags = cfg->seq_gap_all ? ROS2EBPF_F_SEQ_GAP : 0;
    for (int i = 0; i < cfg->n_exclude; i++)
        if (!strcmp(cfg->exclude[i], name))
            flags |= ROS2EBPF_F_EXCLUDE;
    for (int i = 0; i < cfg->n_seq_gap; i++)
        if (!strcmp(cfg->seq_gap[i], name))
            flags |= ROS2EBPF_F_SEQ_GAP;
    return flags;
}

int probes_write_filters(struct probes *p, const struct config *cfg)
{
    int fd = bpf_map__fd(p->rmw->maps.name_filter);
    /* clear */
    __u64 key, next;
    __u64 *prev = NULL;
    while (bpf_map_get_next_key(fd, prev, &next) == 0) {
        key = next;
        bpf_map_delete_elem(fd, &key);
        prev = NULL;      /* restart from the beginning after a delete */
    }
    int n = 0;
    for (int i = 0; i < cfg->n_exclude; i++) {
        __u64 h = ros2ebpf_name_hash(cfg->exclude[i], ROS2EBPF_NAME_LEN);
        __u32 f = flags_for_name(cfg, cfg->exclude[i]);
        if (bpf_map_update_elem(fd, &h, &f, BPF_ANY) == 0)
            n++;
    }
    for (int i = 0; i < cfg->n_seq_gap; i++) {
        __u64 h = ros2ebpf_name_hash(cfg->seq_gap[i], ROS2EBPF_NAME_LEN);
        __u32 f = flags_for_name(cfg, cfg->seq_gap[i]);
        if (bpf_map_update_elem(fd, &h, &f, BPF_ANY) == 0)
            n++;
    }
    log_debug("name filter: %d entries", n);

    /* Re-flag endpoints that already exist (filters only apply at first sight
     * in the probe). Updating a stats value from user space can lose a hit
     * that lands between lookup and update; acceptable on reload. */
    int ifd = bpf_map__fd(p->rmw->maps.endpoint_info);
    int efd = bpf_map__fd(p->rmw->maps.endpoints);
    struct ros2ebpf_endpoint_key k, nk;
    void *pk = NULL;
    int changed = 0;
    while (bpf_map_get_next_key(ifd, pk, &nk) == 0) {
        k = nk;
        pk = &k;
        struct ros2ebpf_endpoint_info info;
        if (bpf_map_lookup_elem(ifd, &k, &info))
            continue;
        if (info.flags & ROS2EBPF_F_NAME_FAIL)
            continue;
        __u8 keep = info.flags & (ROS2EBPF_F_NAME_FAIL | ROS2EBPF_F_ID_FAIL);
        __u8 want = (__u8)(flags_for_name(cfg, info.name) | keep);
        if (want == info.flags)
            continue;
        info.flags = want;
        bpf_map_update_elem(ifd, &k, &info, BPF_EXIST);
        struct ros2ebpf_endpoint_stats st;
        if (bpf_map_lookup_elem(efd, &k, &st) == 0) {
            st.flags = want;
            bpf_map_update_elem(efd, &k, &st, BPF_EXIST);
        }
        changed++;
    }
    if (changed)
        log_info("filters applied to %d existing endpoints", changed);
    return 0;
}

void probes_fill_maps(const struct probes *p, struct ros2ebpf_maps *m)
{
    m->fd[ROS2EBPF_MI_CONFIG] = bpf_map__fd(p->rmw->maps.config);
    m->fd[ROS2EBPF_MI_ENDPOINTS] = bpf_map__fd(p->rmw->maps.endpoints);
    m->fd[ROS2EBPF_MI_ENDPOINT_INFO] = bpf_map__fd(p->rmw->maps.endpoint_info);
    m->fd[ROS2EBPF_MI_PROCESSES] = bpf_map__fd(p->rmw->maps.processes);
    m->fd[ROS2EBPF_MI_SELF] = bpf_map__fd(p->rmw->maps.self_counters);
    m->fd[ROS2EBPF_MI_UDP_DROPS] = p->udp ? bpf_map__fd(p->udp->maps.udp_drops) : -1;
}

int probes_seen_fd(const struct probes *p)
{
    return bpf_map__fd(p->rmw->maps.seen_pids);
}
