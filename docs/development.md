# Development

## Build

Native (Ubuntu 24.04):

```bash
sudo apt install -y build-essential cmake pkg-config clang-18 llvm-18 libbpf-dev \
     linux-tools-common linux-tools-generic libelf-dev zlib1g-dev libyaml-dev libjson-c-dev \
     python3-yaml python3-pytest
sudo ln -sf /usr/bin/clang-18 /usr/local/bin/clang
sudo ln -sf "$(ls -d /usr/lib/linux-tools/*/ | head -1)bpftool" /usr/local/bin/bpftool
make all test cli-test
```

In the toolchain container (nothing installed on the host):

```bash
make build-image            # docker/Dockerfile.build -> ros2ebpf-build:local
make in-docker-test         # cmake build + ctest + pytest inside it, build/ on the host
```

As ROS packages in a colcon workspace (`ros2ebpf_daemon` and `ros2ebpf_exporter` are plain CMake
packages with a `package.xml`, `ros2ebpf` is ament_python):

```bash
colcon build --packages-up-to ros2ebpf ros2ebpf_daemon
```

The in-tree `build/` directory produced by `make` / `cmake` is marked with `COLCON_IGNORE` (by the
Makefile and by both CMake projects at configure time), so a checkout that was also built natively
or packaged with `make deb` does not confuse colcon with the staged copies of `package.xml` under
`build/*/_CPack_Packages/`. If you configured with plain `cmake` before that marker existed, run
`touch build/COLCON_IGNORE` once or remove `build/`.

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

Debian packages with CPack (built on the target architecture, or in the toolchain container):

```bash
make deb                    # build/pkg/ros2ebpf-daemon_0.1.0_amd64.deb (daemon + exporter + ctl + unit + examples)
                            # build/pkg/ros2ebpf-exporter_0.1.0_amd64.deb (library + headers + standalone exporter)
sudo apt install ./build/pkg/ros2ebpf-daemon_0.1.0_amd64.deb
```

The daemon links the exporter library statically (built from the sibling directory), so it does
not depend on `ros2ebpf-exporter`; install that one only for the standalone binary / headers.
`postinst` creates the `ros2ebpf` group and runs `systemctl daemon-reload`; it does not enable the
unit. Container images: `docker/Dockerfile.daemon`, `docker/Dockerfile.cli` (`make images`).

The CLI is released like any ROS 2 Python package (bloom / buildfarm), or `pip install ./ros2ebpf`.

## Deploy sources of truth

`deploy/prometheus/` and `deploy/grafana/` are the sources; `make k8s-sync` copies them into
`deploy/k8s/central/kustomize/plain/files/` (kustomize cannot read outside its directory) and
regenerates `deploy/k8s/central/helm/prometheusrule-ros2.yaml`. Run it after editing rules or
dashboards and commit the result; `make lint` runs it and validates everything.

## CI

`.github/workflows/ci.yaml`: native build + unit tests, a real load of the BPF objects on the
runner kernel (verifier check, `/metrics` smoke test), CLI tests + flake8, container image builds
with a privileged smoke test, and validation of everything under `deploy/` (promtool, amtool,
kustomize, compose, alloy fmt).
