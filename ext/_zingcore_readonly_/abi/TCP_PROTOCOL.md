# net/tcp v1 Protocol Specification

**Version:** 1  
**Capability Name:** `net/tcp`  
**Status:** Golden (Stable)

## Overview

The `net/tcp` capability provides sandboxed TCP sockets to guest programs.

- The cap is opened with `zi_cap_open()` and returns a pollable handle.
- The returned handle implements the standard stream operations: `zi_read`, `zi_write`, `zi_end`.
- All socket I/O is nonblocking; would-block returns `ZI_E_AGAIN`.
- `sys/loop@v1` is used for readiness (WATCH/POLL), then the guest retries.

This document describes the on-wire ABI for opening sockets and (in listener mode) accepting connections.

## Sandbox Model

Sandboxing is enforced at open time.

### Outbound connections: `ZI_NET_ALLOW`

Outbound connect attempts are checked against the `ZI_NET_ALLOW` allowlist.

### Listening sockets: `ZI_NET_LISTEN_ALLOW`

Listener (bind+listen) opens are checked against the `ZI_NET_LISTEN_ALLOW` allowlist.

### Allowlist syntax

Both `ZI_NET_ALLOW` and `ZI_NET_LISTEN_ALLOW` use the same syntax:

- If unset or empty: only loopback hosts are permitted (`localhost`, `127.0.0.1`, `::1`, `[::1]`).
- If set to `any`: any host:port is permitted.
- Otherwise: comma-separated tokens. Supported tokens:
  - `loopback` (allows loopback hosts)
  - `host:*` (allows all ports for a specific host)
  - `host:port` (allows a specific port)
  - `*:*` (allows any host and any port)

Host comparisons are case-insensitive and accept IPv6 bracket form (e.g. `[::1]`).

For IPv6 literals, the runtime strips a single pair of surrounding brackets (`[... ]`) before allowlist matching and before passing the host to `getaddrinfo`.

### Important: what is (and is not) validated

The allowlist is matched against the **host string provided in the open params** plus the port.

- It does **not** currently support CIDR ranges or IP-class allow rules.
- For DNS names, the runtime does **not** currently validate the resolved IPs against an IP-based allowlist (because the allowlist is not IP-based).
- Name resolution is performed via the platform resolver (`getaddrinfo`) after the allowlist check.

Security implication: if you allow a DNS name in `ZI_NET_ALLOW`, you are trusting DNS for that name (no separate “DNS rebinding protection” is provided by this cap as specified/implemented here).

If an open is not allowed, the runtime returns `ZI_E_DENIED`.

## Opening Sockets

The cap is opened via `zi_cap_open(kind="net", name="tcp", version=1, params_ptr, params_len)`.

### Open params layout

Open params are a packed little-endian struct:

```
Offset | Size | Field
-------|------|-------------------------------
0      | 8    | host_ptr   (u64)
8      | 4    | host_len   (u32)
12     | 4    | port       (u32)
16     | 4    | flags      (u32)
20     | 4    | backlog        (u32) [optional]
24     | 8    | out_port_ptr   (u64) [optional]
```

- `host_ptr`/`host_len` point to UTF-8 bytes for the host (not NUL-terminated).
- `host_len` must be 1..255 and must not contain embedded NUL.
- `port` must be 1..65535 for connect mode.
- For listener mode, `port` may be `0` to request an ephemeral port.

### Flags

- `0`: outbound connect stream.
- `ZI_TCP_OPEN_LISTEN`: create a listener socket (bind+listen).

Socket option flags (best-effort):

- `ZI_TCP_OPEN_REUSEADDR`: reserved (runtime may always set `SO_REUSEADDR` for listeners).
- `ZI_TCP_OPEN_REUSEPORT`: set `SO_REUSEPORT` on the listener (if supported).
- `ZI_TCP_OPEN_IPV6ONLY`: set `IPV6_V6ONLY` on IPv6 listeners (if supported).
- `ZI_TCP_OPEN_NODELAY`: set `TCP_NODELAY` on stream sockets.
- `ZI_TCP_OPEN_KEEPALIVE`: set `SO_KEEPALIVE` on stream sockets.

Unknown flags are rejected with `ZI_E_INVALID`.

### Listener host wildcard

In listener mode only, `host="*"` means wildcard bind (all interfaces). For non-wildcard binds, the host string is passed to name resolution.

### Backlog

In listener mode, if `params_len >= 24`, `backlog` is read:

- `0` means runtime default.
- Values greater than 65535 may be clamped.

### Discovering the bound port (listener + port=0)

In listener mode, if `params_len >= 32` and `out_port_ptr != 0`, the runtime writes the actual bound port (u32 little-endian) to guest memory at `out_port_ptr`.

## Handle Semantics

### Connect mode (stream handle)

The returned handle supports:

- `zi_read`: reads from the socket.
- `zi_write`: writes to the socket.
- `zi_end`: closes the socket.

The socket is nonblocking:

- If the connection is still in progress, `zi_read`/`zi_write` may return `ZI_E_AGAIN`.
- On success, `zi_read` returns `0` at EOF (peer closed).
- Would-block returns `ZI_E_AGAIN`.

### Listen mode (listener handle)

The returned handle supports:

- `zi_read`: performs a single accept and returns an accept record.
- `zi_end`: closes the listener.

`zi_write` is not supported on listeners and returns `ZI_E_NOSYS`.

The listener handle is pollable with `sys/loop@v1`. Readability indicates an accept is likely to succeed; spurious readiness is allowed (guest must tolerate `ZI_E_AGAIN`).

## Accept Record

Listener `zi_read()` returns one or more fixed 32-byte records (little-endian). The caller must pass `cap >= 32`. If `cap > 32`, the runtime may return multiple records in a single call (return value is a multiple of 32).

```
Offset | Size | Field
-------|------|-------------------------------
0      | 4    | conn_handle (u32)
4      | 4    | peer_port   (u32)
8      | 16   | peer_addr   (u8[16]) IPv4-mapped-IPv6
24     | 4    | local_port  (u32)
28     | 4    | reserved    (u32) = 0
```

- `conn_handle` is a newly allocated stream handle for the accepted connection.
- `peer_addr` is the remote address in IPv4-mapped-IPv6 form (`::ffff:a.b.c.d` for IPv4).
- The record includes `local_port` only (no local address bytes).

## Nonblocking + `sys/loop` usage

All I/O uses nonblocking semantics. Guests should:

1. Attempt operation (`zi_read`/`zi_write`).
2. If it returns `ZI_E_AGAIN`, use `sys/loop` WATCH for readability/writability.
3. POLL, then retry the operation.

Spurious wakeups are permitted; the guest must tolerate re-trying and receiving `ZI_E_AGAIN` again.

## Notes / Limitations

- There is no `GETPEERNAME`/metadata query operation in v1.
- Connect mode does not support `port=0`.
- Listener mode supports `port=0` for ephemeral bind (and can report the bound port via `out_port_ptr`).

### Half-close (shutdown write)

TCP stream handles support a write-side half-close via `zi_ctl`:

- Send a `ZCL1` request to `zi_ctl` with `op = ZI_CTL_OP_HANDLE_OP`.
- Payload (16 bytes, little-endian): `version=1`, `handle`, `op=ZI_HANDLE_OP_SHUT_WR`, `reserved=0`.

After shutdown-write:

- The peer will eventually observe EOF (`zi_read` returns 0) once all queued bytes are delivered.
- Further `zi_write` calls on that handle fail (writes are disabled), but `zi_read` may continue to succeed.

## Version History

- **v1 (2.5):** Stable `zi_cap_open`-based TCP streams with nonblocking I/O; listener support via `ZI_TCP_OPEN_LISTEN` and accept records.
