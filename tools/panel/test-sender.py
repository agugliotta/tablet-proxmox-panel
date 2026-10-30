#!/usr/bin/env python3
"""Offline discovery, framing and pseudo-terminal tests; no real USB is opened."""
import importlib.util
import json
import os
from pathlib import Path
import select
import tempfile
from unittest.mock import patch
import transport

spec = importlib.util.spec_from_file_location('sender', Path(__file__).with_name('send-demo.py'))
sender = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sender)

tree = {'IORegistryEntryChildren': [
    {'USB Serial Number': sender.SERIAL, 'idVendor': 0x1d6b, 'idProduct': 0x0104,
     'IORegistryEntryChildren': [
         {'bInterfaceNumber': 1, 'IORegistryEntryChildren': [{'IOCalloutDevice': '/dev/cu.console'}]},
         {'bInterfaceNumber': 3, 'IORegistryEntryChildren': [{'IOCalloutDevice': '/dev/cu.data'}]},
     ]},
    {'USB Serial Number': 'unrelated', 'bInterfaceNumber': 3,
     'IORegistryEntryChildren': [{'IOCalloutDevice': '/dev/cu.other'}]},
]}
assert sender.mac_ports(tree) == ['/dev/cu.data']
assert sender.mac_ports([tree, tree]) == ['/dev/cu.data']
assert sender.mac_ports({'IORegistryEntryChildren': []}) == []

# A detach during configuration must close the descriptor even if restore also fails.
with patch.object(transport.os, 'open', return_value=123), \
     patch.object(transport.fcntl, 'flock'), \
     patch.object(transport.termios, 'tcgetattr', return_value=[]), \
     patch.object(transport.tty, 'setraw', side_effect=transport.termios.error('detached')), \
     patch.object(transport.termios, 'tcsetattr', side_effect=transport.termios.error('detached')), \
     patch.object(transport.os, 'close') as close:
    try:
        transport.open_data('/test-only')
    except transport.termios.error:
        pass
    else:
        raise AssertionError('Detach was ignored')
    close.assert_called_once_with(123)

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)/'demo.json'
    path.write_bytes(Path(__file__).with_name('demo.json').read_bytes())
    data = sender.frame(path)
    assert len(data) <= sender.MAX_LINE+1 and data.count(b'\n') == 1
    assert json.loads(data)['cpu_pct'] == 23
    for invalid in [{'v': 2, 'demo': True}, {'v': 1, 'demo': False},
                    {'v': 1, 'demo': True, 'cpu_pct': float('nan')}]:
        path.write_text(json.dumps(invalid))
        try:
            sender.frame(path)
        except ValueError:
            pass
        else:
            raise AssertionError('Invalid demo accepted')
    path.write_text('x'*(sender.MAX_LINE+1))
    try:
        sender.frame(path)
    except ValueError:
        pass
    else:
        raise AssertionError('Oversize demo accepted')

master, slave = os.openpty()
fd, original = sender.open_data(os.ttyname(slave))
try:
    sender.write_frame(fd, data)
    assert select.select([master], [], [], 1)[0]
    received = bytearray()
    while len(received) < len(data):
        received.extend(os.read(master, 4097))
    assert received == data
finally:
    sender.close_data(fd, original)
    os.close(master)
    os.close(slave)
print('SENDER_SELF_CHECK_OK: USB identity/function selection, bounded JSON, pseudo-terminal transport')
