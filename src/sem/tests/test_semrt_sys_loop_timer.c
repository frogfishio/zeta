#include "hosted_zabi.h"
#include "zcl1.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char* msg) {
  fprintf(stderr, "sem_unit_tests: %s\n", msg);
  return 1;
}

static void u32le(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xffu);
  p[1] = (uint8_t)((v >> 8) & 0xffu);
  p[2] = (uint8_t)((v >> 16) & 0xffu);
  p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void u64le(uint8_t* p, uint64_t v) {
  u32le(p + 0, (uint32_t)(v & 0xFFFFFFFFu));
  u32le(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static zi_handle_t open_cap(sir_hosted_zabi_t* rt, const char* kind, const char* name) {
  const uint32_t kind_len = (uint32_t)strlen(kind);
  const uint32_t name_len = (uint32_t)strlen(name);

  zi_ptr_t kind_ptr = sir_zi_alloc(rt, kind_len);
  zi_ptr_t name_ptr = sir_zi_alloc(rt, name_len);
  if (!kind_ptr || !name_ptr) return (zi_handle_t)-10;

  uint8_t* w = NULL;
  if (!sem_guest_mem_map_rw(rt->mem, kind_ptr, kind_len, &w) || !w) return (zi_handle_t)-10;
  memcpy(w, kind, kind_len);
  if (!sem_guest_mem_map_rw(rt->mem, name_ptr, name_len, &w) || !w) return (zi_handle_t)-10;
  memcpy(w, name, name_len);

  uint8_t open_req[40];
  memset(open_req, 0, sizeof(open_req));
  u64le(open_req + 0, (uint64_t)kind_ptr);
  u32le(open_req + 8, kind_len);
  u64le(open_req + 12, (uint64_t)name_ptr);
  u32le(open_req + 20, name_len);
  u32le(open_req + 24, 0);
  u64le(open_req + 28, 0);
  u32le(open_req + 36, 0);

  const zi_ptr_t open_req_ptr = sir_zi_alloc(rt, (zi_size32_t)sizeof(open_req));
  if (!open_req_ptr) return (zi_handle_t)-10;
  if (!sem_guest_mem_map_rw(rt->mem, open_req_ptr, (zi_size32_t)sizeof(open_req), &w) || !w) return (zi_handle_t)-10;
  memcpy(w, open_req, sizeof(open_req));

  return sir_zi_cap_open(rt, open_req_ptr);
}

static int write_frame(sir_hosted_zabi_t* rt, zi_handle_t h, const uint8_t* bytes, uint32_t n) {
  const zi_ptr_t p = sir_zi_alloc(rt, n);
  if (!p) return fail("alloc write buf failed");
  uint8_t* w = NULL;
  if (!sem_guest_mem_map_rw(rt->mem, p, n, &w) || !w) return fail("map write buf failed");
  memcpy(w, bytes, n);
  const int32_t rc = sir_zi_write(rt, h, p, n);
  if (rc < 0) return fail("zi_write failed");
  if ((uint32_t)rc != n) return fail("zi_write short write");
  return 0;
}

static int read_one_frame(sir_hosted_zabi_t* rt, zi_handle_t h, uint8_t* out, uint32_t out_cap, uint32_t* out_n) {
  const zi_ptr_t p = sir_zi_alloc(rt, out_cap);
  if (!p) return fail("alloc read buf failed");
  const int32_t rc = sir_zi_read(rt, h, p, out_cap);
  if (rc < 0) return fail("zi_read failed");
  const uint32_t n = (uint32_t)rc;
  const uint8_t* r = NULL;
  if (!sem_guest_mem_map_ro(rt->mem, p, n, &r) || !r) return fail("map read buf failed");
  memcpy(out, r, n);
  *out_n = n;
  return 0;
}

int main(void) {
  sem_cap_t caps[1];
  memset(caps, 0, sizeof(caps));
  caps[0].kind = "sys";
  caps[0].name = "loop";
  caps[0].flags = SEM_ZI_CAP_CAN_OPEN | SEM_ZI_CAP_MAY_BLOCK;

  sir_hosted_zabi_t rt;
  if (!sir_hosted_zabi_init(&rt,
                            (sir_hosted_zabi_cfg_t){.guest_mem_cap = 1024 * 1024, .guest_mem_base = 0x10000ull, .caps = caps, .cap_count = 1})) {
    return fail("sir_hosted_zabi_init failed");
  }

  const zi_handle_t h = open_cap(&rt, "sys", "loop");
  if (h < 3) return fail("cap_open sys/loop failed");

  // TIMER_ARM (op=3): timer_id=1, due=10ms relative, interval=0
  uint8_t arm_payload[28];
  u64le(arm_payload + 0, 1);
  u64le(arm_payload + 8, 10ull * 1000000ull);
  u64le(arm_payload + 16, 0);
  u32le(arm_payload + 24, 1u);

  uint8_t arm_frame[ZCL1_HDR_SIZE + sizeof(arm_payload)];
  uint32_t arm_len = 0;
  if (!zcl1_write(arm_frame, (uint32_t)sizeof(arm_frame), 3, 10, 0, arm_payload, (uint32_t)sizeof(arm_payload), &arm_len))
    return fail("zcl1_write arm failed");
  if (write_frame(&rt, h, arm_frame, arm_len) != 0) return 1;

  uint8_t resp[512];
  uint32_t resp_n = 0;
  if (read_one_frame(&rt, h, resp, (uint32_t)sizeof(resp), &resp_n) != 0) return 1;

  zcl1_hdr_t rh = {0};
  const uint8_t* rp = NULL;
  if (!zcl1_parse(resp, resp_n, &rh, &rp)) return fail("bad arm response frame");
  if (rh.op != 3 || rh.rid != 10 || rh.status != 1 || rh.payload_len != 0) return fail("arm response mismatch");

  // POLL (op=5): max_events=4, timeout_ms=100
  uint8_t poll_payload[8];
  u32le(poll_payload + 0, 4);
  u32le(poll_payload + 4, 100);

  uint8_t poll_frame[ZCL1_HDR_SIZE + sizeof(poll_payload)];
  uint32_t poll_len = 0;
  if (!zcl1_write(poll_frame, (uint32_t)sizeof(poll_frame), 5, 11, 0, poll_payload, (uint32_t)sizeof(poll_payload), &poll_len))
    return fail("zcl1_write poll failed");
  if (write_frame(&rt, h, poll_frame, poll_len) != 0) return 1;

  if (read_one_frame(&rt, h, resp, (uint32_t)sizeof(resp), &resp_n) != 0) return 1;
  if (!zcl1_parse(resp, resp_n, &rh, &rp)) return fail("bad poll response frame");
  if (rh.op != 5 || rh.rid != 11 || rh.status != 1) return fail("poll response hdr mismatch");
  if (rh.payload_len < 16) return fail("poll payload too small");

  const uint32_t version = zcl1_read_u32le(rp + 0);
  const uint32_t flags = zcl1_read_u32le(rp + 4);
  const uint32_t event_count = zcl1_read_u32le(rp + 8);
  const uint32_t reserved = zcl1_read_u32le(rp + 12);
  (void)flags;
  if (version != 1) return fail("poll version mismatch");
  if (reserved != 0) return fail("poll reserved mismatch");
  if (event_count < 1) return fail("expected at least one timer event");
  if (16u + event_count * 32u > rh.payload_len) return fail("poll payload len mismatch");

  const uint8_t* ev0 = rp + 16;
  const uint32_t kind = zcl1_read_u32le(ev0 + 0);
  const uint32_t ev_handle = zcl1_read_u32le(ev0 + 8);
  const uint64_t ev_id = (uint64_t)zcl1_read_u32le(ev0 + 16) | ((uint64_t)zcl1_read_u32le(ev0 + 20) << 32);
  if (kind != 2) return fail("expected TIMER event kind");
  if (ev_handle != 0) return fail("expected TIMER handle=0");
  if (ev_id != 1) return fail("expected timer_id=1");

  (void)sir_zi_end(&rt, h);
  sir_hosted_zabi_dispose(&rt);
  return 0;
}
