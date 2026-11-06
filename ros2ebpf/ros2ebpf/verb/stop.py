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

import sys

from ros2ebpf import daemon
from ros2ebpf.verb import VerbExtension


class StopVerb(VerbExtension):
    """Stop the ros2ebpfd of this host; counters stay in the pinned maps for the next start."""

    def add_arguments(self, parser, cli_name):
        parser.add_argument(
            '--timeout', type=float, default=10.0, metavar='SECONDS',
            help='How long to wait for the daemon to exit (default: %(default)s)')

    def main(self, *, args):
        try:
            pid = daemon.stop(args.socket, args.timeout)
        except (TimeoutError, OSError) as e:
            print(f'error: {e}', file=sys.stderr)
            return 1
        if pid is None:
            print(f'ros2ebpfd is not running ({args.socket})')
            return 0
        print(f'ros2ebpfd (pid {pid}) stopped')
        return 0
