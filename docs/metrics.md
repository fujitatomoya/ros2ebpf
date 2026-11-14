# Metrics reference

All series are produced by the exporter library from the pinned BPF maps at scrape time.
The daemon never computes a rate; use `rate()` / `increase()` server-side.

## Labels

| Label | On | Source |
|---|---|---|
| `host` | every series | daemon (`host:` config, `ROS2EBPF_HOST`, or kernel hostname) |
| `robot` | every series | collector `external_labels` on `remote_write`; or daemon `robot:` fallback |
| `domain_id` | endpoint series, `ros2_process_info` | `ROS_DOMAIN_ID` of the process; `"unknown"` if `environ` unreadable |
| `comm` | endpoint series, `ros2_process_info`, `ros2_udp_rcvbuf_drop_total` | `/proc/<pid>/comm` (15 chars) |
| `topic` / `service` | endpoint series | name as seen by rmw (fully expanded, 127 chars max) |
| `side` | service series | `client` or `server` |
| `rmw` | endpoint series only with `exporter.rmw_label: true`; always on `ros2_process_info` | `RMW_IMPLEMENTATION`, else the identifier read from the rmw handle |
| `pid`, `container`, `localhost_only` | `ros2_process_info` only | `/proc` |
| `le` | histogram buckets | `histogram_buckets_seconds` |

Endpoints that share a label set are summed (two publishers of `/tf` in one process; two `talker` processes).
Join process facts with `... * on (host, comm, domain_id) group_left (pid, container) ros2_process_info` when needed.

## Endpoint metrics

| Metric | Type | Meaning |
|---|---|---|
| `ros2_rmw_publish_total` | counter | calls to `rmw_publish` / `rmw_publish_loaned_message` per publisher |
| `ros2_rmw_publish_period_seconds` | histogram | interval between consecutive publishes of one publisher (`_bucket`, `_sum`, `_count`) |
| `ros2_rmw_take_total` | counter | `rmw_take_with_info` returning `taken == true` per subscription |
| `ros2_rmw_take_seq_gap_total` | counter | gap events in `publication_sequence_number` (only topics in `probes.seq_gap`) |
| `ros2_rmw_take_seq_lost_total` | counter | messages skipped according to those gaps |
| `ros2_rmw_service_request_total{side}` | counter | `client`: `rmw_send_request`; `server`: taken `rmw_take_request` |
| `ros2_rmw_service_response_total{side}` | counter | `server`: `rmw_send_response`; `client`: taken `rmw_take_response` |
| `ros2_udp_rcvbuf_drop_total{port,comm}` | counter | datagrams dropped on receive because the socket queue was full; `comm` resolved from `/proc/net/udp` when the port belongs to a known ROS process |

Counters disappear with their endpoint (`rmw_destroy_*`) or process.
Pinned maps keep them across daemon restarts.

## Robot-level data

| Metric | Type | Meaning |
|---|---|---|
| `ros2_topic_expected_hz{topic}` | gauge | from `expectations.yaml`; no `host` label (one host of the robot serves it) |
| `ros2_topic_expected_takers{topic}` | gauge | optional expected number of takers |

## Identity

| Metric | Type | Meaning |
|---|---|---|
| `ros2_process_info{comm,pid,domain_id,rmw,container,localhost_only}` | gauge = 1 | one per process with endpoints seen |
| `ros2ebpf_host_info{machine_id,distro,arch,kernel,shim_path,offset_table,exporter_mode}` | gauge = 1 | one per host |
| `ros2ebpf_build_info{version}` | gauge = 1 | |

## Daemon / exporter health

| Metric | Type | Meaning |
|---|---|---|
| `ros2ebpf_probes_attached` / `ros2ebpf_probes_configured` | gauge | BPF links attached vs. asked for by config; alert when different |
| `ros2ebpf_map_entries{map}` | gauge | `endpoints`, `processes`, `udp_drops` |
| `ros2ebpf_map_abi` | gauge | pinned map layout version |
| `ros2ebpf_name_read_failures_total` | counter | `topic_name` unreadable in the probe (offset mismatch) |
| `ros2ebpf_id_read_failures_total` | counter | `implementation_identifier` unreadable |
| `ros2ebpf_unknown_rmw_processes` | gauge | processes whose identifier does not start with `rmw_` (offset mismatch) |
| `ros2ebpf_events_dropped_total` | counter | ring buffer full: a pid may stay unresolved until the 30 s GC |
| `ros2ebpf_map_full_total` | counter | endpoint map full (65536 entries) |
| `ros2ebpf_environ_read_failures_total` | counter | processes with `domain_id="unknown"` |
| `ros2ebpf_identity_resolve_failures_total` | counter | processes gone before `/proc` could be read |
| `ros2ebpf_unresolved_endpoints` | gauge | endpoints skipped this scrape (identity pending) |
| `ros2ebpf_unnamed_endpoints` | gauge | endpoints skipped (name unreadable) |
| `ros2ebpf_snapshot_read_errors` | gauge | map lookups that failed during this scrape (entries deleted mid-walk) |
| `ros2ebpf_scrape_duration_seconds` | gauge | previous scrape |

## PromQL cookbook

```promql
# publish rate per topic on a robot
sum by (topic) (rate(ros2_rmw_publish_total{robot="amr-07"}[1m]))

# the same, split by host (where is /tf published from?)
sum by (host, topic) (rate(ros2_rmw_publish_total{robot="amr-07"}[1m]))

# ratio to expectation (alerts use 0.8)
sum by (robot, topic) (rate(ros2_rmw_publish_total[30s])) / on (robot, topic) group_left ros2_topic_expected_hz

# cross-host loss: takes vs publishes x expected takers
sum by (robot, topic) (rate(ros2_rmw_take_total[1m]))
  / on (robot, topic)
  (sum by (robot, topic) (rate(ros2_rmw_publish_total[1m])) * on (robot, topic) group_left ros2_topic_expected_takers)

# p99 inter-publish interval (jitter) of one publisher
histogram_quantile(0.99, sum by (le) (rate(ros2_rmw_publish_period_seconds_bucket{robot="amr-07",topic="/imu/data"}[5m])))

# service latency proxy: requests sent vs responses taken on the client side
sum by (service) (rate(ros2_rmw_service_request_total{side="client"}[1m]))
  - sum by (service) (rate(ros2_rmw_service_response_total{side="client"}[1m]))

# processes with endpoints but unknown domain (identity problem)
ros2_process_info{domain_id="unknown"}

# which offset table row (rolling/<arch>) each host uses
count by (arch, offset_table) (ros2ebpf_host_info)
```
