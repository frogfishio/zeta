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
// Open params are a packed little-endian struct (20 bytes):
//   u64 host_ptr  (UTF-8 host bytes, not NUL-terminated)
//   u32 host_len
//   u32 port      (1..65535)
//   u32 flags     (reserved; must be 0)
//
// Sandboxing:
// - By default (ZI_NET_ALLOW unset/empty), only loopback hosts are permitted:
//   "localhost", "127.0.0.1", "::1" (also accepts "[::1]").
// - If ZI_NET_ALLOW is set:
//   - "any" allows any host:port.
//   - Comma-separated entries of the form "host:port" or "host:*" or "loopback".

#define ZI_CAP_KIND_NET "net"
#define ZI_CAP_NAME_TCP "tcp"

const zi_cap_v1 *zi_net_tcp25_cap(void);
int zi_net_tcp25_register(void);

// Implementation hook used by zi_cap_open() when net/tcp is selected.
// Returns a handle (>=3) on success or a negative ZI_E_* error.
zi_handle_t zi_net_tcp25_open_from_params(zi_ptr_t params_ptr, zi_size32_t params_len);

#ifdef __cplusplus
} // extern "C"
#endif
