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

from ros2ebpf.api import ConnectError
from ros2ebpf.api import format_table
from ros2ebpf.api import resolve_targets
from ros2ebpf.api import RpcError
from ros2ebpf.verb import VerbExtension


def _fmt_uptime(s):
    s = int(s)
    if s < 3600:
        return f'{s // 60}m{s % 60:02d}s'
    if s < 86400:
        return f'{s // 3600}h{(s % 3600) // 60:02d}m'
    return f'{s // 86400}d{(s % 86400) // 3600:02d}h'


def collect(clients, method, params=None):
    """Call `method` on every client; returns ({name: result}, {name: error})."""
    results, errors = {}, {}
    for c in clients:
        try:
            results[c.name] = c.call(method, params)
        except (ConnectError, RpcError) as e:
            errors[c.name] = str(e)
        finally:
            c.close()
    return results, errors


class StatusVerb(VerbExtension):
    """Show attached probes, shims, exporter health and identity of the daemon(s)."""

    def add_arguments(self, parser, cli_name):
        parser.add_argument(
            '-v', '--verbose', action='store_true',
            help='Per-family probe details, shims and self counters')
        parser.add_argument(
            '-q', '--quiet', action='store_true',
            help='No output; exit code only (0 = every target healthy)')

    def main(self, *, args):
        try:
            clients = resolve_targets(args)
        except ConnectError as e:
            print(f'error: {e}', file=sys.stderr)
            return 2
        results, errors = collect(clients, 'status')

        if args.format == 'json':
            out = {name: r for name, r in results.items()}
            for name, err in errors.items():
                out[name] = {'error': err}
            if not args.quiet:
                print(json.dumps(out, indent=2, sort_keys=True))
        elif not args.quiet:
            self.print_table(results, errors, args.verbose)
        for err in errors.values():
            print(f'error: {err}', file=sys.stderr)
        healthy = all(r.get('healthy') for r in results.values()) and not errors
        return 0 if healthy else 1

    @staticmethod
    def print_table(results, errors, verbose):
        rows = []
        for name, r in results.items():
            fams = r.get('probes', {})
            fam_txt = ' '.join(
                f"{f}={'on' if v.get('enabled') else 'off'}"
                + (f"({v.get('attached')}/{v.get('expected')})" if v.get('enabled') else '')
                for f, v in fams.items())
            ex = r.get('exporter', {})
            ident = r.get('identity', {})
            rows.append([
                name,
                'yes' if r.get('healthy') else 'NO',
                r.get('host', ''),
                r.get('robot') or '-',
                f"{r.get('probes_attached')}/{r.get('probes_expected')}",
                len(r.get('shims', [])),
                f"{ex.get('mode')}{'' if ex.get('serving') or ex.get('mode') == 'external' else '(DOWN)'}",
                r.get('maps', {}).get('processes', 0),
                ','.join(str(d) for d in ident.get('domains_seen', [])) or '-',
                r.get('offset_table') or '-',
                _fmt_uptime(r.get('uptime_seconds', 0)),
            ])
            if verbose:
                rows[-1].append(fam_txt)
        for name, err in errors.items():
            rows.append([name, 'ERR', '', '', '', '', '', '', '', '', ''] + ([''] if verbose else []))
        headers = ['Target', 'Healthy', 'Host', 'Robot', 'Links', 'Shims', 'Exporter',
                   'Procs', 'Domains', 'Offsets', 'Uptime']
        if verbose:
            headers.append('Families')
        print(format_table(headers, rows))
        if verbose:
            for name, r in results.items():
                print(f'\n[{name}] version {r.get("version")}  machine_id {r.get("machine_id")}  '
                      f'{r.get("distro")}/{r.get("arch")} kernel {r.get("kernel")}')
                print(f'  config: {r.get("config_path")}   pin_root: {r.get("pin_root")}'
                      f'{"   (maps recreated at start)" if r.get("maps_recreated_at_start") else ""}')
                print(f'  control: {r.get("control", {}).get("socket")}'
                      f'  http: {r.get("control", {}).get("http") or "-"}')
                ex = r.get('exporter', {})
                print(f'  exporter: {ex.get("mode")} on {ex.get("listen")}, {ex.get("scrapes")} scrapes, '
                      f'last {ex.get("last_scrape_duration_seconds", 0) * 1000:.1f} ms, '
                      f'{ex.get("expectations", 0)} expectations')
                for s in r.get('shims', []):
                    print(f'  shim: {s.get("path")}  [{s.get("distro")}/{s.get("arch")}, {s.get("source")}'
                          f'{", ATTACH FAILED" if s.get("attach_failed") else ""}]')
                m = r.get('maps', {})
                sc = r.get('self_counters', {})
                ident = r.get('identity', {})
                print(f'  maps: endpoints={m.get("endpoints")} processes={m.get("processes")} '
                      f'udp_drops={m.get("udp_drops")} abi={m.get("abi")}')
                print(f'  self: name_read_failures={sc.get("name_read_failures")} '
                      f'id_read_failures={sc.get("id_read_failures")} events_dropped={sc.get("events_dropped")} '
                      f'map_full={sc.get("map_full")} environ_read_failures={ident.get("environ_read_failures")}')
