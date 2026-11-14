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

- Works against ROS 2 Rolling, binaries or a source build, any RMW, any client library.
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
4. [Tutorial A: the development container](#4-tutorial-a-the-development-container)
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
deploy/prometheus/        prometheus.yml (central), prometheus-local.yml (one host, tutorial A), rules/ros2.yaml (alerts), alertmanager.yml
deploy/grafana/           provisioning + dashboards: fleet, robot, host, daemon health
docker/                   Dockerfile.daemon, .cli, .demo (deployment images, rolling binaries)
scripts/github_workflows.sh   CI: rolling source build, unit tests, end-to-end (runs locally in the ros2dev container too)
docs/                     design.md, metrics.md, deployment.md, development.md, testing.md
```

## 3. Requirements

ros2ebpf follows **ROS 2 Rolling** only, which implies Ubuntu 26.04.
Everything (the Rolling source build, the daemon, the CLI, the demo nodes, Prometheus and Grafana) runs in containers, so the host needs a kernel the BPF programs can load into and a container runtime, nothing else:

| | Required | Check |
|---|---|---|
| Kernel | Linux ≥ 5.8 with BTF (`CONFIG_DEBUG_INFO_BTF`); the Ubuntu 26.04 stock kernel qualifies | `ls /sys/kernel/btf/vmlinux` |
| bpffs | mounted at `/sys/fs/bpf` (systemd does this) | `mount \| grep bpf` |
| Container runtime | Docker (or Podman) that can run a privileged container in the host pid namespace | `docker run --rm --privileged --pid host ubuntu:26.04 true` |
| Arch | x86_64 or aarch64 | |

`ros2ebpfd` needs root, or `CAP_BPF CAP_PERFMON CAP_SYS_PTRACE` (+`CAP_SYS_RESOURCE`); the containers below run privileged, and the daemon is root inside them.
ROS 2 lives inside the containers: the Rolling source build of §4, or the `ros:rolling` images of §5.
Nothing from ROS is installed on the host, and a shim of any other distro is refused by the daemon.
`ros2 ebpf doctor` prints all of this for the host it runs on.

## 4. Tutorial A: the development container

One container holds a ROS 2 Rolling source build, the ros2ebpf packages built against it, the demo nodes and the daemon; Prometheus and Grafana are two more containers next to it.
This is the development environment, what CI runs on every commit (`scripts/github_workflows.sh` does 4.1 and 4.2 unattended in the same image), and the one way to build ros2ebpf until the packages are released.

### 4.1 Start the container

```bash
mkdir -p ~/ros2_ws/src && git clone https://github.com/fujitatomoya/ros2ebpf.git ~/ros2_ws/src/ros2ebpf
docker run -it --name ros2ebpf-dev --privileged --pid host --network host \
  -v /sys/fs/bpf:/sys/fs/bpf -v /etc/machine-id:/etc/machine-id:ro \
  -v ~/ros2_ws:/ws -w /ws tomoyafujita/ros2dev:rolling bash
```

| Option | Why |
|---|---|
| `--privileged` | load the BPF programs and attach the uprobes (`CAP_BPF`, `CAP_PERFMON`, `CAP_SYS_PTRACE`); without it the daemon fails at `attach … Operation not permitted` |
| `--pid host` | the BPF programs report host pids and the daemon resolves them through `/proc`; without it every process is `unknown`. It is also how the daemon finds ROS processes in other containers |
| `--network host` | `:9477/metrics` is reachable from the host and from the Prometheus container; nodes in the container talk DDS with nodes outside it |
| `-v /sys/fs/bpf:/sys/fs/bpf` | the pinned maps live on the host's bpffs, so counters survive the container |
| `-v ~/ros2_ws:/ws` | the workspace stays on the host: the build survives the container, the editor on the host sees the code |
| `-v /etc/machine-id` | the `machine_id` label; optional |

`tomoyafujita/ros2dev:rolling` ([ros2_devenv_builder](https://github.com/fujitatomoya/ros2_devenv_builder)) is Ubuntu 26.04 with every build dependency of Rolling installed.
Any Ubuntu 26.04 container prepared for a Rolling source build (the ROS apt repository, `colcon`, `vcs`, `rosdep install --from-paths src --ignore-src -y --skip-keys "libbpf-dev bpftool"` over the imported sources) works the same with the same `docker run` options.
A second shell in the container is `docker exec -it ros2ebpf-dev bash`; after `exit`, `docker start -ai ros2ebpf-dev` resumes it.

### 4.2 Build

Inside the container:

```bash
apt update && apt install -y clang llvm libbpf-dev bpftool libelf-dev zlib1g-dev libyaml-dev libjson-c-dev \
    pkg-config python3-yaml python3-colorama
vcs import --shallow --input https://raw.githubusercontent.com/ros2/ros2/rolling/ros2.repos src
for p in ros2/rmw_zenoh ros2/rmw_connextdds ros2/rmw_cyclonedds eclipse-cyclonedds/cyclonedds \
         eclipse-iceoryx/iceoryx ros2/rmw_fastrtps/rmw_fastrtps_dynamic_cpp; do touch src/$p/COLCON_IGNORE; done
colcon build --merge-install --packages-up-to rmw ros2cli demo_nodes_cpp \
    --cmake-args -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DRMW_IMPLEMENTATION_DISABLE_RUNTIME_SELECTION=OFF
colcon build --merge-install --packages-select ros2ebpf_exporter ros2ebpf_daemon ros2ebpf_interfaces ros2ebpf
source install/setup.bash
ros2ebpfd --version && ros2 ebpf --help
```

The first `colcon build` is mainline: `rmw`, `rmw_implementation` with Fast DDS, `ros2cli` and the demo nodes, about 170 packages and a few minutes on a workstation.
One RMW is all the daemon needs to be probed through, and the other RMWs with their vendored middlewares are most of the cost of a full build, hence the `COLCON_IGNORE` files.
`-DRMW_IMPLEMENTATION_DISABLE_RUNTIME_SELECTION=OFF` keeps `librmw_implementation.so`, the library the daemon probes, when only one RMW is built.
The second `colcon build` is the four ros2ebpf packages; `--merge-install` gives the single prefix (`install/bin`, `install/etc/ros2ebpf`, `install/lib/librmw_implementation.so`) the rest of this tutorial uses.

Already running a Rolling source workspace in a container?
Clone ros2ebpf into its `src/`, start the container with the options of 4.1, and run the last three commands only; without `--merge-install` the shim is `install/rmw_implementation/lib/librmw_implementation.so`.

### 4.3 Configure

```bash
mkdir -p /etc/ros2ebpf
cp install/etc/ros2ebpf/config.example.yaml /etc/ros2ebpf/config.yaml
cp install/etc/ros2ebpf/expectations.example.yaml /etc/ros2ebpf/expectations.yaml
sed -i 's|/opt/ros/rolling/lib/librmw_implementation.so|/ws/install/lib/librmw_implementation.so|' /etc/ros2ebpf/config.yaml
ros2ebpfd --check -c /etc/ros2ebpf/config.yaml      # config parses, shim exports the rmw_* symbols, BTF present
```

The one thing to edit is the shim path: the `librmw_implementation.so` of the source build.
A shim outside `/opt/ros/` is taken for Rolling; a shim under `/opt/ros/<other distro>/` is refused.
Leave `discover_shims: true` (the default) and ROS processes in other containers are found too.
Set `robot:` only if you do not run a collector that adds the `robot` label (§8).

### 4.4 Start it when you need it

`ros2ebpfd` is not a service that runs all the time.
Every probed call costs a little in the kernel, so you start the daemon when you want to look at the system (something is off on the network, a robot misbehaves) and stop it when you are done, the way `ros2 daemon` is started on request.
`ros2 ebpf start` spawns it detached from your shell, waits until it answers on the control socket and prints a summary; `ros2 ebpf stop` asks it to exit.
The maps stay pinned in bpffs, so counters continue across stop and start.

```bash
ros2 ebpf start         # -c /etc/ros2ebpf/config.yaml; output in ~/.ros/log/ros2ebpfd.log
ros2 ebpf status
ros2 ebpf stop
```

In the container everything is root.
On a host where the CLI is not root, `ros2 ebpf start` runs the daemon through `sudo -n` (`sudo -v` once, or a NOPASSWD sudoers line for `ros2ebpfd`) and the daemon hands its control socket to that user, or to the `ros2ebpf` group when it exists (`sudo groupadd -f ros2ebpf && sudo usermod -aG ros2ebpf "$USER"` lets a whole team talk to it).
`ros2 ebpf start -c FILE --log-level debug --log-file FILE` selects the config, the verbosity and where the output goes; `--no-sudo` when the binary already has the capabilities.

Expected log lines (`tail ~/.ros/log/ros2ebpfd.log`):

```
I ros2ebpfd 0.1.0 starting: host=main machine_id=… distro=ubuntu-26.04 arch=x86_64 kernel=7.0.0-…
I shim /ws/install/lib/librmw_implementation.so: distro=rolling arch=x86_64 dev=… ino=… (config)
I probe family publish enabled: 2/2 links
I probe family take enabled: 2/2 links
I probe family services enabled: 6/6 links
I probe family udp_drops enabled
I metrics at http://0.0.0.0:9477/metrics
I control socket at /run/ros2ebpf/control.sock
I ready: 16/16 probe links attached, 1 shim(s), offset table rolling/x86_64
```

To run in the foreground instead: `ros2ebpfd -c /etc/ros2ebpf/config.yaml --log-level debug` (Ctrl-C stops it; `ros2 ebpf stop` works on it too).

### 4.5 Verify with demo nodes

```bash
ros2 ebpf doctor                      # every line PASS/INFO
ros2 ebpf status -v                   # Healthy=yes, Links 16/16

# shells 2 and 3: docker exec -it ros2ebpf-dev bash; source /ws/install/setup.bash
# (any domain; the daemon reads ROS_DOMAIN_ID per process)
ROS_DOMAIN_ID=5 ros2 run demo_nodes_cpp talker
ROS_DOMAIN_ID=5 ros2 run demo_nodes_cpp listener

ros2 ebpf top --once --interval 3
#  Target  Kind          Name      Domain  Comm      PIDs   Hz    Hz(2nd)  Gaps
#  local   subscription  /chatter  5       listener  12345  1.00  -        0
#  local   publisher     /chatter  5       talker    12344  1.00  -        -

curl -s localhost:9477/metrics | grep -E '^ros2_(rmw_publish_total|rmw_take_total|process_info)'
```

Nodes that were already running before the daemon started are counted from their next call: the uprobe lives on the library's inode, not on a process.

For a live view instead of one table, `ros2 ebpf monitor` opens a full-screen terminal UI (colour is used only there): endpoint rates with a sparkline of the last intervals, the processes behind them, and the daemon health, with k9s-like keys (`e`, `p`, `h` to switch views, `/` to filter, `s` to sort, `q` to quit).
Nothing else is needed, no Prometheus and no browser.

### 4.6 Prometheus and Grafana

Two more containers on the host, started from the repository checkout (`~/ros2_ws/src/ros2ebpf`), with the configuration, rules and dashboards from `deploy/`:

```bash
docker network create ros2ebpf
docker run -d --name prometheus --network ros2ebpf --add-host host.docker.internal:host-gateway -p 9090:9090 \
  -v "$PWD/deploy/prometheus:/etc/prometheus:ro" prom/prometheus:v3.5.0 --config.file=/etc/prometheus/prometheus-local.yml
docker run -d --name grafana --network ros2ebpf -p 3000:3000 -e GF_SECURITY_ADMIN_PASSWORD=admin \
  -v "$PWD/deploy/grafana/provisioning:/etc/grafana/provisioning:ro" \
  -v "$PWD/deploy/grafana/dashboards:/var/lib/grafana/dashboards:ro" grafana/grafana:12.1.0
```

[deploy/prometheus/prometheus-local.yml](deploy/prometheus/prometheus-local.yml) scrapes the daemon every 5 s at `host.docker.internal:9477` (the host, as seen from a container on the bridge network `ros2ebpf`; the daemon listens on the host network) with the label `robot="bench-01"`, and loads the alert rules of `deploy/prometheus/rules/ros2.yaml`.
Grafana finds Prometheus by its container name on that network, which is what the provisioned datasource expects.

- <http://localhost:9090/targets>: `ros2ebpf` is `UP`; <http://localhost:9090/alerts> lists the rules; query `rate(ros2_rmw_publish_total[1m])` while the talker runs.
- <http://localhost:3000> (admin / admin), Dashboards, folder **ROS 2**: "ROS 2 fleet" (robot `bench-01`), "ROS 2 robot", "ROS 2 host" (publish and take rates per topic, process table) and "ros2ebpf daemon health".
  The dashboards take 30 s to fill after the talker starts.

Alertmanager and the per-robot collector are part of the real layout (§8 and [docs/deployment.md](docs/deployment.md)); here Prometheus evaluates the rules by itself.
`docker rm -f prometheus grafana && docker network rm ros2ebpf` removes both.

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

# (b) a node in the development container of §4 (also --pid host): found the same way within 5 s
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
| `shims[].path` | — | `librmw_implementation.so` to probe (*restart*): `/opt/ros/rolling/lib/…` or the one of a Rolling source build; `arch` optional (`uname -m`) |
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
| `monitor [--interval S] [--view endpoints\|processes\|health] [--filter F] [--once] [--no-color]` | full-screen terminal view, no Prometheus or browser needed: endpoint rates with sparklines, processes, daemon health; `e`/`p`/`h` switch views, `/` filters, `s` sorts, `?` help, `q` quits; `--once` prints one plain frame |
| `top [--topic T] [--domain D] [--kind K] [--interval S] [--once]` | live rates from two map reads (bench tool) |
| `config show \| get KEY \| set KEY VALUE [--no-reload] \| keys` | edit the config file with validation, then reload |
| `expectations capture [--duration S] [--takers] [-o FILE] \| show` | write expectations.yaml from observed rates |
| `doctor [--config F] [--shim P]` | kernel, BTF, bpffs, capabilities, shim symbols, daemon, exporter |
| `metrics [--grep S]` | the exposition, through the control socket (works in `external` mode too) |

Without ROS on a host, `ros2ebpf-ctl status|enable|disable|reload|snapshot|config|metrics` does the same over the socket (`-q` for exit-code-only), and `ros2ebpf-ctl rpc` is the ssh transport the CLI uses.

## 8. Multi-host robots and fleets

Short version; the full procedure with verification steps is in [docs/deployment.md](docs/deployment.md).

1. On **every host** of the robot: run `ros2ebpfd` (the development container of §4, or the image of §5 and the DaemonSet on a cluster) while you want statistics.
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
| `ros2ebpf_name_read_failures_total` grows, `ros2ebpf_unknown_rmw_processes > 0` | struct offsets differ from the table: this Rolling is older or newer than the row in `ros2ebpf_daemon/src/offsets.c`; run `ros2ebpf_daemon/offsets/gen.sh <prefix>` and update the row ([offsets/README.md](ros2ebpf_daemon/offsets/README.md)) |
| `shim … ROS 2 humble on x86_64 is not supported` | ros2ebpf follows Rolling only; a shim under `/opt/ros/<other distro>/` is skipped |
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
- ROS 2 Rolling only (binaries or a source build), Linux only, x86_64 and aarch64; struct offsets are ABI (one table row per arch, checked against mainline in CI and against the running process at runtime).
- Endpoint counters live with the endpoint: a destroyed publisher's series disappears.
- The daemon is meant to run on request, not permanently: while it runs every probed `rmw_*` call pays the BPF program (§10 of docs/design.md has the cost model).

Details and the rationale behind each decision: [docs/design.md](docs/design.md).

## 11. Development and testing

- [docs/development.md](docs/development.md): building, code layout, the map ABI contract, adding probes, offsets, releasing, CI.
- [docs/testing.md](docs/testing.md): unit tests (C and Python), the end-to-end test that CI runs on every commit, and the manual multi-host test plan.

Everything CI does, unattended, in the image of §4 (privileged for the BPF programs, host pid namespace for the daemon):

```bash
docker run --rm --privileged --pid host -v "$PWD":/src:ro -w /src tomoyafujita/ros2dev:rolling ./scripts/github_workflows.sh
```

License: Apache-2.0.
