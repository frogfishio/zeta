#pragma once

#include <stdint.h>

#include "zi_caps.h"
#include "zi_sysabi25.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZI_CAP_KIND_SYS "sys"
#define ZI_CAP_NAME_INFO "info"

typedef enum zi_sys_info_op_v1 {
  ZI_SYS_INFO_OP_INFO = 1,
  ZI_SYS_INFO_OP_STATS = 2,
  ZI_SYS_INFO_OP_TIME_NOW = 3,
  ZI_SYS_INFO_OP_RANDOM_SEED = 4,
} zi_sys_info_op_v1;

// Registers sys/info@v1 in the capability registry.
int zi_sys_info25_register(void);

// Returns the cap descriptor.
const zi_cap_v1 *zi_sys_info25_cap(void);

// Opens a sys/info handle; params must be empty for v1.
zi_handle_t zi_sys_info25_open_from_params(zi_ptr_t params_ptr, zi_size32_t params_len);

#ifdef __cplusplus
} // extern "C"
#endif
