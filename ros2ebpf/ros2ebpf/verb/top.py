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

import json
import sys
import time

from ros2ebpf.api import compute_rates
from ros2ebpf.api import ConnectError
from ros2ebpf.api import format_table
from ros2ebpf.api import resolve_targets
from ros2ebpf.api import RpcError
from ros2ebpf.verb import VerbExtension


class TopVerb(VerbExtension):
    """Live per-endpoint rates computed client-side from two map reads (bench use; the server does this in production)."""

    def add_arguments(self, parser, cli_name):
        parser.add_argument('--topic', '-t', help='Only this topic / service name')
        parser.add_argument('--domain', '-d', help='Only this ROS_DOMAIN_ID')
        parser.add_argument(
            '--kind', '-k', choices=('publisher', 'subscription', 'client', 'service'),
            help='Only this endpoint kind')
        parser.add_argument(
            '--interval', '-i', type=float, default=2.0,
            help='Seconds between the two reads (default: %(default)s)')
        parser.add_argument('--once', '-1', action='store_true', help='Print one table and exit')
        parser.add_argument(
            '--lines', '-n', type=int, default=40, help='Rows to show (default: %(default)s)')

    def main(self, *, args):
        try:
            clients = resolve_targets(args)
        except ConnectError as e:
            print(f'error: {e}', file=sys.stderr)
            return 2
        try:
            prev = {c.name: c.call('snapshot') for c in clients}
            while True:
                time.sleep(max(args.interval, 0.2))
                cur = {}
                rows = []
                for c in clients:
                    cur[c.name] = c.call('snapshot')
                    for r in compute_rates(prev[c.name], cur[c.name], topic=args.topic,
                                           domain=args.domain, kind=args.kind):
                        r['target'] = c.name
                        rows.append(r)
                prev = cur
                rows.sort(key=lambda r: (-r['rate'], r['name']))
                rows = rows[:args.lines]
                if args.format == 'json':
                    print(json.dumps(rows))
                else:
                    if not args.once:
                        print('\033[2J\033[H', end='')
                    print(f'ros2 ebpf top  interval={args.interval}s  {time.strftime("%H:%M:%S")}')
                    table = [[
                        r['target'], r['kind'], r['name'], r['domain_id'], r['comm'],
                        ','.join(str(p) for p in r['pids']),
                        f"{r['rate']:.2f}",
                        f"{r['rate2']:.2f}" if r['kind'] in ('client', 'service') else '-',
                        r['seq_gaps'] if r['kind'] == 'subscription' else '-',
                    ] for r in rows]
                    print(format_table(
                        ['Target', 'Kind', 'Name', 'Domain', 'Comm', 'PIDs', 'Hz', 'Hz(2nd)', 'Gaps'], table))
                    if not rows:
                        print('(no endpoint activity in this interval)')
                sys.stdout.flush()
                if args.once:
                    break
        except KeyboardInterrupt:
            pass
        except (ConnectError, RpcError) as e:
            print(f'error: {e}', file=sys.stderr)
            return 1
        finally:
            for c in clients:
                c.close()
        return 0
