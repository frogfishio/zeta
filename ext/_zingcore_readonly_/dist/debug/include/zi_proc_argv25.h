#pragma once

#include <stdint.h>

#include "zi_caps.h"
#include "zi_sysabi25.h"

#ifdef __cplusplus
extern "C" {
#endif

// Standard capability identity for argv exposure.
//
// kind = "proc"
// name = "argv"
//
// This cap is opened via zi_cap_open() (with empty params) and yields a
// read-only stream handle. The stream content is a packed little-endian blob:
//
//   u32 version (currently 1)
//   u32 argc
//   repeat argc times:
//     u32 len
//     bytes[len]
//
// Strings are raw bytes from the host argv; they are not NUL-terminated.

#define ZI_CAP_KIND_PROC "proc"
#define ZI_CAP_NAME_ARGV "argv"

const zi_cap_v1 *zi_proc_argv25_cap(void);
int zi_proc_argv25_register(void);

// Implementation hook used by zi_cap_open() when proc/argv is selected.
// Returns a handle (>=3) on success or a negative ZI_E_* error.
zi_handle_t zi_proc_argv25_open(void);

#ifdef __cplusplus
} // extern "C"
#endif
