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

"""The monitor view model and `ros2 ebpf monitor --once`, against synthetic snapshots."""

import argparse

from ros2ebpf import monitor as m
from ros2ebpf.verb.monitor import MonitorVerb


def _ep(pid, handle, kind, name, count, count2=0, gaps=0, comm='talker', domain=5, rmw='rmw_fastrtps_cpp'):
    return {'pid': pid, 'handle': handle, 'kind': kind, 'name': name, 'count': count, 'count2': count2,
            'seq_gaps': gaps, 'seq_lost': 0, 'comm': comm, 'domain_id': domain, 'rmw': rmw, 'flags': 0}


def _status(healthy=True, attached=16, shims=1, dropped=0):
    return {'version': '0.1.0', 'pid': 42, 'uptime_seconds': 3725, 'host': 'main', 'robot': '',
            'distro': 'ubuntu-26.04', 'arch': 'x86_64', 'kernel': '7.0.0', 'healthy': healthy,
            'probes_attached': attached, 'probes_expected': 16, 'offset_table': 'rolling/x86_64',
            'maps_recreated_at_start': False,
            'probes': {'publish': {'attached': 2, 'expected': 2, 'enabled': True},
                       'take': {'attached': 2, 'expected': 2, 'enabled': True}},
            'shims': [{'path': '/opt/ros/rolling/lib/librmw_implementation.so', 'distro': 'rolling',
                       'arch': 'x86_64', 'source': 'config', 'attach_failed': False}][:shims],
            'exporter': {'mode': 'inprocess', 'listen': '0.0.0.0:9477', 'serving': True, 'scrapes': 3,
                         'last_scrape_duration_seconds': 0.0002, 'expectations': 0},
            'maps': {'endpoints': 3, 'processes': 2, 'udp_drops': 0, 'abi': 1},
            'self_counters': {'name_read_failures': 0, 'id_read_failures': 0,
                              'events_dropped': dropped, 'map_full': 0},
            'identity': {'domains_seen': [5], 'resolved': 2, 'environ_read_failures': 0, 'resolve_failures': 0}}


SNAP_A = {'ts_ns': 0, 'endpoints': [
    _ep(1, 0x10, 'publisher', '/chatter', 100),
    _ep(2, 0x10, 'subscription', '/chatter', 90, comm='listener'),
    _ep(3, 0x10, 'service', '/add_two_ints', 10, count2=10, comm='add_two_ints_se'),
]}
SNAP_B = {'ts_ns': 2_000_000_000, 'endpoints': [
    _ep(1, 0x10, 'publisher', '/chatter', 120),
    _ep(2, 0x10, 'subscription', '/chatter', 110, gaps=2, comm='listener'),
    _ep(3, 0x10, 'service', '/add_two_ints', 12, count2=11, comm='add_two_ints_se'),
    _ep(4, 0x10, 'publisher', '/idle', 0, comm='idle'),
    dict(_ep(1, 0x11, 'publisher', '/rosout', 500), flags=1),
]}


def test_sparkline():
    assert m.sparkline([], width=4) == '    '
    assert m.sparkline([0, 1, 2, 4], width=4) == ' ▂▄█'
    assert m.sparkline([0, 1, 2, 4], width=4, charset=m.SPARK_ASCII) == ' :=%'
    assert m.sparkline([1, 1], width=4) == '  ██'
    assert len(m.sparkline(range(30))) == m.HISTORY


def test_uptime():
    assert m.fmt_uptime(65) == '1m05s' and m.fmt_uptime(3725) == '1h02m' and m.fmt_uptime(90000) == '1d01h'


def test_process_rows():
    rows = m.process_rows(SNAP_A, SNAP_B)
    by = {p['pid']: p for p in rows}
    assert by[1]['pub'] == 10.0 and by[1]['endpoints'] == 1          # /rosout tombstone not counted
    assert by[2]['take'] == 10.0 and by[2]['gaps'] == 2 and by[2]['comm'] == 'listener'
    assert by[3]['calls'] == 1.0 and by[3]['rmw'] == 'rmw_fastrtps_cpp'
    assert rows[0]['pid'] in (1, 2) and rows[-1]['pid'] == 4          # sorted by total rate
    assert m.process_rows(None, SNAP_B)[0]['pub'] == 0                # no previous snapshot: no rates


def _monitor():
    mon = m.Monitor(interval=2.0)
    mon.update({'local': _status()}, {'local': SNAP_A})
    assert mon.endpoints == [] and '(waiting for the first interval)' in mon.render_text()
    mon.update({'local': _status()}, {'local': SNAP_B})
    return mon


def test_endpoints_view_and_history():
    mon = _monitor()
    names = [(r['kind'], r['name']) for r in mon.endpoints]
    assert names[0] == ('publisher', '/chatter') or names[0] == ('subscription', '/chatter')   # 10 Hz each
    assert ('publisher', '/rosout') not in names
    sub = next(r for r in mon.endpoints if r['kind'] == 'subscription')
    assert sub['seq_gaps'] == 2 and sub['history'] == [10.0]
    text = mon.render_text(width=140)
    assert 'ros2 ebpf monitor  [endpoints]' in text and 'local: healthy 16/16 links 1 shim(s)' in text
    assert '/chatter' in text and 'add_two_ints_se' in text and m.KEYS_HELP in text
    styles = {t: s for t, s in mon.render(140)}
    assert any(s == 'warn' and 'listener' in t for t, s in styles.items())     # gaps highlighted
    assert any(s == 'dim' and '/idle' in t for t, s in styles.items())         # idle dimmed
    mon.update({'local': _status()}, {'local': dict(SNAP_B, ts_ns=4_000_000_000)})
    pub = next(r for r in mon.endpoints if r['name'] == '/chatter' and r['kind'] == 'publisher')
    assert pub['history'] == [10.0, 0.0] and pub['rate'] == 0.0


def test_sort_and_filter():
    mon = _monitor()
    mon.next_sort()
    assert mon.sort == 'name' and mon.endpoints[0]['name'] == '/add_two_ints'
    mon.next_sort()
    assert mon.sort == 'gaps' and mon.endpoints[0]['kind'] == 'subscription'
    mon.next_sort()
    assert mon.sort == 'rate'
    mon.filter = 'LISTEN'
    body = [t for t, _ in mon.body()]
    assert any('listener' in t for t in body) and not any('talker' in t for t in body)
    mon.filter = 'nothing-matches'
    assert '(no endpoint activity in this interval)' in [t for t, _ in mon.body()]


def test_processes_and_health_views():
    mon = _monitor()
    mon.set_view('processes')
    text = mon.render_text()
    assert '[processes]' in text and 'Pub Hz' in text and 'listener' in text and 'rmw_fastrtps_cpp' in text
    mon.set_view('health')
    text = mon.render_text()
    assert 'ros2ebpfd 0.1.0  pid 42  up 1h02m  host main' in text
    assert 'healthy: yes' in text and 'probes: 16/16  publish 2/2  take 2/2' in text
    assert 'shim: /opt/ros/rolling/lib/librmw_implementation.so  rolling/x86_64  config' in text
    assert 'exporter: inprocess 0.0.0.0:9477 serving  scrapes 3  last 0.2ms' in text
    mon.set_view('bogus')
    assert mon.view == 'health'


def test_health_styles_and_errors():
    mon = m.Monitor()
    mon.update({'a': _status(healthy=False, attached=10, shims=0, dropped=7)}, {'a': SNAP_B},
               errors={'b': 'cannot connect to /x'})
    summary, style = mon.health_summary()
    assert 'a: NOT healthy 10/16 links 0 shim(s)' in summary and 'b: unreachable' in summary and style == 'bad'
    mon.set_view('health')
    lines = mon.render()
    assert ('  healthy: NO', 'bad') in lines
    assert any(s == 'warn' and 'shim: none found yet' in t for t, s in lines)
    assert any(s == 'bad' and 'events_dropped 7' in t for t, s in lines)
    assert any(s == 'bad' and t.startswith('b: cannot connect') for t, s in lines)


def test_render_height():
    mon = _monitor()
    lines = mon.render(width=100, height=5)
    assert len(lines) == 5 and lines[-1][0] == m.KEYS_HELP[:100]


class _Client:
    """Stands in for a UnixClient: answers status and snapshot, alternating snapshots."""

    def __init__(self, name='local'):
        self.name = name
        self.calls = 0

    def call(self, method, params=None):
        if method == 'status':
            return _status()
        self.calls += 1
        return SNAP_A if self.calls == 1 else dict(SNAP_B, ts_ns=self.calls * 1_000_000_000)

    def close(self):
        pass


def test_monitor_once(monkeypatch, capsys):
    monkeypatch.setattr('ros2ebpf.verb.monitor.resolve_targets', lambda args: [_Client()])
    monkeypatch.setattr('ros2ebpf.verb.monitor.time.sleep', lambda s: None)
    args = argparse.Namespace(interval=1.0, once=True, view='endpoints', filter='', no_color=True, width=120)
    assert MonitorVerb().main(args=args) == 0
    out = capsys.readouterr().out
    assert '[endpoints]' in out and '/chatter' in out and 'listener' in out and m.KEYS_HELP in out
    args.view = 'health'
    assert MonitorVerb().main(args=args) == 0
    assert 'healthy: yes' in capsys.readouterr().out
