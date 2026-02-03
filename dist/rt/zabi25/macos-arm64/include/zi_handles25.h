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
} zi_handle_ops_v1;

// Initializes the handle table (safe to call multiple times).
int zi_handles25_init(void);

// Allocates a new handle (>=3) with associated ops/ctx.
// Returns 0 on failure.
zi_handle_t zi_handle25_alloc(const zi_handle_ops_v1 *ops, void *ctx, uint32_t hflags);

// Looks up an existing handle.
// Returns 1 on success.
int zi_handle25_lookup(zi_handle_t h, const zi_handle_ops_v1 **out_ops, void **out_ctx, uint32_t *out_hflags);

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
