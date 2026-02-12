#pragma once

#include <stdint.h>

#include "zi_caps.h"
#include "zi_sysabi25.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZI_CAP_KIND_SYS "sys"
#define ZI_CAP_NAME_LOOP "loop"

typedef enum zi_sys_loop_op_v1 {
  ZI_SYS_LOOP_OP_WATCH = 1,
  ZI_SYS_LOOP_OP_UNWATCH = 2,
  ZI_SYS_LOOP_OP_TIMER_ARM = 3,
  ZI_SYS_LOOP_OP_TIMER_CANCEL = 4,
  ZI_SYS_LOOP_OP_POLL = 5,
} zi_sys_loop_op_v1;

// Registers sys/loop@v1 in the capability registry.
int zi_sys_loop25_register(void);

// Returns the cap descriptor.
const zi_cap_v1 *zi_sys_loop25_cap(void);

// Opens a sys/loop handle; params must be empty for v1.
zi_handle_t zi_sys_loop25_open_from_params(zi_ptr_t params_ptr, zi_size32_t params_len);

#ifdef __cplusplus
} // extern "C"
#endif
