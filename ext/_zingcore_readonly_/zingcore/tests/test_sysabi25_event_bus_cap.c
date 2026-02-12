#include "zi_caps.h"
#include "zi_event_bus25.h"
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

static int parse_event_payload(const uint8_t *pl, uint32_t pl_len, uint32_t *out_sub_id,
                              const uint8_t **out_topic, uint32_t *out_topic_len,
                              const uint8_t **out_data, uint32_t *out_data_len) {
  if (!pl || pl_len < 4u + 4u + 4u) return 0;
  uint32_t sub_id = read_u32le(pl + 0);
  uint32_t topic_len = read_u32le(pl + 4);
  if (topic_len == 0 || 8u + topic_len + 4u > pl_len) return 0;
  const uint8_t *topic = pl + 8;
  uint32_t off = 8u + topic_len;
  uint32_t data_len = read_u32le(pl + off);
  off += 4;
  if (off + data_len != pl_len) return 0;
  const uint8_t *data = pl + off;

  if (out_sub_id) *out_sub_id = sub_id;
  if (out_topic) *out_topic = topic;
  if (out_topic_len) *out_topic_len = topic_len;
  if (out_data) *out_data = data;
  if (out_data_len) *out_data_len = data_len;
  return 1;
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

  if (!zi_event_bus25_register()) {
    fprintf(stderr, "zi_event_bus25_register failed\n");
    return 1;
  }

  // Negative: open with params rejected.
  {
    uint8_t req[40];
    uint8_t dummy = 0;
    build_open_req(req, ZI_CAP_KIND_EVENT, ZI_CAP_NAME_BUS, &dummy, 1);
    zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)req);
    if (h != ZI_E_INVALID) {
      fprintf(stderr, "expected invalid for params, got %d\n", h);
      return 1;
    }
  }

  // Open two handles: subscriber + publisher.
  uint8_t req_sub[40];
  uint8_t req_pub[40];
  build_open_req(req_sub, ZI_CAP_KIND_EVENT, ZI_CAP_NAME_BUS, NULL, 0);
  build_open_req(req_pub, ZI_CAP_KIND_EVENT, ZI_CAP_NAME_BUS, NULL, 0);

  zi_handle_t h_sub = zi_cap_open((zi_ptr_t)(uintptr_t)req_sub);
  zi_handle_t h_pub = zi_cap_open((zi_ptr_t)(uintptr_t)req_pub);
  if (h_sub < 3 || h_pub < 3) {
    fprintf(stderr, "expected handles, got sub=%d pub=%d\n", h_sub, h_pub);
    return 1;
  }

  const char *topic = "ui.click";
  const uint32_t topic_len = (uint32_t)strlen(topic);

  // SUBSCRIBE on subscriber.
  uint32_t sub_id = 0;
  {
    uint8_t payload[128];
    uint32_t off = 0;
    write_u32le(payload + off, topic_len);
    off += 4;
    memcpy(payload + off, topic, topic_len);
    off += topic_len;
    write_u32le(payload + off, 0);
    off += 4;

    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_EVENT_BUS_OP_SUBSCRIBE, 1, payload, off);
    if (zi_write(h_sub, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)(24u + off)) != (int32_t)(24u + off)) {
      fprintf(stderr, "SUBSCRIBE write failed\n");
      return 1;
    }

    uint8_t buf[1024];
    uint32_t got = 0;
    if (!drain(buf, (uint32_t)sizeof(buf), h_sub, &got) || got < 24) {
      fprintf(stderr, "SUBSCRIBE read failed\n");
      return 1;
    }

    zi_zcl1_frame z;
    if (!zi_zcl1_parse(buf, got, &z)) {
      fprintf(stderr, "SUBSCRIBE parse failed\n");
      return 1;
    }
    if (z.op != ZI_EVENT_BUS_OP_SUBSCRIBE || z.rid != 1 || z.payload_len != 4) {
      fprintf(stderr, "SUBSCRIBE response mismatch\n");
      return 1;
    }
    sub_id = read_u32le(z.payload);
    if (sub_id == 0) {
      fprintf(stderr, "SUBSCRIBE returned sub_id=0\n");
      return 1;
    }
  }

  // PUBLISH on publisher.
  {
    const char *data = "left";
    const uint32_t data_len = (uint32_t)strlen(data);

    uint8_t payload[256];
    uint32_t off = 0;
    write_u32le(payload + off, topic_len);
    off += 4;
    memcpy(payload + off, topic, topic_len);
    off += topic_len;
    write_u32le(payload + off, data_len);
    off += 4;
    memcpy(payload + off, data, data_len);
    off += data_len;

    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_EVENT_BUS_OP_PUBLISH, 2, payload, off);
    if (zi_write(h_pub, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)(24u + off)) != (int32_t)(24u + off)) {
      fprintf(stderr, "PUBLISH write failed\n");
      return 1;
    }

    uint8_t buf[1024];
    uint32_t got = 0;
    if (!drain(buf, (uint32_t)sizeof(buf), h_pub, &got) || got < 24) {
      fprintf(stderr, "PUBLISH read failed\n");
      return 1;
    }

    zi_zcl1_frame z;
    if (!zi_zcl1_parse(buf, got, &z)) {
      fprintf(stderr, "PUBLISH parse failed\n");
      return 1;
    }
    if (z.op != ZI_EVENT_BUS_OP_PUBLISH || z.rid != 2 || z.payload_len != 4) {
      fprintf(stderr, "PUBLISH response mismatch\n");
      return 1;
    }
    uint32_t delivered = read_u32le(z.payload);
    if (delivered != 1) {
      fprintf(stderr, "expected delivered=1, got %u\n", delivered);
      return 1;
    }
  }

  // Subscriber must see an EVENT with rid=2.
  {
    uint8_t buf[2048];
    uint32_t got = 0;
    if (!drain(buf, (uint32_t)sizeof(buf), h_sub, &got) || got < 24) {
      fprintf(stderr, "EVENT read failed\n");
      return 1;
    }

    zi_zcl1_frame z;
    if (!zi_zcl1_parse(buf, got, &z)) {
      fprintf(stderr, "EVENT parse failed\n");
      return 1;
    }
    if (z.op != ZI_EVENT_BUS_EV_EVENT || z.rid != 2) {
      fprintf(stderr, "expected EVENT op=100 rid=2, got op=%u rid=%u\n", (unsigned)z.op, (unsigned)z.rid);
      return 1;
    }

    uint32_t got_sub_id = 0;
    const uint8_t *got_topic = NULL;
    uint32_t got_topic_len = 0;
    const uint8_t *got_data = NULL;
    uint32_t got_data_len = 0;

    if (!parse_event_payload(z.payload, z.payload_len, &got_sub_id, &got_topic, &got_topic_len, &got_data, &got_data_len)) {
      fprintf(stderr, "EVENT payload parse failed\n");
      return 1;
    }

    if (got_sub_id != sub_id) {
      fprintf(stderr, "EVENT sub_id mismatch\n");
      return 1;
    }
    if (got_topic_len != topic_len || memcmp(got_topic, topic, topic_len) != 0) {
      fprintf(stderr, "EVENT topic mismatch\n");
      return 1;
    }
    if (got_data_len != 4 || memcmp(got_data, "left", 4) != 0) {
      fprintf(stderr, "EVENT data mismatch\n");
      return 1;
    }
  }

  // UNSUBSCRIBE.
  {
    uint8_t payload[4];
    write_u32le(payload + 0, sub_id);
    uint8_t fr[24 + 4];
    build_zcl1_req(fr, (uint16_t)ZI_EVENT_BUS_OP_UNSUBSCRIBE, 3, payload, 4);
    if (zi_write(h_sub, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "UNSUBSCRIBE write failed\n");
      return 1;
    }

    uint8_t buf[1024];
    uint32_t got = 0;
    if (!drain(buf, (uint32_t)sizeof(buf), h_sub, &got) || got < 24) {
      fprintf(stderr, "UNSUBSCRIBE read failed\n");
      return 1;
    }

    zi_zcl1_frame z;
    if (!zi_zcl1_parse(buf, got, &z)) {
      fprintf(stderr, "UNSUBSCRIBE parse failed\n");
      return 1;
    }
    if (z.op != ZI_EVENT_BUS_OP_UNSUBSCRIBE || z.rid != 3 || z.payload_len != 4) {
      fprintf(stderr, "UNSUBSCRIBE response mismatch\n");
      return 1;
    }
    uint32_t removed = read_u32le(z.payload);
    if (removed != 1) {
      fprintf(stderr, "expected removed=1, got %u\n", removed);
      return 1;
    }
  }

  (void)zi_end(h_sub);
  (void)zi_end(h_pub);

  return 0;
}
