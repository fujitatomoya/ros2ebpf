# Testing

## 1. Unit tests (no privileges, no BPF)

| Test | What it covers | Run |
|---|---|---|
| `ros2ebpf_exporter/test/test_render.c` | rendering from an in-memory snapshot: aggregation by label set, exclusion tombstones, unresolved/unnamed endpoints, escaping, cumulative histogram with `+Inf`/`_sum`/`_count`, service sides, UDP attribution, expectations, process/host info, self-health, `robot`/`rmw` labels, `expectations.yaml` parser | `colcon test --packages-select ros2ebpf_exporter` |
| `ros2ebpf_daemon/test/test_config.c` | full `config.yaml` parse, defaults, `seq_gap: all`, empty lists clearing defaults, invalid values, immutable-key diff, offset table lookup, distro-from-path, ELF symbol check | `colcon test --packages-select ros2ebpf_daemon` |
| `ros2ebpf/test/test_api.py` | unix-socket JSON-RPC client against a fake daemon, transports from host specs, `fleet.yaml`, target resolution, table formatting, rate computation (aggregation, filters, tombstones) | `colcon test --packages-select ros2ebpf` (or `cd ros2ebpf && pytest`) |
| `ros2ebpf/test/test_verbs.py` | config get/set on a file, value coercion, status table, ELF parser on libc and (if present) a real shim | same |

`make test cli-test` or `make in-docker-test`; `colcon test-result --verbose` prints the results
(the C tests go through `ament_add_test`). `make in-docker-ci` runs the whole CI script against a
Rolling source build (tests, flake8, offset table vs. mainline `rmw`, BPF verifier check, deb).

## 2. Container end-to-end (`scripts/e2e.sh`, `make e2e`)

Needs Docker, a kernel with BTF, bpffs. No ROS on the host.

Scenario: `ros2ebpf-demo:local` (ROS 2 Rolling: `talker`, `listener`, `add_two_ints_server`, a
client every second, domain 5) in one container; `ros2ebpfd:local` in another with `--pid host
--privileged`, no config file. Assertions on `ros2ebpf-ctl metrics`:

- publish / take counters for `/chatter` with `host`, `domain_id=5`, `comm`, growing over 3 s;
- service request and response counters on the server side;
- publish-period histogram populated;
- `/rosout` excluded by default;
- `ros2_process_info` carries the container id, an `rmw_*` implementation and the domain;
- `ros2ebpf_host_info{offset_table="rolling/…"}` (shim discovered through `/proc`);
- `probes_attached == probes_configured`, zero name read failures;
- short-lived client processes are swept (bounded number of process series);
- `disable take` turns the family off and `status -q` stays 0; `enable take` re-attaches 2/2 links;
- counters survive `docker restart` of the daemon (pinned maps).

Last verified run (Ubuntu 24.04 host, kernel 7.0, ROS 2 Rolling demo image on Ubuntu 26.04):

```
PASS talker publishes /chatter in domain 5
PASS listener takes /chatter
PASS service requests on the server side
PASS service responses on the server side
PASS publish period histogram has samples
PASS /rosout excluded
PASS process identity with container id
PASS host info with discovered offset table
PASS probes attached == configured (16)
PASS no name read failures
PASS counter grows (9 -> 12)
PASS exited processes are swept (4 process series)
PASS disable take -> family off
PASS status stays healthy (family no longer requested)
PASS enable take -> 2/2 links
PASS counters survived daemon restart (12 -> 18)
e2e: all checks passed
```

The CLI image was exercised against the same daemon: `ros2 ebpf status -v`, `top --once`,
`doctor`, `config show`, `metrics --grep`, `expectations capture --takers`.

## 3. Host-only check (needs root on a ROS 2 host)

```bash
sudo install/bin/ros2ebpfd --check -c /etc/ros2ebpf/config.yaml
sudo install/bin/ros2ebpfd -c /etc/ros2ebpf/config.yaml --log-level debug &
ros2 run demo_nodes_cpp talker &
install/bin/ros2ebpf-ctl status
curl -s localhost:9477/metrics | grep chatter
```

Expect `ready: 16/16 probe links attached` (one shim) and `ros2_rmw_publish_total{...topic="/chatter"}`.

## 4. Multi-host test plan (manual)

Prerequisites: two or more hosts on one LAN, `ros2ebpfd` on each (package or container), a
collector on one of them, a central Prometheus (the compose bench on a laptop is enough:
`docker compose up -d prometheus alertmanager grafana` and point the collector's
`REMOTE_WRITE_URL` at `http://<laptop>:9090/api/v1/write`).

| # | Step | Expected |
|---|---|---|
| 1 | `ros2ebpf-ctl status -q` on every host | exit 0; `ros2ebpf_host_info` with distinct `host`, correct `distro`/`arch`/`offset_table` |
| 2 | collector scraping all hosts | `up{job="ros2ebpf"}` = 1 per host in central, `robot` label set |
| 3 | talker on host A, listener on host B, same domain | `rate(ros2_rmw_publish_total{host="A"}[1m])` ≈ `rate(ros2_rmw_take_total{host="B"}[1m])` |
| 4 | mixed distro / arch (e.g. Humble on an aarch64 host) | step 3 holds; `offset_table` differs per host; `ros2ebpf_name_read_failures_total` stays 0 |
| 5 | a node in a container on host B | `ros2_process_info{host="B",container!=""}`; counted like any other |
| 6 | `ros2 ebpf status --robot <name>` from the operator machine (fleet.yaml, ssh) | one healthy row per host |
| 7 | expectations with `takers`; stop the listener on B | `Ros2CrossHostLoss` fires within ~3 min; resolves after restart |
| 8 | stop the talker | `Ros2TopicSilent` / `Ros2TopicRateLow` fire; resolve after restart |
| 9 | disconnect host B from the LAN for 3 min | `Ros2ebpfHostDown{host="B"}` fires; no gap in B's series for the time it was still scrapeable |
| 10 | disconnect the collector from central for 10 min | `Ros2ebpfRobotBlind` after 5 min; samples backfill on reconnect (WAL) |
| 11 | `systemctl restart ros2ebpfd` on A while the talker runs | counters continue; `maps_recreated_at_start: false` |
| 12 | two robots with the same domain id and topic names | `Ros2DomainShared` (info) fires; dashboards still separate them by `robot` |
| 13 | upgrade the daemon package on one host | if the map ABI changed: one reset, log line `recreating`; otherwise counters continue |

Record the outcome per row with the Prometheus query used; failures in rows 3–5 point at the
probe/offset layer, 6 at the CLI/transport, 7–10 at rules/collector, 11–13 at packaging.
