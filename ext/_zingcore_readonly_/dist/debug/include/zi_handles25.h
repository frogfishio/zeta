#pragma once

#include <stdint.h>

#include "zi_sysabi25.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zi_handle_ops_v1 {
  int32_t (*read)(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap);
  int32_t (*write)(void *ctx, zi_ptr_t src_ptr, zi_size32_t len);
  int32_t (*end)(void *ctx);
  // Optional per-handle control operations (invoked via zi_ctl op ZI_CTL_OP_HANDLE_OP).
  int32_t (*ctl)(void *ctx, uint32_t op, zi_ptr_t arg_ptr, zi_size32_t arg_len);
} zi_handle_ops_v1;

// Optional internal poll interface for integrating handles with sys/loop.
//
// This is NOT a guest-visible ABI. It allows the runtime to watch readiness for
// handles that are backed by OS waitables (typically file descriptors).
typedef struct zi_handle_poll_ops_v1 {
  // Returns 1 and writes a non-negative fd to out_fd if the handle is pollable.
  // Returns 0 if the handle is not pollable or has no associated fd.
  int (*get_fd)(void *ctx, int *out_fd);

  // Optional: returns current level-triggered readiness bits for the handle.
  // Bits use the guest-visible sys/loop readiness mask (READABLE/WRITABLE/etc).
  // sys/loop may use this to report readiness independent of OS POLLOUT/POLLIN.
  uint32_t (*get_ready)(void *ctx);

  // Optional: drains/acknowledges the wakeup fd returned by get_fd().
  // This is used when the returned fd is a pure wakeup notifier rather than a
  // data stream that sys/loop should not consume.
  void (*drain_wakeup)(void *ctx);
} zi_handle_poll_ops_v1;

// Initializes the handle table (safe to call multiple times).
int zi_handles25_init(void);

// Allocates a new handle (>=3) with associated ops/ctx.
// Returns 0 on failure.
zi_handle_t zi_handle25_alloc(const zi_handle_ops_v1 *ops, void *ctx, uint32_t hflags);

// Allocates a new handle (>=3) with associated ops/ctx and optional poll ops.
// Returns 0 on failure.
zi_handle_t zi_handle25_alloc_with_poll(const zi_handle_ops_v1 *ops, const zi_handle_poll_ops_v1 *poll_ops, void *ctx, uint32_t hflags);

// Looks up an existing handle.
// Returns 1 on success.
int zi_handle25_lookup(zi_handle_t h, const zi_handle_ops_v1 **out_ops, void **out_ctx, uint32_t *out_hflags);

// If the handle is pollable, returns 1 and stores its fd in out_fd.
// Otherwise returns 0.
int zi_handle25_poll_fd(zi_handle_t h, int *out_fd);

// If the handle has poll ops, returns 1 and stores poll_ops/ctx.
// Otherwise returns 0.
int zi_handle25_poll_ops(zi_handle_t h, const zi_handle_poll_ops_v1 **out_poll_ops, void **out_ctx);

// Releases (invalidates) an existing handle.
// Returns 1 if the handle existed and was released.
int zi_handle25_release(zi_handle_t h);

// Returns 0 if unknown.
uint32_t zi_handle25_hflags(zi_handle_t h);

// Test-only reset.
void zi_handles25_reset_for_test(void);

#ifdef __cplusplus
} // extern "C"
#endif
