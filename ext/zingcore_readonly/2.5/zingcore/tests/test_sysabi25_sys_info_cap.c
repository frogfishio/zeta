#include "zi_caps.h"
#include "zi_handles25.h"
#include "zi_runtime25.h"
#include "zi_sys_info25.h"
#include "zi_sysabi25.h"
#include "zi_zcl1.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_u16le(uint8_t *p, uint16_t v) { zi_zcl1_write_u16(p, v); }
static void write_u32le(uint8_t *p, uint32_t v) { zi_zcl1_write_u32(p, v); }

static void write_u64le(uint8_t *p, uint64_t v) {
  write_u32le(p + 0, (uint32_t)(v & 0xFFFFFFFFu));
  write_u32le(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static uint32_t read_u32le(const uint8_t *p) { return zi_zcl1_read_u32(p); }

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

static int drain(uint8_t *buf, uint32_t cap, zi_handle_t h, uint32_t *out_len) {
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

static int expect_ok_status(const uint8_t *frame, uint32_t frame_len) {
  if (!frame || frame_len < 24u) return 0;
  // ZCL1 status at offset 12.
  return read_u32le(frame + 12) == 1u;
}

int main(void) {
  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);

  zi_caps_reset_for_test();
  zi_handles25_reset_for_test();

  if (!zi_caps_init()) {
    fprintf(stderr, "zi_caps_init failed\n");
    return 1;
  }
  if (!zi_sys_info25_register()) {
    fprintf(stderr, "zi_sys_info25_register failed\n");
    return 1;
  }

  // Negative: open with params rejected.
  {
    uint8_t req[40];
    uint8_t dummy = 0;
    build_open_req(req, ZI_CAP_KIND_SYS, ZI_CAP_NAME_INFO, &dummy, 1);
    zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)req);
    if (h != ZI_E_INVALID) {
      fprintf(stderr, "expected invalid for params, got %d\n", h);
      return 1;
    }
  }

  // Open handle.
  uint8_t req[40];
  build_open_req(req, ZI_CAP_KIND_SYS, ZI_CAP_NAME_INFO, NULL, 0);
  zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)req);
  if (h < 3) {
    fprintf(stderr, "expected handle, got %d\n", h);
    return 1;
  }

  // INFO.
  {
    uint8_t fr[24];
    build_zcl1_req(fr, (uint16_t)ZI_SYS_INFO_OP_INFO, 1);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "INFO write failed\n");
      return 1;
    }

    uint8_t buf[4096];
    uint32_t got = 0;
    if (!drain(buf, (uint32_t)sizeof(buf), h, &got) || got < 24u) {
      fprintf(stderr, "INFO read failed\n");
      return 1;
    }
    if (!expect_ok_status(buf, got)) {
      fprintf(stderr, "INFO not ok\n");
      return 1;
    }
    zi_zcl1_frame z;
    if (!zi_zcl1_parse(buf, got, &z) || z.op != ZI_SYS_INFO_OP_INFO || z.rid != 1) {
      fprintf(stderr, "INFO parse mismatch\n");
      return 1;
    }
    if (z.payload_len < 16u) {
      fprintf(stderr, "INFO payload too small\n");
      return 1;
    }
    if (read_u32le(z.payload + 0) != 1u) {
      fprintf(stderr, "INFO version mismatch\n");
      return 1;
    }
    uint32_t cpu_count = read_u32le(z.payload + 8);
    uint32_t page_size = read_u32le(z.payload + 12);
    if (cpu_count == 0) {
      fprintf(stderr, "INFO cpu_count=0\n");
      return 1;
    }
    if (page_size == 0) {
      fprintf(stderr, "INFO page_size=0\n");
      return 1;
    }
  }

  // TIME_NOW.
  {
    uint8_t fr[24];
    build_zcl1_req(fr, (uint16_t)ZI_SYS_INFO_OP_TIME_NOW, 2);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "TIME_NOW write failed\n");
      return 1;
    }

    uint8_t buf[256];
    uint32_t got = 0;
    if (!drain(buf, (uint32_t)sizeof(buf), h, &got) || got < 24u) {
      fprintf(stderr, "TIME_NOW read failed\n");
      return 1;
    }
    if (!expect_ok_status(buf, got)) {
      fprintf(stderr, "TIME_NOW not ok\n");
      return 1;
    }
    zi_zcl1_frame z;
    if (!zi_zcl1_parse(buf, got, &z) || z.op != ZI_SYS_INFO_OP_TIME_NOW || z.rid != 2) {
      fprintf(stderr, "TIME_NOW parse mismatch\n");
      return 1;
    }
    if (z.payload_len != 20u) {
      fprintf(stderr, "TIME_NOW payload_len mismatch\n");
      return 1;
    }
    if (read_u32le(z.payload + 0) != 1u) {
      fprintf(stderr, "TIME_NOW version mismatch\n");
      return 1;
    }
  }

  // RANDOM_SEED.
  {
    uint8_t fr[24];
    build_zcl1_req(fr, (uint16_t)ZI_SYS_INFO_OP_RANDOM_SEED, 3);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "RANDOM_SEED write failed\n");
      return 1;
    }

    uint8_t buf[256];
    uint32_t got = 0;
    if (!drain(buf, (uint32_t)sizeof(buf), h, &got) || got < 24u) {
      fprintf(stderr, "RANDOM_SEED read failed\n");
      return 1;
    }
    if (!expect_ok_status(buf, got)) {
      fprintf(stderr, "RANDOM_SEED not ok\n");
      return 1;
    }
    zi_zcl1_frame z;
    if (!zi_zcl1_parse(buf, got, &z) || z.op != ZI_SYS_INFO_OP_RANDOM_SEED || z.rid != 3) {
      fprintf(stderr, "RANDOM_SEED parse mismatch\n");
      return 1;
    }
    if (z.payload_len != 40u) {
      fprintf(stderr, "RANDOM_SEED payload_len mismatch (got %u)\n", (unsigned)z.payload_len);
      return 1;
    }
    if (read_u32le(z.payload + 0) != 1u) {
      fprintf(stderr, "RANDOM_SEED version mismatch\n");
      return 1;
    }
    uint32_t seed_len = read_u32le(z.payload + 4);
    if (seed_len != 32u) {
      fprintf(stderr, "RANDOM_SEED seed_len mismatch\n");
      return 1;
    }
  }

  // STATS.
  {
    uint8_t fr[24];
    build_zcl1_req(fr, (uint16_t)ZI_SYS_INFO_OP_STATS, 4);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "STATS write failed\n");
      return 1;
    }

    uint8_t buf[512];
    uint32_t got = 0;
    if (!drain(buf, (uint32_t)sizeof(buf), h, &got) || got < 24u) {
      fprintf(stderr, "STATS read failed\n");
      return 1;
    }
    if (!expect_ok_status(buf, got)) {
      fprintf(stderr, "STATS not ok\n");
      return 1;
    }
    zi_zcl1_frame z;
    if (!zi_zcl1_parse(buf, got, &z) || z.op != ZI_SYS_INFO_OP_STATS || z.rid != 4) {
      fprintf(stderr, "STATS parse mismatch\n");
      return 1;
    }
    if (z.payload_len < 16u) {
      fprintf(stderr, "STATS payload too small\n");
      return 1;
    }
    if (read_u32le(z.payload + 0) != 1u) {
      fprintf(stderr, "STATS version mismatch\n");
      return 1;
    }
  }

  (void)zi_end(h);
  return 0;
}
