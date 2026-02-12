#include "zi_proc_argv25.h"

#include "zi_handles25.h"
#include "zi_runtime25.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint8_t *blob;
  uint32_t len;
  uint32_t pos;
} zi_argv_stream;

static void write_u32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static int32_t argv_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  zi_argv_stream *s = (zi_argv_stream *)ctx;
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

static int32_t argv_write(void *ctx, zi_ptr_t src_ptr, zi_size32_t len) {
  (void)ctx;
  (void)src_ptr;
  (void)len;
  return ZI_E_DENIED;
}

static int32_t argv_end(void *ctx) {
  zi_argv_stream *s = (zi_argv_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  free(s->blob);
  free(s);
  return 0;
}

static const zi_handle_ops_v1 argv_ops = {
    .read = argv_read,
    .write = argv_write,
    .end = argv_end,
};

static zi_argv_stream *build_argv_stream(void) {
  int argc = 0;
  const char *const *argv = NULL;
  zi_runtime25_get_argv(&argc, &argv);

  if (argc < 0) argc = 0;

  uint64_t total64 = 8;
  for (int i = 0; i < argc; i++) {
    const char *s = (argv && argv[i]) ? argv[i] : "";
    size_t sl = strlen(s);
    if (sl > 0x7FFFFFFFu) return NULL;
    total64 += 4 + (uint64_t)sl;
    if (total64 > 0x7FFFFFFFu) return NULL;
  }

  uint32_t total = (uint32_t)total64;
  uint8_t *blob = (uint8_t *)malloc(total);
  if (!blob) return NULL;

  write_u32le(blob + 0, 1);
  write_u32le(blob + 4, (uint32_t)argc);

  uint32_t off = 8;
  for (int i = 0; i < argc; i++) {
    const char *s = (argv && argv[i]) ? argv[i] : "";
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

  zi_argv_stream *st = (zi_argv_stream *)calloc(1, sizeof(*st));
  if (!st) {
    free(blob);
    return NULL;
  }
  st->blob = blob;
  st->len = total;
  st->pos = 0;
  return st;
}

zi_handle_t zi_proc_argv25_open(void) {
  zi_argv_stream *st = build_argv_stream();
  if (!st) return (zi_handle_t)ZI_E_OOM;

  uint32_t hflags = ZI_H_READABLE | ZI_H_ENDABLE;
  zi_handle_t h = zi_handle25_alloc(&argv_ops, st, hflags);
  if (h == 0) {
    argv_end(st);
    return (zi_handle_t)ZI_E_OOM;
  }
  return h;
}

static const uint8_t cap_meta[] =
    "{\"kind\":\"proc\",\"name\":\"argv\",\"open\":{\"params\":\"(none)\"},\"format\":\"u32 version; u32 argc; repeat(argc){u32 len; bytes[len]}\"}";

static const zi_cap_v1 cap_proc_argv_v1 = {
    .kind = ZI_CAP_KIND_PROC,
    .name = ZI_CAP_NAME_ARGV,
    .version = 1,
    .cap_flags = ZI_CAP_CAN_OPEN,
    .meta = cap_meta,
    .meta_len = (uint32_t)(sizeof(cap_meta) - 1),
};

const zi_cap_v1 *zi_proc_argv25_cap(void) { return &cap_proc_argv_v1; }

int zi_proc_argv25_register(void) { return zi_cap_register(&cap_proc_argv_v1); }
