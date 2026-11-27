#!/bin/sh
# Checks the <arch> rows of ros2ebpf_daemon/src/offsets.c against the rmw headers of the
# ROS 2 binaries of that architecture: for every distro with an <arch> row, offsets/gen.sh
# runs in the official ros:<distro>-ros-base image for linux/<arch> (under qemu-user on a
# host of another architecture) and must print the same values.
#
#   usage: scripts/check_offsets_arch.sh [aarch64 | x86_64]     (default aarch64)
#
# Runs on the host, not in the development container: it needs docker, and binfmt/qemu for
# a foreign architecture (docker/setup-qemu-action in CI, `docker run --privileged --rm
# tonistiigi/binfmt --install arm64` elsewhere). CI runs it for aarch64 in a minute, next to the
# arm64 job (the mainline source build on an aarch64 runner); the x86_64 rows are checked against
# the mainline source build by scripts/github_workflows.sh.
#
# This checks the struct offsets only, against the released binaries of each distro: the BPF
# programs are not loaded (qemu-user has no BPF).
set -e
ARCH=${1:-aarch64}
case "$ARCH" in
  aarch64) PLATFORM=linux/arm64 ;;
  x86_64) PLATFORM=linux/amd64 ;;
  *) echo "usage: $0 [aarch64 | x86_64]" >&2; exit 2 ;;
esac
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OFFSETS_C=$ROOT/ros2ebpf_daemon/src/offsets.c
ROWS=$(sed -n "s/.*{ *\"\([a-z]*\)\", *\"${ARCH}\", *\([0-9]*\), *\([0-9]*\) *}.*/\1 \2 \3/p" "$OFFSETS_C")
if [ -z "$ROWS" ]; then
  echo "no ${ARCH} row in src/offsets.c" >&2
  exit 1
fi
FAIL=0
echo "$ROWS" | {
  while read -r DISTRO NAME SEQ; do
    echo "== ${DISTRO}/${ARCH}: ros:${DISTRO}-ros-base (${PLATFORM})"
    OUT=$(docker run --rm --quiet --platform "$PLATFORM" -v "$ROOT/ros2ebpf_daemon/offsets:/offsets:ro" \
      "ros:${DISTRO}-ros-base" sh -c '
        command -v cc >/dev/null || { apt-get update -qq && apt-get install -y -qq gcc libc6-dev >/dev/null; } &&
        sh /offsets/gen.sh /opt/ros/'"$DISTRO") || { echo "gen.sh failed for ${DISTRO}/${ARCH}"; FAIL=1; continue; }
    echo "$OUT"
    ACTUAL=$(echo "$OUT" | sed -n 's/.*{ *"\([^"]*\)", *"\([^"]*\)", *\([0-9]*\), *\([0-9]*\) *}.*/\1 \2 \3 \4/p')
    if [ "$ACTUAL" != "$DISTRO $ARCH $NAME $SEQ" ]; then
      echo "offset table row (${DISTRO}, ${ARCH}) = [${NAME} ${SEQ}] does not match the ${DISTRO} binaries = [${ACTUAL}]: update src/offsets.c"
      FAIL=1
    else
      echo "offsets for (${DISTRO}, ${ARCH}) match the ${DISTRO} binaries: ${NAME} ${SEQ}"
    fi
  done
  exit $FAIL
}
