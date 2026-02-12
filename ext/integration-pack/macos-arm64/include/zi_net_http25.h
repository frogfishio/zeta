#pragma once

#include <stdint.h>

#include "zi_caps.h"
#include "zi_sysabi25.h"

#ifdef __cplusplus
extern "C" {
#endif

// Golden capability: net/http (version 1)
//
// kind = "net"
// name = "http"
//
// This cap is opened via zi_cap_open() and yields a bidirectional stream handle.
// Requests are written as ZCL1 frames; responses/events are read back as ZCL1 frames.
// See: src/zingcore/2.5/abi/HTTP_PROTOCOL.md

#define ZI_CAP_KIND_NET "net"
#define ZI_CAP_NAME_HTTP "http"

const zi_cap_v1 *zi_net_http25_cap(void);
int zi_net_http25_register(void);

// Implementation hook used by zi_cap_open() when net/http is selected.
// Returns a handle (>=3) on success or a negative ZI_E_* error.
zi_handle_t zi_net_http25_open_from_params(zi_ptr_t params_ptr, zi_size32_t params_len);

#ifdef __cplusplus
} // extern "C"
#endif
