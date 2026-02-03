#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * zABI System ABI — 2.5 wire contract
 *
 * Note on naming:
 * - The wire ABI is the `zi_*` symbol set (these names stay semantic).
 * - The "25" in this header filename selects the 2.5 wire contract at compile-time.
 * - zingcore's wiring/embedding APIs are family-namespaced separately (e.g. `zingcore25_*`).
 *
 * Minimal core ABI (always present):
 *   zi_abi_version
 *   zi_ctl
 *   zi_read, zi_write, zi_end
 *   zi_alloc, zi_free
 *   zi_telemetry
 *
 * Optional extension (caps model):
 *   If a runtime exposes any capabilities, it must also provide zi_cap_* and
 *   zi_handle_hflags.
 */

#define ZI_SYSABI25_ZABI_VERSION 0x00020005u

typedef int32_t zi_handle_t;

typedef uint64_t zi_ptr_t;
typedef uint32_t zi_size32_t;

enum {
  ZI_OK = 0,

  ZI_E_INVALID  = -1,
  ZI_E_BOUNDS   = -2,
  ZI_E_NOENT    = -3,
  ZI_E_DENIED   = -4,
  ZI_E_CLOSED   = -5,
  ZI_E_AGAIN    = -6,
  ZI_E_NOSYS    = -7,
  ZI_E_OOM      = -8,
  ZI_E_IO       = -9,
  ZI_E_INTERNAL = -10,
};

enum {
  ZI_CTL_OP_CAPS_LIST     = 1,
  ZI_CTL_OP_CAPS_DESCRIBE = 2,
  ZI_CTL_OP_CAPS_OPEN     = 3,
};

enum {
  ZI_CAP_CAN_OPEN  = 1u << 0,
  ZI_CAP_PURE      = 1u << 1,
  ZI_CAP_MAY_BLOCK = 1u << 2,
};

enum {
  ZI_H_READABLE = 1u << 0,
  ZI_H_WRITABLE = 1u << 1,
  ZI_H_ENDABLE  = 1u << 2,
  ZI_H_SEEKABLE = 1u << 3,
};

/* --- Minimal core surface (expected everywhere) --- */
uint32_t zi_abi_version(void);

int32_t zi_ctl(zi_ptr_t req_ptr, zi_size32_t req_len,
               zi_ptr_t resp_ptr, zi_size32_t resp_cap);

int32_t zi_read(zi_handle_t h, zi_ptr_t dst_ptr, zi_size32_t cap);
int32_t zi_write(zi_handle_t h, zi_ptr_t src_ptr, zi_size32_t len);
int32_t zi_end(zi_handle_t h);

zi_ptr_t zi_alloc(zi_size32_t size);
int32_t zi_free(zi_ptr_t ptr);

int32_t zi_telemetry(zi_ptr_t topic_ptr, zi_size32_t topic_len,
                     zi_ptr_t msg_ptr, zi_size32_t msg_len);

/* --- Caps extension (optional; required if any caps exist) --- */

int32_t zi_cap_count(void);
int32_t zi_cap_get_size(int32_t index);
int32_t zi_cap_get(int32_t index, zi_ptr_t out_ptr, zi_size32_t out_cap);
zi_handle_t zi_cap_open(zi_ptr_t req_ptr);
uint32_t zi_handle_hflags(zi_handle_t h);

#ifdef __cplusplus
} // extern "C"
#endif
