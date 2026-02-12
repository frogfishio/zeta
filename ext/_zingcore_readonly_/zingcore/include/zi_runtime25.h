#pragma once

#include <stdint.h>

#include "zi_sysabi25.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * zingcore “family 25” wiring API (NOT the wire/system ABI).
 *
 * - The wire/system ABI is the `zi_*` syscall surface in zi_sysabi25.h.
 * - The `*25*` suffix is a family namespace for the zingcore implementation/wiring layer.
 *
 * Policy: 25 is a long-lived family identifier (2.5, 2.6, 2.7, …) until we make a truly
 * incompatible wiring break (e.g. a future family like 38).
 */

typedef struct zi_mem_v1 {
  void *ctx;
  // Map a guest pointer range for read. Returns 1 on success.
  int (*map_ro)(void *ctx, zi_ptr_t ptr, zi_size32_t len, const uint8_t **out);
  // Map a guest pointer range for write. Returns 1 on success.
  int (*map_rw)(void *ctx, zi_ptr_t ptr, zi_size32_t len, uint8_t **out);
} zi_mem_v1;

typedef struct zi_host_v1 {
  void *ctx;

  // Optional overrides. If NULL, zingcore provides a default.
  uint32_t (*abi_version)(void *ctx);

  // Core syscall hooks. If NULL, zingcore may return ZI_E_NOSYS (or noop for telemetry).
  int32_t (*ctl)(void *ctx, zi_ptr_t req_ptr, zi_size32_t req_len,
                 zi_ptr_t resp_ptr, zi_size32_t resp_cap);

  int32_t (*read)(void *ctx, zi_handle_t h, zi_ptr_t dst_ptr, zi_size32_t cap);
  int32_t (*write)(void *ctx, zi_handle_t h, zi_ptr_t src_ptr, zi_size32_t len);
  int32_t (*end)(void *ctx, zi_handle_t h);

  zi_ptr_t (*alloc)(void *ctx, zi_size32_t size);
  int32_t (*free)(void *ctx, zi_ptr_t ptr);

  int32_t (*telemetry)(void *ctx, zi_ptr_t topic_ptr, zi_size32_t topic_len,
                       zi_ptr_t msg_ptr, zi_size32_t msg_len);
} zi_host_v1;

// Configure the process-global host adapter and memory mapper.
//
// These are optional: if not set, core syscalls return ZI_E_NOSYS (telemetry is a noop).
void zi_runtime25_set_host(const zi_host_v1 *host);
void zi_runtime25_set_mem(const zi_mem_v1 *mem);

const zi_host_v1 *zi_runtime25_host(void);
const zi_mem_v1 *zi_runtime25_mem(void);

// Configure the process argv snapshot for capability implementations.
// If not set, argv-related caps treat argc as 0.
void zi_runtime25_set_argv(int argc, const char *const *argv);
void zi_runtime25_get_argv(int *out_argc, const char *const **out_argv);

// Configure the process environment snapshot for capability implementations.
// If not set, env-related caps treat envc as 0.
void zi_runtime25_set_env(int envc, const char *const *envp);
void zi_runtime25_get_env(int *out_envc, const char *const **out_envp);

// Helpers for common embedding modes.
//
// Native embedding where guest pointers are process pointers.
void zi_mem_v1_native_init(zi_mem_v1 *out);

#ifdef __cplusplus
} // extern "C"
#endif
