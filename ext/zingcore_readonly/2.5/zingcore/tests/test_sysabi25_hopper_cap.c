#include "zi_caps.h"
#include "zi_proc_hopper25.h"
#include "zi_runtime25.h"
#include "zi_sysabi25.h"

#include <stdio.h>
#include <string.h>

static void write_u32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void write_u64le(uint8_t *p, uint64_t v) {
  write_u32le(p + 0, (uint32_t)(v & 0xFFFFFFFFu));
  write_u32le(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static uint32_t read_u32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t read_i32le(const uint8_t *p) {
  return (int32_t)read_u32le(p);
}

static void build_open_req(uint8_t req[40], const char *kind, const char *name, const void *params, uint32_t params_len) {
  // Packed open request (see zi_syscalls_caps25.c):
  // u64 kind_ptr, u32 kind_len, u64 name_ptr, u32 name_len, u32 mode, u64 params_ptr, u32 params_len
  write_u64le(req + 0, (uint64_t)(uintptr_t)kind);
  write_u32le(req + 8, (uint32_t)strlen(kind));
  write_u64le(req + 12, (uint64_t)(uintptr_t)name);
  write_u32le(req + 20, (uint32_t)strlen(name));
  write_u32le(req + 24, 0);
  write_u64le(req + 28, (uint64_t)(uintptr_t)params);
  write_u32le(req + 36, params_len);
}

static void write_u16le(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void build_zcl1_req(uint8_t *out, uint16_t op, uint32_t rid, const uint8_t *payload, uint32_t payload_len) {
  memcpy(out + 0, "ZCL1", 4);
  write_u16le(out + 4, 1);
  write_u16le(out + 6, op);
  write_u32le(out + 8, rid);
  write_u32le(out + 12, 0);
  write_u32le(out + 16, 0);
  write_u32le(out + 20, payload_len);
  if (payload_len && payload) memcpy(out + 24, payload, payload_len);
}

static int read_full_frame(zi_handle_t h, uint8_t *buf, uint32_t cap, uint32_t *out_len) {
  uint32_t off = 0;
  for (int spins = 0; spins < 100000; spins++) {
    int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(buf + off), (zi_size32_t)(cap - off));
    if (n == ZI_E_AGAIN) continue;
    if (n < 0) return 0;
    off += (uint32_t)n;
    if (off >= 24) {
      uint32_t payload_len = read_u32le(buf + 20);
      if (off >= 24u + payload_len) {
        *out_len = 24u + payload_len;
        return 1;
      }
    }
    if (n == 0) break;
  }
  return 0;
}

int main(void) {
  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);

  zi_caps_reset_for_test();
  if (!zi_caps_init()) {
    fprintf(stderr, "zi_caps_init failed\n");
    return 1;
  }
  if (!zi_proc_hopper25_register()) {
    fprintf(stderr, "zi_proc_hopper25_register failed\n");
    return 1;
  }

  // Open with explicit small params.
  uint8_t params[12];
  write_u32le(params + 0, 1);
  write_u32le(params + 4, 256);
  write_u32le(params + 8, 8);

  uint8_t open_req[40];
  build_open_req(open_req, ZI_CAP_KIND_PROC, ZI_CAP_NAME_HOPPER, params, (uint32_t)sizeof(params));

  zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)open_req);
  if (h < 3) {
    fprintf(stderr, "expected handle, got %d\n", h);
    return 1;
  }

  uint8_t resp[2048];
  uint32_t resp_len = 0;

  // RECORD layout_id=1
  {
    uint8_t payload[4];
    write_u32le(payload + 0, 1);
    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_HOPPER_OP_RECORD, 1, payload, (uint32_t)sizeof(payload));

    int32_t wn = zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr));
    if (wn != (int32_t)sizeof(fr)) {
      fprintf(stderr, "write RECORD failed: %d\n", wn);
      return 1;
    }

    if (!read_full_frame(h, resp, (uint32_t)sizeof(resp), &resp_len)) {
      fprintf(stderr, "read RECORD failed\n");
      return 1;
    }

    if (read_u32le(resp + 12) != 1) {
      fprintf(stderr, "RECORD status not ok\n");
      return 1;
    }
    if (read_u32le(resp + 20) != 8) {
      fprintf(stderr, "RECORD payload size wrong\n");
      return 1;
    }
  }

  const uint8_t *pl = resp + 24;
  uint32_t herr = read_u32le(pl + 0);
  int32_t ref = read_i32le(pl + 4);
  if (herr != 0 || ref < 0) {
    fprintf(stderr, "RECORD failed herr=%u ref=%d\n", herr, ref);
    return 1;
  }

  // SET_BYTES field 0
  {
    const char *msg = "hi";
    uint8_t payload[12 + 2];
    write_u32le(payload + 0, (uint32_t)ref);
    write_u32le(payload + 4, 0);
    write_u32le(payload + 8, 2);
    memcpy(payload + 12, msg, 2);

    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_HOPPER_OP_FIELD_SET_BYTES, 2, payload, (uint32_t)sizeof(payload));
    int32_t wn = zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr));
    if (wn != (int32_t)sizeof(fr)) {
      fprintf(stderr, "write SET_BYTES failed: %d\n", wn);
      return 1;
    }

    if (!read_full_frame(h, resp, (uint32_t)sizeof(resp), &resp_len)) {
      fprintf(stderr, "read SET_BYTES failed\n");
      return 1;
    }
    if (read_u32le(resp + 12) != 1 || read_u32le(resp + 20) != 4 || read_u32le(resp + 24) != 0) {
      fprintf(stderr, "SET_BYTES failed\n");
      return 1;
    }
  }

  // SET_I32 field 1
  {
    uint8_t payload[12];
    write_u32le(payload + 0, (uint32_t)ref);
    write_u32le(payload + 4, 1);
    write_u32le(payload + 8, 123);

    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_HOPPER_OP_FIELD_SET_I32, 3, payload, (uint32_t)sizeof(payload));
    int32_t wn = zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr));
    if (wn != (int32_t)sizeof(fr)) {
      fprintf(stderr, "write SET_I32 failed: %d\n", wn);
      return 1;
    }

    if (!read_full_frame(h, resp, (uint32_t)sizeof(resp), &resp_len)) {
      fprintf(stderr, "read SET_I32 failed\n");
      return 1;
    }
    if (read_u32le(resp + 12) != 1 || read_u32le(resp + 20) != 4 || read_u32le(resp + 24) != 0) {
      fprintf(stderr, "SET_I32 failed\n");
      return 1;
    }
  }

  // GET_BYTES field 0 (expect "hi  ")
  {
    uint8_t payload[8];
    write_u32le(payload + 0, (uint32_t)ref);
    write_u32le(payload + 4, 0);

    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_HOPPER_OP_FIELD_GET_BYTES, 4, payload, (uint32_t)sizeof(payload));
    int32_t wn = zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr));
    if (wn != (int32_t)sizeof(fr)) {
      fprintf(stderr, "write GET_BYTES failed: %d\n", wn);
      return 1;
    }

    if (!read_full_frame(h, resp, (uint32_t)sizeof(resp), &resp_len)) {
      fprintf(stderr, "read GET_BYTES failed\n");
      return 1;
    }

    if (read_u32le(resp + 12) != 1) {
      fprintf(stderr, "GET_BYTES status not ok\n");
      return 1;
    }

    const uint8_t *pl2 = resp + 24;
    uint32_t err = read_u32le(pl2 + 0);
    uint32_t blen = read_u32le(pl2 + 4);
    if (err != 0 || blen != 4 || memcmp(pl2 + 8, "hi  ", 4) != 0) {
      fprintf(stderr, "GET_BYTES mismatch err=%u blen=%u\n", err, blen);
      return 1;
    }
  }

  // GET_I32 field 1 (expect 123)
  {
    uint8_t payload[8];
    write_u32le(payload + 0, (uint32_t)ref);
    write_u32le(payload + 4, 1);

    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_HOPPER_OP_FIELD_GET_I32, 5, payload, (uint32_t)sizeof(payload));
    int32_t wn = zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr));
    if (wn != (int32_t)sizeof(fr)) {
      fprintf(stderr, "write GET_I32 failed: %d\n", wn);
      return 1;
    }

    if (!read_full_frame(h, resp, (uint32_t)sizeof(resp), &resp_len)) {
      fprintf(stderr, "read GET_I32 failed\n");
      return 1;
    }

    const uint8_t *pl2 = resp + 24;
    uint32_t err = read_u32le(pl2 + 0);
    int32_t v = read_i32le(pl2 + 4);
    if (err != 0 || v != 123) {
      fprintf(stderr, "GET_I32 mismatch err=%u v=%d\n", err, v);
      return 1;
    }
  }

  if (zi_end(h) != 0) {
    fprintf(stderr, "end failed\n");
    return 1;
  }

  printf("ok\n");
  return 0;
}
