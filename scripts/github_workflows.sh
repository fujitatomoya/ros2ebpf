#!/bin/bash

#####################################################################
# ros2ebpf: ROS 2 endpoint statistics via eBPF.
#
# This script builds and tests ros2ebpf against a ROS 2 Rolling *source*
# build inside the ros2dev container image, see
# https://github.com/fujitatomoya/ros2_devenv_builder
#
# ros2ebpf follows rolling only (fujitatomoya/ros2ebpf#8): the daemon
# probes librmw_implementation.so through struct offsets of rmw, and the
# CLI is a ros2cli plugin. Mainline head of rmw, rmw_implementation and
# ros2cli (with everything they need, about 140 packages) is built from
# source here, the offset table in ros2ebpf_daemon/src/offsets.c is
# checked against the mainline rmw headers, the daemon attaches its
# uprobes to the mainline-built librmw_implementation.so, and `ros2 ebpf`
# runs on the mainline ros2cli.
#
# To avoid updating and modifying the files under `.github/workflows`,
# this script should be adjusted accordingly, and `.github/workflows`
# just calls this script in the workflow pipeline. The same script runs
# locally with `make in-docker-ci`.
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
    # mainline source (rmw, rmw_implementation and ros2cli with their dependencies are built below)
    vcs import --shallow --input https://raw.githubusercontent.com/ros2/ros2/${ROS_DISTRO}/ros2.repos src
    # move ros2ebpf directory to colcon workspace (without a previous local build)
    rm -rf ${COLCON_WORKSPACE}/src/ros2ebpf
    cp -rf $there ${COLCON_WORKSPACE}/src/ros2ebpf
    rm -rf ${COLCON_WORKSPACE}/src/ros2ebpf/{build,install,log}
}

function build_colcon_package () {
    trap exit_trap ERR
    echo "[${FUNCNAME[0]}]: build rmw, rmw_implementation, ros2cli (mainline) and the ros2ebpf packages."
    cd ${COLCON_WORKSPACE}
    # ros2ebpf (the CLI) depends on ros2cli, which pulls rclpy, rcl, rmw_implementation and the
    # RMW implementations: that is the rolling source build the daemon is tested against below.
    colcon build --merge-install --event-handlers console_cohesion+ \
        --packages-up-to rmw ${ROS2EBPF_PACKAGES} \
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

function load_bpf_objects () {
    # The container runs privileged (see .github/workflows/ci.yaml): the daemon loads its BPF
    # objects on the runner kernel (verifier check) and attaches the uprobes to the
    # librmw_implementation.so that was just built from mainline source. Then the daemon and the
    # `ros2 ebpf` CLI (on the mainline ros2cli) are queried through the control socket.
    trap exit_trap ERR
    echo "[${FUNCNAME[0]}]: load the BPF objects, probe the mainline librmw_implementation.so, query the daemon."
    cd ${COLCON_WORKSPACE}
    mount -t bpf bpf /sys/fs/bpf 2>/dev/null || true
    cat > /tmp/ci.yaml <<YAML
shims:
  - path: ${COLCON_WORKSPACE}/install/lib/librmw_implementation.so
    distro: ${ROS_DISTRO}
discover_shims: false
exporter: {listen: 127.0.0.1:19477}
control: {socket: /tmp/r2e.sock}
pin_root: /sys/fs/bpf/ros2ebpf-ci
YAML
    ./install/bin/ros2ebpfd --check -c /tmp/ci.yaml
    timeout 20 ./install/bin/ros2ebpfd -c /tmp/ci.yaml --log-level info &
    sleep 5
    ./install/bin/ros2ebpf-ctl -s /tmp/r2e.sock status        # exit code 1 unless healthy
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
    wait || true
}

function build_debian_packages () {
    trap exit_trap ERR
    echo "[${FUNCNAME[0]}]: build the Debian packages with cpack."
    cd ${COLCON_WORKSPACE}/src/ros2ebpf
    make deb BUILD=${COLCON_WORKSPACE}/deb ROS_SETUP=${COLCON_WORKSPACE}/install/setup.sh
    dpkg-deb -c ${COLCON_WORKSPACE}/deb/pkg/ros2ebpf-daemon_*.deb | grep -E 'usr/bin/ros2ebpfd|usr/lib/systemd/system/ros2ebpfd.service|etc/ros2ebpf/config.example.yaml'
    dpkg-deb -c ${COLCON_WORKSPACE}/deb/pkg/ros2ebpf-exporter_*.deb | grep -E 'usr/lib/libros2ebpf_exporter.a|usr/include/ros2ebpf/maps.h'
}

########
# Main #
########

export DEBIAN_FRONTEND=noninteractive
export COLCON_WORKSPACE=/tmp/colcon_ws
export ROS_DISTRO=${ROS_DISTRO:-rolling}
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
load_bpf_objects
build_debian_packages

exit 0
