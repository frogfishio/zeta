#include "zi_hopabi25.h"
#include "zi_runtime25.h"

#include "vendor/hopper/hopper.h"

#include <stdio.h>
#include <string.h>

static uint32_t read_u32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t read_i32le(const uint8_t *p) {
  return (int32_t)read_u32le(p);
}

int main(void) {
  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);

  // Open default Hopper instance.
  int32_t hop = zi_hop_open(0, 0);
  if (hop < 0) {
    fprintf(stderr, "zi_hop_open failed: %d\n", hop);
    return 1;
  }

  // Generic alloc.
  uint8_t ref_le[4] = {0};
  int32_t err = zi_hop_alloc(hop, 32, 8, (zi_ptr_t)(uintptr_t)ref_le);
  if (err != 0) {
    fprintf(stderr, "zi_hop_alloc err=%d\n", err);
    return 1;
  }
  int32_t ref = read_i32le(ref_le);
  if (ref < 0) {
    fprintf(stderr, "bad ref %d\n", ref);
    return 1;
  }

  // Built-in record layout_id=1.
  memset(ref_le, 0, sizeof(ref_le));
  err = zi_hop_record(hop, 1, (zi_ptr_t)(uintptr_t)ref_le);
  if (err != 0) {
    fprintf(stderr, "zi_hop_record err=%d\n", err);
    return 1;
  }
  int32_t rref = read_i32le(ref_le);
  if (rref < 0) {
    fprintf(stderr, "bad record ref %d\n", rref);
    return 1;
  }

  // Set/get bytes field 0 (raw) should pad to 4.
  const char *msg = "hi";
  err = zi_hop_field_set_bytes(hop, rref, 0, (zi_ptr_t)(uintptr_t)msg, 2);
  if (err != 0) {
    fprintf(stderr, "set_bytes err=%d\n", err);
    return 1;
  }

  uint8_t out[4] = {0};
  uint8_t written_le[4] = {0};
  err = zi_hop_field_get_bytes(hop, rref, 0, (zi_ptr_t)(uintptr_t)out, 4, (zi_ptr_t)(uintptr_t)written_le);
  if (err != 0) {
    fprintf(stderr, "get_bytes err=%d\n", err);
    return 1;
  }
  if (read_u32le(written_le) != 4 || memcmp(out, "hi  ", 4) != 0) {
    fprintf(stderr, "get_bytes mismatch\n");
    return 1;
  }

  // Set/get i32 field 1
  err = zi_hop_field_set_i32(hop, rref, 1, 123);
  if (err != 0) {
    fprintf(stderr, "set_i32 err=%d\n", err);
    return 1;
  }

  uint8_t v_le[4] = {0};
  err = zi_hop_field_get_i32(hop, rref, 1, (zi_ptr_t)(uintptr_t)v_le);
  if (err != 0) {
    fprintf(stderr, "get_i32 err=%d\n", err);
    return 1;
  }
  if (read_i32le(v_le) != 123) {
    fprintf(stderr, "get_i32 mismatch\n");
    return 1;
  }

  // ---- negative paths ----

  // Bad alloc params.
  err = zi_hop_alloc(hop, 0, 1, (zi_ptr_t)(uintptr_t)ref_le);
  if (err != (int32_t)HOPPER_E_BAD_FIELD) {
    fprintf(stderr, "expected HOPPER_E_BAD_FIELD for size=0, got %d\n", err);
    return 1;
  }

  err = zi_hop_alloc(hop, 8, 3, (zi_ptr_t)(uintptr_t)ref_le);
  if (err != (int32_t)HOPPER_E_BAD_FIELD) {
    fprintf(stderr, "expected HOPPER_E_BAD_FIELD for bad align, got %d\n", err);
    return 1;
  }

  // Too-small dst buffer for a fixed-width field.
  memset(out, 0, sizeof(out));
  memset(written_le, 0, sizeof(written_le));
  err = zi_hop_field_get_bytes(hop, rref, 0, (zi_ptr_t)(uintptr_t)out, 3, (zi_ptr_t)(uintptr_t)written_le);
  if (err != ZI_E_BOUNDS) {
    fprintf(stderr, "expected ZI_E_BOUNDS for small dst_cap, got %d\n", err);
    return 1;
  }

  // Invalid field index.
  err = zi_hop_field_get_bytes(hop, rref, 99, (zi_ptr_t)(uintptr_t)out, 4, (zi_ptr_t)(uintptr_t)written_le);
  if (err != (int32_t)HOPPER_E_BAD_FIELD) {
    fprintf(stderr, "expected HOPPER_E_BAD_FIELD for bad field, got %d\n", err);
    return 1;
  }

  // Field type mismatch.
  err = zi_hop_field_set_i32(hop, rref, 0, 1);
  if (err != (int32_t)HOPPER_E_BAD_FIELD) {
    fprintf(stderr, "expected HOPPER_E_BAD_FIELD for i32->bytes, got %d\n", err);
    return 1;
  }

  // Too-long bytes write.
  err = zi_hop_field_set_bytes(hop, rref, 0, (zi_ptr_t)(uintptr_t)"hello", 5);
  if (err != (int32_t)HOPPER_E_PIC_INVALID) {
    fprintf(stderr, "expected HOPPER_E_PIC_INVALID for long bytes, got %d\n", err);
    return 1;
  }

  // Built-in get_bytes only supports layout_id=1; generic allocs use layout_id=0.
  err = zi_hop_field_get_bytes(hop, ref, 0, (zi_ptr_t)(uintptr_t)out, 4, (zi_ptr_t)(uintptr_t)written_le);
  if (err != (int32_t)HOPPER_E_BAD_LAYOUT) {
    fprintf(stderr, "expected HOPPER_E_BAD_LAYOUT for non-layout-1 ref, got %d\n", err);
    return 1;
  }

  // Invalid ref.
  err = zi_hop_free(hop, 999999);
  if (err != (int32_t)HOPPER_E_BAD_REF) {
    fprintf(stderr, "expected HOPPER_E_BAD_REF for invalid ref, got %d\n", err);
    return 1;
  }

  if (zi_hop_close(hop) != 0) {
    fprintf(stderr, "zi_hop_close failed\n");
    return 1;
  }

  printf("ok\n");
  return 0;
}
