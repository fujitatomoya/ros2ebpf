# ros2ebpf

**ROS 2 full observability statistics empowered by eBPF.**

`ros2ebpf` gives you publish / take / service-call rates, drop indicators and per-process identity for every ROS 2 process on a host, **without touching, rebuilding or restarting the nodes**.
A small daemon (`ros2ebpfd`) attaches eBPF uprobes to the shipped `librmw_implementation.so`, aggregates in kernel maps, and serves Prometheus metrics.
It runs on demand: `ros2 ebpf start` when you want to look at a robot, `ros2 ebpf stop` when you are done, the same way `ros2 daemon` is started by request.
Alert rules, Grafana dashboards and a Kubernetes DaemonSet for whole-cluster statistics are included.

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
- Design: [docs/design.md](docs/design.md).
  Metrics: [docs/metrics.md](docs/metrics.md).
  Multi-host / fleet: [docs/deployment.md](docs/deployment.md).

## Contents

1. [How it works](#1-how-it-works)
2. [Repository layout](#2-repository-layout)
3. [Requirements](#3-requirements)
4. [Tutorial A: host-only install](#4-tutorial-a-host-only-install)
5. [Tutorial B: daemon in a container, nodes anywhere](#5-tutorial-b-daemon-in-a-container-nodes-anywhere)
6. [Configuration](#6-configuration)
7. [The `ros2 ebpf` CLI](#7-the-ros2-ebpf-cli)
8. [Multi-host robots and fleets](#8-multi-host-robots-and-fleets)
9. [Troubleshooting](#9-troubleshooting)
10. [Limitations](#10-limitations)
11. [Development and testing](#11-development-and-testing)

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

Every `rmw_*` entry point receives a public handle whose first fields are `{implementation_identifier, data, topic_name|service_name}`.
The probe reads the name once per handle, keys counters by `(pid, handle)`, and the exporter joins them with per-process identity (`comm`, `domain_id`, container) at scrape time.
Per-robot identity (`robot=`) is added by the collector (Grafana Alloy / Prometheus agent) on `remote_write`.

Components:

| Component | Package | Form |
|---|---|---|
| `ros2ebpfd` daemon + `ros2ebpf-ctl` | `ros2ebpf_daemon` | C, libbpf CO-RE; no ROS dependency; started on demand by the CLI, or a container (Kubernetes DaemonSet) |
| exporter library + `ros2ebpf-exporter` | `ros2ebpf_exporter` | C; the map → Prometheus transform; standalone binary for `exporter.mode: external` |
| `ros2 ebpf` CLI | `ros2ebpf` | Python `ros2cli` extension |
| control protocol | `ros2ebpf_interfaces` | JSON-RPC schema shared by CLI and daemon |
| Prometheus / Alertmanager / Grafana / Alloy | `deploy/` | upstream images, our config |

## 2. Repository layout

```
ros2ebpf/                 ros2 ebpf CLI (ament_python)
ros2ebpf_daemon/          ros2ebpfd (ament_cmake): bpf/ (BPF C), src/ (loader, identity, control), config/, offsets/
ros2ebpf_exporter/        exporter library (ament_cmake), include/ros2ebpf/maps.h (map contract), ros2ebpf-exporter
ros2ebpf_interfaces/      control protocol schema + README (ament_cmake)
deploy/k8s/               robot DaemonSet + Alloy; central Prometheus/Alertmanager/Grafana (kustomize, helm values)
deploy/alloy/             collector configuration for a robot host (Grafana Alloy)
deploy/prometheus/        prometheus.yml, rules/ros2.yaml (alerts), alertmanager.yml
deploy/grafana/           provisioning + dashboards: fleet, robot, host, daemon health
docker/                   Dockerfile.daemon, .cli, .demo (deployment images, rolling binaries)
scripts/github_workflows.sh   CI: rolling source build, unit tests, end-to-end (runs locally in the ros2dev container too)
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

- ros2ebpf follows **ROS 2 Rolling** only: it is built and tested against Rolling (CI builds mainline from source), and a distro branch is created when the next release is branched.
  The build needs `ament_cmake` and `colcon` from a Rolling install (binary or source); the packages are built like any other ROS 2 workspace.
  The daemon and exporter still do not use ROS at runtime.
- Ubuntu 26.04: `clang`, `llvm`, `libbpf-dev` (≥ 1.0), `bpftool`, `cmake`, `libyaml-dev`, `libjson-c-dev`, `libelf-dev`, `zlib1g-dev`.
- Ubuntu 22.04 ships libbpf 0.5, which is too old: build on 24.04 or newer, use the `ros2ebpfd` container image, or build libbpf ≥ 1.3 from source first.

`ros2 ebpf doctor` prints all of this for the host it runs on.

## 4. Tutorial A: host-only install

Everything on one machine, no containers: ROS 2 nodes on the host, the daemon started from the CLI when you want statistics, Prometheus scraping it directly.
The commands target Ubuntu 26.04 with ROS 2 Rolling binaries.

### 4.1 Build and install the daemon and exporter

```bash
sudo apt install -y build-essential cmake pkg-config clang llvm libbpf-dev bpftool \
     libelf-dev zlib1g-dev libyaml-dev libjson-c-dev \
     ros-rolling-ament-cmake python3-colcon-common-extensions

git clone https://github.com/fujitatomoya/ros2ebpf.git
cd ros2ebpf
source /opt/ros/rolling/setup.bash
colcon build --merge-install --packages-up-to ros2ebpf_daemon
colcon test --merge-install && colcon test-result --verbose
# install/bin/{ros2ebpfd,ros2ebpf-ctl,ros2ebpf-exporter}, install/etc/ros2ebpf/*.example.yaml
```

The repository is a colcon workspace on its own; it can equally be cloned into `src/` of an existing one.
Binary packages come from the ROS build farm like for any ROS 2 package once the packages are released there (`ros-rolling-ros2ebpf-daemon`); until then the workspace `install/` is what runs on the host.

### 4.2 Configure

```bash
sudo mkdir -p /etc/ros2ebpf
sudo cp install/etc/ros2ebpf/config.example.yaml /etc/ros2ebpf/config.yaml
sudo cp install/etc/ros2ebpf/expectations.example.yaml /etc/ros2ebpf/expectations.yaml
sudoedit /etc/ros2ebpf/config.yaml
```

The minimum to edit is the shim path for your distro:

```yaml
shims:
  - path: /opt/ros/rolling/lib/librmw_implementation.so
```

Leave `discover_shims: true` (the default) and ROS processes in containers are found too.
Set `robot:` only if you do not run a collector that adds the `robot` label (§8).

Preflight:

```bash
sudo install/bin/ros2ebpfd --check -c /etc/ros2ebpf/config.yaml      # config parses, shim exports the rmw_* symbols, BTF present
```

### 4.3 Start it when you need it

`ros2ebpfd` is not a service that runs all the time.
Every probed call costs a little in the kernel, so you start the daemon when you want to look at the system (something is off on the network, a robot misbehaves) and stop it when you are done, the way `ros2 daemon` is started on request.
`ros2 ebpf start` spawns it detached from your shell, through `sudo -n` when you are not root (it needs `CAP_BPF` and friends), waits until it answers on the control socket and prints a summary; `ros2 ebpf stop` asks it to exit.
The maps stay pinned in bpffs, so counters continue across stop and start.

```bash
sudo -v                 # once: `ros2 ebpf start` uses `sudo -n` (or add a NOPASSWD sudoers line for install/bin/ros2ebpfd)
ros2 ebpf start         # -c /etc/ros2ebpf/config.yaml; output in ~/.ros/log/ros2ebpfd.log
ros2 ebpf status
ros2 ebpf stop
```

The daemon hands its control socket to the user who ran `sudo` (or to the `ros2ebpf` group when that group exists: `sudo groupadd -f ros2ebpf && sudo usermod -aG ros2ebpf "$USER"` lets a whole team talk to it).
`ros2 ebpf start -c FILE --log-level debug --log-file FILE` selects the config, the verbosity and where the output goes; `--no-sudo` when the binary already has the capabilities.

Expected log lines (`tail ~/.ros/log/ros2ebpfd.log`):

```
I ros2ebpfd 0.1.0 starting: host=main machine_id=… distro=ubuntu-26.04 arch=x86_64 kernel=7.0.0-…
I shim /opt/ros/rolling/lib/librmw_implementation.so: distro=rolling arch=x86_64 dev=… ino=… (config)
I probe family publish enabled: 2/2 links
I probe family take enabled: 2/2 links
I probe family services enabled: 6/6 links
I probe family udp_drops enabled
I metrics at http://0.0.0.0:9477/metrics
I control socket at /run/ros2ebpf/control.sock (group ros2ebpf)
I ready: 16/16 probe links attached, 1 shim(s), offset table rolling/x86_64
```

To run in the foreground instead: `sudo install/bin/ros2ebpfd -c /etc/ros2ebpf/config.yaml --log-level debug` (Ctrl-C stops it; `ros2 ebpf stop` works on it too).

### 4.4 Install the CLI

The CLI is a normal ROS 2 Python package in the same workspace:

```bash
colcon build --merge-install --packages-select ros2ebpf
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

Nodes that were already running before the daemon started are counted from their next call: the uprobe lives on the library's inode, not on a process.

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

For the real per-robot collector + central server layout see §8 and [docs/deployment.md](docs/deployment.md).

### 4.7 Day-2 operations

```bash
ros2 ebpf config set probes.seq_gap '[/chatter]'   # edits config.yaml, then reloads the daemon
ros2 ebpf config set topics.exclude '[/parameter_events, /rosout, /tf]'
ros2 ebpf disable udp_drops                          # detach a family at runtime, counters kept
ros2 ebpf expectations capture --duration 60 --takers -o /tmp/expectations.yaml   # then copy to /etc/ros2ebpf
ros2 ebpf reload                                     # re-read config.yaml / expectations.yaml (or SIGHUP)
ros2 ebpf stop && ros2 ebpf start                    # counters survive: maps are pinned in bpffs
ros2 ebpf stop && sudo rm -rf /sys/fs/bpf/ros2ebpf && ros2 ebpf start   # reset counters
```

## 5. Tutorial B: daemon in a container, nodes anywhere

This is the "prove it attaches from outside" case, and what the `images` job of CI checks on every commit.
The daemon runs in its own container and observes ROS 2 nodes that run (a) in another container and (b) on the host.

```bash
docker build -f docker/Dockerfile.daemon -t ros2ebpfd:local .     # the images, on rolling binaries
docker build -f docker/Dockerfile.cli -t ros2ebpf-cli:local .
docker build -f docker/Dockerfile.demo -t ros2ebpf-demo:local .

# (a) ROS 2 nodes in their own container (Rolling, domain 5)
docker run -d --name demo --network host -e ROS_DOMAIN_ID=5 ros2ebpf-demo:local

# the daemon, with no config file at all: discovery finds the demo container's shim
docker run -d --name ros2ebpfd --pid host --privileged \
  -v /sys/fs/bpf:/sys/fs/bpf -v /sys/kernel/btf:/sys/kernel/btf:ro -v /etc/machine-id:/etc/machine-id:ro \
  -v ros2ebpf-run:/run/ros2ebpf -p 9477:9477 -e ROS2EBPF_HOST=$(hostname) ros2ebpfd:local

docker logs ros2ebpfd
#  I shim /proc/431/root/opt/ros/rolling/lib/librmw_implementation.so: distro=rolling arch=x86_64 … (discovered)
#  I ready: 16/16 probe links attached, 1 shim(s), offset table rolling/x86_64
#  I pid 4312: comm=talker domain_id=5 rmw=- container=fb6f819f393f

# (b) a node on the host, if ROS 2 is installed there: found the same way within 5 s
ROS_DOMAIN_ID=7 ros2 run demo_nodes_cpp talker &

docker run --rm -v ros2ebpf-run:/run/ros2ebpf ros2ebpf-cli:local ros2 ebpf status -v
docker run --rm -v ros2ebpf-run:/run/ros2ebpf ros2ebpf-cli:local ros2 ebpf top --once
docker exec ros2ebpfd ros2ebpf-ctl metrics | grep -E 'ros2_(rmw_publish_total|process_info)'
```

The `container` label of `ros2_process_info` carries the container id, `domain_id` the per-process `ROS_DOMAIN_ID`, and the shim entry in `status` shows `discovered`.
Two containers from the same image share one inode and therefore one set of probes; no double counting.

CI runs this scenario on every commit (`images` job); the full end-to-end assertions (counters present and growing, `/rosout` excluded, identity labels, exit sweeps, enable/disable, counters surviving a daemon restart) run in the `rolling` job against a mainline source build, see [docs/testing.md](docs/testing.md).

## 6. Configuration

### 6.1 `/etc/ros2ebpf/config.yaml` (per host)

The annotated reference is [ros2ebpf_daemon/config/config.example.yaml](ros2ebpf_daemon/config/config.example.yaml).
Keys marked *restart* are read at start only; everything else is applied by `ros2 ebpf reload` (or SIGHUP).

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

### 6.2 `/etc/ros2ebpf/expectations.yaml` (per robot)

```yaml
/odom: 50                                 # expected publish rate, Hz
/lidar/points: {hz: 10, takers: 2}        # takers: endpoints expected to take it (cross-host loss rule)
/imu/data:
  hz: 200
  takers: 1
```

A property of the robot, not the host: serve it from one host of the robot (the one running the collector) and leave `expectations_file` pointing at a missing file on the others.
`ros2 ebpf expectations capture --duration 60 --takers` writes a starting point from observed rates.

### 6.3 `/etc/ros2ebpf/fleet.yaml` (CLI only)

```yaml
robots:
  amr-07:
    hosts:
      - main                                                      # ssh target
      - {name: perception, address: 192.168.7.12, transport: http, port: 9478}
      - {name: arm, address: ops@arm.local, transport: ssh}
```

`ros2 ebpf status --robot amr-07` then prints one row per host.

## 7. The `ros2 ebpf` CLI

Global options: `--socket PATH` (local), `--host H` (repeatable: `user@host` via ssh, `host:9478` via HTTP, `unix:///path`), `--robot R` with `--fleet-file`, `--format table|json`.

| Verb | What it does |
|---|---|
| `start [-c FILE] [--log-level L] [--log-file FILE] [--timeout S] [--no-sudo] [--binary P]` | spawn `ros2ebpfd` detached (via `sudo -n` when not root), wait until it answers, print the summary; exit 1 if not healthy |
| `stop [--timeout S]` | ask the daemon to exit (counters stay in the pinned maps); exit 0 when it was not running |
| `status [-v] [-q]` | health table; exit 1 if any configured family is not fully attached (readiness check) |
| `enable <family>` / `disable <family>` | attach / detach `publish`, `take`, `services`, `udp_drops`; counters kept |
| `reload` | re-read config.yaml + expectations.yaml, no detach |
| `top [--topic T] [--domain D] [--kind K] [--interval S] [--once]` | live rates from two map reads (bench tool) |
| `config show \| get KEY \| set KEY VALUE [--no-reload] \| keys` | edit the config file with validation, then reload |
| `expectations capture [--duration S] [--takers] [-o FILE] \| show` | write expectations.yaml from observed rates |
| `doctor [--config F] [--shim P]` | kernel, BTF, bpffs, capabilities, shim symbols, daemon, exporter |
| `metrics [--grep S]` | the exposition, through the control socket (works in `external` mode too) |

Without ROS on a host, `ros2ebpf-ctl status|enable|disable|reload|snapshot|config|metrics` does the same over the socket (`-q` for exit-code-only), and `ros2ebpf-ctl rpc` is the ssh transport the CLI uses.

## 8. Multi-host robots and fleets

Short version; the full procedure with verification steps is in [docs/deployment.md](docs/deployment.md).

1. On **every host** of the robot: install `ros2ebpfd` (§4) and start it while you want statistics (`ros2 ebpf start`, or the DaemonSet on a cluster).
   Set `host:` only if the kernel hostname is not meaningful.
2. On **one host** of the robot (usually the main computer): run the collector ([deploy/alloy/config.alloy](deploy/alloy/config.alloy) or a Prometheus agent) scraping `host1:9477, host2:9477, …` with `honor_labels: true`, a local WAL, and `remote_write` to central with `external_labels: {robot: <name>}`.
   Put `expectations.yaml` on this host only.
3. **Central**: Prometheus with `--web.enable-remote-write-receiver` (or Mimir), the rules in `deploy/prometheus/rules/ros2.yaml`, Alertmanager, Grafana with `deploy/grafana/`; Kubernetes manifests in `deploy/k8s/central/`.
4. Operator shell: `fleet.yaml` (§6.3) and `ros2 ebpf status --robot amr-07`.

Ports: `9477/tcp` metrics (robot LAN only), `9478/tcp` optional control HTTP (robot LAN only, no auth), `remote_write` outbound from the collector to central (TLS + per-robot token at the central ingress).

## 9. Troubleshooting

Run `ros2 ebpf doctor` first.
Then:

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

## 10. Limitations

- Intra-process publishes (all subscribers in the same process) bypass `rmw_publish` and are not counted.
  Phase 2: `rclcpp_intra_publish` tracepoint.
- Sequence-gap tracking needs an RMW that fills `publication_sequence_number` (Fast DDS, Connext) and is exact only for single-publisher topics.
- Cross-host loss is a rate inequality over a window, not a per-message count (no shared clock).
- `domain_id` comes from the environment only; `rcl_init` options are not visible.
- Linux only; x86_64 and aarch64; struct offsets are ABI (table keyed by distro/arch, checked at runtime).
- Endpoint counters live with the endpoint: a destroyed publisher's series disappears.
- The daemon is meant to run on request, not permanently: while it runs every probed `rmw_*` call pays the BPF program (§10 of docs/design.md has the cost model).

Details and the rationale behind each decision: [docs/design.md](docs/design.md).

## 11. Development and testing

- [docs/development.md](docs/development.md): building, code layout, the map ABI contract, adding probes, offsets, releasing, CI.
- [docs/testing.md](docs/testing.md): unit tests (C and Python), the end-to-end test that CI runs on every commit, and the manual multi-host test plan.

Everything CI does, locally, against a ROS 2 Rolling source build (privileged for the BPF programs):

```bash
docker run --rm --privileged --pid host -v "$PWD":/src:ro -w /src tomoyafujita/ros2dev:rolling ./scripts/github_workflows.sh
```

License: Apache-2.0.
