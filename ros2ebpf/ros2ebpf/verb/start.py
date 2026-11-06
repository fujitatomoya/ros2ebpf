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

import os
import sys

from ros2ebpf import daemon
from ros2ebpf.verb import VerbExtension


class StartVerb(VerbExtension):
    """Start ros2ebpfd on this host, detached (through sudo when not root), and wait until it answers."""

    def add_arguments(self, parser, cli_name):
        parser.add_argument(
            '-c', '--config', default=daemon.DEFAULT_CONFIG,
            help='Daemon configuration file (default: %(default)s)')
        parser.add_argument(
            '--log-level', default=None, metavar='LEVEL',
            help='Daemon log level (debug, info, warn, error)')
        parser.add_argument(
            '--log-file', default=None, metavar='FILE',
            help='Where the daemon output goes (default: $ROS_LOG_DIR or ~/.ros/log, ros2ebpfd.log)')
        parser.add_argument(
            '--binary', default=None, metavar='PATH',
            help='ros2ebpfd to run (default: the one on PATH)')
        parser.add_argument(
            '--timeout', type=float, default=15.0, metavar='SECONDS',
            help='How long to wait for the control socket (default: %(default)s)')
        parser.add_argument(
            '--no-sudo', action='store_true',
            help='Run the daemon as this user even when not root (capabilities already granted)')

    def main(self, *, args):
        sock = args.socket
        st = daemon.status(sock)
        if st is not None:
            print(f"ros2ebpfd already running (pid {st.get('pid')}, {sock})")
            return 0
        try:
            binary = daemon.find_binary(args.binary)
        except FileNotFoundError as e:
            print(f'error: {e}', file=sys.stderr)
            return 2
        if not os.path.exists(args.config):
            print(f'error: {args.config} not found: copy config.example.yaml there, or pass -c',
                  file=sys.stderr)
            return 2
        log_file = args.log_file or daemon.default_log_file()
        argv = daemon.daemon_argv(binary, args.config, args.log_level,
                                  sudo=False if args.no_sudo else None)
        try:
            proc = daemon.spawn(argv, log_file)
        except OSError as e:
            print(f'error: cannot start {argv[0]}: {e}', file=sys.stderr)
            return 1
        try:
            st = daemon.wait_ready(proc, sock, args.timeout)
        except (RuntimeError, TimeoutError) as e:
            print(f'error: {e}', file=sys.stderr)
            log = daemon.tail(log_file)
            if log:
                print(f'--- {log_file}:\n{log}', file=sys.stderr)
            if argv[0] == 'sudo':
                print("hint: 'sudo -n' must work for ros2ebpfd: run 'sudo -v' first, or add a "
                      'NOPASSWD sudoers line for it', file=sys.stderr)
            return 1
        healthy = st.get('healthy')
        print(f"ros2ebpfd started (pid {st.get('pid')}): "
              f"{st.get('probes_attached')}/{st.get('probes_expected')} probe links, "
              f"{len(st.get('shims') or [])} shim(s), offset table {st.get('offset_table') or '-'}, "
              f"{'healthy' if healthy else 'NOT healthy'}; log: {log_file}")
        if not healthy:
            print("hint: 'ros2 ebpf status -v' shows what is missing (no shim found yet?)",
                  file=sys.stderr)
        return 0 if healthy else 1
