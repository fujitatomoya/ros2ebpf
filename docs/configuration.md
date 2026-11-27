# Configuration

Three YAML files, all optional beyond the shim path.
`/etc/ros2ebpf/config.yaml` is read by the daemon and belongs to one host.
`/etc/ros2ebpf/expectations.yaml` describes expected rates and belongs to one robot.
`/etc/ros2ebpf/fleet.yaml` lists the hosts of each robot and is read by the `ros2 ebpf` CLI only.

## `/etc/ros2ebpf/config.yaml` (per host)

The annotated reference is [ros2ebpf_daemon/config/config.example.yaml](../ros2ebpf_daemon/config/config.example.yaml); the colcon build installs it as `install/ros2ebpf_daemon/etc/ros2ebpf/config.example.yaml`.
Keys marked *restart* are read at start only; everything else is applied by `ros2 ebpf reload` (or SIGHUP).

The one key without a usable default is the shim path: the `librmw_implementation.so` the daemon probes.
For a Rolling source build it is `<workspace>/install/rmw_implementation/lib/librmw_implementation.so`, as seen from where the daemon runs; a shim outside `/opt/ros/` is taken for Rolling, a shim under `/opt/ros/<other distro>/` is refused.
Leave `discover_shims: true` (the default) and ROS processes in other containers are found too.
Set `robot:` only if you do not run a collector that adds the `robot` label ([deployment.md](deployment.md)).

| Key | Default | Meaning |
|---|---|---|
| `host` | kernel hostname | `host` label on every series (*restart*; env `ROS2EBPF_HOST` also works) |
| `robot` | `""` | fallback `robot` label; normally the collector adds it (env `ROS2EBPF_ROBOT`) |
| `shims[].path` | — | `librmw_implementation.so` to probe (*restart*): `/opt/ros/rolling/lib/…` or the one of a Rolling source build; `arch` optional (`uname -m`) |
| `discover_shims` | `true` | scan `/proc/<pid>/maps` every 5 s for shims in containers |
| `identity.read_environ` | `true` | read `ROS_DOMAIN_ID`, `RMW_IMPLEMENTATION`, `ROS_LOCALHOST_ONLY`; needs `CAP_SYS_PTRACE` |
| `identity.default_domain_id` | `0` | when the variable is unset |
| `probes.publish/take/services/udp_drops` | `true` | probe families |
| `probes.seq_gap` | `[]` | topics with sequence-gap tracking on take, or `all` |
| `topics.exclude` | `[/parameter_events, /rosout]` | never counted (an explicit `[]` clears the default) |
| `histogram_buckets_seconds` | 0.95, 0.98, 1.02, 1.05, 1.1, 1.25 × 10 ms, 100 ms and 1 s; ±2 % around 20, 33.3, 50, 200, 500 ms and 2 s; 5 s (31 bounds) | inter-publish interval histogram, ≤ 31 bounds; for a topic with another rate, replace some with bounds around its period |
| `exporter.mode` | `inprocess` | `external` disables the built-in `/metrics`; run `ros2ebpf-exporter` (*restart*) |
| `exporter.listen` | `0.0.0.0:9477` | (*restart*) |
| `exporter.rmw_label` | `false` | add `rmw="…"` to endpoint series |
| `control.socket` | `/run/ros2ebpf/control.sock` | (*restart*) |
| `control.http` | `""` | e.g. `0.0.0.0:9478`: unauthenticated JSON-RPC over HTTP for `ros2 ebpf --host h:9478` (*restart*) |
| `expectations_file` | `/etc/ros2ebpf/expectations.yaml` | exported as `ros2_topic_expected_*` if present |
| `pin_root` | `/sys/fs/bpf/ros2ebpf` | (*restart*) |
| `log_level` | `info` | `error`, `warn`, `info`, `debug` |

`ros2 ebpf config show | get KEY | set KEY VALUE | keys` edits this file with validation and reloads the daemon ([cli.md](cli.md#day-2-operations)).

## `/etc/ros2ebpf/expectations.yaml` (per robot)

```yaml
/odom: 50                                 # expected publish rate, Hz
/lidar/points: {hz: 10, takers: 2}        # takers: endpoints expected to take it (cross-host loss rule)
/imu/data:
  hz: 200
  takers: 1
```

A property of the robot, not the host: serve it from one host of the robot (the one running the collector) and leave `expectations_file` pointing at a missing file on the others.
`ros2 ebpf expectations capture --duration 60 --takers` writes a starting point from observed rates.
The alert rules in `deploy/prometheus/rules/ros2.yaml` compare `ros2_topic_expected_hz` and `ros2_topic_expected_takers` with the observed rates ([metrics.md](metrics.md)).

## `/etc/ros2ebpf/fleet.yaml` (CLI only)

```yaml
robots:
  amr-07:
    hosts:
      - main                                                      # ssh target
      - {name: perception, address: 192.168.7.12, transport: http, port: 9478}
      - {name: arm, address: ops@arm.local, transport: ssh}
```

`ros2 ebpf status --robot amr-07` then prints one row per host; `--fleet-file` selects another file.
The transports are those of `--host` ([cli.md](cli.md#global-options)).
