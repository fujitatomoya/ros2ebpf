#!/bin/sh
# Install bpftool on Ubuntu from the kernel's linux-tools packages (run after apt-get update, as root).
#
# `linux-tools-generic` is a meta package that depends on linux-tools-<newest kernel>-generic; while
# the archive mirrors are syncing that versioned package can be missing and apt refuses the whole
# install. Pick the newest versioned package that is actually in the index instead: for building
# BPF skeletons any recent bpftool works, it does not have to match the running kernel.
set -eu
# Ubuntu 26.04 (resolute) and newer ship bpftool as a package of its own.
if apt-cache show bpftool >/dev/null 2>&1; then
    apt-get install -y --no-install-recommends bpftool
    bpftool version
    exit 0
fi
pkg=$(apt-cache search --names-only '^linux-tools-[0-9][0-9.]*-[0-9]*-generic$' | awk '{print $1}' | sort -V | tail -1)
[ -n "$pkg" ] || { echo "install-bpftool: no linux-tools-<version>-generic package in the apt index" >&2; exit 1; }
apt-get install -y --no-install-recommends linux-tools-common "$pkg"
ln -sf "$(ls -d /usr/lib/linux-tools/*/ | head -1)bpftool" /usr/local/bin/bpftool
bpftool version
