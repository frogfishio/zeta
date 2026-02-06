# net/tcp v1 Protocol Specification

**Version:** 1  
**Capability Name:** `net/tcp`  
**Status:** Golden (Stable)

## Overview

The `net/tcp` capability provides sandboxed TCP networking to guest programs. It implements the standard handle stream protocol (`zi_read`, `zi_write`, `zi_end`) over TCP sockets opened via ZCL1 control operations.

**Key Design Principles:**
- **Sandbox First:** All network access is restricted by the `ZI_NET_ALLOW` environment variable
- **Stream Semantics:** Sockets are accessed via standard handle operations (read/write/end)
- **ZCL1 Control Plane:** Socket open/connect operations use ZCL1 for structured requests
- **Client-Only:** v1 supports outbound connections only (no listening/accept)

## Sandbox Model

### ZI_NET_ALLOW Environment Variable

The runtime **MUST** enforce network sandboxing via the `ZI_NET_ALLOW` environment variable:

```bash
# Allow specific domains
ZI_NET_ALLOW="example.com,*.api.service.com" zrun program.wat

# Allow IP ranges (CIDR notation)
ZI_NET_ALLOW="192.168.1.0/24,10.0.0.5" zrun program.wat

# Wildcard (allow all - USE WITH CAUTION)
ZI_NET_ALLOW="*" zrun program.wat
```

**Semantics:**
- Comma-separated list of allowed connection targets
- Each entry can be:
  - Domain name (exact match): `example.com`
  - Wildcard domain (prefix match): `*.example.com`
  - IPv4 address: `192.0.2.1`
  - IPv4 CIDR range: `192.0.2.0/24`
  - IPv6 address: `2001:db8::1`
  - IPv6 CIDR range: `2001:db8::/32`
  - Wildcard `*` (allow all)
- If `ZI_NET_ALLOW` is unset or empty, the capability **MUST NOT** be registered
- Connection attempts to disallowed targets **MUST** fail with `EACCES`

### DNS Resolution

- Implementations **MUST** resolve domain names to IP addresses before connection
- Implementations **MUST** validate resolved IP addresses against `ZI_NET_ALLOW`
- DNS resolution happens **within** the sandbox check (prevent DNS rebinding attacks)
- Implementations **SHOULD** cache DNS results (but respect TTL)

### Port Restrictions

- Implementations **MAY** impose port restrictions (e.g., block privileged ports <1024)
- Default policy: **allow all ports** unless restricted by platform policy
- Port restrictions are **independent** of `ZI_NET_ALLOW` sandbox

## ZCL1 Operations

All operations use the standard ZCL1 frame format (see `ZCL1_PROTOCOL.md`).

### 1. CONNECT (op=1)

Opens a TCP connection and returns a handle for streaming I/O.

**Request Payload:**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 2    | port        | Destination port (1-65535)
2      | 2    | flags       | Connection flags (reserved, must be 0)
4      | 4    | timeout_ms  | Connection timeout in milliseconds (0=default)
8      | N    | host        | UTF-8 hostname or IP (NOT null-terminated)
```

**Response Payload (success, status=0):**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | handle      | New socket handle (>=3)
4      | 4    | local_port  | Local port number (ephemeral)
8      | 16   | remote_addr | Remote IP address (16 bytes: IPv4-mapped-IPv6)
```

**Response Payload (error, status!=0):**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | errno       | POSIX errno code
4      | N    | message     | UTF-8 error description
```

**Error Codes:**
- `13` (EACCES) - Connection denied by `ZI_NET_ALLOW` sandbox
- `22` (EINVAL) - Invalid hostname or port
- `60` (ETIMEDOUT) - Connection timeout
- `61` (ECONNREFUSED) - Connection refused by remote host
- `65` (EHOSTUNREACH) - Host unreachable (no route)
- `111` (ECONNRESET) - Connection reset by peer (during handshake)

**Behavioral Requirements:**
- Opened handles support `zi_read`, `zi_write`, `zi_end`
- `zi_end` on a socket handle **MUST** close the connection and release the handle
- Multiple concurrent connections are supported
- Handle flags are `ZI_H_READABLE | ZI_H_WRITABLE` (sockets are bidirectional)

**IPv4-Mapped IPv6 Format:**

Remote addresses are returned in **IPv4-mapped IPv6** format (16 bytes):

```
IPv4 address 192.0.2.1:
[0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xFF, 0xFF, 0xC0, 0x00, 0x02, 0x01]
 (::ffff:192.0.2.1)

IPv6 address 2001:db8::1:
[0x20, 0x01, 0x0D, 0xB8, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01]
```

### 2. GETPEERNAME (op=2)

Retrieves the remote address of a connected socket.

**Request Payload:**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | handle      | Socket handle to query
```

**Response Payload (success, status=0):**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 16   | remote_addr | Remote IP address (IPv4-mapped-IPv6)
4      | 2    | remote_port | Remote port number
```

**Response Payload (error, status!=0):**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | errno       | Error code
4      | N    | message     | UTF-8 error description
```

**Error Codes:**
- `9` (EBADF) - Invalid handle or handle not a socket
- `107` (ENOTCONN) - Socket is not connected

## Stream Operations

Opened socket handles support the standard handle protocol:

### zi_read(handle, dst_ptr, cap)

**Semantics:**
- Reads up to `cap` bytes from the socket into guest memory at `dst_ptr`
- Returns number of bytes read (0-cap), or negative errno on error
- Returns `0` on graceful disconnect (EOF)
- **Blocking:** Waits until data is available or connection closes
- `ECONNRESET` if connection reset by peer
- `ETIMEDOUT` if read timeout exceeded (if configured)

### zi_write(handle, src_ptr, len)

**Semantics:**
- Writes `len` bytes from guest memory at `src_ptr` to the socket
- Returns number of bytes written (0-len), or negative errno on error
- **Blocking:** Waits until all bytes are buffered or connection closes
- `EPIPE` if connection closed by remote (write after close)
- `ECONNRESET` if connection reset by peer

### zi_end(handle)

**Semantics:**
- Closes the socket and releases the handle
- Sends TCP FIN (graceful shutdown)
- Returns `0` on success, negative errno on error
- Handle becomes invalid after this call
- Idempotent (calling twice is safe)

## Protocol Example

**Connecting to a remote server:**

```
// Guest calls zi_ctl with ZCL1 frame:
Magic:       "ZCL1"
Version:     1
Op:          1 (CONNECT)
RID:         3001
Status:      0
Reserved:    0
PayloadLen:  19
Payload:     [port=443, flags=0, timeout_ms=5000, host="example.com"]

// Runtime responds:
Magic:       "ZCL1"
Version:     1
Op:          1
RID:         3001
Status:      0 (success)
Reserved:    0
PayloadLen:  24
Payload:     [handle=3, local_port=54321, remote_addr=[...]]

// Guest can now use zi_write(3, ...) to send data, zi_read(3, ...) to receive
```

**Connection denied by sandbox:**

```
// Guest attempts to connect to disallowed host:
Op:          1 (CONNECT)
Payload:     [port=80, flags=0, timeout_ms=5000, host="forbidden.com"]

// Runtime responds:
Status:      13 (EACCES)
Payload:     [errno=13, message="Connection denied by ZI_NET_ALLOW policy"]
```

## TLS Support

**v1 does NOT include built-in TLS support.** Applications requiring TLS must:

1. **Use application-layer TLS libraries** (e.g., BoringSSL, mbedTLS) compiled to WASM
2. **Use proc/hopper TLS functions** (if provided by runtime)
3. **Use async capabilities** for non-blocking TLS handshakes

Future versions may add a `net/tls` capability with built-in TLS support.

## Non-Blocking I/O

**v1 uses blocking I/O only.** Applications requiring non-blocking I/O should:

1. **Use async/default capability** for concurrent connections
2. **Use multiple instances** of the guest program (one per connection)
3. **Wait for v2** which may add non-blocking socket modes

## Conformance Requirements

Implementations **MUST**:
1. Enforce `ZI_NET_ALLOW` sandbox strictly (no unapproved connections)
2. Support CONNECT operation with DNS resolution
3. Implement standard stream semantics (read/write/end)
4. Return correct POSIX errno codes for all error conditions
5. Support concurrent socket handles
6. Use IPv4-mapped-IPv6 format for all IP addresses
7. Perform DNS resolution within the sandbox check

Implementations **SHOULD**:
- Support both IPv4 and IPv6 destinations
- Cache DNS results (respecting TTL)
- Provide TCP keepalive options (platform default)
- Support graceful shutdown (TCP FIN on zi_end)

Implementations **MAY**:
- Impose limits on concurrent socket count
- Impose timeout defaults (read/write/connect)
- Restrict privileged ports (<1024)
- Provide GETPEERNAME operation for connection metadata

## Security Considerations

- **Sandbox Enforcement:** Strictly validate all connections against `ZI_NET_ALLOW`
- **DNS Rebinding:** Re-validate IP after DNS resolution (don't cache IPs across sandbox changes)
- **Resource Limits:** Impose limits on socket count, buffer sizes, timeout durations
- **SSRF Prevention:** Validate resolved IPs against private/loopback ranges if needed
- **Error Disclosure:** Error messages MUST NOT leak network topology or host information
- **Port Scanning:** Rate-limit connection attempts to prevent port scanning abuse

## Performance Notes

- **Connection Pooling:** Guest programs SHOULD reuse connections when possible
- **Buffering:** Implementations SHOULD buffer socket I/O for efficiency
- **Concurrent Connections:** Use async/default for high-concurrency scenarios
- **DNS Caching:** DNS results SHOULD be cached to reduce latency

## Future Extensions

Planned for future versions (v2+):

- **TCP server support** (listen/accept operations)
- **UDP sockets** (datagram protocol)
- **Non-blocking I/O modes** (O_NONBLOCK equivalent)
- **Socket options** (keepalive, nodelay, buffer sizes)
- **Unix domain sockets** (local IPC)
- **Built-in TLS** (net/tls capability)

## Version History

- **v1 (2.5):** Initial stable specification (golden capability, client-only TCP)
