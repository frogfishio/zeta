#include "zi_caps.h"
#include "zi_handles25.h"
#include "zi_runtime25.h"
#include "zi_sysabi25.h"

#include <stdio.h>
#include <string.h>

// Conformance test: zi_abi_version returns 0x00020005

int main(void) {
  uint32_t ver = zi_abi_version();
  
  if (ver != ZI_SYSABI25_ZABI_VERSION) {
    fprintf(stderr, "FAIL: zi_abi_version returned 0x%08x, expected 0x%08x\n",
            ver, ZI_SYSABI25_ZABI_VERSION);
    return 1;
  }
  
  if (ver != 0x00020005u) {
    fprintf(stderr, "FAIL: zi_abi_version returned 0x%08x, expected 0x00020005\n", ver);
    return 1;
  }
  
  printf("PASS: zi_abi_version = 0x%08x\n", ver);
  return 0;
}
