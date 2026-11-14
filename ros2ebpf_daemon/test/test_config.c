// SPDX-License-Identifier: Apache-2.0
/* Parses test/config_full.yaml and checks every field; also exercises offsets. */
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "offsets.h"
#include "log.h"

int g_log_level = LOG_WARN;
static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "test";
    char path[512];
    snprintf(path, sizeof(path), "%s/config_full.yaml", dir);

    struct config c;
    config_defaults(&c);
    CHECK(c.n_buckets == 10 && c.probe_publish && c.read_environ && c.discover_shims);
    CHECK(c.n_exclude == 2 && !strcmp(c.exclude[1], "/rosout"));
    CHECK(config_load(&c, path) == 0);
    CHECK(!strcmp(c.host, "main"));
    CHECK(!strcmp(c.robot, "amr-07"));
    CHECK(c.n_shims == 2);
    CHECK(!strcmp(c.shims[0].arch, "x86_64") &&
          !strcmp(c.shims[0].path, "/opt/ros/rolling/lib/librmw_implementation.so"));
    CHECK(!strcmp(c.shims[1].path, "/ws/install/lib/librmw_implementation.so") && !c.shims[1].arch[0]);
    CHECK(c.discover_shims == 0);
    CHECK(c.read_environ == 1 && c.default_domain_id == 3);
    CHECK(c.probe_publish == 1 && c.probe_take == 1 && c.probe_services == 0 && c.probe_udp_drops == 1);
    CHECK(c.n_seq_gap == 2 && !strcmp(c.seq_gap[0], "/lidar/points") && !strcmp(c.seq_gap[1], "/camera/image_raw"));
    CHECK(c.seq_gap_all == 0);
    CHECK(c.n_exclude == 2 && !strcmp(c.exclude[0], "/parameter_events") && !strcmp(c.exclude[1], "/rosout"));
    CHECK(c.n_buckets == 4 && c.buckets[0] == 0.001 && c.buckets[3] == 1.0);
    CHECK(!strcmp(c.exporter_mode, "external") && !strcmp(c.exporter_listen, "127.0.0.1:9477") && c.rmw_label == 1);
    CHECK(!strcmp(c.control_socket, "/tmp/ctl.sock") && !strcmp(c.control_http, "0.0.0.0:9478"));
    CHECK(!strcmp(c.expectations_file, "/etc/ros2ebpf/exp.yaml"));
    CHECK(!strcmp(c.pin_root, "/sys/fs/bpf/test"));
    CHECK(c.log_level == LOG_DEBUG);

    /* seq_gap: all */
    snprintf(path, sizeof(path), "%s/config_seqall.yaml", dir);
    struct config c2;
    config_defaults(&c2);
    CHECK(config_load(&c2, path) == 0);
    CHECK(c2.seq_gap_all == 1 && c2.n_seq_gap == 0);
    CHECK(c2.n_buckets == 10);  /* defaults kept */
    CHECK(c2.n_exclude == 0);   /* explicit empty list clears the default */

    /* invalid */
    snprintf(path, sizeof(path), "%s/config_bad.yaml", dir);
    struct config c3;
    config_defaults(&c3);
    CHECK(config_load(&c3, path) != 0);

    CHECK(config_runtime_immutable_changed(&c, &c2) == 1);
    CHECK(config_runtime_immutable_changed(&c, &c) == 0);

    /* offsets: Rolling only, x86_64 and aarch64 */
    const struct offset_entry *e = offsets_lookup("rolling", "x86_64");
    CHECK(e && e->name_off == 16 && e->msginfo_pubseq == 16);
    e = offsets_lookup("rolling", "aarch64");
    CHECK(e && e->name_off == 16 && e->msginfo_pubseq == 16);
    CHECK(offsets_lookup("humble", "x86_64") == NULL);
    CHECK(offsets_lookup("rolling", "riscv64") == NULL);
    CHECK(offsets_lookup("", "x86_64") == NULL);
    char d[32];
    CHECK(offsets_distro_from_path("/proc/123/root/opt/ros/rolling/lib/librmw_implementation.so", d, sizeof(d)) == 0 && !strcmp(d, "rolling"));
    CHECK(offsets_distro_from_path("/opt/ros/humble/lib/librmw_implementation.so", d, sizeof(d)) == 0 && !strcmp(d, "humble"));
    /* a source build: not under /opt/ros, taken for Rolling by the daemon */
    CHECK(offsets_distro_from_path("/ws/install/lib/librmw_implementation.so", d, sizeof(d)) != 0);
    CHECK(offsets_distro_from_path("/usr/lib/librmw_implementation.so", d, sizeof(d)) != 0);
    char missing[256];
    CHECK(offsets_check_shim("/nonexistent.so", missing, sizeof(missing)) != 0);
    CHECK(offsets_check_shim(argv[0], missing, sizeof(missing)) != 0 && strstr(missing, "rmw_publish"));

    if (fails)
        fprintf(stderr, "%d check(s) failed\n", fails);
    else
        printf("test_config: all checks passed\n");
    return fails ? 1 : 0;
}
