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

import yaml

from ros2ebpf.api import ConnectError
from ros2ebpf.api import DEFAULT_CONFIG
from ros2ebpf.api import resolve_targets
from ros2ebpf.api import RpcError
from ros2ebpf.verb import VerbExtension

# key -> (type, help); list types accept YAML flow sequences on the command line
KNOWN_KEYS = {
    'host': (str, 'label on every series (restart)'),
    'robot': (str, 'fallback robot label'),
    'discover_shims': (bool, 'scan /proc for shims in containers'),
    'identity.read_environ': (bool, 'read ROS_DOMAIN_ID from /proc/<pid>/environ'),
    'identity.default_domain_id': (int, 'domain id when unset'),
    'probes.publish': (bool, ''),
    'probes.take': (bool, ''),
    'probes.services': (bool, ''),
    'probes.udp_drops': (bool, ''),
    'probes.seq_gap': (list, 'topics with sequence gap tracking, or "all"'),
    'topics.exclude': (list, 'topics never counted'),
    'histogram_buckets_seconds': (list, 'ascending upper bounds, max 15'),
    'exporter.mode': (str, 'inprocess | external (restart)'),
    'exporter.listen': (str, 'host:port (restart)'),
    'exporter.rmw_label': (bool, 'add rmw label'),
    'control.socket': (str, '(restart)'),
    'control.http': (str, '(restart)'),
    'expectations_file': (str, ''),
    'pin_root': (str, '(restart)'),
    'offset_table': (str, 'force distro/arch row'),
    'log_level': (str, 'error|warn|info|debug'),
}


def get_path(doc, key):
    cur = doc
    for part in key.split('.'):
        if not isinstance(cur, dict) or part not in cur:
            return None
        cur = cur[part]
    return cur


def set_path(doc, key, value):
    parts = key.split('.')
    cur = doc
    for part in parts[:-1]:
        if not isinstance(cur.get(part), dict):
            cur[part] = {}
        cur = cur[part]
    cur[parts[-1]] = value


def fmt_value(val):
    """One-line YAML rendering of a value (no document-end marker)."""
    return yaml.safe_dump(val, default_flow_style=True).replace('\n...', '').strip()


def coerce(key, raw):
    """Parse a command-line value for `key`; raises ValueError."""
    typ = KNOWN_KEYS[key][0]
    val = yaml.safe_load(raw) if raw != '' else ''
    if typ is bool:
        if isinstance(val, bool):
            return val
        raise ValueError(f'{key} expects true/false')
    if typ is int:
        if isinstance(val, int) and not isinstance(val, bool):
            return val
        raise ValueError(f'{key} expects an integer')
    if typ is list:
        if key == 'probes.seq_gap' and val in ('all', 'none'):
            return val
        if isinstance(val, list):
            return val
        if isinstance(val, (str, float, int)):
            return [val]
        raise ValueError(f'{key} expects a list like [/a, /b]')
    return str(val) if val is not None else ''


class ConfigVerb(VerbExtension):
    """Show, get or set daemon configuration (edits /etc/ros2ebpf/config.yaml, then reloads)."""

    def add_arguments(self, parser, cli_name):
        sub = parser.add_subparsers(dest='action', required=True)
        sub.add_parser('show', help='Effective configuration as the daemon sees it')
        g = sub.add_parser('get', help='Read a key from the config file')
        g.add_argument('key', choices=sorted(KNOWN_KEYS))
        s = sub.add_parser('set', help='Write a key to the config file and reload')
        s.add_argument('key', choices=sorted(KNOWN_KEYS))
        s.add_argument('value', help='YAML value, e.g. true, 3, "[/a, /b]"')
        s.add_argument('--no-reload', action='store_true', help='Only edit the file')
        sub.add_parser('keys', help='List settable keys')
        parser.add_argument(
            '--file', default=DEFAULT_CONFIG, help='Config file for get/set (default: %(default)s)')

    def main(self, *, args):
        if args.action == 'keys':
            for k, (typ, hlp) in sorted(KNOWN_KEYS.items()):
                print(f'{k:<32} {typ.__name__:<5} {hlp}')
            return 0
        if args.action == 'show':
            try:
                clients = resolve_targets(args)
            except ConnectError as e:
                print(f'error: {e}', file=sys.stderr)
                return 2
            rc = 0
            for c in clients:
                try:
                    r = c.call('config')
                    if args.format == 'json':
                        print(json.dumps(r, indent=2))
                    else:
                        if len(clients) > 1:
                            print(f'===== {c.name} =====')
                        print(yaml.safe_dump(r, sort_keys=False))
                except (ConnectError, RpcError) as e:
                    print(f'{c.name}: error: {e}', file=sys.stderr)
                    rc = 1
                finally:
                    c.close()
            return rc

        try:
            with open(args.file) as f:
                doc = yaml.safe_load(f) or {}
        except OSError as e:
            print(f'error: {args.file}: {e}', file=sys.stderr)
            return 2
        if args.action == 'get':
            val = get_path(doc, args.key)
            print(fmt_value(val) if val is not None else '(unset)')
            return 0

        try:
            value = coerce(args.key, args.value)
        except ValueError as e:
            print(f'error: {e}', file=sys.stderr)
            return 2
        set_path(doc, args.key, value)
        try:
            with open(args.file, 'w') as f:
                yaml.safe_dump(doc, f, sort_keys=False)
        except OSError as e:
            print(f'error: cannot write {args.file}: {e} (try sudo)', file=sys.stderr)
            return 2
        print(f'{args.file}: {args.key} = {fmt_value(value)}')
        if '(restart)' in KNOWN_KEYS[args.key][1]:
            print('note: this key is read at start; restart ros2ebpfd for it to take effect')
        if args.no_reload:
            return 0
        try:
            clients = resolve_targets(args)
            for c in clients:
                try:
                    print(f"{c.name}: {c.call('reload').get('message', 'reloaded')}")
                finally:
                    c.close()
        except (ConnectError, RpcError) as e:
            print(f'warning: edited the file but reload failed: {e}', file=sys.stderr)
            return 1
        return 0
