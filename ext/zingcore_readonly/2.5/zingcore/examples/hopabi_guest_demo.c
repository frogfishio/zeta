#include "zi_hopabi25.h"
#include "zi_runtime25.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t read_u32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t read_i32le(const uint8_t *p) {
  return (int32_t)read_u32le(p);
}

// ---- guest code ----
// This is the part you want to be portable across native/wasm/jit.
// It assumes the embedder has already configured zi_runtime25 memory mapping.
static int guest_entry(void) {
  int32_t hop = zi_hop_open(0, 0);
  if (hop < 0) {
    fprintf(stderr, "zi_hop_open failed: %d\n", hop);
    return 1;
  }

  // 1) Generic allocation (layout_id=0) for raw buffers.
  uint8_t buf_ref_le[4] = {0};
  int32_t err = zi_hop_alloc(hop, 64, 16, (zi_ptr_t)(uintptr_t)buf_ref_le);
  if (err != 0) {
    fprintf(stderr, "zi_hop_alloc failed: %d\n", err);
    return 1;
  }
  int32_t buf_ref = read_i32le(buf_ref_le);
  printf("buf_ref=%d\n", buf_ref);

  // 2) Record allocation (layout_id=1) using the built-in demo catalog.
  // Fields:
  //   field 0 (bytes, width 4, pad ' ') at offset 0
  //   field 1 (u32-ish numeric exposed as i32) at offset 4
  uint8_t rec_ref_le[4] = {0};
  err = zi_hop_record(hop, 1, (zi_ptr_t)(uintptr_t)rec_ref_le);
  if (err != 0) {
    fprintf(stderr, "zi_hop_record failed: %d\n", err);
    return 1;
  }
  int32_t rec_ref = read_i32le(rec_ref_le);
  printf("rec_ref=%d\n", rec_ref);

  // Set bytes field 0 (pads to 4)
  err = zi_hop_field_set_bytes(hop, rec_ref, 0, (zi_ptr_t)(uintptr_t)"hi", 2);
  if (err != 0) {
    fprintf(stderr, "set_bytes failed: %d\n", err);
    return 1;
  }

  // Set numeric field 1
  err = zi_hop_field_set_i32(hop, rec_ref, 1, 123);
  if (err != 0) {
    fprintf(stderr, "set_i32 failed: %d\n", err);
    return 1;
  }

  // Read bytes field 0 into local buffer
  uint8_t out[4] = {0};
  uint8_t written_le[4] = {0};
  err = zi_hop_field_get_bytes(hop, rec_ref, 0, (zi_ptr_t)(uintptr_t)out, sizeof(out),
                               (zi_ptr_t)(uintptr_t)written_le);
  if (err != 0) {
    fprintf(stderr, "get_bytes failed: %d\n", err);
    return 1;
  }
  uint32_t written = read_u32le(written_le);
  printf("raw='%.4s' (written=%u)\n", (const char *)out, written);

  // Read numeric field 1
  uint8_t v_le[4] = {0};
  err = zi_hop_field_get_i32(hop, rec_ref, 1, (zi_ptr_t)(uintptr_t)v_le);
  if (err != 0) {
    fprintf(stderr, "get_i32 failed: %d\n", err);
    return 1;
  }
  printf("num=%d\n", read_i32le(v_le));

  // Freeing a ref releases the slot; arena bytes are not reclaimed (arena-style).
  err = zi_hop_free(hop, buf_ref);
  if (err != 0) {
    fprintf(stderr, "zi_hop_free failed: %d\n", err);
    return 1;
  }

  if (zi_hop_close(hop) != 0) {
    fprintf(stderr, "zi_hop_close failed\n");
    return 1;
  }

  printf("ok\n");
  return 0;
}

// ---- embedder/host code ----
// Native demo: the embedder configures runtime services, then calls guest code.
int main(void) {
  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);
  return guest_entry();
}
