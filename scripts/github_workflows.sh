#!/bin/bash

#####################################################################
# ros2ebpf: ROS 2 endpoint statistics via eBPF.
#
# This script builds and tests ros2ebpf against a ROS 2 Rolling *source*
# build inside the ros2dev container image, see
# https://github.com/fujitatomoya/ros2_devenv_builder
#
# ros2ebpf follows rolling only (fujitatomoya/ros2ebpf#8, #15): the daemon
# probes librmw_implementation.so through struct offsets of rmw, and the
# CLI is a ros2cli plugin. Mainline head of rmw, rmw_implementation,
# ros2cli and demo_nodes_cpp (with everything they need) is built from
# source here; then the offset table in ros2ebpf_daemon/src/offsets.c is
# checked against the mainline rmw headers, the daemon attaches its
# uprobes to the mainline-built librmw_implementation.so, `ros2 ebpf
# start` / `stop` on the mainline ros2cli drive its lifecycle, and the
# end-to-end test runs the mainline demo nodes under the daemon and
# asserts on the metrics.
#
# To avoid updating and modifying the files under `.github/workflows`,
# this script should be adjusted accordingly, and `.github/workflows`
# just calls this script in the workflow pipeline. The same script runs
# locally (privileged for the BPF programs, host pid namespace so the daemon
# can resolve the pids the BPF programs report, as in any containerised deployment):
#
#   docker run --rm --privileged --pid host -v "$PWD":/src:ro -w /src \
#     tomoyafujita/ros2dev:rolling ./scripts/github_workflows.sh
#
# The interactive version of the same build, in the same container, is
# README section 4 (development container); docs/development.md has the rest.
#
#####################################################################

########################
# Function Definitions #
########################

function mark {
    export $1=`pwd`;
}

function exit_trap() {
    # shellcheck disable=SC2317  # Don't warn about unreachable commands in this function
    if [ $? != 0 ]; then
        echo "Command [$BASH_COMMAND] is failed"
        [ -f /tmp/ros2ebpfd.log ] && { echo "--- ros2ebpfd log (tail)"; tail -40 /tmp/ros2ebpfd.log; }
        exit 1
    fi
}

function install_prerequisites () {
    trap exit_trap ERR
    echo "[${FUNCNAME[0]}]: update and install dependent packages."
    apt update && apt upgrade -y
    # the ros2dev image has every ROS build dependency; the BPF toolchain is ours.
    # python3-colorama: ros2cli mainline dependency newer than the image's rosdep run.
    apt install -y git clang llvm libbpf-dev bpftool libelf-dev zlib1g-dev \
        libyaml-dev libjson-c-dev pkg-config python3-pytest python3-flake8 python3-yaml \
        python3-colorama
    clang --version | head -1
    bpftool version
    cd $there
}

function setup_colcon_env () {
    trap exit_trap ERR
    echo "[${FUNCNAME[0]}]: set up colcon build environment."
    mkdir -p ${COLCON_WORKSPACE}/src
    cd ${COLCON_WORKSPACE}
    # mainline source; rmw, rmw_implementation, ros2cli and demo_nodes_cpp with their dependencies are built below
    vcs import --shallow --input https://raw.githubusercontent.com/ros2/ros2/${ROS_DISTRO}/ros2.repos src
    # One RMW (Fast DDS, the default) is all the daemon needs to be probed through; the other RMW
    # implementations and their vendored middlewares (Zenoh's Rust build above all) cost most of
    # the disk and time of a full build and do not fit a GitHub-hosted runner.
    local skip
    for skip in ros2/rmw_zenoh ros2/rmw_connextdds ros2/rmw_cyclonedds eclipse-cyclonedds/cyclonedds \
                eclipse-iceoryx/iceoryx ros2/rmw_fastrtps/rmw_fastrtps_dynamic_cpp; do
        [ -d "src/${skip}" ] && touch "src/${skip}/COLCON_IGNORE"
    done
    df -h /tmp | tail -1
    # move ros2ebpf directory to colcon workspace (without a previous local build)
    rm -rf ${COLCON_WORKSPACE}/src/ros2ebpf
    cp -rf $there ${COLCON_WORKSPACE}/src/ros2ebpf
    rm -rf ${COLCON_WORKSPACE}/src/ros2ebpf/{build,install,log}
}

function build_colcon_package () {
    trap exit_trap ERR
    echo "[${FUNCNAME[0]}]: build rmw, rmw_implementation, ros2cli, demo_nodes_cpp (mainline) and the ros2ebpf packages."
    cd ${COLCON_WORKSPACE}
    # Mainline first: ros2cli pulls rclpy, rcl and rmw_implementation (with Fast DDS),
    # demo_nodes_cpp adds rclcpp. Release and no test targets keep it within the runner's disk;
    # the build trees are dropped afterwards, install/ is all the rest needs.
    # RMW_IMPLEMENTATION_DISABLE_RUNTIME_SELECTION=OFF: with a single RMW available,
    # rmw_implementation would link to it directly and not build librmw_implementation.so, the
    # library the daemon probes.
    colcon build --merge-install --event-handlers console_cohesion+ \
        --packages-up-to rmw ros2cli demo_nodes_cpp \
        --cmake-args -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
                     -DRMW_IMPLEMENTATION_DISABLE_RUNTIME_SELECTION=OFF
    du -sh build install | tr '\n' ' '; echo; df -h /tmp | tail -1
    rm -rf build
    # then the ros2ebpf packages with their tests
    colcon build --merge-install --event-handlers console_cohesion+ \
        --packages-select ${ROS2EBPF_PACKAGES} \
        --cmake-args -DCMAKE_BUILD_TYPE=Debug
    ./install/bin/ros2ebpfd --version
}

function test_colcon_package () {
    trap exit_trap ERR
    echo "[${FUNCNAME[0]}]: test the ros2ebpf packages."
    cd ${COLCON_WORKSPACE}
    source ./install/setup.bash
    colcon test --merge-install --event-handlers console_cohesion+ --packages-select ${ROS2EBPF_PACKAGES}
    colcon test-result --verbose
    (cd src/ros2ebpf/ros2ebpf && python3 -m flake8 --select=E,W,F --extend-ignore=W503 --max-line-length=120 ros2ebpf test)
}

function check_rmw_offsets () {
    trap exit_trap ERR
    echo "[${FUNCNAME[0]}]: compare src/offsets.c with the rmw headers of mainline head."
    cd ${COLCON_WORKSPACE}
    local arch
    arch=$(uname -m)
    local expected
    expected=$(sed -n "s/.*{ *\"${ROS_DISTRO}\", *\"${arch}\", *\([0-9]*\), *\([0-9]*\) *}.*/\1 \2/p" \
        src/ros2ebpf/ros2ebpf_daemon/src/offsets.c)
    local out
    out=$(sh src/ros2ebpf/ros2ebpf_daemon/offsets/gen.sh "${COLCON_WORKSPACE}/install")
    echo "$out"
    local actual
    actual=$(echo "$out" | sed -n 's/.*{ *"[^"]*", *"[^"]*", *\([0-9]*\), *\([0-9]*\) *}.*/\1 \2/p')
    if [ -z "$expected" ] || [ "$expected" != "$actual" ]; then
        echo "offset table row (${ROS_DISTRO}, ${arch}) = [${expected}] does not match mainline rmw = [${actual}]: update src/offsets.c"
        exit 1
    fi
    echo "offsets for (${ROS_DISTRO}, ${arch}) match mainline rmw: ${actual}"
}

function start_daemon () {
    # `ros2 ebpf start` (mainline ros2cli) spawns the daemon detached, as root here, so no sudo.
    # The container runs privileged in the host pid namespace (see .github/workflows/ci.yaml):
    # the daemon loads its BPF objects on the runner kernel (verifier check) and attaches the
    # uprobes to the librmw_implementation.so that was just built from mainline source.
    trap exit_trap ERR
    cd ${COLCON_WORKSPACE}
    ros2 ebpf --socket /tmp/r2e.sock start -c /tmp/ci.yaml --log-level info \
        --log-file /tmp/ros2ebpfd.log --timeout 30
    DAEMON_PID=$(./install/bin/ros2ebpf-ctl -s /tmp/r2e.sock status | sed -n 's/^ *"pid": *\([0-9]*\).*/\1/p' | head -1)
    [ -n "${DAEMON_PID}" ] || { echo "no pid in status"; exit 1; }
    echo "ros2ebpfd pid ${DAEMON_PID}"
}

function stop_daemon () {
    trap exit_trap ERR
    cd ${COLCON_WORKSPACE}
    ros2 ebpf --socket /tmp/r2e.sock stop
    if [ -n "${DAEMON_PID:-}" ] && kill -0 ${DAEMON_PID} 2>/dev/null; then
        echo "ros2ebpfd (pid ${DAEMON_PID}) still alive after stop"
        exit 1
    fi
}

function verify_daemon () {
    trap exit_trap ERR
    echo "[${FUNCNAME[0]}]: load the BPF objects, probe the mainline librmw_implementation.so, query the daemon."
    cd ${COLCON_WORKSPACE}
    mount -t bpf bpf /sys/fs/bpf 2>/dev/null || true
    # the shim of the source build is not under /opt/ros/: the daemon takes it for rolling
    cat > /tmp/ci.yaml <<YAML
host: e2e
shims:
  - path: ${COLCON_WORKSPACE}/install/lib/librmw_implementation.so
discover_shims: false
exporter: {listen: 127.0.0.1:19477}
control: {socket: /tmp/r2e.sock}
pin_root: /sys/fs/bpf/ros2ebpf-ci
YAML
    ./install/bin/ros2ebpfd --check -c /tmp/ci.yaml
    start_daemon
    ./install/bin/ros2ebpf-ctl -s /tmp/r2e.sock status
    local metrics
    metrics=$(./install/bin/ros2ebpf-ctl -s /tmp/r2e.sock metrics)
    echo "$metrics" | grep -E '^ros2ebpf_(build_info|map_abi|host_info|probes_attached|probes_configured)'
    local attached configured
    attached=$(echo "$metrics" | sed -n 's/^ros2ebpf_probes_attached{[^}]*} //p')
    configured=$(echo "$metrics" | sed -n 's/^ros2ebpf_probes_configured{[^}]*} //p')
    if [ -z "$attached" ] || [ "$attached" != "$configured" ]; then
        echo "probe links attached ($attached) != configured ($configured)"
        exit 1
    fi
    echo "$metrics" | grep -q "offset_table=\"${ROS_DISTRO}/" || { echo "offset table ${ROS_DISTRO}/* not in use"; exit 1; }
    # the CLI on the mainline ros2cli
    ros2 ebpf --help > /dev/null
    ROS2EBPF_SOCKET=/tmp/r2e.sock ros2 ebpf status
}

function end_to_end () {
    # Mainline demo nodes (talker, listener, add_two_ints server + a client once per second) run
    # under the daemon; assertions on `ros2ebpf-ctl metrics`.
    trap exit_trap ERR
    echo "[${FUNCNAME[0]}]: end-to-end with the mainline demo nodes."
    cd ${COLCON_WORKSPACE}
    local bin=${COLCON_WORKSPACE}/install/lib/demo_nodes_cpp
    export ROS_DOMAIN_ID=5
    $bin/talker > /dev/null 2>&1 &
    $bin/listener > /dev/null 2>&1 &
    $bin/add_two_ints_server > /dev/null 2>&1 &
    ( while true; do $bin/add_two_ints_client > /dev/null 2>&1 || true; sleep 1; done ) &
    DEMO_PIDS=$(jobs -p | tr '\n' ' ')
    sleep 10

    m() { ./install/bin/ros2ebpf-ctl -s /tmp/r2e.sock metrics; }
    val() { m | grep -F "$1" | grep -v '^#' | awk '{print $NF}' | head -1; }
    local fail=0
    check() {  # check <description> <grep pattern>
        if m | grep -qE "$2"; then echo "PASS $1"; else echo "FAIL $1 (pattern: $2)"; fail=1; fi
    }
    check "talker publishes /chatter in domain ${ROS_DOMAIN_ID}" "^ros2_rmw_publish_total\{host=\"e2e\",domain_id=\"${ROS_DOMAIN_ID}\",comm=\"talker\",topic=\"/chatter\"\} [1-9]"
    check "listener takes /chatter"                     "^ros2_rmw_take_total\{host=\"e2e\",domain_id=\"${ROS_DOMAIN_ID}\",comm=\"listener\",topic=\"/chatter\"\} [1-9]"
    check "service requests on the server side"         "^ros2_rmw_service_request_total\{.*comm=\"add_two_ints_se\",service=\"/add_two_ints\",side=\"server\"\} [1-9]"
    check "service responses on the server side"        "^ros2_rmw_service_response_total\{.*comm=\"add_two_ints_se\",service=\"/add_two_ints\",side=\"server\"\} [1-9]"
    check "publish period histogram has samples"        "^ros2_rmw_publish_period_seconds_count\{.*comm=\"talker\",topic=\"/chatter\"\} [1-9]"
    if m | grep -q 'topic="/rosout"'; then echo "FAIL /rosout should be excluded"; fail=1; else echo "PASS /rosout excluded"; fi
    check "process identity"                            "^ros2_process_info\{host=\"e2e\",comm=\"talker\",pid=\"[0-9]+\",domain_id=\"${ROS_DOMAIN_ID}\",rmw=\"rmw_[a-z_]+\""
    check "host info with the ${ROS_DISTRO} offset table" "^ros2ebpf_host_info\{.*offset_table=\"${ROS_DISTRO}/[a-z0-9_]+\""
    local nr
    nr=$(val 'ros2ebpf_name_read_failures_total{host="e2e"}')
    if [ "$nr" = "0" ]; then echo "PASS no name read failures"; else echo "FAIL name read failures: $nr"; fail=1; fi

    local c1 c2
    c1=$(val 'ros2_rmw_publish_total{host="e2e",domain_id="'${ROS_DOMAIN_ID}'",comm="talker",topic="/chatter"}'); sleep 3
    c2=$(val 'ros2_rmw_publish_total{host="e2e",domain_id="'${ROS_DOMAIN_ID}'",comm="talker",topic="/chatter"}')
    if [ "${c2:-0}" -gt "${c1:-0}" ]; then echo "PASS counter grows ($c1 -> $c2)"; else echo "FAIL counter did not grow ($c1 -> $c2)"; fail=1; fi

    # process exit sweep: short-lived add_two_ints_client pids must not accumulate
    local procs
    procs=$(m | grep -c '^ros2_process_info' || true)
    if [ "$procs" -le 8 ]; then echo "PASS exited processes are swept ($procs process series)"; else echo "FAIL $procs process series: exit sweep not working"; fail=1; fi

    # enable / disable round trip through the control socket
    ./install/bin/ros2ebpf-ctl -s /tmp/r2e.sock disable take > /dev/null
    if ./install/bin/ros2ebpf-ctl -s /tmp/r2e.sock status | tr -d ' \n' | grep -q '"take":{"configured":false,"enabled":false'; then echo "PASS disable take -> family off"; else echo "FAIL disable take"; fail=1; fi
    ./install/bin/ros2ebpf-ctl -s /tmp/r2e.sock status -q && echo "PASS status stays healthy (family no longer requested)" || { echo "FAIL healthy after disable"; fail=1; }
    ./install/bin/ros2ebpf-ctl -s /tmp/r2e.sock enable take > /dev/null
    if ./install/bin/ros2ebpf-ctl -s /tmp/r2e.sock status | tr -d ' \n' | grep -q '"take":{"configured":true,"enabled":true,"attached":2,"expected":2'; then echo "PASS enable take -> 2/2 links"; else echo "FAIL re-enable take"; fail=1; fi

    # stop / start: counters survive through the pinned maps
    local before after
    before=$(val 'ros2_rmw_publish_total{host="e2e",domain_id="'${ROS_DOMAIN_ID}'",comm="talker",topic="/chatter"}')
    stop_daemon
    start_daemon; sleep 3
    after=$(val 'ros2_rmw_publish_total{host="e2e",domain_id="'${ROS_DOMAIN_ID}'",comm="talker",topic="/chatter"}')
    if [ "${after:-0}" -ge "${before:-1}" ]; then echo "PASS counters survived daemon restart ($before -> $after)"; else echo "FAIL counters reset on restart ($before -> $after)"; fail=1; fi

    # the CLI against the running daemon
    ROS2EBPF_SOCKET=/tmp/r2e.sock ros2 ebpf status
    ROS2EBPF_SOCKET=/tmp/r2e.sock ros2 ebpf top --once
    # the TUI: one plain frame, then the real curses UI for a few seconds under a pseudo-terminal
    local frame
    frame=$(ROS2EBPF_SOCKET=/tmp/r2e.sock ros2 ebpf monitor --once --interval 2)
    echo "$frame"
    echo "$frame" | grep -q 'local: healthy 16/16 links' || { echo "FAIL monitor --once health line"; fail=1; }
    echo "$frame" | grep -qE 'publisher +/chatter +5 +talker' || { echo "FAIL monitor --once talker row"; fail=1; }
    echo "$frame" | grep -qE 'subscription +/chatter +5 +listener' || { echo "FAIL monitor --once listener row"; fail=1; }
    ROS2EBPF_SOCKET=/tmp/r2e.sock ros2 ebpf monitor --once --view health | grep -q 'healthy: yes' || { echo "FAIL monitor --once health view"; fail=1; }
    ROS2EBPF_SOCKET=/tmp/r2e.sock ros2 ebpf monitor --once --view processes | grep -qE '[0-9]+ +talker +5 +rmw_' || { echo "FAIL monitor --once processes view"; fail=1; }
    # `script` gives the UI a pseudo-terminal and forwards its stdin to it: switch views, then quit
    # with `q`. `timeout --foreground` keeps the UI in the foreground process group (a background
    # one reading the tty is stopped by SIGTTIN); -k is the safety net.
    rm -f /tmp/monitor.tty
    ( sleep 4; printf 'p'; sleep 1; printf 'h'; sleep 1; printf 'e'; sleep 1; printf 'q' ) | \
        TERM=xterm-256color COLUMNS=160 LINES=40 script -qec \
        "ROS2EBPF_SOCKET=/tmp/r2e.sock timeout --foreground -k 3 20 ros2 ebpf monitor --interval 1" /tmp/monitor.tty > /dev/null
    local tty_rc=$?
    if [ "$tty_rc" = 0 ] && grep -aq '/chatter' /tmp/monitor.tty && grep -aq 'Pub Hz' /tmp/monitor.tty \
       && grep -aq 'healthy: yes' /tmp/monitor.tty; then
        echo "PASS curses monitor under a pty: endpoints, processes and health views, quit with q"
    else
        echo "FAIL curses monitor under a pty (exit $tty_rc)"; sed 's/\x1b\[[0-9;?]*[A-Za-z]//g' /tmp/monitor.tty | tail -20; fail=1
    fi

    stop_daemon
    # only the processes started here: the container shares the host pid namespace
    kill ${DEMO_PIDS} 2>/dev/null || true
    wait 2>/dev/null || true
    if [ "$fail" = 0 ]; then echo "e2e: all checks passed"; else echo "e2e: FAILED"; exit 1; fi
}

########
# Main #
########

export DEBIAN_FRONTEND=noninteractive
export COLCON_WORKSPACE=/tmp/colcon_ws
# rolling only (#15): the branch of ros2.repos, and the row of the offset table that is checked
export ROS_DISTRO=rolling
export ROS2EBPF_PACKAGES="ros2ebpf_exporter ros2ebpf_daemon ros2ebpf_interfaces ros2ebpf"

# mark the working space root directory, so that we can come back anytime with `cd $there`
mark there

# set the trap on error
trap exit_trap ERR

# call functions in sequence
install_prerequisites
setup_colcon_env
build_colcon_package
test_colcon_package
check_rmw_offsets
verify_daemon
end_to_end

exit 0
