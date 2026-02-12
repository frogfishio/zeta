#pragma once

#include <stdint.h>

#include "zi_async.h"
#include "zi_caps.h"
#include "zi_sysabi25.h"

#ifdef __cplusplus
extern "C" {
#endif

// Golden capability: async/default (version 1)
//
// kind = "async"
// name = "default"
//
// This cap provides a basic async invocation + future event stream.
// It is opened via zi_cap_open() and yields a bidirectional stream handle.
// Requests are written as ZCL1 frames; responses/events are read back as ZCL1 frames.
//
// This integrates with the zingcore async selector registry (zi_async_*):
// INVOKE targets a (cap_kind, cap_name, selector) registered in the selector registry.

#define ZI_CAP_KIND_ASYNC "async"
#define ZI_CAP_NAME_DEFAULT "default"

// ZCL1 request ops.
enum {
  ZI_ASYNC_OP_LIST = 1,
  ZI_ASYNC_OP_INVOKE = 2,
  ZI_ASYNC_OP_CANCEL = 3,
};

// ZCL1 event ops (asynchronous notifications appended after responses).
enum {
  ZI_ASYNC_EV_ACK = 100,
  ZI_ASYNC_EV_FAIL = 101,
  ZI_ASYNC_EV_FUTURE_OK = 102,
  ZI_ASYNC_EV_FUTURE_FAIL = 103,
  ZI_ASYNC_EV_FUTURE_CANCEL = 104,
};

// Async error codes used in response payloads.
enum {
  ZI_ASYNC_OK = 0,
  ZI_ASYNC_E_NOENT = 1,
  ZI_ASYNC_E_INVALID = 2,
  ZI_ASYNC_E_OOM = 3,
  ZI_ASYNC_E_INTERNAL = 4,
  ZI_ASYNC_E_UNSUPPORTED = 5,
};

// Returns the cap descriptor for async/default (version 1).
const zi_cap_v1 *zi_async_default25_cap(void);

// Convenience: registers async/default into the global cap registry.
// Requires zi_caps_init() to have been called.
int zi_async_default25_register(void);

// Convenience: registers built-in selectors for async/default into the selector registry.
// Requires:
// - zi_async_init() already called
// - async/default cap already registered (cap must exist for selector registration)
//
// Currently registers:
// - ping.v1 : immediately ack + future_ok("pong")
int zi_async_default25_register_selectors(void);

// Implementation hook used by zi_cap_open() when async/default is selected.
// Returns a handle (>=3) on success or a negative ZI_E_* error.
zi_handle_t zi_async_default25_open_from_params(zi_ptr_t params_ptr, zi_size32_t params_len);

#ifdef __cplusplus
} // extern "C"
#endif
