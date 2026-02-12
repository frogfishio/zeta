#include "sem_host.h"

#include "zcl1.h"

#include <stdlib.h>
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
    const uint64_t need = 4ull + kind_len + 4ull + name_len + 4ull;
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

static int32_t sem_write_ok(uint8_t* resp, uint32_t resp_cap, uint16_t op, uint32_t rid, const uint8_t* payload, uint32_t payload_len) {
  uint32_t out_len = 0;
  if (!zcl1_write(resp, resp_cap, op, rid, 1, payload, payload_len, &out_len)) {
    return SEM_ZI_E_BOUNDS;
  }
  return (int32_t)out_len;
}

static int32_t sem_write_denied(uint8_t* resp, uint32_t resp_cap, uint16_t op, uint32_t rid, const char* what) {
  return sem_write_error(resp, resp_cap, op, rid, "sem.zi_ctl.denied", what ? what : "capability not enabled", "");
}

enum {
  SEM_HOST_MAX_BLOB = 64u * 1024u,
};

int32_t sem_zi_ctl(void* user, const uint8_t* req, uint32_t req_len, uint8_t* resp, uint32_t resp_cap) {
  const sem_host_t* h = (const sem_host_t*)user;

  zcl1_hdr_t rh = {0};
  const uint8_t* payload = NULL;
  if (!zcl1_parse(req, req_len, &rh, &payload)) {
    return SEM_ZI_E_INVALID;
  }

  if (rh.status != 0) {
    return SEM_ZI_E_INVALID;
  }

  if (rh.op == SEM_ZI_CTL_OP_CAPS_LIST) {
    if (rh.payload_len != 0) {
      return sem_write_error(resp, resp_cap, rh.op, rh.rid, "sem.zi_ctl.invalid", "CAPS_LIST payload must be empty", "");
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

  // --- sem host protocol ops (op >= 1000) ---
  if (rh.op == SEM_ZI_CTL_OP_SEM_ARGV_COUNT) {
    if (!h || !h->cfg.argv_enabled) return sem_write_denied(resp, resp_cap, rh.op, rh.rid, "argv not enabled");
    if (rh.payload_len != 0) {
      return sem_write_error(resp, resp_cap, rh.op, rh.rid, "sem.zi_ctl.invalid", "ARGV_COUNT payload must be empty", "");
    }
    uint8_t payload_buf[4];
    zcl1_write_u32le(payload_buf + 0, h->cfg.argv_count);
    return sem_write_ok(resp, resp_cap, rh.op, rh.rid, payload_buf, (uint32_t)sizeof(payload_buf));
  }

  if (rh.op == SEM_ZI_CTL_OP_SEM_ARGV_GET) {
    if (!h || !h->cfg.argv_enabled) return sem_write_denied(resp, resp_cap, rh.op, rh.rid, "argv not enabled");
    if (rh.payload_len != 4) {
      return sem_write_error(resp, resp_cap, rh.op, rh.rid, "sem.zi_ctl.invalid", "ARGV_GET payload must be u32 index", "");
    }
    const uint32_t index = zcl1_read_u32le(payload + 0);
    if (index >= h->cfg.argv_count) {
      return sem_write_error(resp, resp_cap, rh.op, rh.rid, "sem.zi_ctl.bounds", "ARGV index out of range", "");
    }
    const char* s = (h->cfg.argv && h->cfg.argv[index]) ? h->cfg.argv[index] : "";
    const uint32_t sl = (uint32_t)strlen(s);
    const uint64_t need = 4ull + (uint64_t)sl;
    if (need > SEM_HOST_MAX_BLOB) {
      return sem_write_error(resp, resp_cap, rh.op, rh.rid, "sem.zi_ctl.invalid", "ARGV item too large", "");
    }
    uint8_t* payload_buf = (uint8_t*)malloc((size_t)need);
    if (!payload_buf) {
      return sem_write_error(resp, resp_cap, rh.op, rh.rid, "sem.zi_ctl.oom", "out of memory", "");
    }
    zcl1_write_u32le(payload_buf + 0, sl);
    if (sl) memcpy(payload_buf + 4, s, sl);
    const int32_t r = sem_write_ok(resp, resp_cap, rh.op, rh.rid, payload_buf, (uint32_t)need);
    free(payload_buf);
    return r;
  }

  if (rh.op == SEM_ZI_CTL_OP_SEM_ENV_COUNT) {
    if (!h || !h->cfg.env_enabled) return sem_write_denied(resp, resp_cap, rh.op, rh.rid, "env not enabled");
    if (rh.payload_len != 0) {
      return sem_write_error(resp, resp_cap, rh.op, rh.rid, "sem.zi_ctl.invalid", "ENV_COUNT payload must be empty", "");
    }
    uint8_t payload_buf[4];
    zcl1_write_u32le(payload_buf + 0, h->cfg.env_count);
    return sem_write_ok(resp, resp_cap, rh.op, rh.rid, payload_buf, (uint32_t)sizeof(payload_buf));
  }

  if (rh.op == SEM_ZI_CTL_OP_SEM_ENV_GET) {
    if (!h || !h->cfg.env_enabled) return sem_write_denied(resp, resp_cap, rh.op, rh.rid, "env not enabled");
    if (rh.payload_len != 4) {
      return sem_write_error(resp, resp_cap, rh.op, rh.rid, "sem.zi_ctl.invalid", "ENV_GET payload must be u32 index", "");
    }
    const uint32_t index = zcl1_read_u32le(payload + 0);
    if (index >= h->cfg.env_count) {
      return sem_write_error(resp, resp_cap, rh.op, rh.rid, "sem.zi_ctl.bounds", "ENV index out of range", "");
    }
    const sem_env_kv_t* kv = (h->cfg.env) ? &h->cfg.env[index] : NULL;
    const char* k = (kv && kv->key) ? kv->key : "";
    const char* v = (kv && kv->val) ? kv->val : "";
    const uint32_t kl = (uint32_t)strlen(k);
    const uint32_t vl = (uint32_t)strlen(v);
    const uint64_t need = 4ull + (uint64_t)kl + 4ull + (uint64_t)vl;
    if (need > SEM_HOST_MAX_BLOB) {
      return sem_write_error(resp, resp_cap, rh.op, rh.rid, "sem.zi_ctl.invalid", "ENV item too large", "");
    }
    uint8_t* payload_buf = (uint8_t*)malloc((size_t)need);
    if (!payload_buf) {
      return sem_write_error(resp, resp_cap, rh.op, rh.rid, "sem.zi_ctl.oom", "out of memory", "");
    }
    uint32_t off = 0;
    zcl1_write_u32le(payload_buf + off, kl);
    off += 4;
    if (kl) memcpy(payload_buf + off, k, kl);
    off += kl;
    zcl1_write_u32le(payload_buf + off, vl);
    off += 4;
    if (vl) memcpy(payload_buf + off, v, vl);
    off += vl;
    const int32_t r = sem_write_ok(resp, resp_cap, rh.op, rh.rid, payload_buf, off);
    free(payload_buf);
    return r;
  }

  return sem_write_error(resp, resp_cap, rh.op, rh.rid, "sem.zi_ctl.nosys", "unsupported zi_ctl op", "");
}

bool sem_build_caps_list_req(uint32_t rid, uint8_t* out, uint32_t cap, uint32_t* out_len) {
  return zcl1_write(out, cap, SEM_ZI_CTL_OP_CAPS_LIST, rid, 0, NULL, 0, out_len);
}

