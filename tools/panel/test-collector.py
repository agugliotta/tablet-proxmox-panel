#!/usr/bin/env python3
"""Collector semantics and errors, without API credentials or host modifications."""
import collector

config = {'node': 'pve', 'storage': 'local-lvm'}
data = {'/nodes/pve/status': {'cpu': 0.237, 'memory': {'used': 12*2**30, 'total': 64*2**30}},
        '/nodes/pve/storage': [{'storage': 'local', 'active': 1, 'used': 20, 'total': 100},
                               {'storage': 'local-lvm', 'active': 1, 'used': 600*2**30, 'total': 800*2**30}],
        '/cluster/resources?type=vm': [{'vmid': 100+i, 'type': 'lxc' if i<18 else 'qemu',
                                     'name': f'service-{i}', 'node': 'pve',
                                     'status': 'running' if i<17 else 'stopped'} for i in range(19)]}
value = collector.snapshot(data.__getitem__, config)
assert value['cpu_pct'] == 24 and value['ram_used_bytes'] == 12*2**30
assert value['disk_used_bytes'] == 600*2**30 and value['storage'] == 'local-lvm'
assert len(value['machines']) == 19 and value['machines'][-1]['kind'] == 'VM'
assert value['machines'][-1]['state'] == 'stopped' and not value['demo'] and value['v'] == 2
assert len(collector.encode(value)) < 24577
data['/cluster/resources?type=vm'][0]['status'] = 'paused'
assert collector.snapshot(data.__getitem__, config)['machines'][0]['state'] == 'unknown'
data['/nodes/pve/status']['cpu'] = None
assert collector.snapshot(data.__getitem__, config)['cpu_pct'] is None
assert collector.snapshot(data.__getitem__, config)['host_state'] == 'unknown'
for bad in (-0.1, 1.1, float('nan'), True, '0.5'):
    data['/nodes/pve/status']['cpu'] = bad
    try:
        collector.snapshot(data.__getitem__, config)
    except ValueError:
        pass
    else:
        raise AssertionError('Invalid CPU accepted')
for used,total in [(2,1),(-1,5),(1,0),(True,5),(1,2**54)]:
    try:
        collector.usage(used,total)
    except ValueError:
        pass
    else:
        raise AssertionError('Invalid usage accepted')
assert collector.usage(None,None) == (None,None)
assert collector.empty_snapshot(config)['host_state'] == 'error'
assert collector.empty_snapshot(config)['machines'] == []
# The deployed HTTP client emits only GET with TLS validation; no POST path exists.
print('COLLECTOR_SELF_CHECK_OK: API units, storage selection, full inventory, unknown/error handling')
