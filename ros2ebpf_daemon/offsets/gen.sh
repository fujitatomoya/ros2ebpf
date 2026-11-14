#!/bin/sh
# Prints the struct offsets ros2ebpfd needs for the ROS 2 Rolling installation
# on this machine, as the row of src/offsets.c for this architecture. CI runs
# it against the mainline source build on every commit and fails when the row
# in src/offsets.c differs (scripts/github_workflows.sh, check_rmw_offsets).
#
#   usage: gen.sh [/opt/ros/rolling | <colcon workspace>/install]
#
# Requires a C compiler and the rmw headers of that installation, in the
# merged layout (/opt/ros/rolling, or a workspace built with --merge-install).
set -e
ROS_ROOT=${1:-/opt/ros/rolling}
if [ ! -d "$ROS_ROOT/include" ]; then
  echo "usage: $0 [/opt/ros/rolling | <colcon workspace>/install]" >&2
  exit 2
fi
DISTRO=rolling
ARCH=$(uname -m)
INC="-I$ROS_ROOT/include -I$ROS_ROOT/include/rmw -I$ROS_ROOT/include/rcutils -I$ROS_ROOT/include/rosidl_runtime_c -I$ROS_ROOT/include/rosidl_typesupport_interface -I$ROS_ROOT/include/rosidl_dynamic_typesupport"
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
