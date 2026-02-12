#include "zi_handles25.h"
#include "zi_runtime25.h"
#include "zi_sysabi25.h"

#include <stdio.h>
#include <string.h>

// Conformance test: Handles 0/1/2 are reserved and cannot be allocated via zi_handle25_alloc

static int32_t dummy_read(void *ctx, zi_ptr_t dst, zi_size32_t cap) {
  (void)ctx; (void)dst; (void)cap;
  return 0;
}

static int32_t dummy_write(void *ctx, zi_ptr_t src, zi_size32_t len) {
  (void)ctx; (void)src; (void)len;
  return 0;
}

static int32_t dummy_end(void *ctx) {
  (void)ctx;
  return 0;
}

static const zi_handle_ops_v1 dummy_ops = {
  .read = dummy_read,
  .write = dummy_write,
  .end = dummy_end,
};

int main(void) {
  zi_handles25_reset_for_test();
  
  if (!zi_handles25_init()) {
    fprintf(stderr, "FAIL: zi_handles25_init failed\n");
    return 1;
  }
  
  // Allocate a handle; should get >=3
  zi_handle_t h = zi_handle25_alloc(&dummy_ops, NULL, ZI_H_READABLE);
  
  if (h < 3) {
    fprintf(stderr, "FAIL: zi_handle25_alloc returned %d, expected >=3\n", h);
    return 1;
  }
  
  // Validate we cannot use 0/1/2 for ops lookup
  const zi_handle_ops_v1 *ops0 = NULL;
  const zi_handle_ops_v1 *ops1 = NULL;
  const zi_handle_ops_v1 *ops2 = NULL;
  void *ctx = NULL;
  uint32_t flags = 0;
  
  int rc0 = zi_handle25_lookup(0, &ops0, &ctx, &flags);
  int rc1 = zi_handle25_lookup(1, &ops1, &ctx, &flags);
  int rc2 = zi_handle25_lookup(2, &ops2, &ctx, &flags);
  
  if (rc0 != 0 || rc1 != 0 || rc2 != 0) {
    fprintf(stderr, "FAIL: handles 0/1/2 lookup succeeded (should be reserved)\n");
    return 1;
  }
  
  // Cleanup
  zi_handle25_release(h);
  
  printf("PASS: handles 0/1/2 are reserved, allocation starts at 3\n");
  return 0;
}
