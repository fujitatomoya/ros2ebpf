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

import curses
import locale
import sys
import time

from ros2ebpf.api import ConnectError
from ros2ebpf.api import resolve_targets
from ros2ebpf.api import RpcError
from ros2ebpf.monitor import KEYS_HELP
from ros2ebpf.monitor import Monitor
from ros2ebpf.verb import VerbExtension

HELP = [
    'ros2 ebpf monitor',
    '',
    '  e   endpoints view: publish / take / service-call rates per endpoint, sparkline of the last intervals',
    '  p   processes view: per ROS 2 process, with its endpoints and rates',
    '  h   health view: the daemon status (probes, shims, exporter, maps, self counters, identity)',
    '  /   filter rows (name, comm, kind, target, domain, pid); Enter applies, Esc cancels',
    '  c   clear the filter',
    '  s   cycle the sort of the endpoints view: rate, name, gaps',
    '  ?   this help',
    '  q   quit',
    '',
    'Rows with sequence gaps are highlighted; idle endpoints are dimmed.',
    'Press any key to go back.',
]


def _utf8():
    try:
        locale.setlocale(locale.LC_ALL, '')
    except locale.Error:
        pass
    return 'utf' in (locale.getpreferredencoding(False) or '').lower()


def _poll(clients, monitor):
    statuses, snapshots, errors = {}, {}, {}
    for c in clients:
        try:
            statuses[c.name] = c.call('status')
            snapshots[c.name] = c.call('snapshot')
        except (ConnectError, RpcError) as e:
            errors[c.name] = str(e)
            c.close()
    monitor.update(statuses, snapshots, errors)


class MonitorVerb(VerbExtension):
    """Live terminal view of the daemon(s): endpoint rates, processes and health, no browser needed."""

    def add_arguments(self, parser, cli_name):
        parser.add_argument(
            '--interval', '-i', type=float, default=2.0,
            help='Seconds between two reads (default: %(default)s)')
        parser.add_argument(
            '--once', '-1', action='store_true',
            help='Print one frame as plain text and exit (scripts, CI)')
        parser.add_argument(
            '--view', choices=('endpoints', 'processes', 'health'), default='endpoints',
            help='Initial view (default: %(default)s)')
        parser.add_argument('--filter', default='', help='Initial row filter')
        parser.add_argument('--no-color', action='store_true', help='Monochrome')
        parser.add_argument(
            '--width', type=int, default=120, help='Line width for --once (default: %(default)s)')

    def main(self, *, args):
        try:
            clients = resolve_targets(args)
        except ConnectError as e:
            print(f'error: {e}', file=sys.stderr)
            return 2
        monitor = Monitor(interval=max(args.interval, 0.2), utf8=_utf8())
        monitor.set_view(args.view)
        monitor.filter = args.filter
        try:
            if args.once:
                _poll(clients, monitor)
                time.sleep(monitor.interval)
                _poll(clients, monitor)
                sys.stdout.write(monitor.render_text(args.width))
                return 0 if not monitor.errors else 1
            curses.wrapper(self._loop, clients, monitor, not args.no_color)
            return 0
        except KeyboardInterrupt:
            return 0
        finally:
            for c in clients:
                c.close()

    # ---- curses front end ------------------------------------------------------
    @staticmethod
    def _loop(scr, clients, monitor, color):
        curses.curs_set(0)
        scr.timeout(100)
        attrs = {'': curses.A_NORMAL, 'dim': curses.A_DIM, 'title': curses.A_BOLD,
                 'header': curses.A_REVERSE, 'ok': curses.A_BOLD, 'warn': curses.A_BOLD, 'bad': curses.A_BOLD}
        if color and curses.has_colors():
            curses.start_color()
            curses.use_default_colors()
            curses.init_pair(1, curses.COLOR_GREEN, -1)
            curses.init_pair(2, curses.COLOR_YELLOW, -1)
            curses.init_pair(3, curses.COLOR_RED, -1)
            curses.init_pair(4, curses.COLOR_BLACK, curses.COLOR_CYAN)
            attrs.update({'ok': curses.color_pair(1) | curses.A_BOLD, 'warn': curses.color_pair(2),
                          'bad': curses.color_pair(3) | curses.A_BOLD, 'header': curses.color_pair(4)})
        _poll(clients, monitor)
        last = time.monotonic()
        editing = None      # filter text being typed, or None
        show_help = False
        while True:
            if time.monotonic() - last >= monitor.interval:
                _poll(clients, monitor)
                last = time.monotonic()
            height, width = scr.getmaxyx()
            scr.erase()
            if show_help:
                for i, line in enumerate(HELP[:height - 1]):
                    scr.addnstr(i, 0, line, width - 1)
            else:
                lines = monitor.render(width, height)
                if editing is not None:
                    lines[-1] = (f'/{editing}', 'title')
                for i, (text, style) in enumerate(lines[:height]):
                    try:
                        scr.addnstr(i, 0, text, width - 1, attrs.get(style, curses.A_NORMAL))
                    except curses.error:
                        pass
            scr.refresh()
            ch = scr.getch()
            if ch == -1:
                continue
            if show_help:
                show_help = False
                continue
            if editing is not None:
                if ch in (10, 13, curses.KEY_ENTER):
                    monitor.filter, editing = editing, None
                elif ch == 27:
                    editing = None
                elif ch in (curses.KEY_BACKSPACE, 127, 8):
                    editing = editing[:-1]
                elif 32 <= ch < 127:
                    editing += chr(ch)
                continue
            if ch in (ord('q'), ord('Q')):
                return
            if ch == ord('e'):
                monitor.set_view('endpoints')
            elif ch == ord('p'):
                monitor.set_view('processes')
            elif ch == ord('h'):
                monitor.set_view('health')
            elif ch == ord('/'):
                editing = ''
            elif ch == ord('c') or ch == 27:
                monitor.filter = ''
            elif ch == ord('s'):
                monitor.next_sort()
            elif ch == ord('?'):
                show_help = True
            elif ch == curses.KEY_RESIZE:
                pass
        # unreachable; KEYS_HELP is shown by Monitor.render
        return KEYS_HELP
