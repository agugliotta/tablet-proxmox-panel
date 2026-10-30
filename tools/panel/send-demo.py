#!/usr/bin/env python3
"""Send bounded demo JSON lines over the tablet's data ACM; standard library only."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import plistlib
import select
import signal
import subprocess
import sys
import termios
import time
import tty

from transport import SERIAL, MAX_LINE, mac_ports, discover, open_data, close_data, write_frame
stopping = False






def frame(path):
    raw = path.read_bytes()
    if len(raw) > MAX_LINE:
        raise ValueError(f'Demo file exceeds {MAX_LINE} bytes')
    value = json.loads(raw)
    if not isinstance(value, dict) or value.get('v') != 1 or value.get('demo') is not True:
        raise ValueError('Only v=1 demo=true messages are enabled')
    encoded = json.dumps(value, separators=(',', ':'), ensure_ascii=True, allow_nan=False).encode('ascii')
    if len(encoded) > MAX_LINE:
        raise ValueError(f'Encoded message exceeds {MAX_LINE} bytes')
    return encoded+b'\n'








def stop(_signum, _frame):
    global stopping
    stopping = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, default=Path(__file__).with_name('demo.json'))
    parser.add_argument('--count', type=int, default=0, help='0 keeps sending; positive value sends this many frames')
    parser.add_argument('--discover', action='store_true', help='Print identified data port without opening it')
    args = parser.parse_args()
    if args.count < 0:
        parser.error('count must be non-negative')
    if args.discover:
        print(discover())
        return
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    fd = None
    count = 0
    try:
        while not stopping and (not args.count or count < args.count):
            started = time.monotonic()
            try:
                data = frame(args.input)  # Re-read so edits update the live panel.
                if fd is None:
                    port = discover()
                    fd, original = open_data(port)
                    write_frame(fd, b'\n')  # Re-synchronize a discarded/incomplete line.
                    print(f'DATA_CONNECTED {port}', flush=True)
                write_frame(fd, data)
                count += 1
                print(f'DATA_SENT sample={count} bytes={len(data)}', flush=True)
            except (OSError, RuntimeError, ValueError, subprocess.SubprocessError, termios.error) as error:
                print(f'DATA_RETRY {error}', file=sys.stderr, flush=True)
                if fd is not None:
                    close_data(fd, original)
                    fd = None
            deadline = started+3
            while not stopping and time.monotonic() < deadline and (not args.count or count < args.count):
                time.sleep(min(0.1, max(0, deadline-time.monotonic())))
    finally:
        if fd is not None:
            close_data(fd, original)


if __name__ == '__main__':
    main()
