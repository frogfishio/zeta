#include "zi_proc_env25.h"

#include "zi_handles25.h"
#include "zi_runtime25.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint8_t *blob;
  uint32_t len;
  uint32_t pos;
} zi_env_stream;

static void write_u32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static int32_t env_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  zi_env_stream *s = (zi_env_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (cap == 0) return 0;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_rw) return ZI_E_NOSYS;

  if (dst_ptr == 0) return ZI_E_BOUNDS;

  uint32_t remaining = (s->pos < s->len) ? (s->len - s->pos) : 0;
  if (remaining == 0) return 0;

  uint32_t n = cap;
  if (n > remaining) n = remaining;

  uint8_t *dst = NULL;
  if (!mem->map_rw(mem->ctx, dst_ptr, (zi_size32_t)n, &dst) || !dst) return ZI_E_BOUNDS;

  memcpy(dst, s->blob + s->pos, n);
  s->pos += n;
  return (int32_t)n;
}

static int32_t env_write(void *ctx, zi_ptr_t src_ptr, zi_size32_t len) {
  (void)ctx;
  (void)src_ptr;
  (void)len;
  return ZI_E_DENIED;
}

static int32_t env_end(void *ctx) {
  zi_env_stream *s = (zi_env_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  free(s->blob);
  free(s);
  return 0;
}

static const zi_handle_ops_v1 env_ops = {
    .read = env_read,
    .write = env_write,
    .end = env_end,
};

static zi_env_stream *build_env_stream(void) {
  int envc = 0;
  const char *const *envp = NULL;
  zi_runtime25_get_env(&envc, &envp);

  if (envc < 0) envc = 0;

  uint64_t total64 = 8;
  for (int i = 0; i < envc; i++) {
    const char *s = (envp && envp[i]) ? envp[i] : "";
    size_t sl = strlen(s);
    if (sl > 0x7FFFFFFFu) return NULL;
    total64 += 4 + (uint64_t)sl;
    if (total64 > 0x7FFFFFFFu) return NULL;
  }

  uint32_t total = (uint32_t)total64;
  uint8_t *blob = (uint8_t *)malloc(total);
  if (!blob) return NULL;

  write_u32le(blob + 0, 1);
  write_u32le(blob + 4, (uint32_t)envc);

  uint32_t off = 8;
  for (int i = 0; i < envc; i++) {
    const char *s = (envp && envp[i]) ? envp[i] : "";
    uint32_t sl = (uint32_t)strlen(s);
    write_u32le(blob + off, sl);
    off += 4;
    if (sl) {
      memcpy(blob + off, s, sl);
      off += sl;
    }
  }

  if (off != total) {
    free(blob);
    return NULL;
  }

  zi_env_stream *st = (zi_env_stream *)calloc(1, sizeof(*st));
  if (!st) {
    free(blob);
    return NULL;
  }
  st->blob = blob;
  st->len = total;
  st->pos = 0;
  return st;
}

zi_handle_t zi_proc_env25_open(void) {
  zi_env_stream *st = build_env_stream();
  if (!st) return (zi_handle_t)ZI_E_OOM;

  uint32_t hflags = ZI_H_READABLE | ZI_H_ENDABLE;
  zi_handle_t h = zi_handle25_alloc(&env_ops, st, hflags);
  if (h == 0) {
    env_end(st);
    return (zi_handle_t)ZI_E_OOM;
  }
  return h;
}

static const uint8_t cap_meta[] =
    "{\"kind\":\"proc\",\"name\":\"env\",\"open\":{\"params\":\"(none)\"},\"format\":\"u32 version; u32 envc; repeat(envc){u32 len; bytes[len]}\"}";

static const zi_cap_v1 cap_proc_env_v1 = {
    .kind = ZI_CAP_KIND_PROC,
    .name = ZI_CAP_NAME_ENV,
    .version = 1,
    .cap_flags = ZI_CAP_CAN_OPEN,
    .meta = cap_meta,
    .meta_len = (uint32_t)(sizeof(cap_meta) - 1),
};

const zi_cap_v1 *zi_proc_env25_cap(void) { return &cap_proc_env_v1; }

int zi_proc_env25_register(void) { return zi_cap_register(&cap_proc_env_v1); }
