# USB panel protocol

The host sends compact ASCII JSON, one full snapshot per LF-terminated line.
CRLF is accepted. The tablet endpoint is `/dev/ttyGS1`; `/dev/ttyGS0` is a
separate development console and must not receive these frames.

## Version 2

All fields below are required. Unknown or duplicate fields are rejected.

| Field | Value |
| --- | --- |
| `v` | Integer 2 |
| `demo` | Boolean; collector uses false, demonstrations use true |
| `host` | 1–32 printable ASCII characters |
| `host_state` | `ok`, `error` or `unknown` |
| `storage` | 1–32 printable ASCII characters; exact selected storage identifier |
| `cpu_pct` | Integer 0–100, or null |
| `ram_used_bytes`, `ram_total_bytes` | Integer bytes; 0 <= used <= total, total > 0; both null if absent |
| `disk_used_bytes`, `disk_total_bytes` | Same bounds; selected storage only, not summed disks |
| `machines` | Complete inventory, 0–128 entries |

Each machine has exactly `id` (integer 1–999999999), `kind` (`VM` or `CT`),
`name` (1–32 printable ASCII characters), and `state` (`running`, `stopped` or
`unknown`). IDs are unique. Byte values cannot exceed 2^53-1. Display conversion
to GiB uses 2^30 bytes. Missing values are not measured zeros.

A valid `host_state=error` message proves communication but does not replace
previous measurements or renew their age. API errors must not turn guests into
stopped machines. A subsequent successful snapshot recovers normal operation.

Version 1 remains accepted for historical demos: `v=1`, `demo=true`, no `storage`
field, at most five machines and machine IDs up to 999999. See `demo.json`.

## Framing and expiry

Maximum line length before LF is 24576 bytes. Incomplete lines expire after
3 seconds from their first byte. Expired, overlong or NUL-containing frames
are discarded through the next LF. Invalid JSON or schema values are discarded
atomically and do not renew sample age. The receiver marks measurements stale
after 10 seconds without a valid new sample, retaining values and their age.

The parser limits nesting to 8; UI receive work is bounded per iteration.
The sender writes an initial LF for synchronization and ordinarily samples every
3 seconds. UI interaction does not authorize commands in the protocol. Frames
carry no API token, credentials or guest power actions.

## Host transport

The development gadget uses serial `eutb710mdq-sd-poc`, VID 1d6b / PID 0104.
Linux discovery identifies the data ACM by control interface 02; macOS uses the
corresponding data interface 3. Never infer the endpoint from a tty suffix alone.
Discovery, serial locking, raw mode and retries are implemented in `transport.py`.
Linux hardware operation and reconnection were tested during development.
Only one sender should own the data endpoint at a time.
