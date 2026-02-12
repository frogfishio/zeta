#include "zi_sysabi25.h"
#include "zi_runtime25.h"

#include <stdio.h>
#include <string.h>

static void zcl1_write_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void zcl1_write_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t zcl1_read_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void build_caps_list_req(uint8_t *req, uint32_t rid) {
  memcpy(req + 0, "ZCL1", 4);
  zcl1_write_u16(req + 4, 1);
  zcl1_write_u16(req + 6, (uint16_t)ZI_CTL_OP_CAPS_LIST);
  zcl1_write_u32(req + 8, rid);
  zcl1_write_u32(req + 12, 0);
  zcl1_write_u32(req + 16, 0);
  zcl1_write_u32(req + 20, 0);
}

int main(void) {
  // Configure native memory mapping so zi_ctl can read/write.
  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);

  if (zi_abi_version() != ZI_SYSABI25_ZABI_VERSION) {
    fprintf(stderr, "zi_abi_version mismatch\n");
    return 1;
  }

  if (zi_read(3, 0, 0) != ZI_E_NOSYS) {
    fprintf(stderr, "expected zi_read nosys\n");
    return 1;
  }
  if (zi_write(3, 0, 0) != ZI_E_NOSYS) {
    fprintf(stderr, "expected zi_write nosys\n");
    return 1;
  }
  if (zi_end(3) != ZI_E_NOSYS) {
    fprintf(stderr, "expected zi_end nosys\n");
    return 1;
  }

  if (zi_alloc(16) != 0) {
    fprintf(stderr, "expected zi_alloc to return 0 without host allocator\n");
    return 1;
  }
  if (zi_free(123) != ZI_E_NOSYS) {
    fprintf(stderr, "expected zi_free nosys without host free\n");
    return 1;
  }

  if (zi_telemetry(0, 0, 0, 0) != 0) {
    fprintf(stderr, "expected zi_telemetry noop\n");
    return 1;
  }

  // CTL exists but with no caps system, it should say NO (ZI_E_NOSYS).
  uint8_t req[24];
  uint8_t resp[128];
  memset(resp, 0xAA, sizeof(resp));
  build_caps_list_req(req, 7);

  int32_t r = zi_ctl((zi_ptr_t)(uintptr_t)req, (zi_size32_t)sizeof(req), (zi_ptr_t)(uintptr_t)resp,
                     (zi_size32_t)sizeof(resp));
  if (r != ZI_E_NOSYS) {
    fprintf(stderr, "expected zi_ctl nosys without caps system, got %d\n", r);
    return 1;
  }

  // Also verify cap_* are treated as absent if caps system not initialized.
  if (zi_cap_count() != ZI_E_NOSYS) {
    fprintf(stderr, "expected zi_cap_count nosys when caps system not initialized\n");
    return 1;
  }

  // Basic sanity on response buffer not being written in nosys case.
  if (zcl1_read_u32(resp) != 0xAAAAAAAAu) {
    fprintf(stderr, "unexpected resp mutation\n");
    return 1;
  }

  printf("ok\n");
  return 0;
}
