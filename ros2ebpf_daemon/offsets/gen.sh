#!/bin/sh
# Prints the struct offsets ros2ebpfd needs for the ROS 2 installation on this
# machine, so a new (distro, arch) row can be added to src/offsets.c (or
# `offset_table:` overridden) for from-source builds or new distros.
#
#   usage: gen.sh [/opt/ros/<distro>]
#
# Requires a C compiler and the rmw headers of that installation.
set -e
ROS_ROOT=${1:-${ROS_DISTRO:+/opt/ros/$ROS_DISTRO}}
if [ -z "$ROS_ROOT" ] || [ ! -d "$ROS_ROOT/include" ]; then
  echo "usage: $0 /opt/ros/<distro>   (or set ROS_DISTRO)" >&2
  exit 2
fi
DISTRO=$(basename "$ROS_ROOT")
ARCH=$(uname -m)
INC="-I$ROS_ROOT/include -I$ROS_ROOT/include/rmw -I$ROS_ROOT/include/rcutils -I$ROS_ROOT/include/rosidl_runtime_c -I$ROS_ROOT/include/rosidl_typesupport_interface"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/off.c" <<'C'
#include <stddef.h>
#include <stdio.h>
#include <rmw/types.h>
int main(void) {
  printf("pub.topic_name=%zu\nsub.topic_name=%zu\nclient.service_name=%zu\nservice.service_name=%zu\n"
         "message_info.publication_sequence_number=%zu\nsizeof(rmw_message_info_t)=%zu\n",
         offsetof(rmw_publisher_t, topic_name), offsetof(rmw_subscription_t, topic_name),
         offsetof(rmw_client_t, service_name), offsetof(rmw_service_t, service_name),
         offsetof(rmw_message_info_t, publication_sequence_number), sizeof(rmw_message_info_t));
  return 0;
}
C
${CC:-cc} $INC "$TMP/off.c" -o "$TMP/off"
echo "# distro=$DISTRO arch=$ARCH"
"$TMP/off"
NAME=$("$TMP/off" | sed -n 's/^pub.topic_name=//p')
SEQ=$("$TMP/off" | sed -n 's/^message_info.publication_sequence_number=//p')
echo "# src/offsets.c row:"
echo "    { \"$DISTRO\", \"$ARCH\", $NAME, $SEQ },"
