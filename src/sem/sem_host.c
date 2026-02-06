#include "sem_host.h"

#include "zcl1.h"

#include <string.h>

void sem_host_init(sem_host_t* h, sem_host_cfg_t cfg) {
  if (!h) return;
  h->cfg = cfg;
}

static bool sem_caps_list_payload(const sem_host_t* h, uint8_t* out, uint32_t cap, uint32_t* out_len) {
  if (!out || !out_len) return false;
  if (cap < 8) return false;

  uint32_t off = 0;
  zcl1_write_u32le(out + off, 1);
  off += 4;
  zcl1_write_u32le(out + off, h ? h->cfg.cap_count : 0);
  off += 4;

  const uint32_t n = h ? h->cfg.cap_count : 0;
  for (uint32_t i = 0; i < n; i++) {
    const sem_cap_t* c = &h->cfg.caps[i];
    const uint32_t kind_len = c->kind ? (uint32_t)strlen(c->kind) : 0;
    const uint32_t name_len = c->name ? (uint32_t)strlen(c->name) : 0;
    const uint32_t meta_len = (c->meta && c->meta_len) ? c->meta_len : 0;
    const uint64_t need = 4ull + kind_len + 4ull + name_len + 4ull + 4ull + meta_len;
    if ((uint64_t)off + need > (uint64_t)cap) return false;
    zcl1_write_u32le(out + off, kind_len);
    off += 4;
    if (kind_len) memcpy(out + off, c->kind, kind_len);
    off += kind_len;
    zcl1_write_u32le(out + off, name_len);
    off += 4;
    if (name_len) memcpy(out + off, c->name, name_len);
    off += name_len;
    zcl1_write_u32le(out + off, c->flags);
    off += 4;
    zcl1_write_u32le(out + off, meta_len);
    off += 4;
    if (meta_len) memcpy(out + off, c->meta, meta_len);
    off += meta_len;
  }

  *out_len = off;
  return true;
}

static int32_t sem_write_error(uint8_t* resp, uint32_t resp_cap, uint16_t op, uint32_t rid, const char* trace,
                               const char* msg, const char* detail) {
  uint8_t payload[512];
  uint32_t payload_len = 0;
  if (!zcl1_write_error_payload(payload, (uint32_t)sizeof(payload), trace, msg, detail, &payload_len)) {
    return SEM_ZI_E_INTERNAL;
  }
  uint32_t resp_len = 0;
  if (!zcl1_write(resp, resp_cap, op, rid, 0, payload, payload_len, &resp_len)) {
    return SEM_ZI_E_BOUNDS;
  }
  return (int32_t)resp_len;
}

int32_t sem_zi_ctl(void* user, const uint8_t* req, uint32_t req_len, uint8_t* resp, uint32_t resp_cap) {
  const sem_host_t* h = (const sem_host_t*)user;

  zcl1_hdr_t rh = {0};
  const uint8_t* payload = NULL;
  if (!zcl1_parse(req, req_len, &rh, &payload)) {
    return SEM_ZI_E_INVALID;
  }

  // Requests must have status=0 and empty reserved (validated by parser).
  if (rh.status != 0) {
    return SEM_ZI_E_INVALID;
  }

  if (rh.op == SEM_ZI_CTL_OP_CAPS_LIST) {
    if (rh.payload_len != 0) {
      return sem_write_error(resp, resp_cap, rh.op, rh.rid, "sem.zi_ctl.invalid", "CAPS_LIST payload must be empty",
                             "");
    }
    uint8_t payload_buf[2048];
    uint32_t payload_len2 = 0;
    if (!sem_caps_list_payload(h, payload_buf, (uint32_t)sizeof(payload_buf), &payload_len2)) {
      return sem_write_error(resp, resp_cap, rh.op, rh.rid, "sem.zi_ctl.internal", "failed to build caps payload", "");
    }

    uint32_t out_len = 0;
    if (!zcl1_write(resp, resp_cap, rh.op, rh.rid, 1, payload_buf, payload_len2, &out_len)) {
      return SEM_ZI_E_BOUNDS;
    }
    return (int32_t)out_len;
  }

  return sem_write_error(resp, resp_cap, rh.op, rh.rid, "sem.zi_ctl.nosys", "unsupported zi_ctl op", "");
}

bool sem_build_caps_list_req(uint32_t rid, uint8_t* out, uint32_t cap, uint32_t* out_len) {
  return zcl1_write(out, cap, SEM_ZI_CTL_OP_CAPS_LIST, rid, 0, NULL, 0, out_len);
}
