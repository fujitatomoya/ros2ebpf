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
Client side of the ros2ebpfd control protocol (JSON-RPC 2.0, one object per line).

Transports:
  * unix socket (local host, default /run/ros2ebpf/control.sock)
  * ssh: `ssh <host> ros2ebpf-ctl rpc` and the same line protocol over the pipes
  * http: POST /rpc on the daemon's optional control.http listener

Targets come from --socket (local), --host (one or more, any transport) or
--robot (every host of that robot in fleet.yaml).
"""

import json
import os
import shlex
import socket
import subprocess
import urllib.error
import urllib.request

import yaml

DEFAULT_SOCKET = '/run/ros2ebpf/control.sock'
DEFAULT_FLEET = '/etc/ros2ebpf/fleet.yaml'
DEFAULT_CONFIG = '/etc/ros2ebpf/config.yaml'
DEFAULT_HTTP_PORT = 9478
FAMILIES = ('publish', 'take', 'services', 'udp_drops')


class ConnectError(Exception):
    """Could not reach the daemon."""


class RpcError(Exception):
    """The daemon answered with a JSON-RPC error."""

    def __init__(self, code, message):
        super().__init__(f'{message} (code {code})')
        self.code = code
        self.message = message


class Client:
    """Base client: subclasses implement _send_line / _recv_line."""

    name = 'local'
    _next_id = 1

    def call(self, method, params=None):
        req = {'jsonrpc': '2.0', 'id': Client._next_id, 'method': method}
        Client._next_id += 1
        if params:
            req['params'] = params
        line = self._roundtrip(json.dumps(req))
        try:
            resp = json.loads(line)
        except ValueError as e:
            raise ConnectError(f'{self.name}: malformed response: {e}')
        if 'error' in resp and resp['error'] is not None:
            err = resp['error']
            raise RpcError(err.get('code', -1), err.get('message', 'error'))
        return resp.get('result', {})

    def _roundtrip(self, line):
        raise NotImplementedError()

    def close(self):
        pass


class UnixClient(Client):

    def __init__(self, path=DEFAULT_SOCKET, name=None):
        self.path = path
        self.name = name or 'local'
        self._sock = None
        self._buf = b''

    def _connect(self):
        if self._sock is not None:
            return
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(10)
        try:
            s.connect(self.path)
        except OSError as e:
            raise ConnectError(
                f'cannot connect to {self.path}: {e.strerror or e} '
                '(is ros2ebpfd running? are you in the ros2ebpf group?)')
        self._sock = s

    def _roundtrip(self, line):
        self._connect()
        try:
            self._sock.sendall(line.encode() + b'\n')
            while b'\n' not in self._buf:
                chunk = self._sock.recv(65536)
                if not chunk:
                    raise ConnectError(f'{self.name}: daemon closed the connection')
                self._buf += chunk
        except OSError as e:
            raise ConnectError(f'{self.name}: {e}')
        resp, _, self._buf = self._buf.partition(b'\n')
        return resp.decode()

    def close(self):
        if self._sock is not None:
            self._sock.close()
            self._sock = None


class SshClient(Client):

    def __init__(self, host, name=None, remote_socket=None, ssh_options=None):
        self.host = host
        self.name = name or host
        self.remote_socket = remote_socket
        self.ssh_options = ssh_options or []
        self._proc = None

    def _start(self):
        if self._proc is not None:
            return
        cmd = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', *self.ssh_options, self.host]
        remote = ['ros2ebpf-ctl']
        if self.remote_socket:
            remote += ['-s', self.remote_socket]
        remote.append('rpc')
        cmd.append(' '.join(shlex.quote(c) for c in remote))
        try:
            self._proc = subprocess.Popen(
                cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        except OSError as e:
            raise ConnectError(f'{self.name}: cannot run ssh: {e}')

    def _roundtrip(self, line):
        self._start()
        try:
            self._proc.stdin.write(line + '\n')
            self._proc.stdin.flush()
            resp = self._proc.stdout.readline()
        except (OSError, ValueError) as e:
            raise ConnectError(f'{self.name}: ssh: {e}')
        if not resp:
            err = self._proc.stderr.read().strip() if self._proc.stderr else ''
            self.close()
            raise ConnectError(f'{self.name}: no response over ssh{": " + err if err else ""}')
        return resp.rstrip('\n')

    def close(self):
        if self._proc is not None:
            try:
                self._proc.stdin.close()
                self._proc.wait(timeout=5)
            except Exception:
                self._proc.kill()
            self._proc = None


class HttpClient(Client):

    def __init__(self, url, name=None):
        self.url = url.rstrip('/')
        self.name = name or url

    def _roundtrip(self, line):
        req = urllib.request.Request(
            self.url + '/rpc', data=line.encode(), method='POST',
            headers={'Content-Type': 'application/json'})
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return r.read().decode()
        except urllib.error.URLError as e:
            raise ConnectError(f'{self.name}: {e.reason}')


def client_for_host(spec, name=None, remote_socket=None):
    """
    Build a client from a host spec string.

    'unix:///path'        -> unix socket
    'http://h:9478'       -> http control listener
    'h:9478'              -> http://h:9478
    'user@h' / 'h'        -> ssh
    """
    if spec.startswith('unix://'):
        return UnixClient(spec[len('unix://'):], name=name or 'local')
    if spec.startswith('http://') or spec.startswith('https://'):
        return HttpClient(spec, name=name)
    host, sep, port = spec.rpartition(':')
    if sep and port.isdigit():
        return HttpClient(f'http://{host}:{port}', name=name or host)
    return SshClient(spec, name=name, remote_socket=remote_socket)


def load_fleet(path=DEFAULT_FLEET):
    """
    fleet.yaml:
      robots:
        amr-07:
          hosts:
            - main                       # ssh
            - {name: perception, address: 192.168.7.12, transport: http, port: 9478}
            - {name: arm, address: user@arm.local, transport: ssh}
    """
    with open(path) as f:
        doc = yaml.safe_load(f) or {}
    robots = doc.get('robots') or {}
    out = {}
    for robot, spec in robots.items():
        hosts = []
        for h in (spec or {}).get('hosts', []):
            if isinstance(h, str):
                hosts.append({'name': h, 'address': h, 'transport': 'ssh'})
            else:
                hosts.append({
                    'name': h.get('name') or h.get('address'),
                    'address': h.get('address') or h.get('name'),
                    'transport': h.get('transport', 'ssh'),
                    'port': h.get('port', DEFAULT_HTTP_PORT),
                    'socket': h.get('socket'),
                })
        out[robot] = hosts
    return out


def add_target_arguments(parser):
    parser.add_argument(
        '--socket', default=os.environ.get('ROS2EBPF_SOCKET', DEFAULT_SOCKET),
        help='Control socket of the local daemon (default: %(default)s)')
    parser.add_argument(
        '--host', action='append', default=[], metavar='HOST',
        help='Other host(s): ssh target (user@host), host:port (http), unix:///path. Repeatable.')
    parser.add_argument(
        '--robot', metavar='ROBOT',
        help='Every host of this robot from the fleet file')
    parser.add_argument(
        '--fleet-file', default=os.environ.get('ROS2EBPF_FLEET', DEFAULT_FLEET),
        help='Fleet description (default: %(default)s)')
    parser.add_argument(
        '--format', choices=('table', 'json'), default='table',
        help='Output format (default: %(default)s)')


def resolve_targets(args):
    """Return a list of clients for the targets selected by the arguments."""
    clients = []
    if getattr(args, 'robot', None):
        try:
            fleet = load_fleet(args.fleet_file)
        except OSError as e:
            raise ConnectError(f'fleet file {args.fleet_file}: {e}')
        if args.robot not in fleet:
            raise ConnectError(
                f"robot '{args.robot}' not in {args.fleet_file} (known: {', '.join(fleet) or 'none'})")
        for h in fleet[args.robot]:
            if h['transport'] == 'http':
                clients.append(HttpClient(f"http://{h['address']}:{h.get('port', DEFAULT_HTTP_PORT)}", name=h['name']))
            elif h['transport'] == 'unix':
                clients.append(UnixClient(h.get('socket') or DEFAULT_SOCKET, name=h['name']))
            else:
                clients.append(SshClient(h['address'], name=h['name'], remote_socket=h.get('socket')))
    for spec in getattr(args, 'host', []) or []:
        for one in spec.split(','):
            one = one.strip()
            if one:
                clients.append(client_for_host(one))
    if not clients:
        clients.append(UnixClient(args.socket))
    return clients


def format_table(headers, rows):
    """Plain fixed-width table; everything is converted with str()."""
    rows = [[str(c) for c in r] for r in rows]
    widths = [len(h) for h in headers]
    for r in rows:
        for i, c in enumerate(r):
            widths[i] = max(widths[i], len(c))
    fmt = '  '.join('{:<%d}' % w for w in widths)
    lines = [fmt.format(*headers).rstrip(), fmt.format(*['-' * w for w in widths]).rstrip()]
    lines += [fmt.format(*r).rstrip() for r in rows]
    return '\n'.join(lines)


def compute_rates(snap_a, snap_b, *, topic=None, domain=None, kind=None):
    """
    Per-(comm, kind, name) rates between two `snapshot` results.

    Returns a list of dicts sorted by rate descending. Rates are computed from
    the sum of counts over endpoints sharing a label set (same aggregation as
    the exporter); an endpoint present only in the second snapshot is counted
    from zero.
    """
    dt = (snap_b['ts_ns'] - snap_a['ts_ns']) / 1e9
    if dt <= 0:
        return []
    before = {(e['pid'], e['handle']): e for e in snap_a['endpoints']}
    agg = {}
    for e in snap_b['endpoints']:
        if e.get('flags', 0) & 1:          # ROS2EBPF_F_EXCLUDE: tombstone, never counted
            continue
        if topic and e['name'] != topic:
            continue
        if domain is not None and str(e.get('domain_id')) != str(domain):
            continue
        if kind and e['kind'] != kind:
            continue
        prev = before.get((e['pid'], e['handle']))
        d1 = e['count'] - (prev['count'] if prev else 0)
        d2 = e['count2'] - (prev['count2'] if prev else 0)
        dg = e['seq_gaps'] - (prev['seq_gaps'] if prev else 0)
        key = (e.get('comm', ''), e['kind'], e['name'], str(e.get('domain_id')))
        a = agg.setdefault(key, {'comm': key[0], 'kind': key[1], 'name': key[2], 'domain_id': key[3],
                                 'count': 0, 'count2': 0, 'seq_gaps': 0, 'endpoints': 0, 'pids': set()})
        a['count'] += d1
        a['count2'] += d2
        a['seq_gaps'] += dg
        a['endpoints'] += 1
        a['pids'].add(e['pid'])
    out = []
    for a in agg.values():
        a['rate'] = a['count'] / dt
        a['rate2'] = a['count2'] / dt
        a['pids'] = sorted(a['pids'])
        out.append(a)
    out.sort(key=lambda x: (-x['rate'], x['name']))
    return out
