// SPDX-License-Identifier: Apache-2.0
/* Renders an in-memory snapshot (no BPF needed) and checks the exposition. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ros2ebpf/exporter.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static int count_lines_with(const char *text, const char *needle)
{
    int n = 0;
    for (const char *p = text; (p = strstr(p, needle)) != NULL; p++)
        n++;
    return n;
}

static void add_endpoint(struct ros2ebpf_snapshot *s, __u32 pid, __u64 handle, __u8 kind,
                         const char *name, const char *rmw, __u64 count, __u64 count2, __u8 flags)
{
    struct ros2ebpf_endpoint_entry *e = &s->endpoints[s->n_endpoints++];
    memset(e, 0, sizeof(*e));
    e->key.pid = pid;
    e->key.handle = handle;
    e->stats.count = count;
    e->stats.count2 = count2;
    e->stats.flags = flags;
    e->stats.kind = kind;
    e->info.kind = kind;
    e->info.flags = flags;
    snprintf(e->info.name, sizeof(e->info.name), "%s", name);
    snprintf(e->info.rmw_id, sizeof(e->info.rmw_id), "%s", rmw);
    e->has_info = 1;
}

static void add_process(struct ros2ebpf_snapshot *s, __u32 pid, const char *comm, __u32 domain)
{
    struct ros2ebpf_process_entry *p = &s->processes[s->n_processes++];
    memset(p, 0, sizeof(*p));
    p->pid = pid;
    p->info.domain_id = domain;
    snprintf(p->info.comm, sizeof(p->info.comm), "%s", comm);
    snprintf(p->info.rmw, sizeof(p->info.rmw), "rmw_fastrtps_cpp");
}

static int fake_resolver(void *ctx, __u16 lport, __u16 family, char *comm, size_t n, __u32 *pid)
{
    (void)ctx; (void)family;
    if (lport == 7400) {
        snprintf(comm, n, "talker");
        *pid = 42;
        return 0;
    }
    return -1;
}

int main(void)
{
    struct ros2ebpf_snapshot s = { 0 };
    s.endpoints = calloc(16, sizeof(*s.endpoints));
    s.processes = calloc(16, sizeof(*s.processes));
    s.udp = calloc(4, sizeof(*s.udp));
    s.config_ok = 1;
    s.config.abi = ROS2EBPF_MAP_ABI;
    s.config.nbuckets = 3;
    s.config.bounds_ns[0] = 1000000;      /* 0.001 */
    s.config.bounds_ns[1] = 10000000;     /* 0.01 */
    s.config.bounds_ns[2] = 100000000;    /* 0.1 */

    add_process(&s, 100, "talker", 0);
    add_process(&s, 101, "talker", 0);              /* same comm: must aggregate */
    add_process(&s, 200, "listener", 7);
    add_process(&s, 300, "weird", ROS2EBPF_DOMAIN_UNKNOWN);
    s.processes[2].info.rmw[0] = '\0';           /* listener: no RMW_IMPLEMENTATION in environ */

    add_endpoint(&s, 100, 0x1000, ROS2EBPF_KIND_PUBLISHER, "/chatter", "rmw_fastrtps_cpp", 10, 0, 0);
    s.endpoints[0].stats.hist[0] = 2; s.endpoints[0].stats.hist[1] = 3; s.endpoints[0].stats.hist[3] = 1;
    s.endpoints[0].stats.period_sum_ns = 1500000000ull;
    add_endpoint(&s, 100, 0x2000, ROS2EBPF_KIND_PUBLISHER, "/chatter", "rmw_fastrtps_cpp", 5, 0, 0);
    add_endpoint(&s, 101, 0x1000, ROS2EBPF_KIND_PUBLISHER, "/chatter", "rmw_fastrtps_cpp", 7, 0, 0);
    add_endpoint(&s, 100, 0x3000, ROS2EBPF_KIND_PUBLISHER, "/rosout", "rmw_fastrtps_cpp", 99, 0, ROS2EBPF_F_EXCLUDE);
    add_endpoint(&s, 200, 0x1000, ROS2EBPF_KIND_SUBSCRIPTION, "/chatter", "rmw_fastrtps_cpp", 20, 0, ROS2EBPF_F_SEQ_GAP);
    s.endpoints[4].stats.seq_gaps = 2; s.endpoints[4].stats.seq_lost = 5;
    add_endpoint(&s, 200, 0x2000, ROS2EBPF_KIND_CLIENT, "/add_two_ints", "rmw_fastrtps_cpp", 3, 2, 0);
    add_endpoint(&s, 100, 0x4000, ROS2EBPF_KIND_SERVICE, "/add_two_ints", "rmw_fastrtps_cpp", 3, 3, 0);
    add_endpoint(&s, 300, 0x1000, ROS2EBPF_KIND_PUBLISHER, "/a\"b\\c", "garbage", 1, 0, 0);
    add_endpoint(&s, 999, 0x1000, ROS2EBPF_KIND_PUBLISHER, "/unresolved", "rmw_fastrtps_cpp", 1, 0, 0);
    add_endpoint(&s, 100, 0x5000, ROS2EBPF_KIND_PUBLISHER, "", "rmw_fastrtps_cpp", 1, 0, ROS2EBPF_F_NAME_FAIL);

    s.udp_ok = 1;
    s.udp[s.n_udp].key.lport = 7400; s.udp[s.n_udp++].stats.drops = 4;
    s.udp[s.n_udp].key.lport = 7401; s.udp[s.n_udp++].stats.drops = 1;
    s.self[ROS2EBPF_SC_NAME_READ_FAIL] = 1;

    struct ros2ebpf_expectation exp[2] = {
        { .topic = "/chatter", .hz = 10, .takers = 1 },
        { .topic = "/odom", .hz = 50, .takers = -1 },
    };
    struct ros2ebpf_host_info hi = { .host = "main", .machine_id = "abc", .distro = "ubuntu-24.04",
                                     .arch = "x86_64", .kernel = "6.8", .shim_path = "/opt/ros/jazzy/lib/librmw_implementation.so",
                                     .offset_table = "jazzy/x86_64", .exporter_mode = "inprocess" };
    struct ros2ebpf_export_opts o = {
        .host = "main", .host_info = &hi, .expectations = exp, .n_expectations = 2,
        .probes_attached = 12, .probes_configured = 12, .environ_read_failures = 1,
        .resolve_port = fake_resolver, .version = "test",
    };
    struct ros2ebpf_buf out = { 0 };
    int rc = ros2ebpf_render(&s, &o, &out);
    CHECK(rc == 0);
    const char *t = out.data;

    /* aggregation: 10 + 5 + 7 publishes on /chatter from comm=talker */
    CHECK(strstr(t, "ros2_rmw_publish_total{host=\"main\",domain_id=\"0\",comm=\"talker\",topic=\"/chatter\"} 22\n"));
    CHECK(count_lines_with(t, "ros2_rmw_publish_total{") == 2); /* talker:/chatter and weird:/a"b\c */
    /* excluded, unresolved and unnamed endpoints are not exported */
    CHECK(!strstr(t, "/rosout"));
    CHECK(!strstr(t, "/unresolved"));
    /* escaping */
    CHECK(strstr(t, "topic=\"/a\\\"b\\\\c\"} 1\n"));
    CHECK(strstr(t, "domain_id=\"unknown\",comm=\"weird\""));
    /* histogram is cumulative with +Inf and sum in seconds */
    CHECK(strstr(t, "ros2_rmw_publish_period_seconds_bucket{host=\"main\",domain_id=\"0\",comm=\"talker\",topic=\"/chatter\",le=\"0.001\"} 2\n"));
    CHECK(strstr(t, "topic=\"/chatter\",le=\"0.01\"} 5\n"));
    CHECK(strstr(t, "topic=\"/chatter\",le=\"0.1\"} 5\n"));
    CHECK(strstr(t, "topic=\"/chatter\",le=\"+Inf\"} 6\n"));
    CHECK(strstr(t, "ros2_rmw_publish_period_seconds_sum{host=\"main\",domain_id=\"0\",comm=\"talker\",topic=\"/chatter\"} 1.5\n"));
    CHECK(strstr(t, "ros2_rmw_publish_period_seconds_count{host=\"main\",domain_id=\"0\",comm=\"talker\",topic=\"/chatter\"} 6\n"));
    /* takes and gaps */
    CHECK(strstr(t, "ros2_rmw_take_total{host=\"main\",domain_id=\"7\",comm=\"listener\",topic=\"/chatter\"} 20\n"));
    CHECK(strstr(t, "ros2_rmw_take_seq_gap_total{host=\"main\",domain_id=\"7\",comm=\"listener\",topic=\"/chatter\"} 2\n"));
    CHECK(strstr(t, "ros2_rmw_take_seq_lost_total{host=\"main\",domain_id=\"7\",comm=\"listener\",topic=\"/chatter\"} 5\n"));
    /* services */
    CHECK(strstr(t, "ros2_rmw_service_request_total{host=\"main\",domain_id=\"7\",comm=\"listener\",service=\"/add_two_ints\",side=\"client\"} 3\n"));
    CHECK(strstr(t, "ros2_rmw_service_response_total{host=\"main\",domain_id=\"7\",comm=\"listener\",service=\"/add_two_ints\",side=\"client\"} 2\n"));
    CHECK(strstr(t, "ros2_rmw_service_request_total{host=\"main\",domain_id=\"0\",comm=\"talker\",service=\"/add_two_ints\",side=\"server\"} 3\n"));
    CHECK(strstr(t, "ros2_rmw_service_response_total{host=\"main\",domain_id=\"0\",comm=\"talker\",service=\"/add_two_ints\",side=\"server\"} 3\n"));
    /* udp */
    CHECK(strstr(t, "ros2_udp_rcvbuf_drop_total{host=\"main\",port=\"7400\",comm=\"talker\"} 4\n"));
    CHECK(strstr(t, "ros2_udp_rcvbuf_drop_total{host=\"main\",port=\"7401\",comm=\"unknown\"} 1\n"));
    /* expectations without host label */
    CHECK(strstr(t, "ros2_topic_expected_hz{topic=\"/chatter\"} 10\n"));
    CHECK(strstr(t, "ros2_topic_expected_hz{topic=\"/odom\"} 50\n"));
    CHECK(strstr(t, "ros2_topic_expected_takers{topic=\"/chatter\"} 1\n"));
    CHECK(!strstr(t, "ros2_topic_expected_takers{topic=\"/odom\"}"));
    /* process / host info / self health */
    CHECK(strstr(t, "ros2_process_info{host=\"main\",comm=\"talker\",pid=\"100\",domain_id=\"0\",rmw=\"rmw_fastrtps_cpp\",container=\"\",localhost_only=\"false\"} 1\n"));
    CHECK(strstr(t, "ros2_process_info{host=\"main\",comm=\"listener\",pid=\"200\",domain_id=\"7\",rmw=\"rmw_fastrtps_cpp\",container=\"\",localhost_only=\"false\"} 1\n"));
    CHECK(strstr(t, "ros2ebpf_host_info{host=\"main\",machine_id=\"abc\",distro=\"ubuntu-24.04\",arch=\"x86_64\",kernel=\"6.8\",shim_path=\"/opt/ros/jazzy/lib/librmw_implementation.so\",offset_table=\"jazzy/x86_64\",exporter_mode=\"inprocess\"} 1\n"));
    CHECK(strstr(t, "ros2ebpf_probes_attached{host=\"main\"} 12\n"));
    CHECK(strstr(t, "ros2ebpf_unknown_rmw_processes{host=\"main\"} 1\n"));
    CHECK(strstr(t, "ros2ebpf_unresolved_endpoints{host=\"main\"} 1\n"));
    CHECK(strstr(t, "ros2ebpf_unnamed_endpoints{host=\"main\"} 1\n"));
    CHECK(strstr(t, "ros2ebpf_name_read_failures_total{host=\"main\"} 1\n"));
    CHECK(strstr(t, "ros2ebpf_environ_read_failures_total{host=\"main\"} 1\n"));
    CHECK(strstr(t, "ros2ebpf_map_entries{host=\"main\",map=\"endpoints\"} 10\n"));

    /* robot fallback label and rmw label */
    o.robot = "amr-07";
    o.rmw_label = 1;
    ros2ebpf_buf_free(&out);
    rc = ros2ebpf_render(&s, &o, &out);
    CHECK(rc == 0);
    CHECK(strstr(out.data, "ros2_rmw_publish_total{host=\"main\",robot=\"amr-07\",domain_id=\"0\",comm=\"talker\",topic=\"/chatter\",rmw=\"rmw_fastrtps_cpp\"} 22\n"));

    /* expectations file parser */
    {
        char path[] = "/tmp/ros2ebpf_exp_XXXXXX";
        int fd = mkstemp(path);
        CHECK(fd >= 0);
        const char *yaml = "# comment\n/odom: 50\n/lidar/points: {hz: 10, takers: 2}\n\"/imu/data\":\n  hz: 200\n  takers: 1\n";
        FILE *f = fdopen(fd, "w");
        fputs(yaml, f);
        fclose(f);
        struct ros2ebpf_expectation *e = NULL;
        size_t n = 0;
        int cnt = ros2ebpf_expectations_load(path, &e, &n);
        CHECK(cnt == 3 && n == 3);
        if (n == 3) {
            CHECK(strcmp(e[0].topic, "/odom") == 0 && e[0].hz == 50 && e[0].takers == -1);
            CHECK(strcmp(e[1].topic, "/lidar/points") == 0 && e[1].hz == 10 && e[1].takers == 2);
            CHECK(strcmp(e[2].topic, "/imu/data") == 0 && e[2].hz == 200 && e[2].takers == 1);
        }
        free(e);
        unlink(path);
    }

    if (fails) {
        fprintf(stderr, "---- output ----\n%s", out.data);
        fprintf(stderr, "%d check(s) failed\n", fails);
    } else {
        printf("test_render: all checks passed (%zu bytes)\n", out.len);
    }
    ros2ebpf_buf_free(&out);
    free(s.endpoints);
    free(s.processes);
    free(s.udp);
    return fails ? 1 : 0;
}
