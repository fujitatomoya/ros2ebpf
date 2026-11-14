// SPDX-License-Identifier: Apache-2.0
/*
 * /etc/ros2ebpf/config.yaml -> struct config, through libyaml's event API.
 * The walker flattens the document into dotted paths ("probes.publish",
 * "shims.0.path", "topics.exclude" per item) which keeps the schema in one
 * switch below and tolerates unknown keys with a warning.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <yaml.h>
#include "config.h"
#include "log.h"

void config_defaults(struct config *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->path, sizeof(c->path), "%s", CFG_DEFAULT_PATH);
    c->discover_shims = 1;
    c->read_environ = 1;
    c->default_domain_id = 0;
    c->probe_publish = c->probe_take = c->probe_services = c->probe_udp_drops = 1;
    static const double b[] = { 0.0005, 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.5, 1 };
    c->n_buckets = sizeof(b) / sizeof(b[0]);
    memcpy(c->buckets, b, sizeof(b));
    c->n_exclude = 2;
    snprintf(c->exclude[0], ROS2EBPF_NAME_LEN, "/parameter_events");
    snprintf(c->exclude[1], ROS2EBPF_NAME_LEN, "/rosout");
    snprintf(c->exporter_mode, sizeof(c->exporter_mode), "inprocess");
    snprintf(c->exporter_listen, sizeof(c->exporter_listen), "0.0.0.0:9477");
    snprintf(c->control_socket, sizeof(c->control_socket), "/run/ros2ebpf/control.sock");
    snprintf(c->expectations_file, sizeof(c->expectations_file), "/etc/ros2ebpf/expectations.yaml");
    snprintf(c->pin_root, sizeof(c->pin_root), "%s", ROS2EBPF_DEFAULT_PIN_ROOT);
    c->log_level = LOG_INFO;
}

static int parse_bool(const char *v, int *out)
{
    if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcmp(v, "1") || !strcasecmp(v, "on")) {
        *out = 1;
        return 0;
    }
    if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcmp(v, "0") || !strcasecmp(v, "off")) {
        *out = 0;
        return 0;
    }
    return -1;
}

static void set_str(char *dst, size_t n, const char *v)
{
    snprintf(dst, n, "%s", v);
}

/* Called for every scalar with its flattened path. Returns 0 or -1 (fatal). */
static int apply(struct config *c, const char *path, const char *val, int in_seq, int seq_index)
{
    int b;
    if (!strcmp(path, "host")) set_str(c->host, sizeof(c->host), val);
    else if (!strcmp(path, "robot")) set_str(c->robot, sizeof(c->robot), val);
    else if (!strncmp(path, "shims.", 6)) {
        int idx = seq_index;
        const char *field = strrchr(path, '.');
        field = field ? field + 1 : "";
        /* path is shims.<n>.<field>; <n> is embedded by the walker */
        sscanf(path, "shims.%d.", &idx);
        if (idx < 0 || idx >= CFG_MAX_SHIMS) {
            log_error("config: too many shims (max %d)", CFG_MAX_SHIMS);
            return -1;
        }
        if (idx >= c->n_shims)
            c->n_shims = idx + 1;
        struct cfg_shim *s = &c->shims[idx];
        if (!strcmp(field, "arch")) set_str(s->arch, sizeof(s->arch), val);
        else if (!strcmp(field, "path")) set_str(s->path, sizeof(s->path), val);
        else log_warn("config: unknown key %s", path);
    }
    else if (!strcmp(path, "discover_shims")) { if (parse_bool(val, &c->discover_shims)) goto bad; }
    else if (!strcmp(path, "identity.read_environ")) { if (parse_bool(val, &c->read_environ)) goto bad; }
    else if (!strcmp(path, "identity.default_domain_id")) c->default_domain_id = atoi(val);
    else if (!strcmp(path, "probes.publish")) { if (parse_bool(val, &c->probe_publish)) goto bad; }
    else if (!strcmp(path, "probes.take")) { if (parse_bool(val, &c->probe_take)) goto bad; }
    else if (!strcmp(path, "probes.services")) { if (parse_bool(val, &c->probe_services)) goto bad; }
    else if (!strcmp(path, "probes.udp_drops")) { if (parse_bool(val, &c->probe_udp_drops)) goto bad; }
    else if (!strcmp(path, "probes.seq_gap")) {
        if (!in_seq) {
            if (!strcasecmp(val, "all")) c->seq_gap_all = 1;
            else if (!strcasecmp(val, "none") || !*val) c->seq_gap_all = 0;
            else if (!parse_bool(val, &b)) c->seq_gap_all = b;
            else goto bad;
        } else if (c->n_seq_gap < CFG_MAX_NAMES) {
            set_str(c->seq_gap[c->n_seq_gap++], ROS2EBPF_NAME_LEN, val);
        }
    }
    else if (!strcmp(path, "topics.exclude")) {
        if (c->n_exclude < CFG_MAX_NAMES)
            set_str(c->exclude[c->n_exclude++], ROS2EBPF_NAME_LEN, val);
    }
    else if (!strcmp(path, "histogram_buckets_seconds")) {
        if (c->n_buckets < CFG_MAX_BUCKETS)
            c->buckets[c->n_buckets++] = atof(val);
        else
            log_warn("config: histogram_buckets_seconds: at most %d buckets, ignoring %s", CFG_MAX_BUCKETS, val);
    }
    else if (!strcmp(path, "exporter.mode")) set_str(c->exporter_mode, sizeof(c->exporter_mode), val);
    else if (!strcmp(path, "exporter.listen")) set_str(c->exporter_listen, sizeof(c->exporter_listen), val);
    else if (!strcmp(path, "exporter.rmw_label")) { if (parse_bool(val, &c->rmw_label)) goto bad; }
    else if (!strcmp(path, "control.socket")) set_str(c->control_socket, sizeof(c->control_socket), val);
    else if (!strcmp(path, "control.http")) set_str(c->control_http, sizeof(c->control_http), val);
    else if (!strcmp(path, "expectations_file")) set_str(c->expectations_file, sizeof(c->expectations_file), val);
    else if (!strcmp(path, "pin_root")) set_str(c->pin_root, sizeof(c->pin_root), val);
    else if (!strcmp(path, "log_level")) {
        if (!strcasecmp(val, "error")) c->log_level = LOG_ERROR;
        else if (!strcasecmp(val, "warn")) c->log_level = LOG_WARN;
        else if (!strcasecmp(val, "info")) c->log_level = LOG_INFO;
        else if (!strcasecmp(val, "debug")) c->log_level = LOG_DEBUG;
        else goto bad;
    }
    else log_warn("config: unknown key %s (ignored)", path);
    return 0;
bad:
    log_error("config: bad value for %s: '%s'", path, val);
    return -1;
}

struct frame;
static int build_path(struct frame *stack, int depth, char *out, size_t n);

/* A list in the file replaces the built-in default list, even an empty one. */
static void begin_list(struct config *c, const char *path)
{
    if (!strcmp(path, "topics.exclude")) c->n_exclude = 0;
    else if (!strcmp(path, "probes.seq_gap")) { c->n_seq_gap = 0; c->seq_gap_all = 0; }
    else if (!strcmp(path, "histogram_buckets_seconds")) c->n_buckets = 0;
    else if (!strcmp(path, "shims")) c->n_shims = 0;
}

struct frame {
    int is_seq;
    int expect_key;      /* maps: next scalar is a key */
    int index;           /* seqs: index of the next item */
    char key[128];       /* maps: current key */
};

/* Map keys joined by '.', with a sequence index embedded only when a map
 * follows it (shims.<n>.path). Returns the index of the innermost sequence. */
static int build_path(struct frame *stack, int depth, char *out, size_t n)
{
    out[0] = '\0';
    int seq_index = 0;
    for (int i = 0; i < depth; i++) {
        struct frame *fr = &stack[i];
        if (fr->is_seq) {
            seq_index = fr->index;
            if (i + 1 < depth && !stack[i + 1].is_seq) {
                char tmp[16];
                snprintf(tmp, sizeof(tmp), "%s%d", out[0] ? "." : "", fr->index);
                strncat(out, tmp, n - strlen(out) - 1);
            }
        } else {
            if (out[0])
                strncat(out, ".", n - strlen(out) - 1);
            strncat(out, fr->key, n - strlen(out) - 1);
        }
    }
    return seq_index;
}

int config_load(struct config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        log_error("config: cannot open %s: %s", path, strerror(errno));
        return -errno;
    }
    snprintf(c->path, sizeof(c->path), "%s", path);

    yaml_parser_t parser;
    yaml_event_t ev;
    if (!yaml_parser_initialize(&parser)) {
        fclose(f);
        return -ENOMEM;
    }
    yaml_parser_set_input_file(&parser, f);

    struct frame stack[16];
    int depth = 0, rc = 0, done = 0;
    char fullpath[512];

    while (!done && rc == 0) {
        if (!yaml_parser_parse(&parser, &ev)) {
            log_error("config: %s:%zu: %s", path, parser.problem_mark.line + 1,
                      parser.problem ? parser.problem : "parse error");
            rc = -1;
            break;
        }
        switch (ev.type) {
        case YAML_STREAM_END_EVENT:
            done = 1;
            break;
        case YAML_MAPPING_START_EVENT:
        case YAML_SEQUENCE_START_EVENT:
            if (depth >= 16) {
                log_error("config: nesting too deep");
                rc = -1;
                break;
            }
            if (ev.type == YAML_SEQUENCE_START_EVENT && depth > 0) {
                /* a list replaces the default list, even when empty */
                build_path(stack, depth, fullpath, sizeof(fullpath));
                begin_list(c, fullpath);
            }
            /* A nested collection sits under the parent's current key / index.
             * Fold the parent's key into a prefix now so children see it. */
            stack[depth].is_seq = ev.type == YAML_SEQUENCE_START_EVENT;
            stack[depth].expect_key = 1;
            stack[depth].index = 0;
            stack[depth].key[0] = '\0';
            depth++;
            break;
        case YAML_MAPPING_END_EVENT:
        case YAML_SEQUENCE_END_EVENT:
            if (depth > 0)
                depth--;
            if (depth > 0 && stack[depth - 1].is_seq)
                stack[depth - 1].index++;
            else if (depth > 0)
                stack[depth - 1].expect_key = 1;
            break;
        case YAML_SCALAR_EVENT: {
            const char *v = (const char *)ev.data.scalar.value;
            if (depth == 0)
                break;
            struct frame *top = &stack[depth - 1];
            if (!top->is_seq && top->expect_key) {
                snprintf(top->key, sizeof(top->key), "%s", v);
                top->expect_key = 0;
                break;
            }
            int in_seq = top->is_seq;
            int seq_index = build_path(stack, depth, fullpath, sizeof(fullpath));
            rc = apply(c, fullpath, v, in_seq, seq_index);
            if (top->is_seq)
                top->index++;
            else
                top->expect_key = 1;
            break;
        }
        default:
            break;
        }
        yaml_event_delete(&ev);
    }
    yaml_parser_delete(&parser);
    fclose(f);
    if (rc)
        return rc;

    if (c->n_buckets < 1) {
        log_error("config: histogram_buckets_seconds must have at least one bound");
        return -1;
    }
    for (int i = 1; i < c->n_buckets; i++)
        if (c->buckets[i] <= c->buckets[i - 1]) {
            log_error("config: histogram_buckets_seconds must be strictly ascending");
            return -1;
        }
    if (strcmp(c->exporter_mode, "inprocess") && strcmp(c->exporter_mode, "external")) {
        log_error("config: exporter.mode must be inprocess or external");
        return -1;
    }
    for (int i = 0; i < c->n_shims; i++)
        if (!c->shims[i].path[0]) {
            log_error("config: shims[%d].path is required", i);
            return -1;
        }
    return 0;
}

int config_runtime_immutable_changed(const struct config *a, const struct config *b)
{
    if (a->n_shims != b->n_shims)
        return 1;
    for (int i = 0; i < a->n_shims; i++)
        if (strcmp(a->shims[i].path, b->shims[i].path))
            return 1;
    return strcmp(a->pin_root, b->pin_root) || strcmp(a->exporter_listen, b->exporter_listen) ||
           strcmp(a->exporter_mode, b->exporter_mode) || strcmp(a->control_socket, b->control_socket) ||
           strcmp(a->control_http, b->control_http) || strcmp(a->host, b->host);
}
