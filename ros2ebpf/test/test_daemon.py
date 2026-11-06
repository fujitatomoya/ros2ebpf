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

"""`ros2 ebpf start` / `ros2 ebpf stop` against a fake ros2ebpfd executable (no root, no BPF)."""

import argparse
import os
import stat
import subprocess
import textwrap

import pytest

from ros2ebpf import daemon
from ros2ebpf.verb.start import StartVerb
from ros2ebpf.verb.stop import StopVerb

FAKE_DAEMON = textwrap.dedent('''\
    #!/usr/bin/env python3
    """Fake ros2ebpfd: -c CONFIG, answers status/stop on the socket from the config."""
    import json, os, socket, sys
    args = sys.argv[1:]
    if '--exit' in args:                       # misbehaving daemon: exit at once
        print('boom: cannot load BPF objects', file=sys.stderr)
        sys.exit(int(args[args.index('--exit') + 1]))
    cfg = open(args[args.index('-c') + 1]).read()
    path = [l.split(':', 1)[1].strip() for l in cfg.splitlines() if l.strip().startswith('socket:')][0]
    print(f'I fake ros2ebpfd starting, args={args}', flush=True)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    if os.path.exists(path):
        os.unlink(path)
    srv.bind(path)
    srv.listen(2)
    stop = False
    while not stop:
        conn, _ = srv.accept()
        with conn:
            buf = b''
            while b'\\n' not in buf:
                chunk = conn.recv(65536)
                if not chunk:
                    break
                buf += chunk
            if not buf:
                continue
            req = json.loads(buf.split(b'\\n', 1)[0])
            if req['method'] == 'status':
                result = {'pid': os.getpid(), 'healthy': True, 'probes_attached': 16,
                          'probes_expected': 16, 'shims': [{'path': '/x'}], 'offset_table': 'rolling/x86_64'}
            elif req['method'] == 'stop':
                result = {'message': 'stopping'}
                stop = True
            else:
                conn.sendall(json.dumps({'jsonrpc': '2.0', 'id': req['id'],
                                         'error': {'code': -32601, 'message': 'unknown'}}).encode() + b'\\n')
                continue
            conn.sendall(json.dumps({'jsonrpc': '2.0', 'id': req['id'], 'result': result}).encode() + b'\\n')
    os.unlink(path)
    print('I fake ros2ebpfd stopped', flush=True)
''')


@pytest.fixture
def fake(tmp_path):
    """Paths of a fake daemon binary, its config (socket under tmp_path) and a log file."""
    binary = tmp_path / 'ros2ebpfd'
    binary.write_text(FAKE_DAEMON)
    binary.chmod(binary.stat().st_mode | stat.S_IXUSR)
    sock = tmp_path / 'ctl.sock'
    config = tmp_path / 'config.yaml'
    config.write_text(f'control:\n  socket: {sock}\n')
    return argparse.Namespace(binary=str(binary), config=str(config), socket=str(sock),
                              log=str(tmp_path / 'log' / 'ros2ebpfd.log'))


def test_daemon_argv():
    assert daemon.daemon_argv('/b/ros2ebpfd', '/c.yaml', sudo=False) == ['/b/ros2ebpfd', '-c', '/c.yaml']
    assert daemon.daemon_argv('/b/ros2ebpfd', '/c.yaml', 'debug', sudo=True) == \
        ['sudo', '-n', '--', '/b/ros2ebpfd', '-c', '/c.yaml', '--log-level', 'debug']
    assert (daemon.daemon_argv('/b', '/c')[0] == 'sudo') == (os.geteuid() != 0)


def test_default_log_file(monkeypatch, tmp_path):
    monkeypatch.setenv('ROS_LOG_DIR', str(tmp_path))
    assert daemon.default_log_file() == str(tmp_path / 'ros2ebpfd.log')
    monkeypatch.delenv('ROS_LOG_DIR')
    assert daemon.default_log_file().endswith('/.ros/log/ros2ebpfd.log')


def test_find_binary(fake, monkeypatch):
    assert daemon.find_binary(fake.binary) == fake.binary
    monkeypatch.setenv('PATH', os.path.dirname(fake.binary))
    assert daemon.find_binary() == fake.binary
    with pytest.raises(FileNotFoundError):
        daemon.find_binary('/nonexistent/ros2ebpfd')


def test_spawn_status_stop(fake):
    assert daemon.status(fake.socket) is None
    proc = daemon.spawn(daemon.daemon_argv(fake.binary, fake.config, sudo=False), fake.log)
    try:
        st = daemon.wait_ready(proc, fake.socket, timeout=10)
        assert st['pid'] == proc.pid and st['healthy']
        assert os.getsid(proc.pid) != os.getsid(0)          # detached: its own session
        assert daemon.status(fake.socket)['pid'] == proc.pid
        assert daemon.stop(fake.socket, timeout=10) == proc.pid
        assert proc.wait(timeout=10) == 0
        assert daemon.status(fake.socket) is None
        assert daemon.stop(fake.socket) is None              # idempotent
    finally:
        if proc.poll() is None:
            proc.kill()
    log = daemon.tail(fake.log)
    assert 'fake ros2ebpfd starting' in log and 'stopped' in log


def test_spawn_failure(fake):
    proc = daemon.spawn([fake.binary, '-c', fake.config, '--exit', '3'], fake.log)
    with pytest.raises(RuntimeError, match='code 3'):
        daemon.wait_ready(proc, fake.socket, timeout=10)
    assert 'boom' in daemon.tail(fake.log)


def test_wait_ready_timeout(fake):
    proc = subprocess.Popen(['sleep', '5'])
    try:
        with pytest.raises(TimeoutError, match='did not answer'):
            daemon.wait_ready(proc, fake.socket, timeout=0.5)
    finally:
        proc.kill()
        proc.wait()
    assert daemon.tail('/nonexistent/log') == ''


def _start_args(fake, **kw):
    ns = argparse.Namespace(socket=fake.socket, config=fake.config, log_level=None,
                            log_file=fake.log, binary=fake.binary, timeout=10.0, no_sudo=True)
    vars(ns).update(kw)
    return ns


def test_start_and_stop_verbs(fake, capsys):
    assert StartVerb().main(args=_start_args(fake)) == 0
    out = capsys.readouterr().out
    assert 'ros2ebpfd started (pid' in out and '16/16 probe links' in out and fake.log in out
    assert StartVerb().main(args=_start_args(fake)) == 0
    assert 'already running' in capsys.readouterr().out
    assert StopVerb().main(args=argparse.Namespace(socket=fake.socket, timeout=10.0)) == 0
    assert 'stopped' in capsys.readouterr().out
    assert StopVerb().main(args=argparse.Namespace(socket=fake.socket, timeout=10.0)) == 0
    assert 'not running' in capsys.readouterr().out


def test_start_verb_errors(fake, capsys):
    assert StartVerb().main(args=_start_args(fake, binary='/nonexistent/ros2ebpfd')) == 2
    assert 'not found' in capsys.readouterr().err
    assert StartVerb().main(args=_start_args(fake, config='/nonexistent.yaml')) == 2
    assert 'not found' in capsys.readouterr().err
    bad = fake.binary + '-bad'
    with open(bad, 'w') as f:
        f.write('#!/bin/sh\necho "boom" >&2\nexit 3\n')
    os.chmod(bad, 0o755)
    assert StartVerb().main(args=_start_args(fake, binary=bad)) == 1
    err = capsys.readouterr().err
    assert 'exited with code 3' in err and 'boom' in err
