# Development

## Build

All four packages are ROS 2 packages built with colcon: `ros2ebpf_exporter`, `ros2ebpf_daemon` and
`ros2ebpf_interfaces` are `ament_cmake`, `ros2ebpf` is `ament_python`. The repository root is a
colcon workspace by itself (`build/`, `install/`, `log/` are git-ignored); it can also be cloned into
`src/` of a larger workspace. The daemon and exporter use ament only at build time: nothing from
ROS is needed on the host they run on.

Native (Ubuntu 24.04, ROS 2 Jazzy or newer):

```bash
sudo apt install -y build-essential cmake pkg-config clang-18 llvm-18 libbpf-dev \
     linux-tools-common linux-tools-generic libelf-dev zlib1g-dev libyaml-dev libjson-c-dev \
     python3-yaml python3-pytest python3-flake8 \
     ros-jazzy-ament-cmake ros-jazzy-ros2cli python3-colcon-common-extensions
sudo ln -sf /usr/bin/clang-18 /usr/local/bin/clang
sudo ln -sf "$(ls -d /usr/lib/linux-tools/*/ | head -1)bpftool" /usr/local/bin/bpftool
# (if apt refuses linux-tools-generic while the mirrors are syncing: sudo sh docker/install-bpftool.sh)
source /opt/ros/jazzy/setup.bash
colcon build --merge-install --cmake-args -DCMAKE_BUILD_TYPE=RelWithDebInfo
colcon test --merge-install && colcon test-result --verbose
```

`make all test cli-test` wraps the same calls (`ROS_DISTRO=jazzy` by default; the Makefile sources
`/opt/ros/$ROS_DISTRO/setup.sh` itself). `--merge-install` gives one prefix (`install/bin`,
`install/etc/ros2ebpf`, `install/lib/systemd/system`) that the tutorials and the container images
rely on; without it every package gets its own prefix under `install/<pkg>/`.

`rosdep install --from-paths . --ignore-src -y --skip-keys "libbpf-dev bpftool"` resolves the
remaining dependencies; `libbpf-dev` and `bpftool` have no rosdep key yet (see Packaging below)
and are installed by hand as above.

In the toolchain container (nothing installed on the host, runs as your user):

```bash
make build-image            # docker/Dockerfile.build -> ros2ebpf-build:local (ros-base + clang/libbpf/bpftool)
make in-docker-test         # colcon build + test + flake8 inside it, build/ install/ log/ on the host
```

Build options (`--cmake-args`): `-DBUILD_TESTING=OFF` skips the unit tests, `-DBPF_TARGET_ARCH=arm64`
cross-compiles the BPF objects. The C tests are registered with `ament_add_test`, so
`colcon test-result` sees them like any other ROS package's tests.

The in-tree build directories of the Debian package build (`make deb`, plain cmake under `build/`)
are marked with `COLCON_IGNORE` so the staged copies of `package.xml` under
`build/*/_CPack_Packages/` never show up as duplicate packages; colcon marks its own build base the
same way.

Cross build for aarch64: `-DBPF_TARGET_ARCH=arm64` for the BPF objects plus a normal CMake
toolchain file for the user-space parts; `docker buildx build --platform linux/arm64 -f
docker/Dockerfile.daemon .` is the simplest route.

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

`maps.h` is included by BPF C, the daemon and the exporter, and is the only shared knowledge about
the kernel data. Rules:

- Any change to a struct layout, map name, key or value type bumps `ROS2EBPF_MAP_ABI`. The daemon
  compares it with `config.abi` of previously pinned maps at start and recreates them when
  different (counters reset once); an external exporter must do the same check.
- Only fixed-width integers and char arrays; no libc types (the header is compiled with
  `clang -target bpf`).
- New fields go at the end of a struct; new maps get a `ROS2EBPF_MAP_*` name and are optional for
  the reader when they belong to an optional probe family (like `udp_drops`).

## Adding a probe

1. Add the program to `rmw_probes.bpf.c` with `SEC("uprobe/<symbol>")` (or `uretprobe/`) using
   `BPF_UPROBE(...)`; reuse `lookup_or_create()` so first-sight, filters and pid announcement stay
   consistent. Keep the hot path to one map lookup and atomic adds.
2. Add a row to `prog_defs[]` in `probes.c` (family, program name, symbol, retprobe flag). Link
   counting and `status` follow automatically.
3. If the symbol belongs to a new family, extend `enum family`, `family_names[]`, the config keys
   and the CLI's `FAMILIES`.
4. Export it in `render.c` and document it in `docs/metrics.md`; extend `test_render.c`.
5. Add the symbol to `required_syms[]` in `offsets.c` and `REQUIRED_SYMBOLS` in `doctor.py` if the
   daemon cannot work without it.

Sanity-check the verifier output with `--log-level debug` (libbpf debug logs are forwarded).

## Struct offsets

See `ros2ebpf_daemon/offsets/README.md`. The runtime check is the `implementation_identifier`
read from offset 0: if it does not start with `rmw_`, the row is wrong for that binary and
`ros2ebpf_unknown_rmw_processes` says so.

## Control protocol

`ros2ebpf_interfaces/README.md` lists the methods; the JSON schemas there are the contract. New
methods are added to `handle_rpc()` in `main.c`, to the schema, and get a verb (or an option of
one) in the CLI.

## Packaging

Debian packages with CPack (built on the target architecture, or in the toolchain container with
`make in-docker-deb`). They install under `/usr` and `/etc`, for hosts without ROS:

```bash
make deb                    # build/pkg/ros2ebpf-daemon_0.1.0_amd64.deb   (ros2ebpfd, ros2ebpf-ctl, unit, example configs)
                            # build/pkg/ros2ebpf-exporter_0.1.0_amd64.deb (static library + headers + standalone exporter)
sudo apt install ./build/pkg/ros2ebpf-daemon_0.1.0_amd64.deb
```

The daemon links the exporter library statically, so `ros2ebpf-daemon` does not depend on
`ros2ebpf-exporter`; install that one only for the standalone binary / headers. The unit is
installed as `/usr/lib/systemd/system/ros2ebpfd.service` with `ExecStart=/usr/bin/ros2ebpfd`.
`postinst` creates the `ros2ebpf` group and runs `systemctl daemon-reload`; it does not enable the
unit. Container images: `docker/Dockerfile.daemon`, `docker/Dockerfile.cli` (`make images`).

Releasing through bloom / the ROS build farm (`ros-<distro>-ros2ebpf-*` packages under
`/opt/ros/<distro>`) is the ROS-native route for all four packages and needs rosdep keys for
`libbpf-dev` and `bpftool` first (a `ros/rosdistro` pull request); until then `rosdep` needs
`--skip-keys "libbpf-dev bpftool"`. The CLI can also be installed with `pip install ./ros2ebpf`.

## Deploy sources of truth

`deploy/prometheus/` and `deploy/grafana/` are the sources; `make k8s-sync` copies them into
`deploy/k8s/central/kustomize/plain/files/` (kustomize cannot read outside its directory) and
regenerates `deploy/k8s/central/helm/prometheusrule-ros2.yaml`. Run it after editing rules or
dashboards and commit the result; `make lint` runs it and validates everything.

## Branches

`rolling` is the default and development branch; pull requests target it. `main` is a read-only
mirror of `rolling` (`.github/workflows/mirror-rolling-to-main.yaml` force-pushes it on every push
to `rolling`, as the ROS 2 core repositories do), so `main` is never pushed to directly. There are
no release branches yet: the first one is created when the packages are released into a ROS
distro, and nothing is backported before that.

## CI

`.github/workflows/ci.yaml`: ROS 2 Jazzy via `ros-tooling/setup-ros`, `colcon build` + `colcon test`
of all packages, flake8 for the CLI, a real load of the BPF objects on the runner kernel (verifier
check, `/metrics` smoke test), the Debian package build, container image builds with a privileged
smoke test, and validation of everything under `deploy/` (promtool, amtool, kustomize, compose,
alloy fmt).
