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

import argparse
import json
import os
import socket
import threading

import pytest

from ros2ebpf.api import client_for_host
from ros2ebpf.api import compute_rates
from ros2ebpf.api import ConnectError
from ros2ebpf.api import format_table
from ros2ebpf.api import HttpClient
from ros2ebpf.api import load_fleet
from ros2ebpf.api import resolve_targets
from ros2ebpf.api import RpcError
from ros2ebpf.api import SshClient
from ros2ebpf.api import UnixClient


class FakeDaemon:
    """Minimal JSON-RPC line server on a unix socket, mimicking ros2ebpfd."""

    def __init__(self, path):
        self.path = path
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.bind(path)
        self.sock.listen(2)
        self.requests = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            with conn:
                buf = b''
                while True:
                    chunk = conn.recv(65536)
                    if not chunk:
                        break
                    buf += chunk
                    while b'\n' in buf:
                        line, _, buf = buf.partition(b'\n')
                        req = json.loads(line)
                        self.requests.append(req)
                        conn.sendall(json.dumps(self.respond(req)).encode() + b'\n')

    def respond(self, req):
        m = req['method']
        if m == 'status':
            return {'jsonrpc': '2.0', 'id': req['id'], 'result': {'healthy': True, 'version': 't'}}
        if m == 'enable':
            return {'jsonrpc': '2.0', 'id': req['id'],
                    'result': {'family': req['params']['family'], 'enabled': True, 'attached': 2, 'expected': 2}}
        return {'jsonrpc': '2.0', 'id': req['id'], 'error': {'code': -32601, 'message': f"unknown method '{m}'"}}

    def close(self):
        self.sock.close()
        os.unlink(self.path)


@pytest.fixture
def daemon(tmp_path):
    d = FakeDaemon(str(tmp_path / 'ctl.sock'))
    yield d
    d.close()


def test_unix_client_roundtrip(daemon):
    c = UnixClient(daemon.path)
    assert c.call('status') == {'healthy': True, 'version': 't'}
    r = c.call('enable', {'family': 'publish'})
    assert r['enabled'] and r['attached'] == 2
    with pytest.raises(RpcError) as ei:
        c.call('bogus')
    assert ei.value.code == -32601
    c.close()
    assert daemon.requests[0]['method'] == 'status'
    assert daemon.requests[1]['params'] == {'family': 'publish'}


def test_unix_client_connect_error(tmp_path):
    c = UnixClient(str(tmp_path / 'missing.sock'))
    with pytest.raises(ConnectError):
        c.call('status')


def test_client_for_host():
    assert isinstance(client_for_host('unix:///tmp/x.sock'), UnixClient)
    assert isinstance(client_for_host('http://h:9478'), HttpClient)
    c = client_for_host('perception:9478')
    assert isinstance(c, HttpClient) and c.url == 'http://perception:9478'
    c = client_for_host('user@main')
    assert isinstance(c, SshClient) and c.host == 'user@main'


def test_fleet_and_targets(tmp_path, daemon):
    fleet = tmp_path / 'fleet.yaml'
    fleet.write_text(
        'robots:\n'
        '  amr-07:\n'
        '    hosts:\n'
        '      - main\n'
        '      - {name: perception, address: 192.168.7.12, transport: http, port: 9478}\n'
        f'      - {{name: here, transport: unix, socket: {daemon.path}}}\n')
    f = load_fleet(str(fleet))
    assert [h['name'] for h in f['amr-07']] == ['main', 'perception', 'here']
    ns = argparse.Namespace(robot='amr-07', host=[], socket='/nonexistent', fleet_file=str(fleet))
    clients = resolve_targets(ns)
    assert [type(c).__name__ for c in clients] == ['SshClient', 'HttpClient', 'UnixClient']
    assert clients[1].url == 'http://192.168.7.12:9478'
    assert clients[2].call('status')['healthy']
    ns = argparse.Namespace(robot=None, host=['a,b:9478'], socket='/x', fleet_file=str(fleet))
    assert [c.name for c in resolve_targets(ns)] == ['a', 'b']
    ns = argparse.Namespace(robot='nope', host=[], socket='/x', fleet_file=str(fleet))
    with pytest.raises(ConnectError):
        resolve_targets(ns)
    ns = argparse.Namespace(robot=None, host=[], socket=daemon.path, fleet_file=str(fleet))
    assert resolve_targets(ns)[0].path == daemon.path


def test_format_table():
    out = format_table(['A', 'Bee'], [[1, 'x'], ['longer', 'y']])
    lines = out.splitlines()
    assert lines[0] == 'A       Bee'
    assert lines[1] == '------  ---'
    assert lines[2] == '1       x'
    assert lines[3] == 'longer  y'


def _ep(pid, handle, kind, name, count, count2=0, gaps=0, comm='talker', domain=0):
    return {'pid': pid, 'handle': handle, 'kind': kind, 'name': name, 'count': count,
            'count2': count2, 'seq_gaps': gaps, 'seq_lost': 0, 'comm': comm, 'domain_id': domain}


def test_compute_rates():
    a = {'ts_ns': 0, 'endpoints': [
        _ep(1, 0x10, 'publisher', '/chatter', 100),
        _ep(1, 0x20, 'publisher', '/chatter', 50),
        _ep(2, 0x10, 'subscription', '/chatter', 10, gaps=0, comm='listener', domain=0),
        _ep(3, 0x10, 'client', '/add', 4, count2=4, comm='cli'),
    ]}
    b = {'ts_ns': 2_000_000_000, 'endpoints': [
        _ep(1, 0x10, 'publisher', '/chatter', 120),
        _ep(1, 0x20, 'publisher', '/chatter', 60),
        _ep(2, 0x10, 'subscription', '/chatter', 40, gaps=3, comm='listener', domain=0),
        _ep(3, 0x10, 'client', '/add', 6, count2=5, comm='cli'),
        _ep(9, 0x10, 'publisher', '/new', 7, comm='late', domain=5),
        dict(_ep(9, 0x11, 'publisher', '/rosout', 70, comm='late', domain=5), flags=1),
    ]}
    rows = compute_rates(a, b)
    by = {(r['kind'], r['name']): r for r in rows}
    assert by[('publisher', '/chatter')]['rate'] == 15.0          # (20 + 10) / 2 s
    assert by[('publisher', '/chatter')]['endpoints'] == 2
    assert by[('subscription', '/chatter')]['rate'] == 15.0 and by[('subscription', '/chatter')]['seq_gaps'] == 3
    assert by[('client', '/add')]['rate'] == 1.0 and by[('client', '/add')]['rate2'] == 0.5
    assert by[('publisher', '/new')]['rate'] == 3.5               # counted from zero
    assert rows[0]['rate'] == 15.0                                 # sorted descending
    assert not any(r['name'] == '/rosout' for r in rows)           # excluded tombstones skipped
    only = compute_rates(a, b, topic='/new')
    assert len(only) == 1 and only[0]['domain_id'] == '5'
    assert compute_rates(a, b, domain=5)[0]['name'] == '/new'
    assert compute_rates(a, b, kind='client')[0]['name'] == '/add'
    assert compute_rates(b, a) == []                               # non-positive dt
