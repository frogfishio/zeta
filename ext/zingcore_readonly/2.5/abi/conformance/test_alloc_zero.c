#include "zi_caps.h"
#include "zi_handles25.h"
#include "zi_runtime25.h"
#include "zi_sysabi25.h"

#include <stdio.h>
#include <stdlib.h>

// Conformance test: zi_alloc(0) behavior
//
// zABI 2.5 does not yet specify whether zi_alloc(0) should:
// - Return 0 (error/invalid)
// - Return a valid pointer (heap base)
//
// This test documents current behavior and will be updated when spec is finalized.

int main(void) {
  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);
  
  zi_ptr_t ptr = zi_alloc(0);
  
  // Current behavior: returns 0 (error)
  if (ptr == 0) {
    printf("PASS: zi_alloc(0) = 0 (error)\n");
    return 0;
  }
  
  // Alternative behavior: returns valid pointer
  if (ptr > 0) {
    printf("PASS: zi_alloc(0) = 0x%llx (valid pointer)\n", (unsigned long long)ptr);
    // Try to free it
    int32_t rc = zi_free(ptr);
    if (rc < 0) {
      fprintf(stderr, "WARNING: zi_free(zi_alloc(0)) failed: %d\n", rc);
    }
    return 0;
  }
  
  fprintf(stderr, "FAIL: zi_alloc(0) returned negative value %lld\n", (long long)ptr);
  return 1;
}
