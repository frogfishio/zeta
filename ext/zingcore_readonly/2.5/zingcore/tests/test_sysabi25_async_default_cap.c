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

static void build_open_req(uint8_t req[40], const char *kind, const char *name, const void *params, uint32_t params_len) {
  write_u64le(req + 0, (uint64_t)(uintptr_t)kind);
  write_u32le(req + 8, (uint32_t)strlen(kind));
  write_u64le(req + 12, (uint64_t)(uintptr_t)name);
  write_u32le(req + 20, (uint32_t)strlen(name));
  write_u32le(req + 24, 0);
  write_u64le(req + 28, (uint64_t)(uintptr_t)params);
  write_u32le(req + 36, params_len);
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

static int drain_frames(uint8_t *buf, uint32_t cap, zi_handle_t h, uint32_t *out_len) {
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

static int parse_future_fail_payload(const uint8_t *pl, uint32_t pl_len, uint64_t *out_fid, char *code, uint32_t code_cap,
                                    char *msg, uint32_t msg_cap) {
  if (!pl || pl_len < 8 + 4 + 4) return 0;
  uint32_t off = 0;
  uint64_t fid = read_u64le(pl + off);
  off += 8;
  uint32_t clen = read_u32le(pl + off);
  off += 4;
  if (off + clen + 4 > pl_len) return 0;
  if (code && code_cap) {
    uint32_t n = (clen < (code_cap - 1)) ? clen : (code_cap - 1);
    memcpy(code, pl + off, n);
    code[n] = '\0';
  }
  off += clen;
  uint32_t mlen = read_u32le(pl + off);
  off += 4;
  if (off + mlen != pl_len) return 0;
  if (msg && msg_cap) {
    uint32_t n = (mlen < (msg_cap - 1)) ? mlen : (msg_cap - 1);
    memcpy(msg, pl + off, n);
    msg[n] = '\0';
  }
  if (out_fid) *out_fid = fid;
  return 1;
}

int main(void) {
  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);

  zi_caps_reset_for_test();
  zi_handles25_reset_for_test();
  zi_async_reset_for_test();

  if (!zi_caps_init()) {
    fprintf(stderr, "zi_caps_init failed\n");
    return 1;
  }
  if (!zi_async_init()) {
    fprintf(stderr, "zi_async_init failed\n");
    return 1;
  }

  if (!zi_async_default25_register()) {
    fprintf(stderr, "zi_async_default25_register failed\n");
    return 1;
  }
  if (!zi_async_default25_register_selectors()) {
    fprintf(stderr, "zi_async_default25_register_selectors failed\n");
    return 1;
  }

  // Negative: open with params rejected.
  {
    uint8_t req[40];
    uint8_t dummy = 0;
    build_open_req(req, ZI_CAP_KIND_ASYNC, ZI_CAP_NAME_DEFAULT, &dummy, 1);
    zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)req);
    if (h != ZI_E_INVALID) {
      fprintf(stderr, "expected invalid for params, got %d\n", h);
      return 1;
    }
  }

  // Open async/default.
  uint8_t req[40];
  build_open_req(req, ZI_CAP_KIND_ASYNC, ZI_CAP_NAME_DEFAULT, NULL, 0);
  zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)req);
  if (h < 3) {
    fprintf(stderr, "expected handle, got %d\n", h);
    return 1;
  }

  // LIST.
  {
    uint8_t fr[24];
    build_zcl1_req(fr, (uint16_t)ZI_ASYNC_OP_LIST, 1, NULL, 0);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "LIST write failed\n");
      return 1;
    }

    uint8_t buf[4096];
    uint32_t got = 0;
    if (!drain_frames(buf, (uint32_t)sizeof(buf), h, &got) || got < 24) {
      fprintf(stderr, "LIST read failed\n");
      return 1;
    }

    zi_zcl1_frame z;
    if (!zi_zcl1_parse(buf, got, &z)) {
      fprintf(stderr, "LIST parse failed\n");
      return 1;
    }
    if (z.op != ZI_ASYNC_OP_LIST || z.rid != 1) {
      fprintf(stderr, "LIST op/rid mismatch\n");
      return 1;
    }
    if (z.payload_len < 8) {
      fprintf(stderr, "LIST payload too small\n");
      return 1;
    }
    uint32_t version = read_u32le(z.payload + 0);
    uint32_t n = read_u32le(z.payload + 4);
    if (version != 1 || n == 0) {
      fprintf(stderr, "LIST bad version or empty selector list\n");
      return 1;
    }
  }

  // INVOKE ping.v1 with future_id=42.
  {
    const char *kind = ZI_CAP_KIND_ASYNC;
    const char *name = ZI_CAP_NAME_DEFAULT;
    const char *sel = "ping.v1";
    uint32_t klen = (uint32_t)strlen(kind);
    uint32_t nlen = (uint32_t)strlen(name);
    uint32_t slen = (uint32_t)strlen(sel);

    uint8_t payload[256];
    uint32_t off = 0;
    write_u32le(payload + off, klen);
    off += 4;
    memcpy(payload + off, kind, klen);
    off += klen;
    write_u32le(payload + off, nlen);
    off += 4;
    memcpy(payload + off, name, nlen);
    off += nlen;
    write_u32le(payload + off, slen);
    off += 4;
    memcpy(payload + off, sel, slen);
    off += slen;
    write_u64le(payload + off, 42u);
    off += 8;
    write_u32le(payload + off, 0u);
    off += 4;

    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_ASYNC_OP_INVOKE, 2, payload, off);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)(24u + off)) != (int32_t)(24u + off)) {
      fprintf(stderr, "INVOKE write failed\n");
      return 1;
    }

    uint8_t buf[4096];
    uint32_t got = 0;
    if (!drain_frames(buf, (uint32_t)sizeof(buf), h, &got) || got < 24) {
      fprintf(stderr, "INVOKE read failed\n");
      return 1;
    }

    int saw_invoke_ok = 0;
    int saw_ack = 0;
    int saw_future_ok = 0;

    uint32_t pos = 0;
    while (pos + 24 <= got) {
      uint32_t payload_len = zi_zcl1_read_u32(buf + pos + 20);
      uint32_t frame_len = 24u + payload_len;
      if (pos + frame_len > got) break;

      zi_zcl1_frame z;
      if (!zi_zcl1_parse(buf + pos, frame_len, &z)) {
        fprintf(stderr, "frame parse failed\n");
        return 1;
      }

      if (z.rid != 2) {
        fprintf(stderr, "unexpected rid in event stream\n");
        return 1;
      }

      if (z.op == ZI_ASYNC_OP_INVOKE) {
        if (z.payload_len != 4) {
          fprintf(stderr, "invoke response payload mismatch\n");
          return 1;
        }
        if (read_u32le(z.payload) != ZI_ASYNC_OK) {
          fprintf(stderr, "invoke response not ok\n");
          return 1;
        }
        saw_invoke_ok = 1;
      } else if (z.op == ZI_ASYNC_EV_ACK) {
        if (z.payload_len != 8 || read_u64le(z.payload) != 42u) {
          fprintf(stderr, "ack payload mismatch\n");
          return 1;
        }
        saw_ack = 1;
      } else if (z.op == ZI_ASYNC_EV_FUTURE_OK) {
        if (z.payload_len < 12 || read_u64le(z.payload + 0) != 42u) {
          fprintf(stderr, "future_ok id mismatch\n");
          return 1;
        }
        uint32_t vlen = read_u32le(z.payload + 8);
        if (12u + vlen != z.payload_len) {
          fprintf(stderr, "future_ok len mismatch\n");
          return 1;
        }
        if (vlen != 4 || memcmp(z.payload + 12, "pong", 4) != 0) {
          fprintf(stderr, "future_ok value mismatch\n");
          return 1;
        }
        saw_future_ok = 1;
      }

      pos += frame_len;
    }

    if (!saw_invoke_ok || !saw_ack || !saw_future_ok) {
      fprintf(stderr, "missing expected frames (invoke_ok=%d ack=%d future_ok=%d)\n", saw_invoke_ok, saw_ack, saw_future_ok);
      return 1;
    }
  }

  // INVOKE fail.v1 with future_id=43.
  {
    const char *kind = ZI_CAP_KIND_ASYNC;
    const char *name = ZI_CAP_NAME_DEFAULT;
    const char *sel = "fail.v1";
    uint32_t klen = (uint32_t)strlen(kind);
    uint32_t nlen = (uint32_t)strlen(name);
    uint32_t slen = (uint32_t)strlen(sel);

    uint8_t payload[256];
    uint32_t off = 0;
    write_u32le(payload + off, klen);
    off += 4;
    memcpy(payload + off, kind, klen);
    off += klen;
    write_u32le(payload + off, nlen);
    off += 4;
    memcpy(payload + off, name, nlen);
    off += nlen;
    write_u32le(payload + off, slen);
    off += 4;
    memcpy(payload + off, sel, slen);
    off += slen;
    write_u64le(payload + off, 43u);
    off += 8;
    write_u32le(payload + off, 0u);
    off += 4;

    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_ASYNC_OP_INVOKE, 3, payload, off);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)(24u + off)) != (int32_t)(24u + off)) {
      fprintf(stderr, "INVOKE fail write failed\n");
      return 1;
    }

    uint8_t buf[4096];
    uint32_t got = 0;
    if (!drain_frames(buf, (uint32_t)sizeof(buf), h, &got) || got < 24) {
      fprintf(stderr, "INVOKE fail read failed\n");
      return 1;
    }

    int saw_fail = 0;
    uint32_t pos = 0;
    while (pos + 24 <= got) {
      uint32_t payload_len = zi_zcl1_read_u32(buf + pos + 20);
      uint32_t frame_len = 24u + payload_len;
      if (pos + frame_len > got) break;
      zi_zcl1_frame z;
      if (!zi_zcl1_parse(buf + pos, frame_len, &z)) return 1;
      if (z.rid != 3) return 1;
      if (z.op == ZI_ASYNC_EV_FUTURE_FAIL) {
        uint64_t fid = 0;
        char code[64];
        char msg[128];
        if (!parse_future_fail_payload(z.payload, z.payload_len, &fid, code, (uint32_t)sizeof(code), msg, (uint32_t)sizeof(msg))) {
          fprintf(stderr, "future_fail payload parse failed\n");
          return 1;
        }
        if (fid != 43u || strcmp(code, "demo.fail") != 0) {
          fprintf(stderr, "future_fail mismatch fid=%llu code=%s\n", (unsigned long long)fid, code);
          return 1;
        }
        saw_fail = 1;
      }
      pos += frame_len;
    }
    if (!saw_fail) {
      fprintf(stderr, "expected future_fail event\n");
      return 1;
    }
  }

  // INVOKE hold.v1 with future_id=44, then CANCEL it.
  {
    const char *kind = ZI_CAP_KIND_ASYNC;
    const char *name = ZI_CAP_NAME_DEFAULT;
    const char *sel = "hold.v1";
    uint32_t klen = (uint32_t)strlen(kind);
    uint32_t nlen = (uint32_t)strlen(name);
    uint32_t slen = (uint32_t)strlen(sel);

    uint8_t payload[256];
    uint32_t off = 0;
    write_u32le(payload + off, klen);
    off += 4;
    memcpy(payload + off, kind, klen);
    off += klen;
    write_u32le(payload + off, nlen);
    off += 4;
    memcpy(payload + off, name, nlen);
    off += nlen;
    write_u32le(payload + off, slen);
    off += 4;
    memcpy(payload + off, sel, slen);
    off += slen;
    write_u64le(payload + off, 44u);
    off += 8;
    write_u32le(payload + off, 0u);
    off += 4;

    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_ASYNC_OP_INVOKE, 4, payload, off);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)(24u + off)) != (int32_t)(24u + off)) {
      fprintf(stderr, "INVOKE hold write failed\n");
      return 1;
    }

    uint8_t buf[4096];
    uint32_t got = 0;
    if (!drain_frames(buf, (uint32_t)sizeof(buf), h, &got) || got < 24) {
      fprintf(stderr, "INVOKE hold read failed\n");
      return 1;
    }
  }

  // CANCEL hold future.
  {
    uint8_t payload[8];
    write_u64le(payload + 0, 44u);
    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_ASYNC_OP_CANCEL, 5, payload, (uint32_t)sizeof(payload));
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "CANCEL write failed\n");
      return 1;
    }

    uint8_t buf[4096];
    uint32_t got = 0;
    if (!drain_frames(buf, (uint32_t)sizeof(buf), h, &got) || got < 48) {
      fprintf(stderr, "CANCEL read failed\n");
      return 1;
    }

    int saw_cancel_ok = 0;
    int saw_future_cancel = 0;
    uint32_t pos = 0;
    while (pos + 24 <= got) {
      uint32_t payload_len = zi_zcl1_read_u32(buf + pos + 20);
      uint32_t frame_len = 24u + payload_len;
      if (pos + frame_len > got) break;
      zi_zcl1_frame z;
      if (!zi_zcl1_parse(buf + pos, frame_len, &z)) return 1;
      if (z.rid != 5) return 1;
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
      fprintf(stderr, "missing cancel frames (ok=%d ev=%d)\n", saw_cancel_ok, saw_future_cancel);
      return 1;
    }
  }

  // Second CANCEL should be NOENT.
  {
    uint8_t payload[8];
    write_u64le(payload + 0, 44u);
    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_ASYNC_OP_CANCEL, 6, payload, (uint32_t)sizeof(payload));
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "CANCEL2 write failed\n");
      return 1;
    }

    uint8_t buf[256];
    uint32_t got = 0;
    if (!drain_frames(buf, (uint32_t)sizeof(buf), h, &got) || got < 24) {
      fprintf(stderr, "CANCEL2 read failed\n");
      return 1;
    }
    zi_zcl1_frame z;
    if (!zi_zcl1_parse(buf, got, &z)) return 1;
    if (z.op != ZI_ASYNC_OP_CANCEL || z.rid != 6 || z.payload_len != 4) return 1;
    if (read_u32le(z.payload) != ZI_ASYNC_E_NOENT) return 1;
  }

  if (zi_end(h) != 0) {
    fprintf(stderr, "zi_end failed\n");
    return 1;
  }

  printf("ok\n");
  return 0;
}
