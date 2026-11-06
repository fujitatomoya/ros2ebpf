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
View model of ``ros2 ebpf monitor``: no terminal code in here, so it is unit-testable.

``Monitor.update()`` takes one ``status`` and one ``snapshot`` per target and derives the rows
of the three views (endpoints, processes, health); ``Monitor.render()`` turns the current view
into styled lines for the curses front end or for ``--once`` plain text.
"""

import math
import time

from ros2ebpf.api import compute_rates
from ros2ebpf.api import format_table

VIEWS = ('endpoints', 'processes', 'health')
SORTS = ('rate', 'name', 'gaps')
SPARK_UTF8 = ' ▁▂▃▄▅▆▇█'
SPARK_ASCII = ' .:-=+*#%'
HISTORY = 12          # sparkline width, in intervals
KEYS_HELP = 'e endpoints  p processes  h health  / filter  s sort  c clear  ? help  q quit'

# styles the front end maps to colours: 'header', 'ok', 'warn', 'bad', 'dim', 'title', ''
Line = tuple  # (text, style)


def sparkline(values, width=HISTORY, charset=SPARK_UTF8):
    """Right-aligned sparkline of the last ``width`` values, scaled to their maximum."""
    vals = list(values)[-width:]
    top = max(vals) if vals else 0
    levels = len(charset) - 1
    out = ''
    for v in vals:
        if v <= 0 or top <= 0:
            out += charset[0]
        else:
            out += charset[max(1, min(levels, math.ceil(v / top * levels)))]
    return out.rjust(width)


def fmt_uptime(seconds):
    s = int(seconds or 0)
    if s < 3600:
        return f'{s // 60}m{s % 60:02d}s'
    if s < 86400:
        return f'{s // 3600}h{(s % 3600) // 60:02d}m'
    return f'{s // 86400}d{(s % 86400) // 3600:02d}h'


def process_rows(prev_snap, snap):
    """Per-pid rows between two snapshots of one target: comm, domain, rmw, endpoints, Hz."""
    dt = (snap['ts_ns'] - prev_snap['ts_ns']) / 1e9 if prev_snap else 0
    before = {(e['pid'], e['handle']): e for e in (prev_snap or {}).get('endpoints', [])}
    procs = {}
    for e in snap.get('endpoints', []):
        if e.get('flags', 0) & 1:
            continue
        if e['pid'] not in procs:
            procs[e['pid']] = {'pid': e['pid'], 'comm': e.get('comm', ''),
                               'domain_id': str(e.get('domain_id')), 'rmw': e.get('rmw', ''),
                               'endpoints': 0, 'pub': 0, 'take': 0, 'calls': 0, 'gaps': 0}
        p = procs[e['pid']]
        p['endpoints'] += 1
        old = before.get((e['pid'], e['handle']))
        d = e['count'] - (old['count'] if old else 0)
        if dt > 0:
            if e['kind'] == 'publisher':
                p['pub'] += d / dt
            elif e['kind'] == 'subscription':
                p['take'] += d / dt
            else:
                p['calls'] += d / dt
        p['gaps'] += e.get('seq_gaps', 0)
    return sorted(procs.values(), key=lambda p: (-(p['pub'] + p['take'] + p['calls']), p['pid']))


class Monitor:
    """State of the monitor: last snapshots, rate history, view, filter and sort."""

    def __init__(self, interval=2.0, utf8=True):
        self.interval = interval
        self.charset = SPARK_UTF8 if utf8 else SPARK_ASCII
        self.view = 'endpoints'
        self.sort = 'rate'
        self.filter = ''
        self.prev = {}          # target -> snapshot
        self.history = {}       # (target, kind, name, domain, comm) -> [rate, ...]
        self.endpoints = []     # rows of the endpoints view (dicts)
        self.processes = []     # rows of the processes view
        self.status = {}        # target -> status dict
        self.errors = {}        # target -> error text
        self.updated = None
        self.frames = 0         # updates so far; rates need two

    # ---- data -------------------------------------------------------------
    def update(self, statuses, snapshots, errors=None):
        """New data: ``statuses`` and ``snapshots`` are dicts keyed by target name."""
        self.status = dict(statuses)
        self.errors = dict(errors or {})
        self.updated = time.time()
        self.frames += 1
        self.endpoints, self.processes = [], []
        for target, snap in snapshots.items():
            prev = self.prev.get(target)
            if prev is not None:
                for r in compute_rates(prev, snap):
                    r['target'] = target
                    key = (target, r['kind'], r['name'], r['domain_id'], r['comm'])
                    hist = self.history.setdefault(key, [])
                    hist.append(r['rate'])
                    del hist[:-HISTORY]
                    r['history'] = list(hist)
                    self.endpoints.append(r)
            for p in process_rows(prev, snap):
                p['target'] = target
                self.processes.append(p)
            self.prev[target] = snap
        self._sort()

    def _sort(self):
        if self.sort == 'name':
            self.endpoints.sort(key=lambda r: (r['name'], r['kind'], r['target']))
        elif self.sort == 'gaps':
            self.endpoints.sort(key=lambda r: (-r['seq_gaps'], -r['rate'], r['name']))
        else:
            self.endpoints.sort(key=lambda r: (-r['rate'], r['name']))

    # ---- interaction ---------------------------------------------------------
    def set_view(self, view):
        if view in VIEWS:
            self.view = view

    def next_sort(self):
        self.sort = SORTS[(SORTS.index(self.sort) + 1) % len(SORTS)]
        self._sort()

    def matches(self, row):
        f = self.filter.lower()
        return not f or any(f in str(row.get(k, '')).lower()
                            for k in ('name', 'comm', 'kind', 'target', 'domain_id', 'pid', 'rmw'))

    # ---- derived ------------------------------------------------------------
    def health_summary(self):
        """One short string per target, and the worst style."""
        parts, worst = [], 'ok'
        for t in sorted(set(self.status) | set(self.errors)):
            if t in self.errors:
                parts.append(f'{t}: unreachable')
                worst = 'bad'
                continue
            st = self.status[t]
            ok = st.get('healthy')
            parts.append(f"{t}: {'healthy' if ok else 'NOT healthy'} "
                         f"{st.get('probes_attached')}/{st.get('probes_expected')} links "
                         f"{len(st.get('shims') or [])} shim(s)")
            if not ok and worst != 'bad':
                worst = 'warn'
        return '  '.join(parts), worst

    def header(self, width):
        """The two top lines: title/health and keys; styled."""
        summary, style = self.health_summary()
        clock = time.strftime('%H:%M:%S', time.localtime(self.updated)) if self.updated else '--:--:--'
        title = f' ros2 ebpf monitor  [{self.view}]  every {self.interval:g}s  {clock}'
        if self.filter:
            title += f'  filter: {self.filter}'
        if self.view == 'endpoints':
            title += f'  sort: {self.sort}'
        return [(title.ljust(width), 'header'), (' ' + summary, style)]

    def body(self):
        """Styled lines of the current view (without the header)."""
        if self.view == 'health':
            return self._health_lines()
        if self.view == 'processes':
            rows = [p for p in self.processes if self.matches(p)]
            table = [[p['target'], p['pid'], p['comm'], p['domain_id'], p['rmw'] or '-', p['endpoints'],
                      f"{p['pub']:.2f}", f"{p['take']:.2f}", f"{p['calls']:.2f}", p['gaps']] for p in rows]
            text = format_table(['Target', 'PID', 'Comm', 'Domain', 'RMW', 'Endpoints',
                                 'Pub Hz', 'Take Hz', 'Call Hz', 'Gaps'], table).splitlines()
            styles = ['title', 'dim'] + ['warn' if p['gaps'] else '' for p in rows]
            lines = list(zip(text, styles))
            if not rows:
                lines.append(('(no ROS 2 process seen yet)', 'dim'))
            return lines
        rows = [r for r in self.endpoints if self.matches(r)]
        table = [[r['target'], r['kind'], r['name'], r['domain_id'], r['comm'],
                  ','.join(str(p) for p in r['pids']), f"{r['rate']:.2f}",
                  f"{r['rate2']:.2f}" if r['kind'] in ('client', 'service') else '-',
                  r['seq_gaps'] if r['kind'] == 'subscription' else '-',
                  sparkline(r['history'], charset=self.charset)] for r in rows]
        text = format_table(['Target', 'Kind', 'Name', 'Domain', 'Comm', 'PIDs', 'Hz', 'Hz(2nd)',
                             'Gaps', f'last {HISTORY}'], table).splitlines()
        styles = ['title', 'dim'] + [
            'warn' if (r['kind'] == 'subscription' and r['seq_gaps']) else ('dim' if r['rate'] == 0 else '')
            for r in rows]
        lines = list(zip(text, styles))
        if not rows:
            lines.append(('(no endpoint activity in this interval)' if self.frames > 1
                          else '(waiting for the first interval)', 'dim'))
        return lines

    def _health_lines(self):
        lines = []
        for t in sorted(set(self.status) | set(self.errors)):
            if t in self.errors:
                lines += [(f'{t}: {self.errors[t]}', 'bad'), ('', '')]
                continue
            st = self.status[t]
            ok = st.get('healthy')
            lines.append((f"{t}: ros2ebpfd {st.get('version')}  pid {st.get('pid')}  "
                          f"up {fmt_uptime(st.get('uptime_seconds'))}  host {st.get('host')}  "
                          f"robot {st.get('robot') or '-'}  {st.get('distro')} {st.get('arch')} "
                          f"kernel {st.get('kernel')}", 'title'))
            lines.append((f"  healthy: {'yes' if ok else 'NO'}", 'ok' if ok else 'bad'))
            fam = st.get('probes') or {}
            fams = '  '.join(f"{n} {f.get('attached')}/{f.get('expected')}"
                             f"{'' if f.get('enabled') else ' (disabled)'}" for n, f in fam.items())
            total_ok = st.get('probes_attached') == st.get('probes_expected')
            lines.append((f"  probes: {st.get('probes_attached')}/{st.get('probes_expected')}  {fams}",
                          'ok' if total_ok else 'warn'))
            shims = st.get('shims') or []
            if shims:
                for s in shims:
                    lines.append((f"  shim: {s.get('path')}  {s.get('distro')}/{s.get('arch')}  "
                                  f"{s.get('source')}{'  ATTACH FAILED' if s.get('attach_failed') else ''}",
                                  'bad' if s.get('attach_failed') else ''))
            else:
                lines.append(('  shim: none found yet (no ROS 2 process running?)', 'warn'))
            lines.append((f"  offset table: {st.get('offset_table') or '-'}  "
                          f"maps recreated at start: {'yes' if st.get('maps_recreated_at_start') else 'no'}", ''))
            ex = st.get('exporter') or {}
            lines.append((f"  exporter: {ex.get('mode')} {ex.get('listen')} "
                          f"{'serving' if ex.get('serving') else 'NOT serving'}  scrapes {ex.get('scrapes')}  "
                          f"last {1000 * (ex.get('last_scrape_duration_seconds') or 0):.1f}ms  "
                          f"expectations {ex.get('expectations')}", '' if ex.get('serving') else 'warn'))
            m = st.get('maps') or {}
            lines.append((f"  maps: endpoints {m.get('endpoints')}  processes {m.get('processes')}  "
                          f"udp_drops {m.get('udp_drops')}  abi {m.get('abi')}", ''))
            sc = st.get('self_counters') or {}
            bad = any(sc.get(k) for k in ('events_dropped', 'map_full', 'name_read_failures'))
            lines.append((f"  self: name_read_failures {sc.get('name_read_failures')}  "
                          f"id_read_failures {sc.get('id_read_failures')}  events_dropped {sc.get('events_dropped')}  "
                          f"map_full {sc.get('map_full')}", 'bad' if bad else ''))
            ident = st.get('identity') or {}
            lines.append((f"  identity: domains {ident.get('domains_seen')}  resolved {ident.get('resolved')}  "
                          f"environ failures {ident.get('environ_read_failures')}  "
                          f"resolve failures {ident.get('resolve_failures')}", ''))
            lines.append(('', ''))
        return lines or [('(no status yet)', 'dim')]

    def render(self, width=120, height=None):
        """Header + body + key line, as styled lines; ``height`` truncates the body."""
        lines = self.header(width) + [('', '')] + self.body()
        if height is not None and len(lines) > height - 1:
            lines = lines[:height - 1]
        lines.append((KEYS_HELP[:width], 'dim'))
        return lines

    def render_text(self, width=120):
        return '\n'.join(text for text, _ in self.render(width)) + '\n'
