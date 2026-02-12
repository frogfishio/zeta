#include "zi_hopabi25.h"

#include "zi_runtime25.h"

#include "vendor/hopper/hopper.h"

#include <stdlib.h>
#include <string.h>

// Note: this provides a direct-call ABI for guests.
// It is intentionally independent of CTL and handle I/O.

// ---- minimal built-in catalog (optional convenience; layout_id=1) ----

static const hopper_field_t HOPABI25_FIELDS[] = {
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

static const hopper_layout_t HOPABI25_LAYOUTS[] = {
    {
        .name_ascii = "Example",
        .name_len = 7,
        .record_bytes = 8,
        .layout_id = 1,
        .fields = HOPABI25_FIELDS,
        .field_count = (uint32_t)(sizeof(HOPABI25_FIELDS) / sizeof(HOPABI25_FIELDS[0])),
    },
};

static const hopper_catalog_t HOPABI25_CATALOG = {
    .abi_version = HOPPER_ABI_VERSION,
    .layouts = HOPABI25_LAYOUTS,
    .layout_count = (uint32_t)(sizeof(HOPABI25_LAYOUTS) / sizeof(HOPABI25_LAYOUTS[0])),
};

// ---- instance table ----

#ifndef ZI_HOPABI25_MAX
#define ZI_HOPABI25_MAX 16
#endif

typedef struct {
  int in_use;
  hopper_t *h;
  void *storage;
  void *arena_mem;
  uint32_t arena_bytes;
  void *ref_mem;
  uint32_t ref_count;
  hopper_config_t cfg;
} hop_entry;

static hop_entry g_hops[ZI_HOPABI25_MAX];

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

static hop_entry *hop_lookup(int32_t hop_id) {
  if (hop_id < 0 || hop_id >= (int32_t)ZI_HOPABI25_MAX) return NULL;
  hop_entry *e = &g_hops[hop_id];
  if (!e->in_use) return NULL;
  return e;
}

static const zi_mem_v1 *req_mem_ro(void) {
  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_ro) return NULL;
  return mem;
}

static const zi_mem_v1 *req_mem_rw(void) {
  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_rw) return NULL;
  return mem;
}

static int32_t map_out_i32(zi_ptr_t out_ptr, int32_t v) {
  const zi_mem_v1 *mem = req_mem_rw();
  if (!mem) return ZI_E_NOSYS;
  uint8_t *out = NULL;
  if (!mem->map_rw(mem->ctx, out_ptr, 4u, &out) || !out) return ZI_E_BOUNDS;
  write_i32le(out, v);
  return 0;
}

static int32_t map_out_u32(zi_ptr_t out_ptr, uint32_t v) {
  const zi_mem_v1 *mem = req_mem_rw();
  if (!mem) return ZI_E_NOSYS;
  uint8_t *out = NULL;
  if (!mem->map_rw(mem->ctx, out_ptr, 4u, &out) || !out) return ZI_E_BOUNDS;
  write_u32le(out, v);
  return 0;
}

int32_t zi_hop_open(zi_ptr_t params_ptr, zi_size32_t params_len) {
  uint32_t arena_bytes = 4u * 1024u * 1024u;
  uint32_t ref_count = 65536u;

  if (params_len != 0) {
    if (params_len != 16u) return ZI_E_INVALID;
    const zi_mem_v1 *mem = req_mem_ro();
    if (!mem) return ZI_E_NOSYS;

    const uint8_t *p = NULL;
    if (!mem->map_ro(mem->ctx, params_ptr, params_len, &p) || !p) return ZI_E_BOUNDS;

    uint32_t ver = read_u32le(p + 0);
    uint32_t ab = read_u32le(p + 4);
    uint32_t rc = read_u32le(p + 8);
    uint32_t flags = read_u32le(p + 12);

    if (ver != ZI_HOPABI25_VERSION) return ZI_E_INVALID;
    if (flags != 0) return ZI_E_INVALID;

    arena_bytes = ab;
    ref_count = rc;

    if (arena_bytes == 0 || arena_bytes > (256u * 1024u * 1024u)) return ZI_E_INVALID;
    if (ref_count == 0 || ref_count > 1000000u) return ZI_E_INVALID;
  }

  int32_t hop_id = -1;
  for (int32_t i = 0; i < (int32_t)ZI_HOPABI25_MAX; i++) {
    if (!g_hops[i].in_use) {
      hop_id = i;
      break;
    }
  }
  if (hop_id < 0) return ZI_E_OOM;

  hop_entry *e = &g_hops[hop_id];
  memset(e, 0, sizeof(*e));

  e->arena_bytes = arena_bytes;
  e->ref_count = ref_count;

  e->arena_mem = calloc(1u, (size_t)arena_bytes);
  if (!e->arena_mem) return ZI_E_OOM;

  size_t ref_bytes = hopper_ref_entry_sizeof() * (size_t)ref_count;
  e->ref_mem = calloc(1u, ref_bytes);
  if (!e->ref_mem) {
    free(e->arena_mem);
    memset(e, 0, sizeof(*e));
    return ZI_E_OOM;
  }

  e->storage = calloc(1u, hopper_sizeof());
  if (!e->storage) {
    free(e->ref_mem);
    free(e->arena_mem);
    memset(e, 0, sizeof(*e));
    return ZI_E_OOM;
  }

  hopper_config_t cfg = {
      .abi_version = HOPPER_ABI_VERSION,
      .arena_mem = e->arena_mem,
      .arena_bytes = arena_bytes,
      .ref_mem = e->ref_mem,
      .ref_count = ref_count,
      .catalog = &HOPABI25_CATALOG,
  };
  e->cfg = cfg;

  hopper_t *h = NULL;
  hopper_err_t err = hopper_init(e->storage, &e->cfg, &h);
  if (err != HOPPER_OK || !h) {
    free(e->storage);
    free(e->ref_mem);
    free(e->arena_mem);
    memset(e, 0, sizeof(*e));
    return ZI_E_INTERNAL;
  }

  e->h = h;
  e->in_use = 1;
  return hop_id;
}

int32_t zi_hop_close(int32_t hop_id) {
  hop_entry *e = hop_lookup(hop_id);
  if (!e) return ZI_E_NOENT;

  free(e->storage);
  free(e->ref_mem);
  free(e->arena_mem);
  memset(e, 0, sizeof(*e));
  return 0;
}

int32_t zi_hop_reset(int32_t hop_id, uint32_t wipe_arena) {
  hop_entry *e = hop_lookup(hop_id);
  if (!e) return ZI_E_NOENT;
  hopper_err_t err = hopper_reset(e->h, wipe_arena ? 1 : 0);
  return (int32_t)err;
}

int32_t zi_hop_alloc(int32_t hop_id, uint32_t size, uint32_t align, zi_ptr_t out_ref_ptr) {
  hop_entry *e = hop_lookup(hop_id);
  if (!e) return ZI_E_NOENT;

  hopper_result_ref_t r = hopper_alloc(e->h, size, align);
  if (!r.ok) return (int32_t)r.err;

  int32_t w = map_out_i32(out_ref_ptr, (int32_t)r.ref);
  if (w < 0) return w;
  return 0;
}

int32_t zi_hop_free(int32_t hop_id, int32_t ref) {
  hop_entry *e = hop_lookup(hop_id);
  if (!e) return ZI_E_NOENT;
  hopper_err_t err = hopper_free(e->h, (hopper_ref_t)ref);
  return (int32_t)err;
}

int32_t zi_hop_record(int32_t hop_id, uint32_t layout_id, zi_ptr_t out_ref_ptr) {
  hop_entry *e = hop_lookup(hop_id);
  if (!e) return ZI_E_NOENT;

  hopper_result_ref_t r = hopper_record(e->h, layout_id);
  if (!r.ok) return (int32_t)r.err;

  int32_t w = map_out_i32(out_ref_ptr, (int32_t)r.ref);
  if (w < 0) return w;
  return 0;
}

int32_t zi_hop_field_set_bytes(int32_t hop_id, int32_t ref, uint32_t field_index,
                               zi_ptr_t bytes_ptr, zi_size32_t bytes_len) {
  hop_entry *e = hop_lookup(hop_id);
  if (!e) return ZI_E_NOENT;

  const zi_mem_v1 *mem = req_mem_ro();
  if (!mem) return ZI_E_NOSYS;

  const uint8_t *bytes = NULL;
  if (bytes_len != 0) {
    if (!mem->map_ro(mem->ctx, bytes_ptr, bytes_len, &bytes) || !bytes) return ZI_E_BOUNDS;
  }

  hopper_err_t err = hopper_field_set_bytes(e->h, (hopper_ref_t)ref, field_index,
                                            (hopper_bytes_t){bytes, (uint32_t)bytes_len});
  return (int32_t)err;
}

int32_t zi_hop_field_get_bytes(int32_t hop_id, int32_t ref, uint32_t field_index,
                               zi_ptr_t dst_ptr, zi_size32_t dst_cap,
                               zi_ptr_t out_written_ptr) {
  hop_entry *e = hop_lookup(hop_id);
  if (!e) return ZI_E_NOENT;

  const zi_mem_v1 *mem = req_mem_rw();
  if (!mem) return ZI_E_NOSYS;

  hopper_ref_info_t info;
  if (!hopper_ref_info(e->h, (hopper_ref_t)ref, &info)) {
    return (int32_t)HOPPER_E_BAD_REF;
  }

  // Only supports the built-in layout for now.
  if (info.layout_id != 1u) {
    return (int32_t)HOPPER_E_BAD_LAYOUT;
  }
  if (field_index >= HOPABI25_LAYOUTS[0].field_count) {
    return (int32_t)HOPPER_E_BAD_FIELD;
  }

  uint32_t need = HOPABI25_LAYOUTS[0].fields[field_index].size;
  if (dst_cap < need) return ZI_E_BOUNDS;

  uint8_t *dst = NULL;
  if (!mem->map_rw(mem->ctx, dst_ptr, dst_cap, &dst) || !dst) return ZI_E_BOUNDS;

  hopper_err_t err = hopper_field_get_bytes(e->h, (hopper_ref_t)ref, field_index,
                                            (hopper_bytes_mut_t){dst, need});
  if (err != HOPPER_OK) return (int32_t)err;

  int32_t w = map_out_u32(out_written_ptr, need);
  if (w < 0) return w;
  return 0;
}

int32_t zi_hop_field_set_i32(int32_t hop_id, int32_t ref, uint32_t field_index, int32_t v) {
  hop_entry *e = hop_lookup(hop_id);
  if (!e) return ZI_E_NOENT;
  hopper_err_t err = hopper_field_set_i32(e->h, (hopper_ref_t)ref, field_index, v);
  return (int32_t)err;
}

int32_t zi_hop_field_get_i32(int32_t hop_id, int32_t ref, uint32_t field_index, zi_ptr_t out_v_ptr) {
  hop_entry *e = hop_lookup(hop_id);
  if (!e) return ZI_E_NOENT;

  hopper_result_i32_t r = hopper_field_get_i32(e->h, (hopper_ref_t)ref, field_index);
  if (!r.ok) return (int32_t)r.err;

  int32_t w = map_out_i32(out_v_ptr, r.v);
  if (w < 0) return w;
  return 0;
}
