#include "zi_sysabi25.h"

#include "zi_runtime25.h"
#include "zi_caps.h"
#include "zi_async_default25.h"
#include "zi_event_bus25.h"
#include "zi_file_aio25.h"
#include "zi_net_tcp25.h"
#include "zi_net_http25.h"
#include "zi_proc_argv25.h"
#include "zi_proc_env25.h"
#include "zi_proc_hopper25.h"
#include "zi_sys_info25.h"
#include "zi_sys_loop25.h"

#include <string.h>

static int32_t err_noent(void) { return ZI_E_NOENT; }

int32_t zi_cap_count(void) {
  const zi_cap_registry_v1 *reg = zi_cap_registry();
  if (!reg) return ZI_E_NOSYS;
  if (reg->cap_count > (size_t)0x7FFFFFFF) return ZI_E_INTERNAL;
  return (int32_t)reg->cap_count;
}

int32_t zi_cap_get_size(int32_t index) {
  const zi_cap_registry_v1 *reg = zi_cap_registry();
  if (!reg) return ZI_E_NOSYS;
  if (index < 0 || (size_t)index >= reg->cap_count) return err_noent();
  const zi_cap_v1 *c = reg->caps[(size_t)index];
  if (!c || !c->kind || !c->name) return ZI_E_INTERNAL;
  uint32_t kind_len = (uint32_t)strlen(c->kind);
  uint32_t name_len = (uint32_t)strlen(c->name);
  uint32_t total = 4 + kind_len + 4 + name_len + 4;
  if (total > 0x7FFFFFFF) return ZI_E_INTERNAL;
  return (int32_t)total;
}

int32_t zi_cap_get(int32_t index, zi_ptr_t out_ptr, zi_size32_t out_cap) {
  const zi_cap_registry_v1 *reg = zi_cap_registry();
  if (!reg) return ZI_E_NOSYS;
  if (index < 0 || (size_t)index >= reg->cap_count) return err_noent();

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_rw) return ZI_E_NOSYS;

  const zi_cap_v1 *c = reg->caps[(size_t)index];
  if (!c || !c->kind || !c->name) return ZI_E_INTERNAL;
  uint32_t kind_len = (uint32_t)strlen(c->kind);
  uint32_t name_len = (uint32_t)strlen(c->name);

  uint32_t need = 4 + kind_len + 4 + name_len + 4;
  if (out_cap < need) return ZI_E_BOUNDS;

  uint8_t *out = NULL;
  if (!mem->map_rw(mem->ctx, out_ptr, out_cap, &out) || !out) return ZI_E_BOUNDS;

  // Packed:
  //   H4 kind_len, bytes[kind_len] kind
  //   H4 name_len, bytes[name_len] name
  //   H4 flags
  out[0] = (uint8_t)(kind_len & 0xFF);
  out[1] = (uint8_t)((kind_len >> 8) & 0xFF);
  out[2] = (uint8_t)((kind_len >> 16) & 0xFF);
  out[3] = (uint8_t)((kind_len >> 24) & 0xFF);
  memcpy(out + 4, c->kind, kind_len);

  uint32_t off = 4 + kind_len;
  out[off + 0] = (uint8_t)(name_len & 0xFF);
  out[off + 1] = (uint8_t)((name_len >> 8) & 0xFF);
  out[off + 2] = (uint8_t)((name_len >> 16) & 0xFF);
  out[off + 3] = (uint8_t)((name_len >> 24) & 0xFF);
  memcpy(out + off + 4, c->name, name_len);
  off += 4 + name_len;

  uint32_t flags = c->cap_flags;
  out[off + 0] = (uint8_t)(flags & 0xFF);
  out[off + 1] = (uint8_t)((flags >> 8) & 0xFF);
  out[off + 2] = (uint8_t)((flags >> 16) & 0xFF);
  out[off + 3] = (uint8_t)((flags >> 24) & 0xFF);

  return (int32_t)need;
}

zi_handle_t zi_cap_open(zi_ptr_t req_ptr) {
  // Packed little-endian open request:
  //   u64 kind_ptr
  //   u32 kind_len
  //   u64 name_ptr
  //   u32 name_len
  //   u32 mode (reserved; must be 0 for now)
  //   u64 params_ptr
  //   u32 params_len
  const zi_cap_registry_v1 *reg = zi_cap_registry();
  if (!reg) return (zi_handle_t)ZI_E_NOSYS;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_ro) return (zi_handle_t)ZI_E_NOSYS;

  const uint8_t *req = NULL;
  if (!mem->map_ro(mem->ctx, req_ptr, 40u, &req) || !req) return (zi_handle_t)ZI_E_BOUNDS;

  // local little-endian readers
  uint32_t kind_len = (uint32_t)req[8] | ((uint32_t)req[9] << 8) | ((uint32_t)req[10] << 16) | ((uint32_t)req[11] << 24);
  uint32_t name_len = (uint32_t)req[20] | ((uint32_t)req[21] << 8) | ((uint32_t)req[22] << 16) | ((uint32_t)req[23] << 24);
  uint32_t mode = (uint32_t)req[24] | ((uint32_t)req[25] << 8) | ((uint32_t)req[26] << 16) | ((uint32_t)req[27] << 24);
  zi_ptr_t kind_ptr = (zi_ptr_t)((uint64_t)req[0] | ((uint64_t)req[1] << 8) | ((uint64_t)req[2] << 16) | ((uint64_t)req[3] << 24) |
                                ((uint64_t)req[4] << 32) | ((uint64_t)req[5] << 40) | ((uint64_t)req[6] << 48) | ((uint64_t)req[7] << 56));
  zi_ptr_t name_ptr = (zi_ptr_t)((uint64_t)req[12] | ((uint64_t)req[13] << 8) | ((uint64_t)req[14] << 16) | ((uint64_t)req[15] << 24) |
                                ((uint64_t)req[16] << 32) | ((uint64_t)req[17] << 40) | ((uint64_t)req[18] << 48) | ((uint64_t)req[19] << 56));
  zi_ptr_t params_ptr = (zi_ptr_t)((uint64_t)req[28] | ((uint64_t)req[29] << 8) | ((uint64_t)req[30] << 16) | ((uint64_t)req[31] << 24) |
                                  ((uint64_t)req[32] << 32) | ((uint64_t)req[33] << 40) | ((uint64_t)req[34] << 48) | ((uint64_t)req[35] << 56));
  uint32_t params_len = (uint32_t)req[36] | ((uint32_t)req[37] << 8) | ((uint32_t)req[38] << 16) | ((uint32_t)req[39] << 24);

  if (mode != 0) return (zi_handle_t)ZI_E_INVALID;
  if (kind_len == 0 || name_len == 0) return (zi_handle_t)ZI_E_INVALID;

  const uint8_t *kind = NULL;
  const uint8_t *name = NULL;
  if (!mem->map_ro(mem->ctx, kind_ptr, (zi_size32_t)kind_len, &kind) || !kind) return (zi_handle_t)ZI_E_BOUNDS;
  if (!mem->map_ro(mem->ctx, name_ptr, (zi_size32_t)name_len, &name) || !name) return (zi_handle_t)ZI_E_BOUNDS;

  // Find the cap in the registry.
  const zi_cap_v1 *found = NULL;
  for (size_t i = 0; i < reg->cap_count; i++) {
    const zi_cap_v1 *c = reg->caps[i];
    if (!c || !c->kind || !c->name) continue;
    if (strlen(c->kind) != (size_t)kind_len) continue;
    if (strlen(c->name) != (size_t)name_len) continue;
    if (memcmp(c->kind, kind, kind_len) != 0) continue;
    if (memcmp(c->name, name, name_len) != 0) continue;
    found = c;
    break;
  }
  if (!found) return (zi_handle_t)ZI_E_NOENT;
  if ((found->cap_flags & ZI_CAP_CAN_OPEN) == 0) return (zi_handle_t)ZI_E_DENIED;

  // file/aio v1 (no params)
  if (strcmp(found->kind, ZI_CAP_KIND_FILE) == 0 && strcmp(found->name, ZI_CAP_NAME_AIO) == 0 && found->version == 1) {
    return zi_file_aio25_open_from_params(params_ptr, (zi_size32_t)params_len);
  }

  // async/default v1 (no params)
  if (strcmp(found->kind, ZI_CAP_KIND_ASYNC) == 0 && strcmp(found->name, ZI_CAP_NAME_DEFAULT) == 0 && found->version == 1) {
    return zi_async_default25_open_from_params(params_ptr, (zi_size32_t)params_len);
  }

  // event/bus v1 (no params)
  if (strcmp(found->kind, ZI_CAP_KIND_EVENT) == 0 && strcmp(found->name, ZI_CAP_NAME_BUS) == 0 && found->version == 1) {
    return zi_event_bus25_open_from_params(params_ptr, (zi_size32_t)params_len);
  }

  // proc/argv v1 (no params)
  if (strcmp(found->kind, ZI_CAP_KIND_PROC) == 0 && strcmp(found->name, ZI_CAP_NAME_ARGV) == 0 && found->version == 1) {
    if (params_len != 0) return (zi_handle_t)ZI_E_INVALID;
    return zi_proc_argv25_open();
  }

  // proc/env v1 (no params)
  if (strcmp(found->kind, ZI_CAP_KIND_PROC) == 0 && strcmp(found->name, ZI_CAP_NAME_ENV) == 0 && found->version == 1) {
    if (params_len != 0) return (zi_handle_t)ZI_E_INVALID;
    return zi_proc_env25_open();
  }

  // proc/hopper v1
  if (strcmp(found->kind, ZI_CAP_KIND_PROC) == 0 && strcmp(found->name, ZI_CAP_NAME_HOPPER) == 0 && found->version == 1) {
    return zi_proc_hopper25_open_from_params(params_ptr, (zi_size32_t)params_len);
  }

  // net/tcp v1
  if (strcmp(found->kind, ZI_CAP_KIND_NET) == 0 && strcmp(found->name, ZI_CAP_NAME_TCP) == 0 && found->version == 1) {
    return zi_net_tcp25_open_from_params(params_ptr, (zi_size32_t)params_len);
  }

  // net/http v1
  if (strcmp(found->kind, ZI_CAP_KIND_NET) == 0 && strcmp(found->name, ZI_CAP_NAME_HTTP) == 0 && found->version == 1) {
    return zi_net_http25_open_from_params(params_ptr, (zi_size32_t)params_len);
  }

  // sys/info v1 (no params)
  if (strcmp(found->kind, ZI_CAP_KIND_SYS) == 0 && strcmp(found->name, ZI_CAP_NAME_INFO) == 0 && found->version == 1) {
    return zi_sys_info25_open_from_params(params_ptr, (zi_size32_t)params_len);
  }

  // sys/loop v1 (no params)
  if (strcmp(found->kind, ZI_CAP_KIND_SYS) == 0 && strcmp(found->name, ZI_CAP_NAME_LOOP) == 0 && found->version == 1) {
    return zi_sys_loop25_open_from_params(params_ptr, (zi_size32_t)params_len);
  }

  return (zi_handle_t)ZI_E_DENIED;
}
