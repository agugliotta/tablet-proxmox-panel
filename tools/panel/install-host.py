#!/usr/bin/env python3
"""Install this project's read-only collector on its confirmed Proxmox host."""
import grp
import json
import os
from pathlib import Path
import pwd
import shutil
import signal
import socket
import subprocess
import time

def run(*args):
    return subprocess.check_output(args, text=True)

def main():
    assert os.geteuid() == 0 and socket.gethostname().split('.')[0] == 'pve'
    stage = Path(__file__).resolve().parent
    app = Path('/opt/tablet-panel')
    config = Path('/etc/tablet-panel')
    unit = Path('/etc/systemd/system/tablet-panel.service')
    marker = config/'managed-by-tablet-project'
    # Refuse collisions; reruns may update only an installation carrying our marker.
    if not marker.exists() and any(p.exists() for p in (app, config, unit)):
        raise RuntimeError('Existing unowned collector installation; no files changed')
    try:
        pwd.getpwnam('tablet-panel')
        if not marker.exists():
            raise RuntimeError('Existing unowned Unix identity')
    except KeyError:
        run('groupadd', '--system', 'tablet-panel')
        run('useradd', '--system', '--gid', 'tablet-panel', '--home-dir', '/nonexistent',
            '--shell', '/usr/sbin/nologin', 'tablet-panel')
    gid = grp.getgrnam('tablet-panel').gr_gid
    config.mkdir(mode=0o750, exist_ok=True)
    os.chown(config, 0, gid)
    marker.write_text('Tablet USB Proxmox panel\n')
    credential = config/'api.json'
    userid, tokenid = 'tablet-panel@pve', 'collector'
    users = json.loads(run('pveum', 'user', 'list', '--output-format', 'json'))
    if not any(u['userid'] == userid for u in users):
        run('pveum', 'user', 'add', userid, '--comment', 'Tablet USB panel read-only')
    elif not credential.exists():
        raise RuntimeError('Existing API identity without project credential; not replaced')
    run('pveum', 'acl', 'modify', '/', '--users', userid, '--roles', 'PVEAuditor')
    if not credential.exists():
        result = json.loads(run('pveum', 'user', 'token', 'add', userid, tokenid,
                                '--privsep', '1', '--comment', 'Tablet USB panel read-only',
                                '--output-format', 'json'))
        # Secret stays on host: never print the command output or include it in evidence.
        fd = os.open(credential, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as f:
            json.dump({'id': userid+'!'+tokenid, 'value': result['value']}, f)
            f.write('\n')
    run('pveum', 'acl', 'modify', '/', '--tokens', userid+'!'+tokenid, '--roles', 'PVEAuditor')
    (config/'config.json').write_text(json.dumps({'url': 'https://127.0.0.1:8006', 'node': 'pve',
                    'storage': 'local-lvm', 'ca': '/etc/tablet-panel/ca.pem'}, indent=2)+'\n')
    shutil.copyfile('/etc/pve/pve-root-ca.pem', config/'ca.pem')
    os.chmod(config/'ca.pem', 0o644)
    app.mkdir(mode=0o755, exist_ok=True)
    for name in ('collector.py', 'transport.py'):
        shutil.copyfile(stage/name, app/name)
        os.chmod(app/name, 0o644)
    # Verify authenticated HTTPS reads and snapshot semantics before enabling anything.
    wire = run('python3', str(app/'collector.py'), '--credential', str(credential), '--once')
    snapshot = json.loads(wire)
    assert snapshot['v'] == 2 and not snapshot['demo'] and snapshot['host_state'] == 'ok'
    run('systemd-analyze', 'verify', str(stage/'tablet-panel.service'))
    shutil.copyfile(stage/'tablet-panel.service', unit)
    shutil.copyfile(stage/'99-tablet-panel.rules', '/etc/udev/rules.d/99-tablet-panel.rules')
    run('udevadm', 'control', '--reload-rules')
    for link in Path('/dev/serial/by-id').glob('*eutb710mdq-sd-poc-if02'):
        run('udevadm', 'trigger', '--action=change', '--subsystem-match=tty',
            '--sysname-match='+link.resolve().name)
    run('udevadm', 'settle')
    old_pidfile = Path('/run/tablet-usb-test/sender.pid')
    if old_pidfile.exists():
        old = int(old_pidfile.read_text())
        cmdline = Path(f'/proc/{old}/cmdline')
        if cmdline.exists() and b'/run/tablet-usb-test/send-demo.py' in cmdline.read_bytes():
            os.kill(old, signal.SIGTERM)
            for _ in range(50):
                if not cmdline.exists():
                    break
                time.sleep(0.1)
    run('systemctl', 'daemon-reload')
    run('systemctl', 'enable', '--now', 'tablet-panel.service')
    run('systemctl', 'restart', 'tablet-panel.service')
    print(json.dumps({'installed': True, 'user': userid, 'api_role': 'PVEAuditor',
                      'guests': len(snapshot['machines']), 'storage': snapshot['storage'],
                      'credential': 'kept only on Proxmox host; not printed'}))

if __name__ == '__main__':
    main()
