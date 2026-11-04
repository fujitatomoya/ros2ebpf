#!/bin/bash
# Runs a talker, a listener and a service pair until stopped.
set -e
ros2 run demo_nodes_cpp talker --ros-args -r __node:=talker &
ros2 run demo_nodes_cpp listener --ros-args -r __node:=listener &
ros2 run demo_nodes_cpp add_two_ints_server &
sleep 2
# a client that calls the service once per second
while true; do
  ros2 run demo_nodes_cpp add_two_ints_client >/dev/null 2>&1 || true
  sleep 1
done
