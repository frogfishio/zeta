#include "zi_async_default25.h"

#include "zi_handles25.h"
#include "zi_runtime25.h"
#include "zi_zcl1.h"

#include <stdlib.h>
#include <string.h>

// ---- cap descriptor ----

static const zi_cap_v1 CAP = {
    .kind = ZI_CAP_KIND_ASYNC,
    .name = ZI_CAP_NAME_DEFAULT,
    .version = 1,
    .cap_flags = ZI_CAP_CAN_OPEN | ZI_CAP_MAY_BLOCK,
    .meta = NULL,
    .meta_len = 0,
};

const zi_cap_v1 *zi_async_default25_cap(void) { return &CAP; }

int zi_async_default25_register(void) { return zi_cap_register(&CAP); }

// ---- built-in selector(s) ----

static int emit_ack_cb(void *ctx, uint64_t req_id, uint64_t future_id);
static int emit_fail_cb(void *ctx, uint64_t req_id, const char *code, const char *msg);
static int emit_future_ok_cb(void *ctx, uint64_t future_id, const uint8_t *val, uint32_t val_len);
static int emit_future_fail_cb(void *ctx, uint64_t future_id, const char *code, const char *msg);
static int emit_future_cancel_cb(void *ctx, uint64_t future_id);

static const zi_async_emit EMIT = {
    .ack = emit_ack_cb,
    .fail = emit_fail_cb,
    .future_ok = emit_future_ok_cb,
    .future_fail = emit_future_fail_cb,
    .future_cancel = emit_future_cancel_cb,
};

static int selector_ping_v1(const zi_async_emit *emit, void *emit_ctx, const uint8_t *params, uint32_t params_len,
                           uint64_t req_id, uint64_t future_id) {
  (void)params;
  (void)params_len;
  if (!emit || !emit->ack || !emit->future_ok) return 0;
  if (!emit->ack(emit_ctx, req_id, future_id)) return 0;
  static const uint8_t pong[] = {'p', 'o', 'n', 'g'};
  if (!emit->future_ok(emit_ctx, future_id, pong, (uint32_t)sizeof(pong))) return 0;
  return 1;
}

static int selector_fail_v1(const zi_async_emit *emit, void *emit_ctx, const uint8_t *params, uint32_t params_len,
                           uint64_t req_id, uint64_t future_id) {
  (void)params;
  (void)params_len;
  if (!emit || !emit->ack || !emit->future_fail) return 0;
  if (!emit->ack(emit_ctx, req_id, future_id)) return 0;
  if (!emit->future_fail(emit_ctx, future_id, "demo.fail", "intentional failure")) return 0;
  return 1;
}

static int selector_hold_v1(const zi_async_emit *emit, void *emit_ctx, const uint8_t *params, uint32_t params_len,
                           uint64_t req_id, uint64_t future_id) {
  (void)params;
  (void)params_len;
  if (!emit || !emit->ack) return 0;
  if (!emit->ack(emit_ctx, req_id, future_id)) return 0;
  // Intentionally do not complete; caller must cancel.
  return 1;
}

static int selector_hold_cancel(void *emit_ctx, uint64_t future_id) {
  (void)emit_ctx;
  (void)future_id;
  return 1;
}

static const zi_async_selector SEL_PING_V1 = {
    .cap_kind = ZI_CAP_KIND_ASYNC,
    .cap_name = ZI_CAP_NAME_DEFAULT,
    .selector = "ping.v1",
    .invoke = selector_ping_v1,
    .cancel = NULL,
};

static const zi_async_selector SEL_FAIL_V1 = {
  .cap_kind = ZI_CAP_KIND_ASYNC,
  .cap_name = ZI_CAP_NAME_DEFAULT,
  .selector = "fail.v1",
  .invoke = selector_fail_v1,
  .cancel = NULL,
};

static const zi_async_selector SEL_HOLD_V1 = {
  .cap_kind = ZI_CAP_KIND_ASYNC,
  .cap_name = ZI_CAP_NAME_DEFAULT,
  .selector = "hold.v1",
  .invoke = selector_hold_v1,
  .cancel = selector_hold_cancel,
};

int zi_async_default25_register_selectors(void) {
  if (!zi_async_register(&SEL_PING_V1)) return 0;
  if (!zi_async_register(&SEL_FAIL_V1)) return 0;
  if (!zi_async_register(&SEL_HOLD_V1)) return 0;
  return 1;
}

// ---- handle implementation ----

typedef struct {
  uint64_t future_id;
  const zi_async_selector *sel;
  uint64_t invoke_rid;
  int in_use;
} zi_async_future_entry;

enum { ZI_ASYNC_FUTURES_MAX = 64 };

typedef struct {
  uint8_t inbuf[65536];
  uint32_t in_len;

  uint8_t outbuf[65536];
  uint32_t out_len;
  uint32_t out_off;

  int closed;

  // Per-request bookkeeping for ensuring at least one ack/fail.
  uint64_t cur_req_id;
  uint64_t cur_emit_rid;
  uint64_t cur_future_id;
  int cur_acked;
  int cur_failed;

  zi_async_future_entry futures[ZI_ASYNC_FUTURES_MAX];
} zi_async_handle_ctx;

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

static int out_append(zi_async_handle_ctx *c, const uint8_t *bytes, uint32_t len) {
  if (!c) return 0;
  if (len == 0) return 1;
  if ((uint64_t)c->out_len + (uint64_t)len > (uint64_t)sizeof(c->outbuf)) return 0;
  memcpy(c->outbuf + c->out_len, bytes, len);
  c->out_len += len;
  return 1;
}

static int out_append_ok_u32(zi_async_handle_ctx *c, uint16_t op, uint32_t rid, uint32_t v) {
  uint8_t payload[4];
  write_u32le(payload + 0, v);
  uint8_t tmp[256];
  int n = zi_zcl1_write_ok(tmp, (uint32_t)sizeof(tmp), op, rid, payload, (uint32_t)sizeof(payload));
  if (n < 0) return 0;
  return out_append(c, tmp, (uint32_t)n);
}

static int out_append_ok_bytes(zi_async_handle_ctx *c, uint16_t op, uint32_t rid, const uint8_t *payload, uint32_t payload_len) {
  uint8_t tmp[65536];
  int n = zi_zcl1_write_ok(tmp, (uint32_t)sizeof(tmp), op, rid, payload, payload_len);
  if (n < 0) return 0;
  return out_append(c, tmp, (uint32_t)n);
}

static int out_append_err(zi_async_handle_ctx *c, uint16_t op, uint32_t rid, const char *code, const char *msg) {
  uint8_t tmp[512];
  int n = zi_zcl1_write_error(tmp, (uint32_t)sizeof(tmp), op, rid, code, msg);
  if (n < 0) return 0;
  return out_append(c, tmp, (uint32_t)n);
}

static int out_append_ev_ack(zi_async_handle_ctx *c, uint32_t rid, uint64_t future_id) {
  uint8_t payload[8];
  write_u64le(payload + 0, future_id);
  return out_append_ok_bytes(c, (uint16_t)ZI_ASYNC_EV_ACK, rid, payload, (uint32_t)sizeof(payload));
}

static int out_append_ev_fail(zi_async_handle_ctx *c, uint16_t op, uint32_t rid, uint64_t future_id, const char *code, const char *msg) {
  uint32_t code_len = code ? (uint32_t)strlen(code) : 0;
  uint32_t msg_len = msg ? (uint32_t)strlen(msg) : 0;

  if (code_len > 1024u || msg_len > 8192u) return 0;

  uint32_t payload_len = 8u + 4u + code_len + 4u + msg_len;
  if (payload_len > 60000u) return 0;

  uint8_t tmp[65536];
  write_u64le(tmp + 0, future_id);
  write_u32le(tmp + 8, code_len);
  if (code_len) memcpy(tmp + 12, code, code_len);
  uint32_t off = 12u + code_len;
  write_u32le(tmp + off, msg_len);
  off += 4;
  if (msg_len) memcpy(tmp + off, msg, msg_len);
  off += msg_len;

  if (off != payload_len) return 0;

  return out_append_ok_bytes(c, op, rid, tmp, payload_len);
}

static int out_append_ev_future_ok(zi_async_handle_ctx *c, uint32_t rid, uint64_t future_id, const uint8_t *val, uint32_t val_len) {
  if (val_len > 60000u) return 0;

  uint32_t payload_len = 8u + 4u + val_len;
  uint8_t tmp[65536];
  write_u64le(tmp + 0, future_id);
  write_u32le(tmp + 8, val_len);
  if (val_len && val) memcpy(tmp + 12, val, val_len);
  return out_append_ok_bytes(c, (uint16_t)ZI_ASYNC_EV_FUTURE_OK, rid, tmp, payload_len);
}

static int out_append_ev_future_cancel(zi_async_handle_ctx *c, uint32_t rid, uint64_t future_id) {
  uint8_t payload[8];
  write_u64le(payload + 0, future_id);
  return out_append_ok_bytes(c, (uint16_t)ZI_ASYNC_EV_FUTURE_CANCEL, rid, payload, (uint32_t)sizeof(payload));
}

static int future_find_idx(zi_async_handle_ctx *c, uint64_t future_id) {
  if (!c) return -1;
  for (int i = 0; i < (int)ZI_ASYNC_FUTURES_MAX; i++) {
    if (c->futures[i].in_use && c->futures[i].future_id == future_id) return i;
  }
  return -1;
}

static int future_alloc(zi_async_handle_ctx *c, uint64_t future_id, const zi_async_selector *sel, uint64_t invoke_rid) {
  if (!c || !sel) return 0;
  if (future_id == 0) return 0;
  if (future_find_idx(c, future_id) >= 0) return 0;
  for (int i = 0; i < (int)ZI_ASYNC_FUTURES_MAX; i++) {
    if (!c->futures[i].in_use) {
      c->futures[i].in_use = 1;
      c->futures[i].future_id = future_id;
      c->futures[i].sel = sel;
      c->futures[i].invoke_rid = invoke_rid;
      return 1;
    }
  }
  return 0;
}

static void future_free(zi_async_handle_ctx *c, uint64_t future_id) {
  int idx = future_find_idx(c, future_id);
  if (idx < 0) return;
  c->futures[idx].in_use = 0;
  c->futures[idx].future_id = 0;
  c->futures[idx].sel = NULL;
  c->futures[idx].invoke_rid = 0;
}

// ---- emit callbacks (selector -> cap handle) ----

static int emit_ack_cb(void *ctx, uint64_t req_id, uint64_t future_id) {
  zi_async_handle_ctx *c = (zi_async_handle_ctx *)ctx;
  if (!c) return 0;
  if (c->cur_req_id != req_id) return 0;
  c->cur_acked = 1;
  return out_append_ev_ack(c, (uint32_t)req_id, future_id);
}

static int emit_fail_cb(void *ctx, uint64_t req_id, const char *code, const char *msg) {
  zi_async_handle_ctx *c = (zi_async_handle_ctx *)ctx;
  if (!c) return 0;
  if (c->cur_req_id != req_id) return 0;
  c->cur_failed = 1;
  if (c->cur_future_id) future_free(c, c->cur_future_id);
  // No future_id available here; encode as 0.
  return out_append_ev_fail(c, (uint16_t)ZI_ASYNC_EV_FAIL, (uint32_t)req_id, 0u, code ? code : "t_async_fail", msg ? msg : "fail");
}

static int emit_future_ok_cb(void *ctx, uint64_t future_id, const uint8_t *val, uint32_t val_len) {
  zi_async_handle_ctx *c = (zi_async_handle_ctx *)ctx;
  if (!c) return 0;
  future_free(c, future_id);
  return out_append_ev_future_ok(c, (uint32_t)c->cur_emit_rid, future_id, val, val_len);
}

static int emit_future_fail_cb(void *ctx, uint64_t future_id, const char *code, const char *msg) {
  zi_async_handle_ctx *c = (zi_async_handle_ctx *)ctx;
  if (!c) return 0;
  future_free(c, future_id);
  return out_append_ev_fail(c, (uint16_t)ZI_ASYNC_EV_FUTURE_FAIL, (uint32_t)c->cur_emit_rid, future_id,
                            code ? code : "t_async_future_fail", msg ? msg : "future fail");
}

static int emit_future_cancel_cb(void *ctx, uint64_t future_id) {
  zi_async_handle_ctx *c = (zi_async_handle_ctx *)ctx;
  if (!c) return 0;
  future_free(c, future_id);
  return out_append_ev_future_cancel(c, (uint32_t)c->cur_emit_rid, future_id);
}

// ---- dispatch ----

static int dispatch_list(zi_async_handle_ctx *c, const zi_zcl1_frame *fr) {
  if (fr->payload_len != 0) {
    (void)out_append_ok_u32(c, fr->op, fr->rid, ZI_ASYNC_E_INVALID);
    return 1;
  }

  const zi_async_registry_v1 *reg = zi_async_registry();
  uint32_t n = reg ? (uint32_t)reg->selector_count : 0;

  // payload: u32 version (1), u32 n, then n entries:
  //   u32 kind_len, bytes[kind_len]
  //   u32 name_len, bytes[name_len]
  //   u32 sel_len, bytes[sel_len]
  uint8_t payload[65536];
  uint32_t off = 0;
  write_u32le(payload + off, 1u);
  off += 4;
  write_u32le(payload + off, n);
  off += 4;

  if (reg) {
    for (uint32_t i = 0; i < n; i++) {
      const zi_async_selector *s = reg->selectors[i];
      if (!s || !s->cap_kind || !s->cap_name || !s->selector) continue;
      uint32_t klen = (uint32_t)strlen(s->cap_kind);
      uint32_t nlen = (uint32_t)strlen(s->cap_name);
      uint32_t slen = (uint32_t)strlen(s->selector);
      if (off + 4 + klen + 4 + nlen + 4 + slen > (uint32_t)sizeof(payload)) break;
      write_u32le(payload + off, klen);
      off += 4;
      memcpy(payload + off, s->cap_kind, klen);
      off += klen;
      write_u32le(payload + off, nlen);
      off += 4;
      memcpy(payload + off, s->cap_name, nlen);
      off += nlen;
      write_u32le(payload + off, slen);
      off += 4;
      memcpy(payload + off, s->selector, slen);
      off += slen;
    }
  }

  return out_append_ok_bytes(c, fr->op, fr->rid, payload, off);
}

static int dispatch_invoke(zi_async_handle_ctx *c, const zi_zcl1_frame *fr) {
  // payload:
  //   u32 kind_len, bytes[kind]
  //   u32 name_len, bytes[name]
  //   u32 selector_len, bytes[selector]
  //   u64 future_id
  //   u32 params_len, bytes[params]
  const uint8_t *p = fr->payload;
  uint32_t n = fr->payload_len;
  uint32_t off = 0;
  if (n < 4) {
    (void)out_append_ok_u32(c, fr->op, fr->rid, ZI_ASYNC_E_INVALID);
    return 1;
  }

  if (n < 4) return 1;
  if (off + 4 > n) {
    (void)out_append_ok_u32(c, fr->op, fr->rid, ZI_ASYNC_E_INVALID);
    return 1;
  }
  uint32_t klen = read_u32le(p + off);
  off += 4;
  if (klen == 0 || off + klen + 4 > n) {
    (void)out_append_ok_u32(c, fr->op, fr->rid, ZI_ASYNC_E_INVALID);
    return 1;
  }
  const char *kind = (const char *)(p + off);
  off += klen;

  uint32_t nlen = read_u32le(p + off);
  off += 4;
  if (nlen == 0 || off + nlen + 4 > n) {
    (void)out_append_ok_u32(c, fr->op, fr->rid, ZI_ASYNC_E_INVALID);
    return 1;
  }
  const char *name = (const char *)(p + off);
  off += nlen;

  uint32_t slen = read_u32le(p + off);
  off += 4;
  if (slen == 0 || off + slen + 8 + 4 > n) {
    (void)out_append_ok_u32(c, fr->op, fr->rid, ZI_ASYNC_E_INVALID);
    return 1;
  }
  const char *sel = (const char *)(p + off);
  off += slen;

  uint64_t future_id = read_u64le(p + off);
  off += 8;

  uint32_t params_len = read_u32le(p + off);
  off += 4;
  if (off + params_len != n) {
    (void)out_append_ok_u32(c, fr->op, fr->rid, ZI_ASYNC_E_INVALID);
    return 1;
  }
  const uint8_t *params = (params_len ? (p + off) : NULL);

  const zi_async_selector *s = zi_async_find(kind, klen, name, nlen, sel, slen);
  if (!s || !s->invoke) {
    (void)out_append_ok_u32(c, fr->op, fr->rid, ZI_ASYNC_E_NOENT);
    // Also emit a failure event for uniformity.
    (void)out_append_ev_fail(c, (uint16_t)ZI_ASYNC_EV_FAIL, fr->rid, future_id, "t_async_noent", "selector not found");
    return 1;
  }

  if (!future_alloc(c, future_id, s, (uint64_t)fr->rid)) {
    (void)out_append_ok_u32(c, fr->op, fr->rid, ZI_ASYNC_E_INVALID);
    (void)out_append_ev_fail(c, (uint16_t)ZI_ASYNC_EV_FAIL, fr->rid, future_id, "t_async_dup_future", "duplicate/invalid future id");
    return 1;
  }

  // Response for the INVOKE request itself: accepted.
  if (!out_append_ok_u32(c, fr->op, fr->rid, ZI_ASYNC_OK)) {
    return 0;
  }

  c->cur_req_id = (uint64_t)fr->rid;
  c->cur_emit_rid = (uint64_t)fr->rid;
  c->cur_future_id = future_id;
  c->cur_acked = 0;
  c->cur_failed = 0;

  int ok = s->invoke(&EMIT, c, params, params_len, (uint64_t)fr->rid, future_id);

  // If the selector did not emit ack/fail, treat as internal and emit fail.
  if (!c->cur_acked && !c->cur_failed) {
    (void)ok;
    future_free(c, future_id);
    (void)out_append_ev_fail(c, (uint16_t)ZI_ASYNC_EV_FAIL, fr->rid, future_id, "t_async_no_ack", "selector did not ack/fail");
  }

  // If it failed synchronously, ensure the pending future is cleared.
  if (!ok) {
    future_free(c, future_id);
  }

  c->cur_future_id = 0;
  c->cur_emit_rid = 0;

  return 1;
}

static int dispatch_cancel(zi_async_handle_ctx *c, const zi_zcl1_frame *fr) {
  // payload: u64 future_id
  if (fr->payload_len != 8) {
    (void)out_append_ok_u32(c, fr->op, fr->rid, ZI_ASYNC_E_INVALID);
    return 1;
  }
  uint64_t future_id = read_u64le(fr->payload);

  int idx = future_find_idx(c, future_id);
  if (idx < 0) {
    (void)out_append_ok_u32(c, fr->op, fr->rid, ZI_ASYNC_E_NOENT);
    return 1;
  }

  const zi_async_selector *s = c->futures[idx].sel;
  if (!s || !s->cancel) {
    (void)out_append_ok_u32(c, fr->op, fr->rid, ZI_ASYNC_E_UNSUPPORTED);
    return 1;
  }

  int ok = s->cancel(c, future_id);
  if (!ok) {
    (void)out_append_ok_u32(c, fr->op, fr->rid, ZI_ASYNC_E_INTERNAL);
    return 1;
  }

  future_free(c, future_id);

  // Response first, then emit a cancellation event.
  if (!out_append_ok_u32(c, fr->op, fr->rid, ZI_ASYNC_OK)) return 0;
  if (!out_append_ev_future_cancel(c, fr->rid, future_id)) return 0;
  return 1;
}

static int dispatch_request(zi_async_handle_ctx *c, const zi_zcl1_frame *fr) {
  if (!c || !fr) return 0;

  switch (fr->op) {
    case ZI_ASYNC_OP_LIST:
      return dispatch_list(c, fr);
    case ZI_ASYNC_OP_INVOKE:
      return dispatch_invoke(c, fr);
    case ZI_ASYNC_OP_CANCEL:
      return dispatch_cancel(c, fr);
    default:
      return out_append_err(c, fr->op, fr->rid, "t_async_unknown_op", "unknown op");
  }
}

static int32_t async_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  zi_async_handle_ctx *c = (zi_async_handle_ctx *)ctx;
  if (!c) return ZI_E_INTERNAL;
  if (c->closed) return 0;
  if (cap == 0) return 0;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_rw) return ZI_E_NOSYS;

  if (c->out_off >= c->out_len) {
    return ZI_E_AGAIN;
  }

  uint8_t *dst = NULL;
  if (!mem->map_rw(mem->ctx, dst_ptr, cap, &dst) || !dst) return ZI_E_BOUNDS;

  uint32_t avail = c->out_len - c->out_off;
  uint32_t n = cap < avail ? cap : avail;
  memcpy(dst, c->outbuf + c->out_off, n);
  c->out_off += n;

  if (c->out_off == c->out_len) {
    c->out_off = 0;
    c->out_len = 0;
  }

  return (int32_t)n;
}

static int32_t async_write(void *ctx, zi_ptr_t src_ptr, zi_size32_t len) {
  zi_async_handle_ctx *c = (zi_async_handle_ctx *)ctx;
  if (!c) return ZI_E_INTERNAL;
  if (c->closed) return ZI_E_CLOSED;
  if (len == 0) return 0;

  if (c->out_len != 0) {
    // One outstanding response/event batch at a time.
    return ZI_E_AGAIN;
  }

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_ro) return ZI_E_NOSYS;

  const uint8_t *src = NULL;
  if (!mem->map_ro(mem->ctx, src_ptr, len, &src) || !src) return ZI_E_BOUNDS;

  if ((uint64_t)c->in_len + (uint64_t)len > (uint64_t)sizeof(c->inbuf)) {
    return ZI_E_BOUNDS;
  }

  memcpy(c->inbuf + c->in_len, src, len);
  c->in_len += len;

  if (c->in_len < 24) {
    return (int32_t)len;
  }

  if (!(c->inbuf[0] == 'Z' && c->inbuf[1] == 'C' && c->inbuf[2] == 'L' && c->inbuf[3] == '1')) {
    c->in_len = 0;
    return ZI_E_INVALID;
  }

  uint32_t payload_len = zi_zcl1_read_u32(c->inbuf + 20);
  uint32_t frame_len = 24u + payload_len;
  if (frame_len > (uint32_t)sizeof(c->inbuf)) {
    c->in_len = 0;
    return ZI_E_BOUNDS;
  }
  if (frame_len > c->in_len) {
    return (int32_t)len;
  }
  if (frame_len != c->in_len) {
    c->in_len = 0;
    return ZI_E_INVALID;
  }

  zi_zcl1_frame fr;
  if (!zi_zcl1_parse(c->inbuf, c->in_len, &fr)) {
    c->in_len = 0;
    return ZI_E_INVALID;
  }

  int ok = dispatch_request(c, &fr);
  c->in_len = 0;

  if (!ok || c->out_len == 0) {
    // Always produce something.
    (void)out_append_err(c, fr.op, fr.rid, "t_async_internal", "dispatch failed");
  }

  c->out_off = 0;
  return (int32_t)len;
}

static int32_t async_end(void *ctx) {
  zi_async_handle_ctx *c = (zi_async_handle_ctx *)ctx;
  if (!c) return ZI_E_INTERNAL;
  c->closed = 1;
  memset(c, 0, sizeof(*c));
  free(c);
  return 0;
}

static const zi_handle_ops_v1 ASYNC_OPS = {
    .read = async_read,
    .write = async_write,
    .end = async_end,
};

zi_handle_t zi_async_default25_open_from_params(zi_ptr_t params_ptr, zi_size32_t params_len) {
  (void)params_ptr;
  if (params_len != 0) return (zi_handle_t)ZI_E_INVALID;

  if (!zi_handles25_init()) {
    return (zi_handle_t)ZI_E_INTERNAL;
  }

  zi_async_handle_ctx *c = (zi_async_handle_ctx *)calloc(1u, sizeof(*c));
  if (!c) return (zi_handle_t)ZI_E_OOM;

  zi_handle_t h = zi_handle25_alloc(&ASYNC_OPS, c, ZI_H_READABLE | ZI_H_WRITABLE | ZI_H_ENDABLE);
  if (h < 3) {
    async_end(c);
    return (zi_handle_t)ZI_E_INTERNAL;
  }
  return h;
}
