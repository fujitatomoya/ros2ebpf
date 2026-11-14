# Testing

## 1. Unit tests (no privileges, no BPF)

| Test | What it covers | Run |
|---|---|---|
| `ros2ebpf_exporter/test/test_render.c` | rendering from an in-memory snapshot: aggregation by label set, exclusion tombstones, unresolved/unnamed endpoints, escaping, cumulative histogram with `+Inf`/`_sum`/`_count`, service sides, UDP attribution, expectations, process/host info, self-health, `robot`/`rmw` labels, `expectations.yaml` parser | `colcon test --packages-select ros2ebpf_exporter` |
| `ros2ebpf_daemon/test/test_config.c` | full `config.yaml` parse, defaults, `seq_gap: all`, empty lists clearing defaults, invalid values, immutable-key diff, offset table lookup, distro-from-path, ELF symbol check | `colcon test --packages-select ros2ebpf_daemon` |
| `ros2ebpf/test/test_api.py` | unix-socket JSON-RPC client against a fake daemon, transports from host specs, `fleet.yaml`, target resolution, table formatting, rate computation (aggregation, filters, tombstones) | `colcon test --packages-select ros2ebpf` (or `cd ros2ebpf && pytest`) |
| `ros2ebpf/test/test_verbs.py` | config get/set on a file, value coercion, status table, ELF parser on libc and (if present) a real shim | same |
| `ros2ebpf/test/test_monitor.py` | the `ros2 ebpf monitor` view model: rates and sparkline history, processes and health views, filter and sort, styles (gaps highlighted, idle dimmed, unhealthy red), `--once` output | same |
| `ros2ebpf/test/test_daemon.py` | `ros2 ebpf start` / `stop`: command line (sudo or not), log file, spawn detached, wait for the socket, stop through the `stop` RPC, failure paths (exit before answering, timeout, missing binary / config), against a fake `ros2ebpfd` executable | same |

`colcon test --merge-install && colcon test-result --verbose` in the workspace (the C tests go through `ament_add_test`).
The whole CI script, including the end-to-end test below, runs locally in the development container ([development.md](development.md), Build).

## 2. End-to-end (CI, every commit)

`end_to_end` in `scripts/github_workflows.sh`, in the `rolling` job of `.github/workflows/ci.yaml` (privileged `tomoyafujita/ros2dev:rolling` container in the host pid namespace, so the BPF programs load on the runner kernel and the daemon can resolve the pids they report).
Everything is built from mainline source first (section 1).

Scenario: the daemon with a static shim on the mainline-built `librmw_implementation.so` (`offset table rolling/<arch>`); the mainline `demo_nodes_cpp` `talker`, `listener`, `add_two_ints_server` and a client every second in domain 5, in the same container.
Assertions on `ros2ebpf-ctl metrics`:

- publish / take counters for `/chatter` with `host`, `domain_id=5`, `comm`, growing over 3 s;
- service request and response counters on the server side;
- publish-period histogram populated;
- `/rosout` excluded by default;
- `ros2_process_info` carries the pid, an `rmw_*` implementation and the domain;
- the daemon is started with `ros2 ebpf start` and stopped with `ros2 ebpf stop` (mainline ros2cli);
- `ros2 ebpf monitor --once` shows the talker and listener rows and the health line, and the curses UI renders `/chatter` for a few seconds under a pseudo-terminal;
- `ros2ebpf_host_info{offset_table="rolling/…"}`;
- `probes_attached == probes_configured`, zero name read failures;
- short-lived client processes are swept (bounded number of process series);
- `disable take` turns the family off and `status -q` stays 0; `enable take` re-attaches 2/2 links;
- counters survive `ros2 ebpf stop` + `ros2 ebpf start` (pinned maps);
- `ros2 ebpf status` and `ros2 ebpf top --once` (mainline ros2cli) against the daemon.

Last verified run (kernel 7.0 host, ros2dev:rolling container, mainline source):

```
PASS talker publishes /chatter in domain 5
PASS listener takes /chatter
PASS service requests on the server side
PASS service responses on the server side
PASS publish period histogram has samples
PASS /rosout excluded
PASS process identity
PASS host info with the rolling offset table
PASS no name read failures
PASS counter grows (9 -> 12)
PASS exited processes are swept (3 process series)
PASS disable take -> family off
PASS status stays healthy (family no longer requested)
PASS enable take -> 2/2 links
PASS counters survived daemon restart (13 -> 17)
e2e: all checks passed
```

## 3. Quick check in the development container (README §4)

```bash
ros2ebpfd --check -c /etc/ros2ebpf/config.yaml
ros2 ebpf start --log-level debug
ros2 run demo_nodes_cpp talker &
ros2 ebpf status
curl -s localhost:9477/metrics | grep chatter
ros2 ebpf stop
```

Expect `16/16 probe links` (one shim) and `ros2_rmw_publish_total{...topic="/chatter"}`; the daemon log is in `~/.ros/log/ros2ebpfd.log`.

## 4. Multi-host test plan (manual)

Prerequisites: two or more hosts on one LAN, `ros2ebpfd` on each (package or container), a collector on one of them, a central Prometheus with the remote-write receiver enabled (`deploy/k8s/central/`, or any `prometheus --web.enable-remote-write-receiver` on a laptop; point the collector's `REMOTE_WRITE_URL` at `http://<laptop>:9090/api/v1/write`).

| # | Step | Expected |
|---|---|---|
| 1 | `ros2ebpf-ctl status -q` on every host | exit 0; `ros2ebpf_host_info` with distinct `host`, correct `arch`/`offset_table` (`rolling/<arch>`) |
| 2 | collector scraping all hosts | `up{job="ros2ebpf"}` = 1 per host in central, `robot` label set |
| 3 | talker on host A, listener on host B, same domain | `rate(ros2_rmw_publish_total{host="A"}[1m])` ≈ `rate(ros2_rmw_take_total{host="B"}[1m])` |
| 4 | mixed arch (an aarch64 host next to x86_64 ones) | step 3 holds; `offset_table` is `rolling/aarch64` there; `ros2ebpf_name_read_failures_total` stays 0 |
| 5 | a node in a container on host B | `ros2_process_info{host="B",container!=""}`; counted like any other |
| 6 | `ros2 ebpf status --robot <name>` from the operator machine (fleet.yaml, ssh) | one healthy row per host |
| 7 | expectations with `takers`; stop the listener on B | `Ros2CrossHostLoss` fires within ~3 min; resolves after restart |
| 8 | stop the talker | `Ros2TopicSilent` / `Ros2TopicRateLow` fire; resolve after restart |
| 9 | disconnect host B from the LAN for 3 min | `Ros2ebpfHostDown{host="B"}` fires; no gap in B's series for the time it was still scrapeable |
| 10 | disconnect the collector from central for 10 min | `Ros2ebpfRobotBlind` after 5 min; samples backfill on reconnect (WAL) |
| 11 | `ros2 ebpf stop && ros2 ebpf start` on A while the talker runs | counters continue; `maps_recreated_at_start: false` |
| 12 | two robots with the same domain id and topic names | `Ros2DomainShared` (info) fires; dashboards still separate them by `robot` |
| 13 | upgrade the daemon package on one host | if the map ABI changed: one reset, log line `recreating`; otherwise counters continue |

Record the outcome per row with the Prometheus query used; failures in rows 3–5 point at the probe/offset layer, 6 at the CLI/transport, 7–10 at rules/collector, 11–13 at packaging.
