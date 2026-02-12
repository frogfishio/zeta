#include "zi_caps.h"
#include "zi_handles25.h"
#include "zi_runtime25.h"
#include "zi_sys_info25.h"
#include "zi_sysabi25.h"
#include "zi_zcl1.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void write_u16le(uint8_t *p, uint16_t v) { zi_zcl1_write_u16(p, v); }
static void write_u32le(uint8_t *p, uint32_t v) { zi_zcl1_write_u32(p, v); }

static void write_u64le(uint8_t *p, uint64_t v) {
  write_u32le(p + 0, (uint32_t)(v & 0xFFFFFFFFu));
  write_u32le(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static void build_open_req(uint8_t req[40], const char *kind, const char *name, const void *params, uint32_t params_len) {
  write_u64le(req + 0, (uint64_t)(uintptr_t)kind);
  write_u32le(req + 8, (uint32_t)strlen(kind));
  write_u64le(req + 12, (uint64_t)(uintptr_t)name);
  write_u32le(req + 20, (uint32_t)strlen(name));
  write_u32le(req + 24, 0);
  write_u64le(req + 28, (uint64_t)(uintptr_t)params);
  write_u32le(req + 36, params_len);
}

static void build_zcl1_req(uint8_t *out, uint16_t op, uint32_t rid) {
  memcpy(out + 0, "ZCL1", 4);
  write_u16le(out + 4, 1);
  write_u16le(out + 6, op);
  write_u32le(out + 8, rid);
  write_u32le(out + 12, 0);
  write_u32le(out + 16, 0);
  write_u32le(out + 20, 0);
}

static int read_frame(zi_handle_t h, uint8_t *buf, uint32_t cap, uint32_t *out_len) {
  uint32_t off = 0;
  for (;;) {
    int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(buf + off), (zi_size32_t)(cap - off));
    if (n == ZI_E_AGAIN) break;
    if (n < 0) return 0;
    if (n == 0) break;
    off += (uint32_t)n;
    if (off == cap) break;
  }
  *out_len = off;
  return 1;
}

static uint32_t read_u32le(const uint8_t *p) { return zi_zcl1_read_u32(p); }

static int expect_ok(const uint8_t *buf, uint32_t len, uint16_t op, uint32_t rid, zi_zcl1_frame *out) {
  if (!zi_zcl1_parse(buf, len, out)) return 0;
  if (out->op != op || out->rid != rid) return 0;
  if (read_u32le(buf + 12) != 1u) return 0;
  return 1;
}

int main(void) {
  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);

  zi_caps_reset_for_test();
  zi_handles25_reset_for_test();

  if (!zi_caps_init()) {
    fprintf(stderr, "FAIL: zi_caps_init\n");
    return 1;
  }
  if (!zi_sys_info25_register()) {
    fprintf(stderr, "FAIL: zi_sys_info25_register\n");
    return 1;
  }

  uint8_t open_req[40];
  build_open_req(open_req, "sys", "info", NULL, 0);
  zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)open_req);
  if (h < 3) {
    fprintf(stderr, "open sys/info@v1 failed: %d\n", h);
    return 1;
  }

  // INFO.
  {
    uint8_t req[24];
    build_zcl1_req(req, 1, 1);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)req, 24) != 24) return 1;
    uint8_t buf[4096];
    uint32_t got = 0;
    if (!read_frame(h, buf, (uint32_t)sizeof(buf), &got)) return 1;
    zi_zcl1_frame z;
    if (!expect_ok(buf, got, 1, 1, &z)) return 1;
    if (z.payload_len < 16u) return 1;
    if (read_u32le(z.payload + 0) != 1u) return 1;
  }

  // TIME_NOW.
  {
    uint8_t req[24];
    build_zcl1_req(req, 3, 2);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)req, 24) != 24) return 1;
    uint8_t buf[256];
    uint32_t got = 0;
    if (!read_frame(h, buf, (uint32_t)sizeof(buf), &got)) return 1;
    zi_zcl1_frame z;
    if (!expect_ok(buf, got, 3, 2, &z)) return 1;
    if (z.payload_len != 20u) return 1;
    if (read_u32le(z.payload + 0) != 1u) return 1;
  }

  // RANDOM_SEED.
  {
    uint8_t req[24];
    build_zcl1_req(req, 4, 3);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)req, 24) != 24) return 1;
    uint8_t buf[256];
    uint32_t got = 0;
    if (!read_frame(h, buf, (uint32_t)sizeof(buf), &got)) return 1;
    zi_zcl1_frame z;
    if (!expect_ok(buf, got, 4, 3, &z)) return 1;
    if (z.payload_len != 40u) return 1;
    if (read_u32le(z.payload + 0) != 1u) return 1;
    if (read_u32le(z.payload + 4) != 32u) return 1;
  }

  // STATS.
  {
    uint8_t req[24];
    build_zcl1_req(req, 2, 4);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)req, 24) != 24) return 1;
    uint8_t buf[512];
    uint32_t got = 0;
    if (!read_frame(h, buf, (uint32_t)sizeof(buf), &got)) return 1;
    zi_zcl1_frame z;
    if (!expect_ok(buf, got, 2, 4, &z)) return 1;
    if (z.payload_len < 16u) return 1;
    if (read_u32le(z.payload + 0) != 1u) return 1;
  }

  (void)zi_end(h);
  printf("PASS sys/info@v1\n");
  return 0;
}
