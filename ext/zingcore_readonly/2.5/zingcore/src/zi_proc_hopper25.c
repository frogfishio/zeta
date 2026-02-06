#include "zi_proc_hopper25.h"

#include "zi_handles25.h"
#include "zi_runtime25.h"
#include "zi_zcl1.h"

#include "vendor/hopper/hopper.h"

#include <stdlib.h>
#include <string.h>

// ---- built-in minimal catalog (layout_id=1) ----

static const hopper_field_t HOPPER25_FIELDS[] = {
    {
        .name_ascii = "raw",
        .name_len = 3,
        .offset = 0,
        .size = 4,
        .kind = HOPPER_FIELD_BYTES,
        .pad_byte = ' ',
        .pic = {0},
        .redefines_index = -1,
    },
    {
        .name_ascii = "num",
        .name_len = 3,
        .offset = 4,
        .size = 3,
        .kind = HOPPER_FIELD_NUM_I32,
        .pad_byte = 0,
        .pic = {
            .digits = 3,
            .scale = 0,
            .is_signed = 0,
            .usage = HOPPER_USAGE_DISPLAY,
            .mask_ascii = NULL,
            .mask_len = 0,
        },
        .redefines_index = -1,
    },
};

static const hopper_layout_t HOPPER25_LAYOUTS[] = {
    {
        .name_ascii = "Example",
        .name_len = 7,
        .record_bytes = 8,
        .layout_id = 1,
        .fields = HOPPER25_FIELDS,
        .field_count = (uint32_t)(sizeof(HOPPER25_FIELDS) / sizeof(HOPPER25_FIELDS[0])),
    },
};

static const hopper_catalog_t HOPPER25_CATALOG = {
    .abi_version = HOPPER_ABI_VERSION,
    .layouts = HOPPER25_LAYOUTS,
    .layout_count = (uint32_t)(sizeof(HOPPER25_LAYOUTS) / sizeof(HOPPER25_LAYOUTS[0])),
};

static const zi_cap_v1 CAP = {
    .kind = ZI_CAP_KIND_PROC,
    .name = ZI_CAP_NAME_HOPPER,
    .version = 1,
    .cap_flags = ZI_CAP_CAN_OPEN,
    .meta = NULL,
    .meta_len = 0,
};

const zi_cap_v1 *zi_proc_hopper25_cap(void) {
  return &CAP;
}

int zi_proc_hopper25_register(void) {
  return zi_cap_register(&CAP);
}

typedef struct zi_hopper_handle_ctx {
  hopper_t *h;
  void *arena_mem;
  uint32_t arena_bytes;

  void *ref_mem;
  uint32_t ref_count;

  void *storage;
  hopper_config_t cfg;

  uint8_t inbuf[65536];
  uint32_t in_len;

  uint8_t outbuf[65536];
  uint32_t out_len;
  uint32_t out_off;

  int closed;
} zi_hopper_handle_ctx;

static void write_u32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void write_i32le(uint8_t *p, int32_t v) {
  write_u32le(p, (uint32_t)v);
}

static uint32_t read_u32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t read_i32le(const uint8_t *p) {
  return (int32_t)read_u32le(p);
}

static int build_ok_u32_u32_u32_u32(zi_hopper_handle_ctx *c, uint16_t op, uint32_t rid,
                                   uint32_t a, uint32_t b, uint32_t d, uint32_t e) {
  uint8_t payload[16];
  write_u32le(payload + 0, a);
  write_u32le(payload + 4, b);
  write_u32le(payload + 8, d);
  write_u32le(payload + 12, e);
  return zi_zcl1_write_ok(c->outbuf, (uint32_t)sizeof(c->outbuf), op, rid, payload, (uint32_t)sizeof(payload));
}

static int build_ok_err_only(zi_hopper_handle_ctx *c, uint16_t op, uint32_t rid, hopper_err_t err) {
  uint8_t payload[4];
  write_u32le(payload + 0, (uint32_t)err);
  return zi_zcl1_write_ok(c->outbuf, (uint32_t)sizeof(c->outbuf), op, rid, payload, (uint32_t)sizeof(payload));
}

static int build_ok_err_i32(zi_hopper_handle_ctx *c, uint16_t op, uint32_t rid, hopper_err_t err, int32_t v) {
  uint8_t payload[8];
  write_u32le(payload + 0, (uint32_t)err);
  write_i32le(payload + 4, v);
  return zi_zcl1_write_ok(c->outbuf, (uint32_t)sizeof(c->outbuf), op, rid, payload, (uint32_t)sizeof(payload));
}

static int build_ok_err_bytes(zi_hopper_handle_ctx *c, uint16_t op, uint32_t rid, hopper_err_t err, const uint8_t *b,
                             uint32_t blen) {
  if (blen > 60000u) {
    return zi_zcl1_write_error(c->outbuf, (uint32_t)sizeof(c->outbuf), op, rid, "t_hopper_oversize", "payload too large");
  }
  uint8_t hdr[8];
  write_u32le(hdr + 0, (uint32_t)err);
  write_u32le(hdr + 4, blen);

  uint32_t payload_len = 8 + blen;
  uint8_t tmp[65536];
  memcpy(tmp, hdr, 8);
  if (blen && b) memcpy(tmp + 8, b, blen);
  return zi_zcl1_write_ok(c->outbuf, (uint32_t)sizeof(c->outbuf), op, rid, tmp, payload_len);
}

static int dispatch_request(zi_hopper_handle_ctx *c, const zi_zcl1_frame *fr) {
  if (!c || !fr) return 0;

  switch (fr->op) {
    case ZI_HOPPER_OP_INFO: {
      // payload:
      //   (empty)
      // response payload:
      //   u32 hopper_abi_version
      //   u32 default_layout_id
      //   u32 arena_bytes
      //   u32 ref_count
      return build_ok_u32_u32_u32_u32(c, fr->op, fr->rid, HOPPER_ABI_VERSION, 1u, c->arena_bytes, c->ref_count);
    }

    case ZI_HOPPER_OP_RESET: {
      if (fr->payload_len != 4) {
        return zi_zcl1_write_error(c->outbuf, (uint32_t)sizeof(c->outbuf), fr->op, fr->rid, "t_hopper_bad_req", "RESET payload");
      }
      uint32_t wipe = read_u32le(fr->payload);
      hopper_err_t err = hopper_reset(c->h, wipe ? 1 : 0);
      return build_ok_err_only(c, fr->op, fr->rid, err);
    }

    case ZI_HOPPER_OP_RECORD: {
      if (fr->payload_len != 4) {
        return zi_zcl1_write_error(c->outbuf, (uint32_t)sizeof(c->outbuf), fr->op, fr->rid, "t_hopper_bad_req", "RECORD payload");
      }
      uint32_t layout_id = read_u32le(fr->payload);
      hopper_result_ref_t r = hopper_record(c->h, layout_id);
      if (!r.ok) {
        return build_ok_err_i32(c, fr->op, fr->rid, r.err, -1);
      }
      return build_ok_err_i32(c, fr->op, fr->rid, HOPPER_OK, (int32_t)r.ref);
    }

    case ZI_HOPPER_OP_FIELD_SET_BYTES: {
      if (fr->payload_len < 12) {
        return zi_zcl1_write_error(c->outbuf, (uint32_t)sizeof(c->outbuf), fr->op, fr->rid, "t_hopper_bad_req", "SET_BYTES header");
      }
      int32_t ref = read_i32le(fr->payload + 0);
      uint32_t field_index = read_u32le(fr->payload + 4);
      uint32_t len = read_u32le(fr->payload + 8);
      if (12u + len != fr->payload_len) {
        return zi_zcl1_write_error(c->outbuf, (uint32_t)sizeof(c->outbuf), fr->op, fr->rid, "t_hopper_bad_req", "SET_BYTES length");
      }
      const uint8_t *bytes = fr->payload + 12;
      hopper_err_t err = hopper_field_set_bytes(c->h, (hopper_ref_t)ref, field_index, (hopper_bytes_t){bytes, len});
      return build_ok_err_only(c, fr->op, fr->rid, err);
    }

    case ZI_HOPPER_OP_FIELD_GET_BYTES: {
      if (fr->payload_len != 8) {
        return zi_zcl1_write_error(c->outbuf, (uint32_t)sizeof(c->outbuf), fr->op, fr->rid, "t_hopper_bad_req", "GET_BYTES payload");
      }
      int32_t ref = read_i32le(fr->payload + 0);
      uint32_t field_index = read_u32le(fr->payload + 4);

      // Determine field size from catalog (built-in).
      hopper_ref_info_t info;
      if (!hopper_ref_info(c->h, (hopper_ref_t)ref, &info)) {
        return build_ok_err_bytes(c, fr->op, fr->rid, HOPPER_E_BAD_REF, NULL, 0);
      }
      if (info.layout_id != 1u) {
        return build_ok_err_bytes(c, fr->op, fr->rid, HOPPER_E_BAD_LAYOUT, NULL, 0);
      }
      if (field_index >= HOPPER25_LAYOUTS[0].field_count) {
        return build_ok_err_bytes(c, fr->op, fr->rid, HOPPER_E_BAD_FIELD, NULL, 0);
      }
      uint32_t need = HOPPER25_LAYOUTS[0].fields[field_index].size;
      uint8_t tmp[1024];
      if (need > sizeof(tmp)) {
        return build_ok_err_bytes(c, fr->op, fr->rid, HOPPER_E_DST_TOO_SMALL, NULL, 0);
      }

      hopper_err_t err = hopper_field_get_bytes(c->h, (hopper_ref_t)ref, field_index, (hopper_bytes_mut_t){tmp, need});
      if (err != HOPPER_OK) {
        return build_ok_err_bytes(c, fr->op, fr->rid, err, NULL, 0);
      }
      return build_ok_err_bytes(c, fr->op, fr->rid, HOPPER_OK, tmp, need);
    }

    case ZI_HOPPER_OP_FIELD_SET_I32: {
      if (fr->payload_len != 12) {
        return zi_zcl1_write_error(c->outbuf, (uint32_t)sizeof(c->outbuf), fr->op, fr->rid, "t_hopper_bad_req", "SET_I32 payload");
      }
      int32_t ref = read_i32le(fr->payload + 0);
      uint32_t field_index = read_u32le(fr->payload + 4);
      int32_t v = read_i32le(fr->payload + 8);
      hopper_err_t err = hopper_field_set_i32(c->h, (hopper_ref_t)ref, field_index, v);
      return build_ok_err_only(c, fr->op, fr->rid, err);
    }

    case ZI_HOPPER_OP_FIELD_GET_I32: {
      if (fr->payload_len != 8) {
        return zi_zcl1_write_error(c->outbuf, (uint32_t)sizeof(c->outbuf), fr->op, fr->rid, "t_hopper_bad_req", "GET_I32 payload");
      }
      int32_t ref = read_i32le(fr->payload + 0);
      uint32_t field_index = read_u32le(fr->payload + 4);
      hopper_result_i32_t r = hopper_field_get_i32(c->h, (hopper_ref_t)ref, field_index);
      if (!r.ok) {
        return build_ok_err_i32(c, fr->op, fr->rid, r.err, 0);
      }
      return build_ok_err_i32(c, fr->op, fr->rid, HOPPER_OK, r.v);
    }

    default:
      return zi_zcl1_write_error(c->outbuf, (uint32_t)sizeof(c->outbuf), fr->op, fr->rid, "t_hopper_unknown_op", "unknown op");
  }
}

static int32_t hopper_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  zi_hopper_handle_ctx *c = (zi_hopper_handle_ctx *)ctx;
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

static int32_t hopper_write(void *ctx, zi_ptr_t src_ptr, zi_size32_t len) {
  zi_hopper_handle_ctx *c = (zi_hopper_handle_ctx *)ctx;
  if (!c) return ZI_E_INTERNAL;
  if (c->closed) return ZI_E_CLOSED;
  if (len == 0) return 0;

  if (c->out_len != 0) {
    // One outstanding response at a time.
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

  // Quick sanity check for ZCL1 framing.
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
    // Partial frame; wait for more.
    return (int32_t)len;
  }
  if (frame_len != c->in_len) {
    // For now, require exactly one frame per request.
    c->in_len = 0;
    return ZI_E_INVALID;
  }

  zi_zcl1_frame fr;
  if (!zi_zcl1_parse(c->inbuf, c->in_len, &fr)) {
    c->in_len = 0;
    return ZI_E_INVALID;
  }

  int outn = dispatch_request(c, &fr);
  c->in_len = 0;

  if (outn < 0) {
    return ZI_E_INTERNAL;
  }
  c->out_len = (uint32_t)outn;
  c->out_off = 0;
  return (int32_t)len;
}

static int32_t hopper_end(void *ctx) {
  zi_hopper_handle_ctx *c = (zi_hopper_handle_ctx *)ctx;
  if (!c) return ZI_E_INTERNAL;
  c->closed = 1;
  free(c->arena_mem);
  free(c->ref_mem);
  free(c->storage);
  memset(c, 0, sizeof(*c));
  free(c);
  return 0;
}

static const zi_handle_ops_v1 HOPPER_OPS = {
    .read = hopper_read,
    .write = hopper_write,
    .end = hopper_end,
};

zi_handle_t zi_proc_hopper25_open_from_params(zi_ptr_t params_ptr, zi_size32_t params_len) {
  uint32_t arena_bytes = 64u * 1024u;
  uint32_t ref_count = 1024u;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_ro) return (zi_handle_t)ZI_E_NOSYS;

  if (params_len != 0) {
    if (params_len != 12) return (zi_handle_t)ZI_E_INVALID;
    const uint8_t *p = NULL;
    if (!mem->map_ro(mem->ctx, params_ptr, params_len, &p) || !p) return (zi_handle_t)ZI_E_BOUNDS;

    uint32_t ver = read_u32le(p + 0);
    if (ver != 1) return (zi_handle_t)ZI_E_INVALID;
    arena_bytes = read_u32le(p + 4);
    ref_count = read_u32le(p + 8);

    if (arena_bytes == 0 || arena_bytes > (16u * 1024u * 1024u)) return (zi_handle_t)ZI_E_INVALID;
    if (ref_count == 0 || ref_count > 65536u) return (zi_handle_t)ZI_E_INVALID;
  }

  zi_hopper_handle_ctx *c = (zi_hopper_handle_ctx *)calloc(1u, sizeof(*c));
  if (!c) return (zi_handle_t)ZI_E_OOM;

  c->arena_bytes = arena_bytes;
  c->ref_count = ref_count;

  c->arena_mem = calloc(1u, arena_bytes);
  if (!c->arena_mem) {
    free(c);
    return (zi_handle_t)ZI_E_OOM;
  }

  size_t ref_bytes = hopper_ref_entry_sizeof() * (size_t)ref_count;
  c->ref_mem = calloc(1u, ref_bytes);
  if (!c->ref_mem) {
    free(c->arena_mem);
    free(c);
    return (zi_handle_t)ZI_E_OOM;
  }

  size_t storage_sz = hopper_sizeof();
  c->storage = calloc(1u, storage_sz);
  if (!c->storage) {
    free(c->ref_mem);
    free(c->arena_mem);
    free(c);
    return (zi_handle_t)ZI_E_OOM;
  }

  hopper_config_t cfg = {
      .abi_version = HOPPER_ABI_VERSION,
      .arena_mem = c->arena_mem,
      .arena_bytes = arena_bytes,
      .ref_mem = c->ref_mem,
      .ref_count = ref_count,
      .catalog = &HOPPER25_CATALOG,
  };
  c->cfg = cfg;

  hopper_t *h = NULL;
  hopper_err_t err = hopper_init(c->storage, &c->cfg, &h);
  if (err != HOPPER_OK || !h) {
    (void)err;
    free(c->storage);
    free(c->ref_mem);
    free(c->arena_mem);
    free(c);
    return (zi_handle_t)ZI_E_INTERNAL;
  }
  c->h = h;

  if (!zi_handles25_init()) {
    hopper_end(c);
    return (zi_handle_t)ZI_E_INTERNAL;
  }

  zi_handle_t handle = zi_handle25_alloc(&HOPPER_OPS, c, ZI_H_READABLE | ZI_H_WRITABLE | ZI_H_ENDABLE);
  if (handle < 3) {
    hopper_end(c);
    return (zi_handle_t)ZI_E_INTERNAL;
  }

  return handle;
}
