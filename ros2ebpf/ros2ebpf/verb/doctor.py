# Copyright 2026 Tomoya Fujita
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
`ros2 ebpf doctor`: local preflight checks that need no running daemon.

Reports what would fail at attach time: kernel version, BTF, bpffs,
capabilities, the shim(s) from config.yaml and their exported symbols, and
whether the daemon and the exporter endpoint answer.
"""

import os
import platform
import re
import struct
import urllib.request

import yaml

from ros2ebpf.api import ConnectError
from ros2ebpf.api import DEFAULT_CONFIG
from ros2ebpf.api import DEFAULT_SOCKET
from ros2ebpf.api import UnixClient
from ros2ebpf.verb import VerbExtension

REQUIRED_SYMBOLS = (
    'rmw_publish', 'rmw_take_with_info', 'rmw_send_request', 'rmw_take_response',
    'rmw_take_request', 'rmw_send_response', 'rmw_destroy_publisher',
    'rmw_destroy_subscription', 'rmw_destroy_client', 'rmw_destroy_service',
    'rmw_get_implementation_identifier',
)
CAP_SYS_ADMIN, CAP_SYS_PTRACE, CAP_PERFMON, CAP_BPF = 21, 19, 38, 39


def elf_dynamic_symbols(path):
    """Return the set of defined dynamic symbol names of an ELF64 file (tiny parser, no deps)."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'\x7fELF' or data[4] != 2:
        raise ValueError('not an ELF64 file')
    little = data[5] == 1
    e = '<' if little else '>'
    e_shoff, = struct.unpack_from(e + 'Q', data, 0x28)
    e_shentsize, e_shnum = struct.unpack_from(e + 'HH', data, 0x3A)
    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link = struct.unpack_from(
            e + 'IIQQQQI', data, off)
        sections.append((sh_type, sh_offset, sh_size, sh_link))
    names = set()
    for sh_type, sh_offset, sh_size, sh_link in sections:
        if sh_type != 11:  # SHT_DYNSYM
            continue
        _, str_off, str_size, _ = sections[sh_link]
        strtab = data[str_off:str_off + str_size]
        for off in range(sh_offset, sh_offset + sh_size, 24):
            st_name, st_info, st_other, st_shndx = struct.unpack_from(e + 'IBBH', data, off)
            if st_shndx == 0:
                continue
            end = strtab.find(b'\0', st_name)
            names.add(strtab[st_name:end].decode(errors='replace'))
    return names


def kernel_version():
    m = re.match(r'(\d+)\.(\d+)', platform.release())
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


def effective_caps():
    try:
        with open('/proc/self/status') as f:
            for line in f:
                if line.startswith('CapEff:'):
                    return int(line.split()[1], 16)
    except OSError:
        pass
    return 0


def bpffs_mounted(path='/sys/fs/bpf'):
    try:
        with open('/proc/mounts') as f:
            return any(line.split()[1] == path and line.split()[2] == 'bpf' for line in f)
    except OSError:
        return False


class DoctorVerb(VerbExtension):
    """Check kernel, bpffs, capabilities, shim symbols and daemon reachability; print what would fail."""

    def add_arguments(self, parser, cli_name):
        parser.add_argument('--config', default=DEFAULT_CONFIG, help='Daemon config to check (default: %(default)s)')
        parser.add_argument('--shim', action='append', default=[], help='Extra shim path(s) to check')

    def main(self, *, args):
        results = []

        def rep(level, what, detail=''):
            results.append((level, what, detail))
            print(f'[{level:<4}] {what}{": " + detail if detail else ""}')

        kv = kernel_version()
        rep('PASS' if kv >= (5, 8) else 'FAIL', f'kernel {platform.release()}',
            '>= 5.8 required (CAP_BPF, ring buffer)' if kv < (5, 8) else '')
        rep('PASS' if os.access('/sys/kernel/btf/vmlinux', os.R_OK) else 'FAIL',
            'BTF /sys/kernel/btf/vmlinux', '' if os.access('/sys/kernel/btf/vmlinux', os.R_OK)
            else 'missing: kernel built without CONFIG_DEBUG_INFO_BTF; CO-RE cannot work')
        rep('PASS' if bpffs_mounted() else 'FAIL', 'bpffs on /sys/fs/bpf',
            '' if bpffs_mounted() else 'mount -t bpf bpf /sys/fs/bpf')

        caps = effective_caps()
        if os.geteuid() == 0 or caps & (1 << CAP_SYS_ADMIN):
            rep('PASS', 'privileges', 'root / CAP_SYS_ADMIN')
        elif caps & (1 << CAP_BPF) and caps & (1 << CAP_PERFMON):
            rep('PASS', 'privileges', 'CAP_BPF + CAP_PERFMON'
                + ('' if caps & (1 << CAP_SYS_PTRACE) else ' (no CAP_SYS_PTRACE: domain_id will be unknown)'))
        else:
            rep('WARN', 'privileges',
                'this shell has no CAP_BPF; fine for the CLI, ros2ebpfd itself needs them '
                '(`ros2 ebpf start` runs it through sudo)')

        shims = list(args.shim)
        cfg = {}
        if os.path.exists(args.config):
            try:
                with open(args.config) as f:
                    cfg = yaml.safe_load(f) or {}
                rep('PASS', f'config {args.config}', 'parsed')
                shims += [s.get('path') for s in (cfg.get('shims') or []) if isinstance(s, dict) and s.get('path')]
            except (OSError, yaml.YAMLError) as e:
                rep('FAIL', f'config {args.config}', str(e))
        else:
            rep('WARN', f'config {args.config}', 'not found (daemon uses defaults + shim discovery)')
        if not shims:
            for d in sorted(os.listdir('/opt/ros')) if os.path.isdir('/opt/ros') else []:
                p = f'/opt/ros/{d}/lib/librmw_implementation.so'
                if os.path.exists(p):
                    shims.append(p)
            if shims:
                rep('INFO', 'shims', f'none configured; found {", ".join(shims)}')
        for p in shims:
            if not os.path.exists(p):
                rep('FAIL', f'shim {p}', 'not found')
                continue
            try:
                syms = elf_dynamic_symbols(p)
            except (OSError, ValueError, struct.error) as e:
                rep('FAIL', f'shim {p}', f'unreadable: {e}')
                continue
            missing = [s for s in REQUIRED_SYMBOLS if s not in syms]
            rep('PASS' if not missing else 'FAIL', f'shim {p}',
                f'{len(syms)} dynamic symbols' if not missing else f'missing {", ".join(missing)}')
        if cfg.get('discover_shims', True):
            rep('INFO', 'shim discovery', 'on: containers are found through /proc/<pid>/maps')

        sock = ((cfg.get('control') or {}).get('socket')) or args.socket or DEFAULT_SOCKET
        try:
            c = UnixClient(sock)
            st = c.call('status')
            c.close()
            rep('PASS' if st.get('healthy') else 'WARN', f'daemon at {sock}',
                f"version {st.get('version')}, {st.get('probes_attached')}/{st.get('probes_expected')} links, "
                f"{len(st.get('shims', []))} shim(s)" + ('' if st.get('healthy') else ' -- see `ros2 ebpf status -v`'))
            listen = (st.get('exporter') or {}).get('listen', '')
            mode = (st.get('exporter') or {}).get('mode')
            if mode == 'inprocess' and listen:
                host, _, port = listen.rpartition(':')
                url = f'http://{"127.0.0.1" if host in ("", "0.0.0.0", "[::]") else host}:{port}/metrics'
                try:
                    with urllib.request.urlopen(url, timeout=5) as r:
                        body = r.read().decode()
                    n = sum(1 for line in body.splitlines() if line and not line.startswith('#'))
                    rep('PASS', f'exporter {url}', f'{n} samples')
                except Exception as e:
                    rep('FAIL', f'exporter {url}',
                        f"{e} (running in a container? the port belongs to the daemon's network namespace)")
            elif mode == 'external':
                rep('INFO', 'exporter', 'external mode: check ros2ebpf-exporter separately')
        except ConnectError as e:
            rep('WARN', f'daemon at {sock}', f'{e}')

        fails = sum(1 for lvl, _, _ in results if lvl == 'FAIL')
        warns = sum(1 for lvl, _, _ in results if lvl == 'WARN')
        print(f'\n{fails} failure(s), {warns} warning(s)')
        return 1 if fails else 0
