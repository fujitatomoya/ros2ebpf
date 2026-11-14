# Deployment: multi-host robots and a fleet

This is the operator procedure for a robot made of several Linux hosts, a fleet of such robots, and a central Prometheus / Alertmanager / Grafana.
The single-host tutorials are in the [README](../README.md).
Every step ends with what to check, so a problem is caught at the layer it belongs to.

```
robot amr-07                                     central
┌───────────────────────────────────────────┐    ┌──────────────────────────────┐
│ main        ros2ebpfd :9477 ──┐            │    │ Prometheus (remote-write rx) │
│ perception  ros2ebpfd :9477 ──┼─ Alloy ────┼───►│  rules/ros2.yaml → Alertmgr  │
│ arm         ros2ebpfd :9477 ──┘  (WAL)     │    │ Grafana dashboards           │
│ expectations.yaml on main only             │    └──────────────────────────────┘
└───────────────────────────────────────────┘    robot amr-08 … ─────────────────┘
```

Deployment unit: host.
Aggregation unit: robot (`robot=` is added by the collector).
Correlation scope: `(robot, domain_id)`.

## 1. Every host of the robot: `ros2ebpfd`

Repeat on each host (`main`, `perception`, `arm` …).
Hosts may run different RMWs and CPU architectures (all on ROS 2 Rolling); each daemon exports what it found in `ros2ebpf_host_info`.

```bash
# install: the development container build (README §4), the ros2ebpfd image (docker/Dockerfile, below), or
# the ros-rolling-ros2ebpf-daemon package once the packages are released to the ROS build farm
sudo mkdir -p /etc/ros2ebpf && sudo cp install/etc/ros2ebpf/config.example.yaml /etc/ros2ebpf/config.yaml
sudoedit /etc/ros2ebpf/config.yaml
```

Per-host settings that matter in a multi-host robot:

```yaml
host: ""                 # keep empty if hostnames are unique and stable across the robot ("main", "perception")
robot: ""                # leave empty: the collector sets it. Set only when scraping without a collector.
shims:
  - path: /opt/ros/rolling/lib/librmw_implementation.so    # or the one of this host's Rolling source build
discover_shims: true
expectations_file: /etc/ros2ebpf/expectations.yaml         # the file exists on ONE host only (see §2)
```

Then, whenever statistics are wanted (the daemon is on demand, see README 4.4):

```bash
ros2 ebpf start                               # detached, through sudo -n; `ros2 ebpf stop` when done
ros2ebpf-ctl status -q && echo healthy       # exit 0; no ROS needed on the host for this
curl -s localhost:9477/metrics | grep -E '^ros2ebpf_(host_info|probes_attached|probes_configured)'
```

Check: `ros2ebpf_probes_attached == ros2ebpf_probes_configured`, `ros2ebpf_host_info` shows the expected `host`, `distro`, `arch`, `offset_table`.
If nodes are running: `ros2_process_info` lists them with the right `domain_id`.

Firewall: allow `9477/tcp` from the collector host only.
Do not expose it beyond the robot LAN.

### Container instead of a package

Same on every host, with Docker/Podman; the flags are those of the development container (README 4.1), and the image is built with `docker build -f docker/Dockerfile -t ros2ebpfd .`:

```bash
docker run -d --name ros2ebpfd --restart unless-stopped --pid host --privileged \
  -v /sys/fs/bpf:/sys/fs/bpf -v /sys/kernel/btf:/sys/kernel/btf:ro -v /etc/machine-id:/etc/machine-id:ro \
  -v /etc/ros2ebpf:/etc/ros2ebpf:ro --network host -e ROS2EBPF_HOST=$(hostname) ghcr.io/fujitatomoya/ros2ebpfd:0.1.0
docker exec ros2ebpfd ros2ebpf-ctl status -q && echo healthy
```

Kubernetes (k3s/MicroK8s on the robot): `deploy/k8s/robot/` runs the same container as a DaemonSet; see §5.

## 2. One host of the robot: expectations and the collector

Pick the host that is always on (usually `main`).

### 2.1 Expectations (per robot)

```bash
# on main, with the robot in its normal operating state, after all hosts run ros2ebpfd:
ros2 ebpf expectations capture --robot amr-07 --duration 120 --takers -o expectations.yaml
# or without fleet.yaml: --host perception --host arm (local main is implied only without --host; add --host unix:///run/ros2ebpf/control.sock)
sudoedit expectations.yaml           # keep the topics you want alerts for; round the numbers
sudo cp expectations.yaml /etc/ros2ebpf/expectations.yaml
ros2 ebpf reload
curl -s localhost:9477/metrics | grep ros2_topic_expected
```

`takers` is the number of endpoints across the robot that are expected to take the topic; it makes the cross-host loss rule exact.
Expectations are exported without a `host` label; the collector adds `robot`.

### 2.2 Collector: Grafana Alloy

```bash
sudo mkdir -p /etc/alloy /var/lib/alloy
sudo cp deploy/alloy/config.alloy /etc/alloy/config.alloy
docker run -d --name alloy --restart unless-stopped --network host \
  -v /etc/alloy/config.alloy:/etc/alloy/config.alloy:ro -v /var/lib/alloy:/var/lib/alloy \
  -e ROBOT=amr-07 -e SITE=osaka-dc1 \
  -e SCRAPE_TARGETS=main:9477,perception:9477,arm:9477 \
  -e REMOTE_WRITE_URL=https://central.example.com/api/v1/write -e REMOTE_WRITE_TOKEN=... \
  grafana/alloy:v1.6.1 run --storage.path=/var/lib/alloy /etc/alloy/config.alloy
```

The shipped `config.alloy` takes `SCRAPE_TARGETS` as a single `__address__`; for several hosts either list several `{ "__address__" = "..." }` entries in `discovery.relabel.ros2ebpf.targets`, or use `discovery.dns` / a file-based target list.
Keep `honor_labels = true` so the exporter's `host` label is not overwritten by `instance`.
The WAL on `/var/lib/alloy` buffers link loss (`max_keepalive_time = "8h"`).

Check on the collector host: `curl -s localhost:12345/-/ready` (Alloy UI on 12345 if enabled), and in Alloy's log that every target is `up`.
A Prometheus agent (`prometheus --agent`) with the equivalent `scrape_configs` + `remote_write` (README §8) works the same way.

## 3. Central

### 3.1 Kubernetes

Plain manifests:

```bash
kubectl create ns observability
kubectl apply -k deploy/k8s/central/kustomize/plain/
kubectl -n observability get pods
kubectl -n observability port-forward svc/grafana 3000:3000
```

Existing Prometheus Operator:

```bash
helm upgrade --install kps prometheus-community/kube-prometheus-stack -n observability \
  -f deploy/k8s/central/helm/values-kube-prometheus-stack.yaml
kubectl -n observability apply -f deploy/k8s/central/helm/prometheusrule-ros2.yaml
kubectl -n observability create configmap ros2ebpf-dashboards --from-file=deploy/grafana/dashboards/ \
  --dry-run=client -o yaml | kubectl label --local -f - grafana_dashboard=1 -o yaml | kubectl apply -f -
```

The ingress for `/api/v1/write` is site specific (`deploy/k8s/central/kustomize/plain/ingress.yaml` is a commented stub): terminate TLS and require a bearer token per robot.

### 3.2 Check

In Prometheus (`/graph`):

| Query | Expect |
|---|---|
| `up{job="ros2ebpf"}` | 1 for every host of every robot, with `robot` and `host` labels |
| `ros2ebpf_probes_attached - ros2ebpf_probes_configured` | 0 everywhere |
| `count by (robot) (ros2_process_info)` | the number of ROS processes per robot |
| `ros2_topic_expected_hz` | one series per robot and topic |
| `/alerts` | `Ros2ebpfHostDown`, `Ros2ebpfRobotBlind`, `Ros2ebpfProbesMissing` inactive |

In Grafana: folder "ROS 2": fleet → robot → host drill-down; "ros2ebpf daemon health".

## 4. Operator shell: the CLI across hosts

```yaml
# /etc/ros2ebpf/fleet.yaml  (on the operator machine)
robots:
  amr-07:
    hosts:
      - ops@main.amr-07.local
      - {name: perception, address: ops@perception.amr-07.local, transport: ssh}
      - {name: arm, address: 192.168.7.30, transport: http, port: 9478}   # needs control.http on that daemon
```

ssh targets need `ros2ebpf-ctl` on the remote host (part of the daemon package) and key-based login; nothing listens on the network for that.
HTTP targets need `control.http: 0.0.0.0:9478` in that host's config and are unauthenticated: robot LAN only.

```bash
ros2 ebpf status --robot amr-07
ros2 ebpf top --robot amr-07 --topic /lidar/points
ros2 ebpf enable take --robot amr-07
ros2 ebpf status --host ops@main.amr-07.local --format json | jq .
```

## 5. Kubernetes on the robot

```bash
kubectl label node main perception arm ros2ebpf/enabled=true
kubectl create ns ros2ebpf
kubectl -n ros2ebpf create configmap ros2ebpf-config --from-file=config.yaml --from-file=expectations.yaml
# edit deploy/k8s/robot/alloy.yaml: ROBOT, SITE, REMOTE_WRITE_URL, the token secret
kubectl apply -k deploy/k8s/robot/
kubectl -n ros2ebpf get pods -o wide                         # one ros2ebpfd per node, Ready (readiness = ros2ebpf-ctl status -q)
kubectl -n ros2ebpf exec ds/ros2ebpfd -- ros2ebpf-ctl status
```

ROS nodes running as pods are covered through discovery; the pod's container id lands in `ros2_process_info{container}`.
Nodes on the node itself are covered as well (hostPID).

## 6. Multi-host verification plan

What to run once all three layers are up (this is also the checklist in [testing.md](testing.md) §4):

1. Per host: `ros2ebpf-ctl status -q` exits 0; `ros2ebpf_host_info` has the right `host`.
2. Start a talker on host A and a listener on host B in the same domain: `ros2_rmw_publish_total{host="A",topic="/chatter"}` and `ros2_rmw_take_total{host="B",topic="/chatter"}` grow at the same rate in Prometheus (`rate(...[1m])`), both with `robot="amr-07"`.
3. `ros2 ebpf status --robot amr-07` shows every host healthy.
4. Set `/chatter: {hz: 1, takers: 1}` in expectations; stop the listener: `Ros2CrossHostLoss` fires within ~3 minutes; stop the talker: `Ros2TopicSilent` / `Ros2TopicRateLow` fire; restart both: alerts resolve.
5. Unplug host B from the LAN for a few minutes: `Ros2ebpfHostDown` fires; the collector's WAL replays B's samples when it is back (no gap for the period B was reachable from the collector).
6. Stop the collector host's link to central for 10 minutes: no alert from central except `Ros2ebpfRobotBlind` after 5 minutes; samples backfill on reconnect.
7. `ros2 ebpf stop && ros2 ebpf start` on any host: counters continue from their previous values (`maps_recreated_at_start: false` in status).

## 7. Security notes

- `ros2ebpfd` runs as root (`ros2 ebpf start` goes through `sudo -n`; the DaemonSet runs it privileged).
  It reads `environ` of ROS processes (`CAP_SYS_PTRACE`); that is the only process data it touches.
- The control socket is group `ros2ebpf`, mode 0660. The optional control HTTP listener has no authentication; leave it off unless the robot LAN is trusted.
- `/metrics` has no authentication either; restrict `9477/tcp` to the collector.
- `remote_write` leaves the robot: use TLS and a per-robot token at the central ingress; rotate tokens by replacing the Secret / env and restarting the collector.
- Nothing in this stack creates a DDS participant or sends anything on the ROS 2 domain.
