"""USB identity discovery and bounded serial I/O, shared by demo and collector."""
import fcntl
import os
from pathlib import Path
import plistlib
import select
import subprocess
import sys
import termios
import time
import tty
SERIAL = 'eutb710mdq-sd-poc'
MAX_LINE = 24576

def mac_ports(nodes):
    """Find the serial client beneath CDC data interface 3 of this USB identity."""
    found = []

    def walk(node, tablet=False, interface=None):
        if node.get('USB Serial Number') == SERIAL:
            tablet = node.get('idVendor') == 0x1d6b and node.get('idProduct') == 0x0104
        if 'bInterfaceNumber' in node:
            interface = node['bInterfaceNumber']
        port = node.get('IOCalloutDevice')
        if tablet and interface == 3 and port:
            found.append(port)
        for child in node.get('IORegistryEntryChildren', []):
            walk(child, tablet, interface)

    for node in [nodes] if isinstance(nodes, dict) else nodes:
        walk(node)
    return sorted(set(found))


def discover():
    if sys.platform == 'darwin':
        raw = subprocess.check_output(['ioreg', '-a', '-l', '-p', 'IOService'], timeout=10)
        found = mac_ports(plistlib.loads(raw))
    elif sys.platform.startswith('linux'):
        found = []
        for entry in Path('/sys/class/tty').glob('ttyACM*'):
            interface = (entry/'device').resolve()
            if not (interface/'bInterfaceNumber').exists():
                continue
            device = interface.parent
            if not all((device/name).exists() for name in ('serial', 'idVendor', 'idProduct')):
                continue
            if ((interface/'bInterfaceNumber').read_text().strip() == '02'
                    and (device/'serial').read_text().strip() == SERIAL
                    and (device/'idVendor').read_text().strip() == '1d6b'
                    and (device/'idProduct').read_text().strip() == '0104'):
                found.append('/dev/'+entry.name)
    else:
        raise RuntimeError('Host supported: macOS or Linux')
    if len(found) != 1:
        raise RuntimeError(f'Expected one tablet data ACM; found {found}')
    return found[0]


def open_data(port):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    original = None
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        original = termios.tcgetattr(fd)
        tty.setraw(fd, termios.TCSANOW)
        attrs = termios.tcgetattr(fd)
        attrs[2] |= termios.CLOCAL | termios.CREAD
        attrs[4] = attrs[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        return fd, original
    except BaseException:
        try:
            if original is not None:
                termios.tcsetattr(fd, termios.TCSANOW, original)
        except termios.error:
            pass
        finally:
            os.close(fd)
        raise


def close_data(fd, original):
    try:
        termios.tcsetattr(fd, termios.TCSANOW, original)
    except termios.error:
        pass  # A physically disconnected USB device may no longer accept ioctls.
    finally:
        os.close(fd)


def write_frame(fd, data):
    remaining = memoryview(data)
    deadline = time.monotonic()+2
    while remaining:
        if time.monotonic() >= deadline:
            raise TimeoutError('Data ACM write timed out')
        if select.select([], [fd], [], 0.1)[1]:
            try:
                n = os.write(fd, remaining)
                if not n:
                    raise OSError('Data ACM closed')
                remaining = remaining[n:]
            except BlockingIOError:
                pass
