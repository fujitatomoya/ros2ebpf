# Development

## Build

All four packages are ROS 2 packages built with colcon: `ros2ebpf_exporter`, `ros2ebpf_daemon` and `ros2ebpf_interfaces` are `ament_cmake`, `ros2ebpf` is `ament_python`.
The repository root is a colcon workspace by itself (`build/`, `install/`, `log/` are git-ignored); it can also be cloned into `src/` of a larger workspace.
The daemon and exporter use ament only at build time: nothing from ROS is needed on the host they run on.

ros2ebpf follows ROS 2 **Rolling** only: everything (implementation, CI, documentation) tracks Rolling, CI tests against a Rolling source build, and a distro branch is created when the next ROS release is branched.
Nothing is backported before that.

Native (Ubuntu 26.04, ROS 2 Rolling binaries):

```bash
sudo apt install -y build-essential cmake pkg-config clang llvm libbpf-dev bpftool \
     libelf-dev zlib1g-dev libyaml-dev libjson-c-dev \
     python3-yaml python3-pytest python3-flake8 \
     ros-rolling-ament-cmake ros-rolling-ros2cli python3-colcon-common-extensions
source /opt/ros/rolling/setup.bash
colcon build --merge-install --cmake-args -DCMAKE_BUILD_TYPE=RelWithDebInfo
colcon test --merge-install && colcon test-result --verbose
```

`--merge-install` gives one prefix (`install/bin`, `install/etc/ros2ebpf`) that the tutorials and the container images rely on; without it every package gets its own prefix under `install/<pkg>/`.

`rosdep install --from-paths . --ignore-src -y --skip-keys "libbpf-dev bpftool"` resolves the remaining dependencies; `libbpf-dev` and `bpftool` have no rosdep key yet (see Packaging below) and are installed by hand as above.

Exactly what CI runs, with nothing installed on the host (privileged for the BPF programs):

```bash
docker run --rm --privileged --pid host -v "$PWD":/src:ro -w /src tomoyafujita/ros2dev:rolling ./scripts/github_workflows.sh
```

That is the Rolling *source* build: `scripts/github_workflows.sh` imports `ros2.repos` of mainline and builds `rmw`, `rmw_implementation`, `ros2cli` and `demo_nodes_cpp` with everything they need (about 170 packages, a few minutes on a workstation) plus the ros2ebpf packages with colcon.
It then runs the unit tests and flake8, checks the offset table in `ros2ebpf_daemon/src/offsets.c` against the mainline `rmw` headers (`offsets/gen.sh`), starts the daemon on the kernel with its uprobes attached to the mainline-built `librmw_implementation.so` (all probe links must attach and the `rolling/<arch>` offset table must be in use), drives it with `ros2 ebpf start` / `stop` on the mainline ros2cli, and runs the end-to-end test with the mainline demo nodes (docs/testing.md).
The image comes from [ros2_devenv_builder](https://github.com/fujitatomoya/ros2_devenv_builder) and carries every ROS build dependency; the script adds the BPF toolchain.

Build options (`--cmake-args`): `-DBUILD_TESTING=OFF` skips the unit tests, `-DBPF_TARGET_ARCH=arm64` cross-compiles the BPF objects.
The C tests are registered with `ament_add_test`, so `colcon test-result` sees them like any other ROS package's tests.


Cross build for aarch64: `-DBPF_TARGET_ARCH=arm64` for the BPF objects plus a normal CMake toolchain file for the user-space parts; `docker buildx build --platform linux/arm64 -f docker/Dockerfile.daemon .` is the simplest route.

## Layout and responsibilities

```
ros2ebpf_exporter/include/ros2ebpf/maps.h   map contract: structs, map names, ABI number, name hash
ros2ebpf_exporter/include/ros2ebpf/exporter.h public API: maps_open, snapshot_read, render, http
ros2ebpf_exporter/src/maps_reader.c         bpf_obj_get + map iteration -> snapshot
ros2ebpf_exporter/src/render.c              snapshot -> Prometheus text (aggregation by label set)
ros2ebpf_exporter/src/http.c                minimal GET /metrics server
ros2ebpf_exporter/src/exporter_main.c       standalone ros2ebpf-exporter (external mode)

ros2ebpf_daemon/bpf/rmw_probes.bpf.c        uprobes: publish, take, services, destroy; tp_btf sched_process_exit
ros2ebpf_daemon/bpf/udp_drops.bpf.c         tp_btf udp_fail_queue_rcv_skb
ros2ebpf_daemon/bpf/vmlinux_min.h           hand-maintained CO-RE subset of kernel types (x86 + arm64)
ros2ebpf_daemon/src/probes.c                skeletons, pin root, shim registry + discovery, attach/detach, filters
ros2ebpf_daemon/src/identity.c              /proc -> processes map, exit sweep, GC
ros2ebpf_daemon/src/control.c               JSON-RPC over unix socket / HTTP
ros2ebpf_daemon/src/config.c                libyaml -> struct config
ros2ebpf_daemon/src/offsets.c               (distro, arch) -> struct offsets; ELF symbol check
ros2ebpf_daemon/src/udpmap.c                port -> pid/comm via /proc/net/udp + /proc/<pid>/fd
ros2ebpf_daemon/src/main.c                  epoll loop, RPC handlers, reload
ros2ebpf_daemon/src/ros2ebpf-ctl.c          thin control client

ros2ebpf/ros2ebpf/api/__init__.py           transports (unix/ssh/http), fleet.yaml, rate computation
ros2ebpf/ros2ebpf/verb/*.py                 one file per verb
```

## The map contract

`maps.h` is included by BPF C, the daemon and the exporter, and is the only shared knowledge about the kernel data.
Rules:

- Any change to a struct layout, map name, key or value type bumps `ROS2EBPF_MAP_ABI`.
  The daemon compares it with `config.abi` of previously pinned maps at start and recreates them when different (counters reset once); an external exporter must do the same check.
- Only fixed-width integers and char arrays; no libc types (the header is compiled with `clang -target bpf`).
- New fields go at the end of a struct; new maps get a `ROS2EBPF_MAP_*` name and are optional for the reader when they belong to an optional probe family (like `udp_drops`).

## Adding a probe

1. Add the program to `rmw_probes.bpf.c` with `SEC("uprobe/<symbol>")` (or `uretprobe/`) using `BPF_UPROBE(...)`; reuse `lookup_or_create()` so first-sight, filters and pid announcement stay consistent.
   Keep the hot path to one map lookup and atomic adds.
2. Add a row to `prog_defs[]` in `probes.c` (family, program name, symbol, retprobe flag).
   Link counting and `status` follow automatically.
3. If the symbol belongs to a new family, extend `enum family`, `family_names[]`, the config keys and the CLI's `FAMILIES`.
4. Export it in `render.c` and document it in `docs/metrics.md`; extend `test_render.c`.
5. Add the symbol to `required_syms[]` in `offsets.c` and `REQUIRED_SYMBOLS` in `doctor.py` if the daemon cannot work without it.

Sanity-check the verifier output with `--log-level debug` (libbpf debug logs are forwarded).

## Struct offsets

See `ros2ebpf_daemon/offsets/README.md`.
The runtime check is the `implementation_identifier` read from offset 0: if it does not start with `rmw_`, the row is wrong for that binary and `ros2ebpf_unknown_rmw_processes` says so.

## Control protocol

`ros2ebpf_interfaces/README.md` lists the methods; the JSON schemas there are the contract.
New methods are added to `handle_rpc()` in `main.c`, to the schema, and get a verb (or an option of one) in the CLI.

## Releasing

The packages are source packages; binaries come from the ROS build farm (bloom release, then `ros-<distro>-ros2ebpf-*` under `/opt/ros/<distro>`) like for any ROS 2 package, and nothing is packaged in this repository.
Prerequisite: rosdep keys for `libbpf-dev` and `bpftool` (a `ros/rosdistro` pull request); until they exist `rosdep install` needs `--skip-keys "libbpf-dev bpftool"`.
The daemon links the exporter library statically and installs the example configs to `etc/ros2ebpf/`; it reads `/etc/ros2ebpf/config.yaml` by default.
There is no service file: the daemon is started on request with `ros2 ebpf start` (README 4.3) or as the Kubernetes DaemonSet.

Container images for the deployments under `deploy/`: `docker/Dockerfile.daemon` (daemon + exporter
+ ctl on Ubuntu 26.04, built from rolling binaries), `docker/Dockerfile.cli`, `docker/Dockerfile.demo`.

## Deploy sources of truth

`deploy/prometheus/` and `deploy/grafana/` are the sources; `deploy/k8s/sync.sh` copies them into `deploy/k8s/central/kustomize/plain/files/` (kustomize cannot read outside its directory) and regenerates `deploy/k8s/central/helm/prometheusrule-ros2.yaml`.
Run it after editing rules or dashboards and commit the result; the `deploy-lint` CI job runs it and fails on a stale copy, then validates everything with promtool, amtool, kustomize and `alloy fmt` (`deploy/alloy/config.alloy`).

## Documentation

Markdown prose is written one sentence per line (#11): wrapped lines are joined, every sentence gets its own line, and lists, tables and code blocks keep their structure.
Diffs then show the sentence that changed instead of a rewrapped paragraph.

## Branches

`rolling` is the default and development branch; pull requests target it.
`main` is a read-only mirror of `rolling` (`.github/workflows/mirror-rolling-to-main.yaml` force-pushes it on every push to `rolling`, as the ROS 2 core repositories do), so `main` is never pushed to directly.
There are no release branches yet: the first one is created when the packages are released into a ROS distro, and nothing is backported before that.

## CI

`.github/workflows/ci.yaml`, rolling only:

- `rolling`: `scripts/github_workflows.sh` inside the `tomoyafujita/ros2dev:rolling` container (privileged): Rolling source build of `rmw` / `rmw_implementation` / `ros2cli` / `demo_nodes_cpp`, `colcon build` + `colcon test` of all packages, flake8, offset table vs. mainline `rmw`, the daemon probing the mainline `librmw_implementation.so` (BPF verifier, all links attached), `ros2 ebpf` on mainline ros2cli, and the end-to-end test with the mainline demo nodes.
  The workflow file only calls the script, so CI changes are script changes that run identically with the `docker run` above.
- `images`: the daemon, CLI and demo images on Rolling binaries (`ros:rolling-*`, Ubuntu 26.04), `ros2 ebpf --help` in the CLI image, and the daemon in its own container discovering the demo container's shim through `/proc` (README tutorial C).
- `deploy-lint`: `deploy/k8s/sync.sh` must leave no diff, then validation of everything under `deploy/` (promtool, amtool, kustomize, alloy fmt).

When a release branch is created, its workflow is a copy of the `rolling` job with the distro's `ros2dev` image, as in the other `fujitatomoya` repositories.
