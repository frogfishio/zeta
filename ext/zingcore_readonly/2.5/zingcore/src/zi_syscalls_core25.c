#include "zi_sysabi25.h"

#include "zi_handles25.h"
#include "zi_runtime25.h"

#include "zi_caps.h"
#include "zi_zcl1.h"

#include <string.h>

uint32_t zi_abi_version(void) {
  const zi_host_v1 *host = zi_runtime25_host();
  if (host && host->abi_version) return host->abi_version(host->ctx);
  return (uint32_t)ZI_SYSABI25_ZABI_VERSION;
}

static int ctl_caps_list(uint8_t *resp, uint32_t resp_cap, uint16_t op, uint32_t rid) {
  const zi_cap_registry_v1 *reg = zi_cap_registry();
  uint32_t n = reg ? (uint32_t)reg->cap_count : 0;

  // Payload:
  //   u32 version (currently 1)
  //   u32 n
  //   repeat n:
  //     u32 kind_len, bytes[kind_len]
  //     u32 name_len, bytes[name_len]
  //     u32 flags
  //     u32 meta_len, bytes[meta_len]
  uint32_t payload_len = 8;
  if (reg) {
    for (uint32_t i = 0; i < n; i++) {
      const zi_cap_v1 *c = reg->caps[i];
      if (!c || !c->kind || !c->name) continue;
      uint32_t kind_len = (uint32_t)strlen(c->kind);
      uint32_t name_len = (uint32_t)strlen(c->name);
      uint32_t meta_len = (c->meta && c->meta_len) ? (uint32_t)c->meta_len : 0;
      payload_len += 4 + kind_len + 4 + name_len + 4 + 4 + meta_len;
    }
  }

  // Allocate on stack if small; otherwise refuse (caller can retry with larger buffer).
  if (payload_len > 65536u) {
    return zi_zcl1_write_error(resp, resp_cap, op, rid, "t_ctl_overflow", "payload too large");
  }
  uint8_t buf[65536];

  zi_zcl1_write_u32(buf + 0, 1);
  zi_zcl1_write_u32(buf + 4, n);
  uint32_t off = 8;
  if (reg) {
    for (uint32_t i = 0; i < n; i++) {
      const zi_cap_v1 *c = reg->caps[i];
      if (!c || !c->kind || !c->name) continue;
      uint32_t kind_len = (uint32_t)strlen(c->kind);
      uint32_t name_len = (uint32_t)strlen(c->name);
      uint32_t meta_len = (c->meta && c->meta_len) ? (uint32_t)c->meta_len : 0;

      zi_zcl1_write_u32(buf + off, kind_len);
      off += 4;
      memcpy(buf + off, c->kind, kind_len);
      off += kind_len;

      zi_zcl1_write_u32(buf + off, name_len);
      off += 4;
      memcpy(buf + off, c->name, name_len);
      off += name_len;

      zi_zcl1_write_u32(buf + off, c->cap_flags);
      off += 4;

      zi_zcl1_write_u32(buf + off, meta_len);
      off += 4;
      if (meta_len && c->meta) {
        memcpy(buf + off, c->meta, meta_len);
        off += meta_len;
      }
    }
  }

  if (off != payload_len) {
    return zi_zcl1_write_error(resp, resp_cap, op, rid, "t_ctl_bad_frame", "size mismatch");
  }
  return zi_zcl1_write_ok(resp, resp_cap, op, rid, buf, payload_len);
}

int32_t zi_ctl(zi_ptr_t req_ptr, zi_size32_t req_len, zi_ptr_t resp_ptr, zi_size32_t resp_cap) {
  const zi_host_v1 *host = zi_runtime25_host();
  if (host && host->ctl) {
    return host->ctl(host->ctx, req_ptr, req_len, resp_ptr, resp_cap);
  }

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_ro || !mem->map_rw) {
    return ZI_E_NOSYS;
  }

  const uint8_t *req = NULL;
  uint8_t *resp = NULL;
  if (!mem->map_ro(mem->ctx, req_ptr, req_len, &req)) return ZI_E_BOUNDS;
  if (!mem->map_rw(mem->ctx, resp_ptr, resp_cap, &resp)) return ZI_E_BOUNDS;

  zi_zcl1_frame fr;
  if (!zi_zcl1_parse(req, req_len, &fr)) {
    return zi_zcl1_write_error(resp, resp_cap, 0, 0, "t_ctl_bad_frame", "parse");
  }

  // Only CAPs list is supported by the core implementation.
  if (fr.op == (uint16_t)ZI_CTL_OP_CAPS_LIST) {
    // If capabilities are not enabled at all, say NO.
    if (!zi_cap_registry()) {
      return ZI_E_NOSYS;
    }
    return ctl_caps_list(resp, resp_cap, fr.op, fr.rid);
  }

  return zi_zcl1_write_error(resp, resp_cap, fr.op, fr.rid, "t_ctl_unknown_op", "unknown operation");
}

int32_t zi_read(zi_handle_t h, zi_ptr_t dst_ptr, zi_size32_t cap) {
  const zi_host_v1 *host = zi_runtime25_host();
  if (host && host->read) return host->read(host->ctx, h, dst_ptr, cap);

  const zi_handle_ops_v1 *ops = NULL;
  void *ctx = NULL;
  if (zi_handle25_lookup(h, &ops, &ctx, NULL) && ops && ops->read) {
    return ops->read(ctx, dst_ptr, cap);
  }
  return ZI_E_NOSYS;
}

int32_t zi_write(zi_handle_t h, zi_ptr_t src_ptr, zi_size32_t len) {
  const zi_host_v1 *host = zi_runtime25_host();
  if (host && host->write) return host->write(host->ctx, h, src_ptr, len);

  const zi_handle_ops_v1 *ops = NULL;
  void *ctx = NULL;
  if (zi_handle25_lookup(h, &ops, &ctx, NULL) && ops && ops->write) {
    return ops->write(ctx, src_ptr, len);
  }
  return ZI_E_NOSYS;
}

int32_t zi_end(zi_handle_t h) {
  const zi_host_v1 *host = zi_runtime25_host();
  if (host && host->end) return host->end(host->ctx, h);

  const zi_handle_ops_v1 *ops = NULL;
  void *ctx = NULL;
  if (!zi_handle25_lookup(h, &ops, &ctx, NULL) || !ops) return ZI_E_NOSYS;

  int32_t r = 0;
  if (ops->end) r = ops->end(ctx);
  (void)zi_handle25_release(h);
  return r;
}

zi_ptr_t zi_alloc(zi_size32_t size) {
  const zi_host_v1 *host = zi_runtime25_host();
  if (host && host->alloc) return host->alloc(host->ctx, size);
  return 0;
}

int32_t zi_free(zi_ptr_t ptr) {
  const zi_host_v1 *host = zi_runtime25_host();
  if (host && host->free) return host->free(host->ctx, ptr);
  (void)ptr;
  return ZI_E_NOSYS;
}

int32_t zi_telemetry(zi_ptr_t topic_ptr, zi_size32_t topic_len, zi_ptr_t msg_ptr, zi_size32_t msg_len) {
  const zi_host_v1 *host = zi_runtime25_host();
  if (host && host->telemetry) return host->telemetry(host->ctx, topic_ptr, topic_len, msg_ptr, msg_len);
  (void)topic_ptr;
  (void)topic_len;
  (void)msg_ptr;
  (void)msg_len;
  return 0;
}

uint32_t zi_handle_hflags(zi_handle_t h) {
  return zi_handle25_hflags(h);
}
