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
Spawn and stop ``ros2ebpfd`` on this host (``ros2 ebpf start`` / ``ros2 ebpf stop``).

The daemon is on demand: it runs only while someone wants statistics, like ``ros2 daemon``.
Unlike ``ros2 daemon`` it needs root (BPF), so when the CLI is not root the daemon is started
through ``sudo -n``; the daemon then hands its control socket to the user who ran sudo.
"""

import os
import shutil
import signal
import subprocess
import time

from ros2ebpf.api import ConnectError
from ros2ebpf.api import DEFAULT_SOCKET
from ros2ebpf.api import RpcError
from ros2ebpf.api import UnixClient

DAEMON_BINARY = 'ros2ebpfd'
DEFAULT_CONFIG = '/etc/ros2ebpf/config.yaml'


def default_log_file():
    """``$ROS_LOG_DIR/ros2ebpfd.log``, or ``~/.ros/log/ros2ebpfd.log``."""
    base = os.environ.get('ROS_LOG_DIR') or os.path.join(os.path.expanduser('~'), '.ros', 'log')
    return os.path.join(base, 'ros2ebpfd.log')


def find_binary(binary=None):
    """Absolute path of the daemon binary (``binary`` or ``ros2ebpfd`` on PATH)."""
    path = shutil.which(binary or DAEMON_BINARY)
    if path is None:
        raise FileNotFoundError(
            f"'{binary or DAEMON_BINARY}' not found on PATH: source the workspace that installed "
            'ros2ebpf_daemon, or pass --binary')
    return os.path.abspath(path)


def daemon_argv(binary, config, log_level=None, sudo=None):
    """Command line for the daemon; ``sudo`` defaults to "not root"."""
    argv = [binary, '-c', config]
    if log_level:
        argv += ['--log-level', log_level]
    if sudo is None:
        sudo = os.geteuid() != 0
    if sudo:
        argv = ['sudo', '-n', '--'] + argv
    return argv


def status(socket_path=DEFAULT_SOCKET):
    """The ``status`` result of the daemon at ``socket_path``, or None if nothing answers."""
    client = UnixClient(socket_path)
    try:
        return client.call('status')
    except (ConnectError, RpcError):
        return None
    finally:
        client.close()


def spawn(argv, log_file):
    """Start ``argv`` detached from this process; stdout/stderr go to ``log_file``."""
    os.makedirs(os.path.dirname(log_file) or '.', exist_ok=True)
    with open(log_file, 'ab') as log:
        return subprocess.Popen(
            argv, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
            cwd='/', start_new_session=True, close_fds=True)


def wait_ready(proc, socket_path, timeout):
    """Status of the spawned daemon once it answers; raises if it exits first or times out."""
    deadline = time.monotonic() + timeout
    while True:
        st = status(socket_path)
        if st is not None:
            return st
        rc = proc.poll()
        if rc is not None:
            raise RuntimeError(f'ros2ebpfd exited with code {rc} before answering on {socket_path}')
        if time.monotonic() > deadline:
            raise TimeoutError(
                f'ros2ebpfd did not answer on {socket_path} within {timeout:g}s '
                '(control.socket in its config must match --socket)')
        time.sleep(0.2)


def _alive(pid):
    """True while ``pid`` runs; a zombie (exited, not yet reaped by its parent) counts as gone."""
    try:
        with open(f'/proc/{pid}/stat') as f:
            state = f.read().rpartition(')')[2].split()[0]
        return state != 'Z'
    except OSError:
        pass
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True   # exists, owned by someone else (root)
    return True


def _terminate(pid):
    """SIGTERM for a daemon without the ``stop`` method; through sudo when it is not ours."""
    try:
        os.kill(pid, signal.SIGTERM)
    except PermissionError:
        subprocess.run(['sudo', '-n', '--', 'kill', '-TERM', str(pid)], check=True)


def stop(socket_path=DEFAULT_SOCKET, timeout=10.0):
    """Ask the daemon to exit and wait until it is gone; returns its pid, or None if not running."""
    st = status(socket_path)
    if st is None:
        return None
    pid = st.get('pid')
    client = UnixClient(socket_path)
    try:
        client.call('stop')
    except RpcError:
        _terminate(pid)
    except ConnectError:
        pass
    finally:
        client.close()
    deadline = time.monotonic() + timeout
    while status(socket_path) is not None or (pid and _alive(pid)):
        if time.monotonic() > deadline:
            raise TimeoutError(f'ros2ebpfd (pid {pid}) is still running after {timeout:g}s')
        time.sleep(0.2)
    return pid


def tail(log_file, lines=20):
    """The last ``lines`` lines of ``log_file`` ('' if it cannot be read)."""
    try:
        with open(log_file, 'rb') as f:
            data = f.read()
    except OSError:
        return ''
    return b'\n'.join(data.splitlines()[-lines:]).decode(errors='replace')
