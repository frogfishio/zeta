#pragma once

#include <stdint.h>

#include "zi_caps.h"
#include "zi_sysabi25.h"

#ifdef __cplusplus
extern "C" {
#endif

// Golden capability: proc/hopper (version 1)
//
// kind = "proc"
// name = "hopper"
//
// This cap is opened via zi_cap_open() and yields a bidirectional handle.
// Requests are written as ZCL1 frames; replies are read back as ZCL1 frames.
//
// Open params (optional) are a packed little-endian struct:
//   u32 version (must be 1)
//   u32 arena_bytes
//   u32 ref_count
//
// If params_len==0, defaults are used.
//
// This initial golden cap uses a small built-in catalog (layout_id=1) suitable
// for smoke tests. It can be extended later to support catalog registration.

#define ZI_CAP_NAME_HOPPER "hopper"

#ifndef ZI_CAP_KIND_PROC
#define ZI_CAP_KIND_PROC "proc"
#endif

enum {
  ZI_HOPPER_OP_INFO = 1,
  ZI_HOPPER_OP_RESET = 2,
  ZI_HOPPER_OP_RECORD = 3,
  ZI_HOPPER_OP_FIELD_SET_BYTES = 4,
  ZI_HOPPER_OP_FIELD_GET_BYTES = 5,
  ZI_HOPPER_OP_FIELD_SET_I32 = 6,
  ZI_HOPPER_OP_FIELD_GET_I32 = 7,
};

const zi_cap_v1 *zi_proc_hopper25_cap(void);
int zi_proc_hopper25_register(void);

// Implementation hook used by zi_cap_open() when proc/hopper is selected.
// Returns a handle (>=3) on success or a negative ZI_E_* error.
zi_handle_t zi_proc_hopper25_open_from_params(zi_ptr_t params_ptr, zi_size32_t params_len);

#ifdef __cplusplus
} // extern "C"
#endif
