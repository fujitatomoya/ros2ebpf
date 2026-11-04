# Struct offset table

`ros2ebpfd` reads two things out of process memory from inside the probe:

| What | Where | Why |
|---|---|---|
| `topic_name` / `service_name` | `rmw_publisher_t`, `rmw_subscription_t`, `rmw_client_t`, `rmw_service_t` (arg 0 of every `rmw_*` call) | the label on every series |
| `publication_sequence_number` | `rmw_message_info_t` (arg 3 of `rmw_take_with_info`) | sequence-gap detection |

These are ABI, not API. The offsets are the same (16 and 16) on every LP64
target from Humble to Rolling because the handle layout
`{ implementation_identifier, data, <name> }` has not changed, but the daemon
still keys them by `(distro, arch)` in `src/offsets.c` and exports the row it
used as `ros2ebpf_host_info{offset_table="..."}` so a future change on one
distro is caught per host instead of silently misreading names.

A mismatch shows up as `ros2ebpf_name_read_failures_total` increasing and/or
`ros2ebpf_unknown_rmw_processes > 0` (the identifier read from offset 0 does
not start with `rmw_`).

## Adding a row

On a machine with the target ROS 2 installation and a C compiler:

```
./gen.sh /opt/ros/<distro>
# distro=jazzy arch=x86_64
pub.topic_name=16
...
# src/offsets.c row:
    { "jazzy", "x86_64", 16, 16 },
```

Paste the row into the table in `src/offsets.c`, or set
`offset_table: <distro>/<arch>` in `config.yaml` to force an existing row, or
rely on the `lp64` fallback row (used with a warning when the distro is unknown).
