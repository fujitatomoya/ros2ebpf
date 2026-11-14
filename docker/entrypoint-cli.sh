#!/bin/bash
set -e
# shellcheck disable=SC1090
source /opt/ros/rolling/setup.bash
exec "$@"
