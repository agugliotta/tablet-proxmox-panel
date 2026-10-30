#!/usr/bin/env python3
"""Read-only local Proxmox API collector; no guest or host actions are implemented."""
import argparse
import json
import logging
import math
import os
from pathlib import Path
import re
import signal
import ssl
import termios
import time
import unicodedata
import urllib.error
import urllib.request

from transport import MAX_LINE, discover, open_data, close_data, write_frame

log = logging.getLogger('tablet-panel')
stopping = False


def display_name(value):
    text = unicodedata.normalize('NFKD', str(value)).encode('ascii', 'replace').decode()
    return ''.join(c if 32 <= ord(c) <= 126 else '?' for c in text)[:32] or '?'


def empty_snapshot(config):
    return {'v': 2, 'demo': False, 'host': config['node'], 'host_state': 'error',
            'storage': config['storage'], 'cpu_pct': None,
            'ram_used_bytes': None, 'ram_total_bytes': None,
            'disk_used_bytes': None, 'disk_total_bytes': None, 'machines': []}


def usage(used, total):
    if used is None or total is None:
        return None, None
    if (type(used) is not int or type(total) is not int or
            not 0 <= used <= total <= 2**53-1 or total == 0):
        raise ValueError('Invalid API memory/storage values')
    return used, total


def snapshot(get, config):
    node, pool = config['node'], config['storage']
    host = get(f'/nodes/{node}/status')
    resources = get('/cluster/resources?type=vm')
    storages = get(f'/nodes/{node}/storage')
    if (not isinstance(host, dict) or not isinstance(resources, list) or
            not isinstance(storages, list) or
            not all(isinstance(v, dict) for v in resources+storages)):
        raise ValueError('Invalid API response structure')
    result = empty_snapshot(config)
    cpu = host.get('cpu')
    if cpu is not None:
        if type(cpu) not in (int, float) or not math.isfinite(cpu) or not 0 <= cpu <= 1:
            raise ValueError('Invalid API CPU fraction')
        result['cpu_pct'] = int(cpu*100+0.5)
    memory = host.get('memory', {})
    if not isinstance(memory, dict):
        raise ValueError('Invalid API memory structure')
    result['ram_used_bytes'], result['ram_total_bytes'] = usage(memory.get('used'), memory.get('total'))
    selected = [s for s in storages if s.get('storage') == pool]
    if len(selected) != 1 or not selected[0].get('active'):
        raise ValueError('Configured storage unavailable')
    result['disk_used_bytes'], result['disk_total_bytes'] = usage(selected[0].get('used'), selected[0].get('total'))
    machines = []
    for vm in resources:
        if vm.get('node') != node or vm.get('type') not in ('qemu', 'lxc'):
            continue
        vmid = vm.get('vmid')
        if type(vmid) is not int or not 1 <= vmid <= 999999999:
            raise ValueError('Invalid API guest id')
        state = vm.get('status')
        machines.append({'id': vmid, 'kind': 'VM' if vm['type'] == 'qemu' else 'CT',
                         'name': display_name(vm.get('name') or str(vmid)),
                         'state': state if state in ('running', 'stopped') else 'unknown'})
    if len(machines) > 128 or len({m['id'] for m in machines}) != len(machines):
        raise ValueError('Guest inventory exceeds protocol limits')
    result['machines'] = sorted(machines, key=lambda m: m['id'])
    result['host_state'] = 'ok' if all(result[k] is not None for k in
                            ('cpu_pct', 'ram_total_bytes', 'disk_total_bytes')) else 'unknown'
    return result


def encode(value):
    raw = json.dumps(value, separators=(',', ':'), ensure_ascii=True, allow_nan=False).encode('ascii')
    if len(raw) > MAX_LINE:
        raise ValueError('Snapshot exceeds protocol size')
    return raw+b'\n'


class Api:
    def __init__(self, config, credential):
        if config['url'] != 'https://127.0.0.1:8006':
            raise ValueError('Only the local Proxmox HTTPS API is permitted')
        for key in ('node', 'storage'):
            if not re.fullmatch(r'[A-Za-z0-9_.-]{1,32}', config[key]):
                raise ValueError('Invalid API path component')
        self.url = config['url']+'/api2/json'
        self.header = 'PVEAPIToken='+credential['id']+'='+credential['value']
        context = ssl.create_default_context(cafile=config['ca'])
        # This host's legacy PVE CA lacks keyUsage; retain chain, expiry and SAN verification.
        context.verify_flags &= ~ssl.VERIFY_X509_STRICT
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
                                                 urllib.request.HTTPSHandler(context=context))

    def get(self, path):
        # All paths come from snapshot(); never accept actions or paths over USB.
        request = urllib.request.Request(self.url+path, headers={'Authorization': self.header}, method='GET')
        with self.opener.open(request, timeout=2) as response:
            raw = response.read(2*1024*1024+1)
        if len(raw) > 2*1024*1024:
            raise ValueError('API response exceeds limit')
        return json.loads(raw)['data']


def stop(_signal, _frame):
    global stopping
    stopping = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=Path('/etc/tablet-panel/config.json'))
    parser.add_argument('--credential', type=Path)
    parser.add_argument('--once', action='store_true', help='Print one real snapshot; do not open USB')
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format='%(levelname)s %(message)s')
    config = json.loads(args.config.read_text())
    credential_path = args.credential or Path(os.environ['CREDENTIALS_DIRECTORY'])/'api.json'
    api = Api(config, json.loads(credential_path.read_text()))
    if args.once:
        print(encode(snapshot(api.get, config)).decode(), end='')
        return
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    fd = None
    api_state = usb_state = None
    runtime = Path('/run/tablet-panel')
    try:
        while not stopping:
            started = time.monotonic()
            try:
                value = snapshot(api.get, config)
                new_api_state = 'healthy'
            except (OSError, ValueError, KeyError, TypeError, urllib.error.URLError) as error:
                value = empty_snapshot(config)
                # Do not log request objects, headers, credential contents or response bodies.
                new_api_state = 'HTTP '+str(error.code) if isinstance(error, urllib.error.HTTPError) else type(error).__name__
            if new_api_state != api_state:
                log.info('API %s', new_api_state)
                api_state = new_api_state
            data = encode(value)
            temporary = runtime/'latest.tmp'
            temporary.write_bytes(data)
            temporary.replace(runtime/'latest.json')
            try:
                if fd is None:
                    port = discover()
                    fd, original = open_data(port)
                    write_frame(fd, b'\n')
                write_frame(fd, data)
                new_usb_state = 'connected'
            except (OSError, RuntimeError, TimeoutError, termios.error):
                new_usb_state = 'waiting'
                if fd is not None:
                    close_data(fd, original)
                    fd = None
            if new_usb_state != usb_state:
                log.info('USB %s', new_usb_state)
                usb_state = new_usb_state
            deadline = started+3
            while not stopping and time.monotonic() < deadline:
                time.sleep(min(0.1, max(0, deadline-time.monotonic())))
    finally:
        if fd is not None:
            close_data(fd, original)


if __name__ == '__main__':
    main()
