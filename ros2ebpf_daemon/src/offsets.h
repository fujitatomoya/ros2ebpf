/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ROS2EBPF_OFFSETS_H
#define ROS2EBPF_OFFSETS_H
#include "ros2ebpf/maps.h"

struct offset_entry {
    const char *distro;
    const char *arch;
    __u32 name_off;          /* topic_name / service_name in every rmw handle */
    __u32 msginfo_pubseq;    /* rmw_message_info_t.publication_sequence_number */
};

/* The row for (distro, arch): "rolling" on x86_64 or aarch64, NULL for anything else. */
const struct offset_entry *offsets_lookup(const char *distro, const char *arch);
/* The <distro> of a shim path /opt/ros/<distro>/lib/librmw_implementation.so (also behind
 * /proc/<pid>/root). -1 when the path is not under /opt/ros/: a colcon install/ of a
 * source build, which ros2ebpf takes for Rolling. */
int offsets_distro_from_path(const char *path, char *out, size_t n);
/* The Rolling row of this machine (uname -m): what the config map holds before the first
 * shim is registered. */
const struct offset_entry *offsets_host_default(void);
/* Fills cfg fields from an entry. */
void offsets_apply(const struct offset_entry *e, struct ros2ebpf_config *cfg);
/* Verify that the shim ELF exports the symbols we attach to. Returns 0 or -1,
 * listing the missing ones in `missing` (comma separated). */
int offsets_check_shim(const char *path, char *missing, size_t n);

#endif
