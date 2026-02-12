#pragma once

#include <stdint.h>

#include "zi_caps.h"
#include "zi_sysabi25.h"

#ifdef __cplusplus
extern "C" {
#endif

// Golden capability: net/tcp (version 1)
//
// kind = "net"
// name = "tcp"
//
// This cap is opened via zi_cap_open() and yields a stream handle usable with:
//   zi_read / zi_write / zi_end
//
// Sockets are nonblocking:
// - zi_read/zi_write return ZI_E_AGAIN on would-block.
// - While connect is still in progress, zi_read/zi_write MAY return ZI_E_AGAIN;
//   guests should wait for writability via sys/loop and retry.
//
// Open params are a packed little-endian struct (20 bytes):
//   u64 host_ptr  (UTF-8 host bytes, not NUL-terminated)
//   u32 host_len
//   u32 port      (connect: 1..65535, listen: 0..65535 where 0 means ephemeral)
//   u32 flags
//
// Flags:
// - 0 (default): outbound connect stream
// - ZI_TCP_OPEN_LISTEN: create a listener socket (bind+listen)
//
// Listener mode params extension (optional):
// - If flags has ZI_TCP_OPEN_LISTEN and params_len >= 24, then:
//   u32 backlog (0 => runtime default)
// - If flags has ZI_TCP_OPEN_LISTEN and params_len >= 32, then:
//   u64 out_port_ptr (guest pointer to u32; runtime writes actual bound port)
//
// Listener handle semantics:
// - The returned handle is pollable with sys/loop.
// - Readiness (readable) means an accept() is likely to succeed.
// - zi_read() returns one or more fixed-size accept records (32 bytes each).
//   If cap > 32, the runtime MAY return multiple records in one call and
//   returns a multiple of 32.
// - If no connection is available, zi_read() returns ZI_E_AGAIN.
//
// Accept record format (32 bytes, little-endian):
//   u32 conn_handle      (>=3)
//   u32 peer_port        (1..65535)
//   u8  peer_addr[16]    (IPv4-mapped-IPv6)
//   u32 local_port       (1..65535)
//   u32 reserved         (0)
//
// Sandboxing:
// - By default (ZI_NET_ALLOW unset/empty), only loopback hosts are permitted:
//   "localhost", "127.0.0.1", "::1" (also accepts "[::1]").
// - If ZI_NET_ALLOW is set:
//   - "any" allows any host:port.
//   - Comma-separated entries of the form "host:port" or "host:*" or "loopback".

#define ZI_CAP_KIND_NET "net"
#define ZI_CAP_NAME_TCP "tcp"

// Open flags.
#define ZI_TCP_OPEN_LISTEN (1u << 0)
#define ZI_TCP_OPEN_REUSEADDR (1u << 1)
#define ZI_TCP_OPEN_REUSEPORT (1u << 2)
#define ZI_TCP_OPEN_IPV6ONLY (1u << 3)
#define ZI_TCP_OPEN_NODELAY (1u << 4)
#define ZI_TCP_OPEN_KEEPALIVE (1u << 5)

const zi_cap_v1 *zi_net_tcp25_cap(void);
int zi_net_tcp25_register(void);

// Implementation hook used by zi_cap_open() when net/tcp is selected.
// Returns a handle (>=3) on success or a negative ZI_E_* error.
zi_handle_t zi_net_tcp25_open_from_params(zi_ptr_t params_ptr, zi_size32_t params_len);

#ifdef __cplusplus
} // extern "C"
#endif
