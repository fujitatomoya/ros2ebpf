# ros2ebpf

**ROS 2 full observability statistics empowered by eBPF.**

`ros2ebpf` gives you always-on publish / take / service-call rates, drop indicators and
per-process identity for every ROS 2 process on a host, **without touching, rebuilding or
restarting the nodes**. A small daemon (`ros2ebpfd`) attaches eBPF uprobes to the shipped
`librmw_implementation.so`, aggregates in kernel maps, and serves Prometheus metrics. A
`ros2 ebpf` CLI controls it. Alert rules, Grafana dashboards and deployment manifests
(systemd, Docker Compose, Kubernetes) are included.

```
ros2_rmw_publish_total{host="main",robot="amr-07",domain_id="5",comm="talker",topic="/chatter"} 1181
ros2_rmw_take_total{host="perception",robot="amr-07",domain_id="5",comm="listener",topic="/chatter"} 1180
ros2_rmw_service_request_total{host="main",robot="amr-07",domain_id="5",comm="add_two_ints_se",service="/add_two_ints",side="server"} 212
ros2_process_info{host="main",robot="amr-07",comm="talker",pid="4211",domain_id="5",rmw="rmw_fastrtps_cpp",container="",localhost_only="false"} 1
```

- Works against Debian ROS 2 binaries (Humble … Rolling), any RMW, any client library.
- One daemon per Linux host; a robot is a set of hosts; a fleet is a set of robots.
- Nodes in containers are covered automatically (the daemon finds their shim through `/proc`).
- The daemon never computes a rate: counters are exported, Prometheus does the math.
- Design: [docs/design.md](docs/design.md). Metrics: [docs/metrics.md](docs/metrics.md).
  Multi-host / fleet: [docs/deployment.md](docs/deployment.md).

## Contents

1. [How it works](#1-how-it-works)
2. [Repository layout](#2-repository-layout)
3. [Requirements](#3-requirements)
4. [Tutorial A: host-only install (systemd)](#4-tutorial-a-host-only-install-systemd)
5. [Tutorial B: containers (Docker Compose bench)](#5-tutorial-b-containers-docker-compose-bench)
6. [Tutorial C: daemon in a container, nodes anywhere](#6-tutorial-c-daemon-in-a-container-nodes-anywhere)
7. [Configuration](#7-configuration)
8. [The `ros2 ebpf` CLI](#8-the-ros2-ebpf-cli)
9. [Multi-host robots and fleets](#9-multi-host-robots-and-fleets)
10. [Troubleshooting](#10-troubleshooting)
11. [Limitations](#11-limitations)
12. [Development and testing](#12-development-and-testing)

## 1. How it works

```
ROS 2 node ──► librcl.so ──► librmw_implementation.so ──► vendor rmw (Fast DDS, Cyclone, …)
                                   │ uprobe on rmw_publish / rmw_take_with_info / rmw_send_request …
                                   ▼
                        BPF program: 1 hash lookup + atomic add per call
                        first sight of a handle: read topic_name once, announce the pid
                                   │
                        BPF maps pinned in /sys/fs/bpf/ros2ebpf  (counters survive daemon restarts)
                                   │
   ros2ebpfd ──── identity: /proc/<pid>/{comm,environ,cgroup} once per process ──► processes map
       │
       ├── :9477/metrics  (Prometheus text; exporter library, in-process by default)
       └── /run/ros2ebpf/control.sock  (JSON-RPC: ros2 ebpf status / enable / disable / reload / top …)
```

Every `rmw_*` entry point receives a public handle whose first fields are
`{implementation_identifier, data, topic_name|service_name}`. The probe reads the name once per
handle, keys counters by `(pid, handle)`, and the exporter joins them with per-process identity
(`comm`, `domain_id`, container) at scrape time. Per-robot identity (`robot=`) is added by the
collector (Grafana Alloy / Prometheus agent) on `remote_write`.

Components:

| Component | Package | Form |
|---|---|---|
| `ros2ebpfd` daemon + `ros2ebpf-ctl` | `ros2ebpf_daemon` | C, libbpf CO-RE; no ROS dependency; systemd unit or container |
| exporter library + `ros2ebpf-exporter` | `ros2ebpf_exporter` | C; the map → Prometheus transform; standalone binary for `exporter.mode: external` |
| `ros2 ebpf` CLI | `ros2ebpf` | Python `ros2cli` extension |
| control protocol | `ros2ebpf_interfaces` | JSON-RPC schema shared by CLI and daemon |
| Prometheus / Alertmanager / Grafana / Alloy | `deploy/` | upstream images, our config |

## 2. Repository layout

```
ros2ebpf/                 ros2 ebpf CLI (ament_python)
ros2ebpf_daemon/          ros2ebpfd: bpf/ (BPF C), src/ (loader, identity, control), config/, systemd/, offsets/
ros2ebpf_exporter/        exporter library, include/ros2ebpf/maps.h (map contract), ros2ebpf-exporter
ros2ebpf_interfaces/      control protocol schema + README
deploy/docker-compose/    bench: daemon + Alloy + Prometheus + Alertmanager + Grafana (+ demo nodes, + cli)
deploy/k8s/               robot DaemonSet + Alloy; central Prometheus/Alertmanager/Grafana (kustomize, helm values)
deploy/prometheus/        prometheus.yml, rules/ros2.yaml (alerts), alertmanager.yml
deploy/grafana/           provisioning + dashboards: fleet, robot, host, daemon health
docker/                   Dockerfile.build (toolchain), .daemon, .cli, .demo
scripts/e2e.sh            container end-to-end test
docs/                     design.md, metrics.md, deployment.md, development.md, testing.md
```

## 3. Requirements

Probed host (robot computer):

| | Required | Check |
|---|---|---|
| Kernel | Linux ≥ 5.8 with BTF (`CONFIG_DEBUG_INFO_BTF`) — Ubuntu 22.04/24.04 stock kernels qualify | `ls /sys/kernel/btf/vmlinux` |
| bpffs | mounted at `/sys/fs/bpf` (systemd does this) | `mount \| grep bpf` |
| Privileges for `ros2ebpfd` | root, or `CAP_BPF CAP_PERFMON CAP_SYS_PTRACE` (+`CAP_SYS_RESOURCE`) | the unit file sets them |
| ROS 2 | any distro from Humble, installed from Debian packages (`/opt/ros/<distro>`); nothing else from ROS is needed by the daemon | |
| Arch | x86_64 or aarch64 | |

Build host (can be the same machine, or the Docker toolchain image):

- Ubuntu 24.04: `clang-18`, `libbpf-dev` (≥ 1.0), `bpftool` (from `linux-tools-generic`), `cmake`,
  `libyaml-dev`, `libjson-c-dev`, `libelf-dev`, `zlib1g-dev`.
- Ubuntu 22.04 ships libbpf 0.5, which is too old: build on 24.04, use the `ros2ebpfd` container
  image, or build libbpf ≥ 1.3 from source first.

`ros2 ebpf doctor` prints all of this for the host it runs on.

## 4. Tutorial A: host-only install (systemd)

Everything on one machine, no containers: ROS 2 nodes on the host, daemon as a systemd service,
Prometheus scraping it directly. Commands were verified on Ubuntu 24.04 with ROS 2 Jazzy.

### 4.1 Build and install the daemon and exporter

```bash
sudo apt install -y build-essential cmake pkg-config clang-18 llvm-18 libbpf-dev \
     linux-tools-common linux-tools-generic libelf-dev zlib1g-dev libyaml-dev libjson-c-dev
sudo ln -sf /usr/bin/clang-18 /usr/local/bin/clang
# bpftool: the wrapper in linux-tools-common needs a matching kernel package; link a real one
sudo ln -sf "$(ls -d /usr/lib/linux-tools/*/ | head -1)bpftool" /usr/local/bin/bpftool

git clone https://github.com/fujitatomoya/ros2ebpf.git
cd ros2ebpf
make all test            # builds build/exporter and build/daemon, runs the unit tests
sudo make install        # /usr/local/sbin/ros2ebpfd, /usr/local/bin/{ros2ebpf-ctl,ros2ebpf-exporter},
                         # /usr/local/lib/systemd/system/ros2ebpfd.service, /usr/local/etc/ros2ebpf/*.example.yaml
```

Or build a Debian package and install that instead (`make deb`, see
[docs/development.md](docs/development.md)).

### 4.2 Configure

```bash
sudo mkdir -p /etc/ros2ebpf
sudo cp /usr/local/etc/ros2ebpf/config.example.yaml /etc/ros2ebpf/config.yaml
sudo cp /usr/local/etc/ros2ebpf/expectations.example.yaml /etc/ros2ebpf/expectations.yaml
sudoedit /etc/ros2ebpf/config.yaml
```

The minimum to edit is the shim path for your distro:

```yaml
shims:
  - path: /opt/ros/jazzy/lib/librmw_implementation.so
```

Leave `discover_shims: true` (the default) and ROS processes in containers are found too. Set
`robot:` only if you do not run a collector that adds the `robot` label (§9).

Preflight:

```bash
sudo ros2ebpfd --check -c /etc/ros2ebpf/config.yaml      # config parses, shim exports the rmw_* symbols, BTF present
```

### 4.3 Run as a service

```bash
sudo groupadd -f ros2ebpf && sudo usermod -aG ros2ebpf "$USER"   # lets your user talk to the control socket
sudo cp /usr/local/lib/systemd/system/ros2ebpfd.service /etc/systemd/system/
sudo sed -i 's|/usr/sbin/ros2ebpfd|/usr/local/sbin/ros2ebpfd|' /etc/systemd/system/ros2ebpfd.service
sudo systemctl daemon-reload
sudo systemctl enable --now ros2ebpfd
sudo systemctl status ros2ebpfd --no-pager
journalctl -u ros2ebpfd -n 20 --no-pager
```

Expected log lines:

```
I ros2ebpfd 0.1.0 starting: host=main machine_id=… distro=ubuntu-24.04 arch=x86_64 kernel=6.8.0-…
I shim /opt/ros/jazzy/lib/librmw_implementation.so: distro=jazzy arch=x86_64 dev=… ino=… (config)
I probe family publish enabled: 2/2 links
I probe family take enabled: 2/2 links
I probe family services enabled: 6/6 links
I probe family udp_drops enabled
I metrics at http://0.0.0.0:9477/metrics
I control socket at /run/ros2ebpf/control.sock (group ros2ebpf)
I ready: 16/16 probe links attached, 1 shim(s), offset table jazzy/x86_64
```

To run in the foreground instead: `sudo ros2ebpfd -c /etc/ros2ebpf/config.yaml --log-level debug`.

### 4.4 Install the CLI

The CLI is a normal ROS 2 Python package. In a colcon workspace:

```bash
mkdir -p ~/ros2ebpf_ws/src && ln -s "$PWD/ros2ebpf" ~/ros2ebpf_ws/src/ros2ebpf
cd ~/ros2ebpf_ws && source /opt/ros/jazzy/setup.bash && colcon build --packages-select ros2ebpf
source install/setup.bash
ros2 ebpf --help
```

(`pip install ./ros2ebpf` into the system Python also works where that is acceptable.)

### 4.5 Verify with demo nodes

```bash
# log out / in (or `newgrp ros2ebpf`) once so the group membership applies
ros2 ebpf doctor                      # every line PASS/INFO
ros2 ebpf status -v                   # Healthy=yes, Links 16/16

# terminal 2 and 3 (any domain; the daemon reads ROS_DOMAIN_ID per process)
ROS_DOMAIN_ID=5 ros2 run demo_nodes_cpp talker
ROS_DOMAIN_ID=5 ros2 run demo_nodes_cpp listener

ros2 ebpf top --once --interval 3
#  Target  Kind          Name      Domain  Comm      PIDs   Hz    Hz(2nd)  Gaps
#  local   subscription  /chatter  5       listener  12345  1.00  -        0
#  local   publisher     /chatter  5       talker    12344  1.00  -        -

curl -s localhost:9477/metrics | grep -E '^ros2_(rmw_publish_total|rmw_take_total|process_info)'
```

Nodes that were already running before the daemon started are counted from their next call: the
uprobe lives on the library's inode, not on a process.

### 4.6 Scrape it

Smallest possible pipeline on the same host (Prometheus binary or container):

```yaml
# prometheus.yml
scrape_configs:
  - job_name: ros2ebpf
    honor_labels: true
    static_configs:
      - targets: [localhost:9477]
        labels: {robot: bench-01}
rule_files: [/path/to/ros2ebpf/deploy/prometheus/rules/ros2.yaml]
```

For the real per-robot collector + central server layout see §9 and
[docs/deployment.md](docs/deployment.md).

### 4.7 Day-2 operations

```bash
ros2 ebpf config set probes.seq_gap '[/chatter]'   # edits config.yaml, then reloads the daemon
ros2 ebpf config set topics.exclude '[/parameter_events, /rosout, /tf]'
ros2 ebpf disable udp_drops                          # detach a family at runtime, counters kept
ros2 ebpf expectations capture --duration 60 --takers -o /tmp/expectations.yaml   # then copy to /etc/ros2ebpf
sudo systemctl reload ros2ebpfd                      # same as `ros2 ebpf reload`
sudo systemctl restart ros2ebpfd                     # counters survive: maps are pinned in bpffs
sudo rm -rf /sys/fs/bpf/ros2ebpf && sudo systemctl restart ros2ebpfd   # reset counters
```

## 5. Tutorial B: containers (Docker Compose bench)

Everything in containers on one machine: daemon, collector (Alloy), Prometheus, Alertmanager,
Grafana, optional demo nodes, and the CLI. Useful to see the whole pipeline and the dashboards.
ROS 2 processes **on the host** are observed too (the daemon container shares the host pid
namespace).

```bash
cd deploy/docker-compose
cp ros2ebpf/config.example.yaml ros2ebpf/config.yaml            # optional; without it discovery defaults are used
cp ros2ebpf/expectations.example.yaml ros2ebpf/expectations.yaml

docker compose up -d --build                                   # daemon, alloy, prometheus, alertmanager, grafana
docker compose --profile demo up -d --build demo                # talker + listener + add_two_ints in a container
docker compose --profile tools run --rm cli ros2 ebpf status -v # CLI through the shared control socket
docker compose --profile tools run --rm cli ros2 ebpf top --once --interval 3
docker compose logs ros2ebpfd | tail
```

Then:

- Grafana: http://localhost:3000 (admin / admin) → folder "ROS 2" → "ROS 2 fleet", "ROS 2 robot"
  (robot = `bench-01`), "ROS 2 host", "ros2ebpf daemon health".
- Prometheus: http://localhost:9090/alerts → `Ros2ebpfProbesMissing`, `Ros2ebpfHostDown` inactive;
  stop the demo (`docker compose --profile demo stop demo`) and `Ros2TopicSilent` / `Ros2TopicRateLow`
  for `/chatter` fire within ~2 minutes (the example `expectations.yaml` expects 1 Hz).
- Metrics: `docker compose exec ros2ebpfd ros2ebpf-ctl metrics | grep chatter`.

What the compose file gives the daemon container, and why (copy this for any other orchestrator):

| Setting | Why |
|---|---|
| `pid: host` | see host and container processes in `/proc`; attach through `/proc/<pid>/root/…` |
| `privileged: true` (or `cap_add: [BPF, PERFMON, SYS_PTRACE, SYS_RESOURCE, SYS_ADMIN]`) | load BPF, attach uprobes, read `environ` |
| `/sys/fs/bpf` (rw) | pinned maps |
| `/sys/kernel/btf` (ro) | CO-RE relocations and `tp_btf` tracepoints |
| `/etc/machine-id` (ro) | `machine_id` label |
| `./ros2ebpf:/etc/ros2ebpf` (ro) | config.yaml / expectations.yaml |
| `ros2ebpf-run:/run/ros2ebpf` volume | control socket shared with the `cli` service |
| `ROS2EBPF_HOST` env | the `host` label (container hostnames are random) |

No tracefs mount is needed (the tracepoints are BTF-typed). No `/opt/ros` mount is needed unless you
list static `shims:` for a ROS install on the host; discovery finds them anyway.

Tear down: `docker compose --profile demo --profile tools down -v`.

## 6. Tutorial C: daemon in a container, nodes anywhere

This is the "prove it attaches from outside" case, and what `scripts/e2e.sh` automates. The
daemon runs in its own container and observes ROS 2 nodes that run (a) in another container and
(b) on the host.

```bash
make images                                   # ros2ebpfd:local, ros2ebpf-cli:local, ros2ebpf-demo:local

# (a) ROS 2 nodes in their own container (Jazzy, domain 5)
docker run -d --name demo --network host -e ROS_DOMAIN_ID=5 ros2ebpf-demo:local

# the daemon, with no config file at all: discovery finds the demo container's shim
docker run -d --name ros2ebpfd --pid host --privileged \
  -v /sys/fs/bpf:/sys/fs/bpf -v /sys/kernel/btf:/sys/kernel/btf:ro -v /etc/machine-id:/etc/machine-id:ro \
  -v ros2ebpf-run:/run/ros2ebpf -p 9477:9477 -e ROS2EBPF_HOST=$(hostname) ros2ebpfd:local

docker logs ros2ebpfd
#  I shim /proc/431/root/opt/ros/jazzy/lib/librmw_implementation.so: distro=jazzy arch=x86_64 … (discovered)
#  I ready: 16/16 probe links attached, 1 shim(s), offset table jazzy/x86_64
#  I pid 4312: comm=talker domain_id=5 rmw=- container=fb6f819f393f

# (b) a node on the host, if ROS 2 is installed there: found the same way within 5 s
ROS_DOMAIN_ID=7 ros2 run demo_nodes_cpp talker &

docker run --rm -v ros2ebpf-run:/run/ros2ebpf ros2ebpf-cli:local ros2 ebpf status -v
docker run --rm -v ros2ebpf-run:/run/ros2ebpf ros2ebpf-cli:local ros2 ebpf top --once
docker exec ros2ebpfd ros2ebpf-ctl metrics | grep -E 'ros2_(rmw_publish_total|process_info)'
```

The `container` label of `ros2_process_info` carries the container id, `domain_id` the per-process
`ROS_DOMAIN_ID`, and the shim entry in `status` shows `discovered`. Two containers from the same
image share one inode and therefore one set of probes; no double counting.

`./scripts/e2e.sh` runs this scenario and asserts on the metrics (counters present and growing,
`/rosout` excluded, identity labels, exit sweeps, enable/disable, counters surviving a daemon restart).

## 7. Configuration

### 7.1 `/etc/ros2ebpf/config.yaml` (per host)

The annotated reference is [ros2ebpf_daemon/config/config.example.yaml](ros2ebpf_daemon/config/config.example.yaml).
Keys marked *restart* are read at start only; everything else is applied by `ros2 ebpf reload`
(or `systemctl reload ros2ebpfd`, or SIGHUP).

| Key | Default | Meaning |
|---|---|---|
| `host` | kernel hostname | `host` label on every series (*restart*; env `ROS2EBPF_HOST` also works) |
| `robot` | `""` | fallback `robot` label; normally the collector adds it (env `ROS2EBPF_ROBOT`) |
| `shims[].path` | — | `librmw_implementation.so` to probe; `distro`/`arch` optional (derived) (*restart*) |
| `discover_shims` | `true` | scan `/proc/<pid>/maps` every 5 s for shims in containers |
| `identity.read_environ` | `true` | read `ROS_DOMAIN_ID`, `RMW_IMPLEMENTATION`, `ROS_LOCALHOST_ONLY`; needs `CAP_SYS_PTRACE` |
| `identity.default_domain_id` | `0` | when the variable is unset |
| `probes.publish/take/services/udp_drops` | `true` | probe families |
| `probes.seq_gap` | `[]` | topics with sequence-gap tracking on take, or `all` |
| `topics.exclude` | `[/parameter_events, /rosout]` | never counted (an explicit `[]` clears the default) |
| `histogram_buckets_seconds` | 0.5 ms … 1 s (10 bounds) | inter-publish interval histogram, ≤ 15 bounds |
| `exporter.mode` | `inprocess` | `external` disables the built-in `/metrics`; run `ros2ebpf-exporter` (*restart*) |
| `exporter.listen` | `0.0.0.0:9477` | (*restart*) |
| `exporter.rmw_label` | `false` | add `rmw="…"` to endpoint series |
| `control.socket` | `/run/ros2ebpf/control.sock` | (*restart*) |
| `control.http` | `""` | e.g. `0.0.0.0:9478`: unauthenticated JSON-RPC over HTTP for `ros2 ebpf --host h:9478` (*restart*) |
| `expectations_file` | `/etc/ros2ebpf/expectations.yaml` | exported as `ros2_topic_expected_*` if present |
| `pin_root` | `/sys/fs/bpf/ros2ebpf` | (*restart*) |
| `offset_table` | derived | force a `distro/arch` row of the struct offset table |
| `log_level` | `info` | `error`, `warn`, `info`, `debug` |

### 7.2 `/etc/ros2ebpf/expectations.yaml` (per robot)

```yaml
/odom: 50                                 # expected publish rate, Hz
/lidar/points: {hz: 10, takers: 2}        # takers: endpoints expected to take it (cross-host loss rule)
/imu/data:
  hz: 200
  takers: 1
```

A property of the robot, not the host: serve it from one host of the robot (the one running the
collector) and leave `expectations_file` pointing at a missing file on the others.
`ros2 ebpf expectations capture --duration 60 --takers` writes a starting point from observed rates.

### 7.3 `/etc/ros2ebpf/fleet.yaml` (CLI only)

```yaml
robots:
  amr-07:
    hosts:
      - main                                                      # ssh target
      - {name: perception, address: 192.168.7.12, transport: http, port: 9478}
      - {name: arm, address: ops@arm.local, transport: ssh}
```

`ros2 ebpf status --robot amr-07` then prints one row per host.

## 8. The `ros2 ebpf` CLI

Global options: `--socket PATH` (local), `--host H` (repeatable: `user@host` via ssh, `host:9478`
via HTTP, `unix:///path`), `--robot R` with `--fleet-file`, `--format table|json`.

| Verb | What it does |
|---|---|
| `status [-v] [-q]` | health table; exit 1 if any configured family is not fully attached (readiness check) |
| `enable <family>` / `disable <family>` | attach / detach `publish`, `take`, `services`, `udp_drops`; counters kept |
| `reload` | re-read config.yaml + expectations.yaml, no detach |
| `top [--topic T] [--domain D] [--kind K] [--interval S] [--once]` | live rates from two map reads (bench tool) |
| `config show \| get KEY \| set KEY VALUE [--no-reload] \| keys` | edit the config file with validation, then reload |
| `expectations capture [--duration S] [--takers] [-o FILE] \| show` | write expectations.yaml from observed rates |
| `doctor [--config F] [--shim P]` | kernel, BTF, bpffs, capabilities, shim symbols, daemon, exporter |
| `metrics [--grep S]` | the exposition, through the control socket (works in `external` mode too) |

Without ROS on a host, `ros2ebpf-ctl status|enable|disable|reload|snapshot|config|metrics` does the
same over the socket (`-q` for exit-code-only), and `ros2ebpf-ctl rpc` is the ssh transport the CLI uses.

## 9. Multi-host robots and fleets

Short version; the full procedure with verification steps is in
[docs/deployment.md](docs/deployment.md).

1. On **every host** of the robot: install and start `ros2ebpfd` (§4). Set `host:` only if the
   kernel hostname is not meaningful.
2. On **one host** of the robot (usually the main computer): run the collector
   ([deploy/docker-compose/alloy/config.alloy](deploy/docker-compose/alloy/config.alloy) or a
   Prometheus agent) scraping `host1:9477, host2:9477, …` with `honor_labels: true`, a local WAL,
   and `remote_write` to central with `external_labels: {robot: <name>}`. Put `expectations.yaml`
   on this host only.
3. **Central**: Prometheus with `--web.enable-remote-write-receiver` (or Mimir), the rules in
   `deploy/prometheus/rules/ros2.yaml`, Alertmanager, Grafana with `deploy/grafana/`. Compose bench:
   `deploy/docker-compose`; Kubernetes: `deploy/k8s/central/`.
4. Operator shell: `fleet.yaml` (§7.3) and `ros2 ebpf status --robot amr-07`.

Ports: `9477/tcp` metrics (robot LAN only), `9478/tcp` optional control HTTP (robot LAN only, no
auth), `remote_write` outbound from the collector to central (TLS + per-robot token at the central
ingress).

## 10. Troubleshooting

Run `ros2 ebpf doctor` first. Then:

| Symptom | Cause / fix |
|---|---|
| `failed to load BPF objects` / `libbpf: … BTF` | kernel without BTF or < 5.8; `ls /sys/kernel/btf/vmlinux` |
| `cannot create pin root /sys/fs/bpf/ros2ebpf` | bpffs not mounted: `sudo mount -t bpf bpf /sys/fs/bpf` |
| `attach … failed: Operation not permitted` | missing `CAP_BPF`/`CAP_PERFMON` (or `CAP_SYS_ADMIN` on < 5.8); container not privileged |
| `no librmw_implementation.so found yet` | no ROS process running and no `shims:` entry; harmless, discovery keeps scanning |
| `domain_id="unknown"` | `/proc/<pid>/environ` unreadable: add `CAP_SYS_PTRACE`, or the process runs in a different user namespace |
| `ros2ebpf_name_read_failures_total` grows, `ros2ebpf_unknown_rmw_processes > 0` | struct offsets differ from the table (from-source ROS build?): run `ros2ebpf_daemon/offsets/gen.sh /opt/ros/<distro>` and set `offset_table:` / add a row |
| `shim … is not a usable librmw_implementation.so: missing …` | wrong path (vendor library instead of the shim?) |
| `ros2 ebpf status`: `cannot connect … Permission denied` | add your user to group `ros2ebpf`, or use `sudo` |
| `pinned maps have ABI N … recreating` | upgrade changed the map layout; counters reset once |
| counters for a one-shot client never appear | endpoint counters are deleted with the endpoint; look at the server side (`side="server"`) |
| exporter check fails in `doctor` inside the CLI container | expected: `:9477` belongs to the daemon's network namespace; check from the host |
| many `pid … exited: removed 0 endpoints` | normal: `rmw_destroy_*` already removed them at clean shutdown |

Debug logging: `ros2 ebpf config set log_level debug` (or `--log-level debug` in the foreground).

## 11. Limitations

- Intra-process publishes (all subscribers in the same process) bypass `rmw_publish` and are not
  counted. Phase 2: `rclcpp_intra_publish` tracepoint.
- Sequence-gap tracking needs an RMW that fills `publication_sequence_number` (Fast DDS, Connext)
  and is exact only for single-publisher topics.
- Cross-host loss is a rate inequality over a window, not a per-message count (no shared clock).
- `domain_id` comes from the environment only; `rcl_init` options are not visible.
- Linux only; x86_64 and aarch64; struct offsets are ABI (table keyed by distro/arch, checked at runtime).
- Endpoint counters live with the endpoint: a destroyed publisher's series disappears.

Details and the rationale behind each decision: [docs/design.md](docs/design.md).

## 12. Development and testing

- [docs/development.md](docs/development.md): building natively or in the toolchain container,
  code layout, the map ABI contract, adding probes, offsets, Debian packaging, CI.
- [docs/testing.md](docs/testing.md): unit tests (C and Python), the container end-to-end test,
  and the manual multi-host test plan.

```bash
make build-image in-docker-test     # build + all unit tests inside the toolchain container (nothing on the host)
make images e2e                     # container end-to-end test (needs Docker, a BTF kernel, bpffs)
make lint                           # promtool / amtool / kustomize / compose validation of deploy/
```

License: Apache-2.0.
