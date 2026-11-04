#!/bin/bash
# End-to-end test with containers only:
#   - r2e-demo:   ROS 2 (demo_nodes_cpp talker/listener/add_two_ints) in its own container
#   - r2e-daemon: ros2ebpfd in another container, host pid namespace, attaching to the
#                 demo container's librmw_implementation.so through /proc/<pid>/root
# Asserts that publish / take / service counters for the demo nodes appear and grow.
set -euo pipefail
DOMAIN=${ROS_DOMAIN_ID:-5}
cleanup() { docker rm -f r2e-demo r2e-daemon >/dev/null 2>&1 || true; }
trap cleanup EXIT
cleanup

docker run -d --name r2e-demo --network host -e ROS_DOMAIN_ID="$DOMAIN" ros2ebpf-demo:local >/dev/null
docker run -d --name r2e-daemon --pid host --privileged \
  -v /sys/fs/bpf:/sys/fs/bpf -v /sys/kernel/btf:/sys/kernel/btf:ro -v /etc/machine-id:/etc/machine-id:ro \
  -p 9477:9477 -e ROS2EBPF_HOST=e2e ros2ebpfd:local >/dev/null

m() { docker exec r2e-daemon ros2ebpf-ctl metrics; }
val() { m | grep -F "$1" | grep -v '^#' | awk '{print $NF}' | head -1; }

echo "waiting for the daemon to attach ..."
for i in $(seq 1 30); do
  if docker exec r2e-daemon ros2ebpf-ctl status -q 2>/dev/null; then break; fi
  sleep 1
done
docker exec r2e-daemon ros2ebpf-ctl status -q || { docker logs r2e-daemon; echo "daemon unhealthy"; exit 1; }
sleep 8

fail=0
check() {  # check <description> <grep pattern>
  if m | grep -qE "$2"; then echo "PASS $1"; else echo "FAIL $1 (pattern: $2)"; fail=1; fi
}
check "talker publishes /chatter in domain $DOMAIN" "^ros2_rmw_publish_total\{host=\"e2e\",domain_id=\"$DOMAIN\",comm=\"talker\",topic=\"/chatter\"\} [1-9]"
check "listener takes /chatter"                     "^ros2_rmw_take_total\{host=\"e2e\",domain_id=\"$DOMAIN\",comm=\"listener\",topic=\"/chatter\"\} [1-9]"
check "service requests on the server side"         "^ros2_rmw_service_request_total\{.*comm=\"add_two_ints_se\",service=\"/add_two_ints\",side=\"server\"\} [1-9]"
check "service responses on the server side"        "^ros2_rmw_service_response_total\{.*comm=\"add_two_ints_se\",service=\"/add_two_ints\",side=\"server\"\} [1-9]"
check "publish period histogram has samples"        "^ros2_rmw_publish_period_seconds_count\{.*comm=\"talker\",topic=\"/chatter\"\} [1-9]"
check "/rosout excluded by default"                 "^ros2ebpf_map_entries"   # presence; exclusion checked below
if m | grep -q 'topic="/rosout"'; then echo "FAIL /rosout should be excluded"; fail=1; else echo "PASS /rosout excluded"; fi
check "process identity with container id"         "^ros2_process_info\{host=\"e2e\",comm=\"talker\",pid=\"[0-9]+\",domain_id=\"$DOMAIN\",rmw=\"rmw_[a-z_]+\",container=\"[0-9a-f]{12}\""
check "host info with discovered offset table"     "^ros2ebpf_host_info\{.*offset_table=\"jazzy/[a-z0-9_]+\""
check "all configured probes attached"             "^ros2ebpf_probes_attached\{host=\"e2e\"\} ([0-9]+)$"
att=$(val 'ros2ebpf_probes_attached{host="e2e"}'); cfg=$(val 'ros2ebpf_probes_configured{host="e2e"}')
if [ "$att" = "$cfg" ]; then echo "PASS probes attached == configured ($att)"; else echo "FAIL probes attached $att != configured $cfg"; fail=1; fi
nr=$(val 'ros2ebpf_name_read_failures_total{host="e2e"}')
if [ "$nr" = "0" ]; then echo "PASS no name read failures"; else echo "FAIL name read failures: $nr"; fail=1; fi

c1=$(val 'ros2_rmw_publish_total{host="e2e",domain_id="'$DOMAIN'",comm="talker",topic="/chatter"}'); sleep 3
c2=$(val 'ros2_rmw_publish_total{host="e2e",domain_id="'$DOMAIN'",comm="talker",topic="/chatter"}')
if [ "$c2" -gt "$c1" ]; then echo "PASS counter grows ($c1 -> $c2)"; else echo "FAIL counter did not grow"; fail=1; fi

# process exit sweep: short-lived add_two_ints_client pids must not accumulate
procs=$(m | grep -c '^ros2_process_info' || true)
if [ "$procs" -le 8 ]; then echo "PASS exited processes are swept ($procs process series)"; else echo "FAIL $procs process series: exit sweep not working"; fail=1; fi

# enable / disable round trip through the control socket
docker exec r2e-daemon ros2ebpf-ctl disable take >/dev/null
if docker exec r2e-daemon ros2ebpf-ctl status | tr -d ' \n' | grep -q '"take":{"configured":false,"enabled":false'; then echo "PASS disable take -> family off"; else echo "FAIL disable take"; fail=1; fi
docker exec r2e-daemon ros2ebpf-ctl status -q && echo "PASS status stays healthy (family no longer requested)" || { echo "FAIL healthy after disable"; fail=1; }
docker exec r2e-daemon ros2ebpf-ctl enable take >/dev/null
if docker exec r2e-daemon ros2ebpf-ctl status | tr -d ' \n' | grep -q '"take":{"configured":true,"enabled":true,"attached":2,"expected":2'; then echo "PASS enable take -> 2/2 links"; else echo "FAIL re-enable take"; fail=1; fi

# restart: counters survive through pinned maps
before=$(val 'ros2_rmw_publish_total{host="e2e",domain_id="'$DOMAIN'",comm="talker",topic="/chatter"}')
docker restart r2e-daemon >/dev/null; sleep 6
after=$(val 'ros2_rmw_publish_total{host="e2e",domain_id="'$DOMAIN'",comm="talker",topic="/chatter"}')
if [ "${after:-0}" -ge "${before:-1}" ]; then echo "PASS counters survived daemon restart ($before -> $after)"; else echo "FAIL counters reset on restart ($before -> $after)"; fail=1; fi

if [ "$fail" = 0 ]; then echo "e2e: all checks passed"; else echo "e2e: FAILED"; docker logs r2e-daemon | tail -30; exit 1; fi
