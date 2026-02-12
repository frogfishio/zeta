#pragma once

#include <stdint.h>

#include "zi_caps.h"
#include "zi_sysabi25.h"

#ifdef __cplusplus
extern "C" {
#endif

// Golden capability: event/bus (version 1)
//
// kind = "event"
// name = "bus"
//
// This cap provides an in-process pub/sub event bus.
// It is opened via zi_cap_open() and yields a bidirectional stream handle.
// Requests are written as ZCL1 frames; responses/events are read back as ZCL1 frames.

#define ZI_CAP_KIND_EVENT "event"
#define ZI_CAP_NAME_BUS "bus"

// ZCL1 request ops.
enum {
  ZI_EVENT_BUS_OP_SUBSCRIBE = 1,
  ZI_EVENT_BUS_OP_UNSUBSCRIBE = 2,
  ZI_EVENT_BUS_OP_PUBLISH = 3,
};

// ZCL1 event ops.
enum {
  ZI_EVENT_BUS_EV_EVENT = 100,
};

const zi_cap_v1 *zi_event_bus25_cap(void);
int zi_event_bus25_register(void);

// Implementation hook used by zi_cap_open() when event/bus is selected.
// Returns a handle (>=3) on success or a negative ZI_E_* error.
zi_handle_t zi_event_bus25_open_from_params(zi_ptr_t params_ptr, zi_size32_t params_len);

#ifdef __cplusplus
} // extern "C"
#endif
