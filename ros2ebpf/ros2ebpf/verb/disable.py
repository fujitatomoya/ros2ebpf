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

from ros2ebpf.api import ConnectError
from ros2ebpf.api import FAMILIES
from ros2ebpf.api import resolve_targets
from ros2ebpf.api import RpcError
from ros2ebpf.verb import VerbExtension


class DisableVerb(VerbExtension):
    """Disable a probe family at runtime (detach links; counters are kept) ."""

    def add_arguments(self, parser, cli_name):
        parser.add_argument('family', choices=FAMILIES, help='Probe family')

    def main(self, *, args):
        try:
            clients = resolve_targets(args)
        except ConnectError as e:
            print(f'error: {e}', file=sys.stderr)
            return 2
        rc = 0
        for c in clients:
            try:
                r = c.call('disable', {'family': args.family})
                state = 'enabled' if r.get('enabled') else 'disabled'
                extra = f" ({r['attached']}/{r['expected']} links)" if r.get('enabled') else ''
                warn = f"  warning: {r['warning']}" if r.get('warning') else ''
                print(f'{c.name}: {args.family} {state}{extra}{warn}')
            except (ConnectError, RpcError) as e:
                print(f'{c.name}: error: {e}', file=sys.stderr)
                rc = 1
            finally:
                c.close()
        return rc
