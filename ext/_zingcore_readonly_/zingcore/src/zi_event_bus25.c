#include "zi_event_bus25.h"

#include "zi_handles25.h"
#include "zi_runtime25.h"
#include "zi_zcl1.h"

#include <stdlib.h>
#include <string.h>

// ---- cap descriptor ----

static const zi_cap_v1 CAP = {
    .kind = ZI_CAP_KIND_EVENT,
    .name = ZI_CAP_NAME_BUS,
    .version = 1,
    .cap_flags = ZI_CAP_CAN_OPEN | ZI_CAP_MAY_BLOCK,
    .meta = NULL,
    .meta_len = 0,
};

const zi_cap_v1 *zi_event_bus25_cap(void) { return &CAP; }

int zi_event_bus25_register(void) { return zi_cap_register(&CAP); }

// ---- in-process bus state ----

typedef struct zi_event_bus_handle_ctx zi_event_bus_handle_ctx;

typedef struct {
  uint32_t id;
  zi_event_bus_handle_ctx *owner;
  uint8_t *topic;
  uint32_t topic_len;
} zi_event_bus_sub;

typedef struct {
  uint32_t next_sub_id;

  zi_event_bus_sub *subs;
  uint32_t sub_count;
  uint32_t sub_cap;
} zi_event_bus_state;

static zi_event_bus_state g_bus;

static void bus_reset_if_needed(void) {
  if (g_bus.next_sub_id == 0) g_bus.next_sub_id = 1;
}

static void bus_free_sub(zi_event_bus_sub *s) {
  if (!s) return;
  free(s->topic);
  s->topic = NULL;
  s->topic_len = 0;
  s->owner = NULL;
  s->id = 0;
}

static int bus_subs_reserve(uint32_t need) {
  if (need <= g_bus.sub_cap) return 1;
  uint32_t new_cap = g_bus.sub_cap ? g_bus.sub_cap : 16;
  while (new_cap < need) {
    if (new_cap > (uint32_t)(0x7FFFFFFFu / 2u)) return 0;
    new_cap *= 2u;
  }
  zi_event_bus_sub *p = (zi_event_bus_sub *)realloc(g_bus.subs, (size_t)new_cap * sizeof(zi_event_bus_sub));
  if (!p) return 0;
  g_bus.subs = p;
  g_bus.sub_cap = new_cap;
  return 1;
}

static uint32_t bus_subscribe(zi_event_bus_handle_ctx *owner, const uint8_t *topic, uint32_t topic_len) {
  bus_reset_if_needed();
  if (!owner || !topic || topic_len == 0) return 0;

  if (!bus_subs_reserve(g_bus.sub_count + 1)) return 0;

  uint8_t *copy = (uint8_t *)malloc(topic_len);
  if (!copy) return 0;
  memcpy(copy, topic, topic_len);

  uint32_t id = g_bus.next_sub_id++;
  if (id == 0) id = g_bus.next_sub_id++; // avoid 0

  zi_event_bus_sub *s = &g_bus.subs[g_bus.sub_count++];
  memset(s, 0, sizeof(*s));
  s->id = id;
  s->owner = owner;
  s->topic = copy;
  s->topic_len = topic_len;
  return id;
}

static uint32_t bus_unsubscribe(uint32_t sub_id) {
  if (sub_id == 0) return 0;
  for (uint32_t i = 0; i < g_bus.sub_count; i++) {
    if (g_bus.subs[i].id == sub_id) {
      bus_free_sub(&g_bus.subs[i]);
      // compact
      if (i + 1 < g_bus.sub_count) {
        memmove(&g_bus.subs[i], &g_bus.subs[i + 1], (size_t)(g_bus.sub_count - (i + 1)) * sizeof(zi_event_bus_sub));
      }
      g_bus.sub_count--;
      return 1;
    }
  }
  return 0;
}

static void bus_unsubscribe_owner(zi_event_bus_handle_ctx *owner) {
  if (!owner) return;
  uint32_t i = 0;
  while (i < g_bus.sub_count) {
    if (g_bus.subs[i].owner == owner) {
      bus_free_sub(&g_bus.subs[i]);
      if (i + 1 < g_bus.sub_count) {
        memmove(&g_bus.subs[i], &g_bus.subs[i + 1], (size_t)(g_bus.sub_count - (i + 1)) * sizeof(zi_event_bus_sub));
      }
      g_bus.sub_count--;
      continue;
    }
    i++;
  }
}

// ---- handle implementation ----

struct zi_event_bus_handle_ctx {
  uint8_t inbuf[65536];
  uint32_t in_len;

  uint8_t outbuf[65536];
  uint32_t out_len;
  uint32_t out_off;

  int closed;
};

static int out_append(zi_event_bus_handle_ctx *c, const uint8_t *bytes, uint32_t len) {
  if (!c) return 0;
  if (len == 0) return 1;
  if ((uint64_t)c->out_len + (uint64_t)len > (uint64_t)sizeof(c->outbuf)) return 0;
  memcpy(c->outbuf + c->out_len, bytes, len);
  c->out_len += len;
  return 1;
}

static int out_append_ok_u32(zi_event_bus_handle_ctx *c, uint16_t op, uint32_t rid, uint32_t v) {
  uint8_t payload[4];
  zi_zcl1_write_u32(payload + 0, v);
  uint8_t tmp[64];
  int n = zi_zcl1_write_ok(tmp, (uint32_t)sizeof(tmp), op, rid, payload, (uint32_t)sizeof(payload));
  if (n < 0) return 0;
  return out_append(c, tmp, (uint32_t)n);
}

static int out_append_err(zi_event_bus_handle_ctx *c, uint16_t op, uint32_t rid, const char *trace, const char *msg) {
  uint8_t tmp[256];
  int n = zi_zcl1_write_error(tmp, (uint32_t)sizeof(tmp), op, rid, trace, msg);
  if (n < 0) return 0;
  return out_append(c, tmp, (uint32_t)n);
}

static int out_append_event(zi_event_bus_handle_ctx *c, uint32_t rid, uint32_t sub_id, const uint8_t *topic, uint32_t topic_len,
                            const uint8_t *data, uint32_t data_len) {
  if (!c) return 0;
  if (topic_len > 60000u || data_len > 60000u) return 0;

  uint32_t payload_len = 4u + 4u + topic_len + 4u + data_len;
  if (payload_len > 65000u) return 0;

  uint8_t tmp[65536];
  zi_zcl1_write_u32(tmp + 0, sub_id);
  zi_zcl1_write_u32(tmp + 4, topic_len);
  if (topic_len) memcpy(tmp + 8, topic, topic_len);
  uint32_t off = 8u + topic_len;
  zi_zcl1_write_u32(tmp + off, data_len);
  off += 4;
  if (data_len) memcpy(tmp + off, data, data_len);
  off += data_len;

  if (off != payload_len) return 0;

  uint8_t fr[65536];
  int n = zi_zcl1_write_ok(fr, (uint32_t)sizeof(fr), (uint16_t)ZI_EVENT_BUS_EV_EVENT, rid, tmp, payload_len);
  if (n < 0) return 0;
  return out_append(c, fr, (uint32_t)n);
}

static int dispatch_subscribe(zi_event_bus_handle_ctx *c, const zi_zcl1_frame *fr) {
  if (!c || !fr) return 0;
  if (fr->payload_len < 8) return out_append_err(c, fr->op, fr->rid, "t_event_bus_bad_sub", "bad SUBSCRIBE payload");

  uint32_t topic_len = zi_zcl1_read_u32(fr->payload + 0);
  if (topic_len == 0 || 4u + topic_len + 4u != fr->payload_len) {
    return out_append_err(c, fr->op, fr->rid, "t_event_bus_bad_sub", "bad SUBSCRIBE payload");
  }

  const uint8_t *topic = fr->payload + 4;
  uint32_t flags = zi_zcl1_read_u32(fr->payload + 4u + topic_len);
  if (flags != 0) return out_append_err(c, fr->op, fr->rid, "t_event_bus_flags", "flags must be 0");

  uint32_t sub_id = bus_subscribe(c, topic, topic_len);
  if (sub_id == 0) return out_append_err(c, fr->op, fr->rid, "t_event_bus_oom", "subscribe failed");
  return out_append_ok_u32(c, fr->op, fr->rid, sub_id);
}

static int dispatch_unsubscribe(zi_event_bus_handle_ctx *c, const zi_zcl1_frame *fr) {
  if (!c || !fr) return 0;
  if (fr->payload_len != 4) return out_append_err(c, fr->op, fr->rid, "t_event_bus_bad_unsub", "bad UNSUBSCRIBE payload");
  uint32_t sub_id = zi_zcl1_read_u32(fr->payload + 0);
  uint32_t removed = bus_unsubscribe(sub_id);
  return out_append_ok_u32(c, fr->op, fr->rid, removed);
}

static uint32_t bus_publish(uint32_t rid, const uint8_t *topic, uint32_t topic_len, const uint8_t *data, uint32_t data_len) {
  uint32_t delivered = 0;
  for (uint32_t i = 0; i < g_bus.sub_count; i++) {
    zi_event_bus_sub *s = &g_bus.subs[i];
    if (!s->owner || !s->topic) continue;
    if (s->topic_len != topic_len) continue;
    if (memcmp(s->topic, topic, topic_len) != 0) continue;

    // Best-effort delivery; if the subscriber buffer is full, skip.
    zi_event_bus_handle_ctx *c = s->owner;
    uint32_t before = c->out_len;
    if (out_append_event(c, rid, s->id, topic, topic_len, data, data_len)) {
      delivered++;
    } else {
      c->out_len = before; // ensure we didn't partially append (out_append_event is atomic, but be defensive)
    }
  }
  return delivered;
}

static int dispatch_publish(zi_event_bus_handle_ctx *c, const zi_zcl1_frame *fr) {
  if (!c || !fr) return 0;
  if (fr->payload_len < 8) return out_append_err(c, fr->op, fr->rid, "t_event_bus_bad_pub", "bad PUBLISH payload");

  uint32_t topic_len = zi_zcl1_read_u32(fr->payload + 0);
  if (topic_len == 0 || 4u + topic_len + 4u > fr->payload_len) {
    return out_append_err(c, fr->op, fr->rid, "t_event_bus_bad_pub", "bad PUBLISH payload");
  }

  const uint8_t *topic = fr->payload + 4;
  uint32_t off = 4u + topic_len;
  uint32_t data_len = zi_zcl1_read_u32(fr->payload + off);
  off += 4;
  if (off + data_len != fr->payload_len) {
    return out_append_err(c, fr->op, fr->rid, "t_event_bus_bad_pub", "bad PUBLISH payload");
  }
  const uint8_t *data = fr->payload + off;

  uint32_t delivered = bus_publish(fr->rid, topic, topic_len, data, data_len);

  // Response is queued on the publishing handle.
  return out_append_ok_u32(c, fr->op, fr->rid, delivered);
}

static int dispatch_request(zi_event_bus_handle_ctx *c, const zi_zcl1_frame *fr) {
  if (!c || !fr) return 0;
  switch (fr->op) {
    case ZI_EVENT_BUS_OP_SUBSCRIBE:
      return dispatch_subscribe(c, fr);
    case ZI_EVENT_BUS_OP_UNSUBSCRIBE:
      return dispatch_unsubscribe(c, fr);
    case ZI_EVENT_BUS_OP_PUBLISH:
      return dispatch_publish(c, fr);
    default:
      return out_append_err(c, fr->op, fr->rid, "t_event_bus_unknown_op", "unknown op");
  }
}

static int32_t bus_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  zi_event_bus_handle_ctx *c = (zi_event_bus_handle_ctx *)ctx;
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

static int32_t bus_write(void *ctx, zi_ptr_t src_ptr, zi_size32_t len) {
  zi_event_bus_handle_ctx *c = (zi_event_bus_handle_ctx *)ctx;
  if (!c) return ZI_E_INTERNAL;
  if (c->closed) return ZI_E_CLOSED;
  if (len == 0) return 0;

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

  uint32_t before_out = c->out_len;
  int ok = dispatch_request(c, &fr);
  c->in_len = 0;

  if (!ok || c->out_len == before_out) {
    (void)out_append_err(c, fr.op, fr.rid, "t_event_bus_internal", "dispatch failed");
  }

  return (int32_t)len;
}

static int32_t bus_end(void *ctx) {
  zi_event_bus_handle_ctx *c = (zi_event_bus_handle_ctx *)ctx;
  if (!c) return ZI_E_INTERNAL;
  c->closed = 1;
  bus_unsubscribe_owner(c);
  memset(c, 0, sizeof(*c));
  free(c);
  return 0;
}

static const zi_handle_ops_v1 BUS_OPS = {
    .read = bus_read,
    .write = bus_write,
    .end = bus_end,
};

zi_handle_t zi_event_bus25_open_from_params(zi_ptr_t params_ptr, zi_size32_t params_len) {
  (void)params_ptr;
  if (params_len != 0) return (zi_handle_t)ZI_E_INVALID;

  if (!zi_handles25_init()) {
    return (zi_handle_t)ZI_E_INTERNAL;
  }

  zi_event_bus_handle_ctx *c = (zi_event_bus_handle_ctx *)calloc(1u, sizeof(*c));
  if (!c) return (zi_handle_t)ZI_E_OOM;

  zi_handle_t h = zi_handle25_alloc(&BUS_OPS, c, ZI_H_READABLE | ZI_H_WRITABLE | ZI_H_ENDABLE);
  if (h < 3) {
    bus_end(c);
    return (zi_handle_t)ZI_E_INTERNAL;
  }
  return h;
}
