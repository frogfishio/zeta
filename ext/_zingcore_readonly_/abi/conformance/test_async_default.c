#include "zi_async.h"
#include "zi_async_default25.h"
#include "zi_caps.h"
#include "zi_handles25.h"
#include "zi_runtime25.h"
#include "zi_sysabi25.h"
#include "zi_zcl1.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_u16le(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

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

static uint64_t read_u64le(const uint8_t *p) {
  uint64_t lo = (uint64_t)read_u32le(p);
  uint64_t hi = (uint64_t)read_u32le(p + 4);
  return lo | (hi << 32);
}

static void build_open_req(uint8_t req[40], const char *kind, const char *name) {
  write_u64le(req + 0, (uint64_t)(uintptr_t)kind);
  write_u32le(req + 8, (uint32_t)strlen(kind));
  write_u64le(req + 12, (uint64_t)(uintptr_t)name);
  write_u32le(req + 20, (uint32_t)strlen(name));
  write_u32le(req + 24, 0);
  write_u64le(req + 28, 0);
  write_u32le(req + 36, 0);
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

static int list_contains_builtin(const uint8_t *payload, uint32_t payload_len) {
  if (!payload || payload_len < 8) return 0;
  uint32_t version = read_u32le(payload + 0);
  uint32_t count = read_u32le(payload + 4);
  if (version != 1 || count < 3) return 0;

  int saw_ping = 0;
  int saw_fail = 0;
  int saw_hold = 0;

  uint32_t off = 8;
  for (uint32_t i = 0; i < count; i++) {
    if (off + 4 > payload_len) return 0;
    uint32_t klen = read_u32le(payload + off);
    off += 4;
    if (klen == 0 || off + klen + 4 > payload_len) return 0;
    const char *kind = (const char *)(payload + off);
    off += klen;

    uint32_t nlen = read_u32le(payload + off);
    off += 4;
    if (nlen == 0 || off + nlen + 4 > payload_len) return 0;
    const char *name = (const char *)(payload + off);
    off += nlen;

    uint32_t slen = read_u32le(payload + off);
    off += 4;
    if (slen == 0 || off + slen > payload_len) return 0;
    const char *sel = (const char *)(payload + off);
    off += slen;

    if (klen == 5 && memcmp(kind, "async", 5) == 0 && nlen == 7 && memcmp(name, "default", 7) == 0) {
      if (slen == 7 && memcmp(sel, "ping.v1", 7) == 0) saw_ping = 1;
      if (slen == 7 && memcmp(sel, "fail.v1", 7) == 0) saw_fail = 1;
      if (slen == 7 && memcmp(sel, "hold.v1", 7) == 0) saw_hold = 1;
    }
  }

  return saw_ping && saw_fail && saw_hold;
}

int main(void) {
  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);

  zi_caps_reset_for_test();
  zi_handles25_reset_for_test();
  zi_async_reset_for_test();

  if (!zi_caps_init() || !zi_async_init()) {
    fprintf(stderr, "FAIL: init failed\n");
    return 1;
  }

  if (!zi_async_default25_register() || !zi_async_default25_register_selectors()) {
    fprintf(stderr, "FAIL: async/default register failed\n");
    return 1;
  }

  uint8_t open_req[40];
  build_open_req(open_req, ZI_CAP_KIND_ASYNC, ZI_CAP_NAME_DEFAULT);
  zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)open_req);
  if (h < 3) {
    fprintf(stderr, "FAIL: zi_cap_open returned %d\n", h);
    return 1;
  }

  // LIST must include built-in selectors.
  {
    uint8_t fr[24];
    build_zcl1_req(fr, (uint16_t)ZI_ASYNC_OP_LIST, 1, NULL, 0);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "FAIL: LIST write\n");
      return 1;
    }
    uint8_t buf[8192];
    uint32_t got = 0;
    if (!drain(buf, (uint32_t)sizeof(buf), h, &got) || got < 24) {
      fprintf(stderr, "FAIL: LIST read\n");
      return 1;
    }
    zi_zcl1_frame z;
    if (!zi_zcl1_parse(buf, got, &z)) {
      fprintf(stderr, "FAIL: LIST parse\n");
      return 1;
    }
    if (z.op != ZI_ASYNC_OP_LIST || z.rid != 1) {
      fprintf(stderr, "FAIL: LIST op/rid mismatch\n");
      return 1;
    }
    if (!list_contains_builtin(z.payload, z.payload_len)) {
      fprintf(stderr, "FAIL: LIST missing built-in selectors\n");
      return 1;
    }
  }

  // INVOKE ping.v1 (future_id=42) must yield INVOKE OK + ACK + FUTURE_OK("pong").
  {
    const char *kind = ZI_CAP_KIND_ASYNC;
    const char *name = ZI_CAP_NAME_DEFAULT;
    const char *sel = "ping.v1";
    uint32_t klen = (uint32_t)strlen(kind);
    uint32_t nlen = (uint32_t)strlen(name);
    uint32_t slen = (uint32_t)strlen(sel);

    uint8_t payload[256];
    uint32_t off = 0;
    write_u32le(payload + off, klen); off += 4;
    memcpy(payload + off, kind, klen); off += klen;
    write_u32le(payload + off, nlen); off += 4;
    memcpy(payload + off, name, nlen); off += nlen;
    write_u32le(payload + off, slen); off += 4;
    memcpy(payload + off, sel, slen); off += slen;
    write_u64le(payload + off, 42u); off += 8;
    write_u32le(payload + off, 0u); off += 4;

    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_ASYNC_OP_INVOKE, 2, payload, off);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)(24u + off)) != (int32_t)(24u + off)) {
      fprintf(stderr, "FAIL: INVOKE ping write\n");
      return 1;
    }

    uint8_t buf[4096];
    uint32_t got = 0;
    if (!drain(buf, (uint32_t)sizeof(buf), h, &got) || got < 24) {
      fprintf(stderr, "FAIL: INVOKE ping read\n");
      return 1;
    }

    int saw_invoke_ok = 0;
    int saw_ack = 0;
    int saw_future_ok = 0;

    uint32_t pos = 0;
    while (pos + 24 <= got) {
      uint32_t pl_len = zi_zcl1_read_u32(buf + pos + 20);
      uint32_t frame_len = 24u + pl_len;
      if (pos + frame_len > got) break;
      zi_zcl1_frame z;
      if (!zi_zcl1_parse(buf + pos, frame_len, &z)) return 1;
      if (z.rid != 2) return 1;

      if (z.op == ZI_ASYNC_OP_INVOKE) {
        if (z.payload_len != 4 || read_u32le(z.payload) != ZI_ASYNC_OK) return 1;
        saw_invoke_ok = 1;
      } else if (z.op == ZI_ASYNC_EV_ACK) {
        if (z.payload_len != 8 || read_u64le(z.payload) != 42u) return 1;
        saw_ack = 1;
      } else if (z.op == ZI_ASYNC_EV_FUTURE_OK) {
        if (z.payload_len < 12 || read_u64le(z.payload + 0) != 42u) return 1;
        uint32_t vlen = read_u32le(z.payload + 8);
        if (12u + vlen != z.payload_len) return 1;
        if (vlen != 4 || memcmp(z.payload + 12, "pong", 4) != 0) return 1;
        saw_future_ok = 1;
      }
      pos += frame_len;
    }

    if (!saw_invoke_ok || !saw_ack || !saw_future_ok) {
      fprintf(stderr, "FAIL: missing ping frames\n");
      return 1;
    }
  }

  // INVOKE hold.v1 (future_id=44) then CANCEL it.
  {
    const char *kind = ZI_CAP_KIND_ASYNC;
    const char *name = ZI_CAP_NAME_DEFAULT;
    const char *sel = "hold.v1";
    uint32_t klen = (uint32_t)strlen(kind);
    uint32_t nlen = (uint32_t)strlen(name);
    uint32_t slen = (uint32_t)strlen(sel);

    uint8_t payload[256];
    uint32_t off = 0;
    write_u32le(payload + off, klen); off += 4;
    memcpy(payload + off, kind, klen); off += klen;
    write_u32le(payload + off, nlen); off += 4;
    memcpy(payload + off, name, nlen); off += nlen;
    write_u32le(payload + off, slen); off += 4;
    memcpy(payload + off, sel, slen); off += slen;
    write_u64le(payload + off, 44u); off += 8;
    write_u32le(payload + off, 0u); off += 4;

    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_ASYNC_OP_INVOKE, 3, payload, off);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)(24u + off)) != (int32_t)(24u + off)) {
      fprintf(stderr, "FAIL: INVOKE hold write\n");
      return 1;
    }

    uint8_t buf[4096];
    uint32_t got = 0;
    if (!drain(buf, (uint32_t)sizeof(buf), h, &got) || got < 24) {
      fprintf(stderr, "FAIL: INVOKE hold read\n");
      return 1;
    }
  }

  // CANCEL the hold future.
  {
    uint8_t payload[8];
    write_u64le(payload + 0, 44u);
    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_ASYNC_OP_CANCEL, 4, payload, (uint32_t)sizeof(payload));
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "FAIL: CANCEL write\n");
      return 1;
    }

    uint8_t buf[4096];
    uint32_t got = 0;
    if (!drain(buf, (uint32_t)sizeof(buf), h, &got) || got < 24) {
      fprintf(stderr, "FAIL: CANCEL read\n");
      return 1;
    }

    int saw_cancel_ok = 0;
    int saw_future_cancel = 0;

    uint32_t pos = 0;
    while (pos + 24 <= got) {
      uint32_t pl_len = zi_zcl1_read_u32(buf + pos + 20);
      uint32_t frame_len = 24u + pl_len;
      if (pos + frame_len > got) break;
      zi_zcl1_frame z;
      if (!zi_zcl1_parse(buf + pos, frame_len, &z)) return 1;
      if (z.rid != 4) return 1;

      if (z.op == ZI_ASYNC_OP_CANCEL) {
        if (z.payload_len != 4 || read_u32le(z.payload) != ZI_ASYNC_OK) return 1;
        saw_cancel_ok = 1;
      } else if (z.op == ZI_ASYNC_EV_FUTURE_CANCEL) {
        if (z.payload_len != 8 || read_u64le(z.payload) != 44u) return 1;
        saw_future_cancel = 1;
      }

      pos += frame_len;
    }

    if (!saw_cancel_ok || !saw_future_cancel) {
      fprintf(stderr, "FAIL: missing cancel frames\n");
      return 1;
    }
  }

  if (zi_end(h) != 0) {
    fprintf(stderr, "FAIL: zi_end failed\n");
    return 1;
  }

  printf("PASS: async/default v1 LIST/INVOKE/CANCEL\n");
  return 0;
}
