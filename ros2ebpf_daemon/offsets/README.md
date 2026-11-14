# Struct offset table

`ros2ebpfd` reads two things out of process memory from inside the probe:

| What | Where | Why |
|---|---|---|
| `topic_name` / `service_name` | `rmw_publisher_t`, `rmw_subscription_t`, `rmw_client_t`, `rmw_service_t` (arg 0 of every `rmw_*` call) | the label on every series |
| `publication_sequence_number` | `rmw_message_info_t` (arg 3 of `rmw_take_with_info`) | sequence-gap detection |

These are ABI, not API.
ros2ebpf follows ROS 2 Rolling only (#15), so `src/offsets.c` has one row per supported architecture, `rolling/x86_64` and `rolling/aarch64`.
CI regenerates the values from the `rmw` headers of mainline on every commit (`check_rmw_offsets` in `scripts/github_workflows.sh`) and fails when the row is stale, and the daemon exports the row it uses as `ros2ebpf_host_info{offset_table="rolling/<arch>"}`.
A shim under `/opt/ros/rolling/` or anywhere else (the `install/` of a source build) is Rolling; a shim under `/opt/ros/<other distro>/` is refused.

A mismatch (a Rolling install older or newer than the row) shows up as `ros2ebpf_name_read_failures_total` increasing and/or `ros2ebpf_unknown_rmw_processes > 0` (the identifier read from offset 0 does not start with `rmw_`).

## Updating the row

In the development container (README §4), or on any machine with the Rolling installation and a C compiler:

```
./gen.sh /ws/install            # or /opt/ros/rolling
# distro=rolling arch=x86_64
pub.topic_name=16
...
# src/offsets.c row:
    { "rolling", "x86_64", 16, 16 },
```

Replace the row in `src/offsets.c` and rebuild.
`gen.sh` needs the merged layout: `/opt/ros/rolling`, or a workspace built with `--merge-install`.
