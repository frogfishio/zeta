#include "semrt.h"

#include "zcl1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  SEMRT_ZI_E_INVALID = -1,
  SEMRT_ZI_E_BOUNDS = -2,
  SEMRT_ZI_E_NOSYS = -7,
  SEMRT_ZI_E_OOM = -8,
  SEMRT_ZI_E_INTERNAL = -10,
};

typedef struct sem_stdio_stream {
  FILE* f;
} sem_stdio_stream_t;

static int32_t stdio_read(void* ctx, sem_guest_mem_t* mem, zi_ptr_t dst_ptr, zi_size32_t cap) {
  sem_stdio_stream_t* s = (sem_stdio_stream_t*)ctx;
  if (!s || !s->f) return SEMRT_ZI_E_INTERNAL;
  if (cap == 0) return 0;

  uint8_t* dst = NULL;
  if (!sem_guest_mem_map_rw(mem, dst_ptr, cap, &dst) || !dst) return SEMRT_ZI_E_BOUNDS;
  const size_t n = fread(dst, 1, (size_t)cap, s->f);
  if (n == 0 && ferror(s->f)) return SEMRT_ZI_E_INTERNAL;
  return (int32_t)n;
}

static int32_t stdio_write(void* ctx, sem_guest_mem_t* mem, zi_ptr_t src_ptr, zi_size32_t len) {
  sem_stdio_stream_t* s = (sem_stdio_stream_t*)ctx;
  if (!s || !s->f) return SEMRT_ZI_E_INTERNAL;
  if (len == 0) return 0;

  const uint8_t* src = NULL;
  if (!sem_guest_mem_map_ro(mem, src_ptr, len, &src) || !src) return SEMRT_ZI_E_BOUNDS;
  const size_t n = fwrite(src, 1, (size_t)len, s->f);
  if (n < (size_t)len) return SEMRT_ZI_E_INTERNAL;
  (void)fflush(s->f);
  return (int32_t)n;
}

static int32_t stdio_end(void* ctx, sem_guest_mem_t* mem) {
  (void)mem;
  sem_stdio_stream_t* s = (sem_stdio_stream_t*)ctx;
  if (!s) return 0;
  // Do not close stdin/out/err; just flush.
  if (s->f) (void)fflush(s->f);
  return 0;
}

static const sem_handle_ops_t stdio_ops = {
    .read = stdio_read,
    .write = stdio_write,
    .end = stdio_end,
};

bool semrt_init(semrt_t* rt, semrt_cfg_t cfg) {
  if (!rt) return false;
  memset(rt, 0, sizeof(*rt));

  if (!sem_guest_mem_init(&rt->mem, cfg.guest_mem_cap ? cfg.guest_mem_cap : (16u * 1024u * 1024u),
                          cfg.guest_mem_base ? cfg.guest_mem_base : 0x10000ull)) {
    return false;
  }

  if (!sem_handles_init(&rt->handles, 4096)) {
    sem_guest_mem_dispose(&rt->mem);
    return false;
  }

  rt->abi_version = cfg.abi_version ? cfg.abi_version : 0x00020005u;

  // zi_ctl host config (CAPS_LIST currently).
  sem_host_init(&rt->ctl_host, (sem_host_cfg_t){.caps = cfg.caps, .cap_count = cfg.cap_count});

  // Install stdin/out/err.
  sem_stdio_stream_t* in = (sem_stdio_stream_t*)calloc(1, sizeof(*in));
  sem_stdio_stream_t* out = (sem_stdio_stream_t*)calloc(1, sizeof(*out));
  sem_stdio_stream_t* err = (sem_stdio_stream_t*)calloc(1, sizeof(*err));
  if (!in || !out || !err) {
    free(in);
    free(out);
    free(err);
    sem_handles_dispose(&rt->handles);
    sem_guest_mem_dispose(&rt->mem);
    return false;
  }
  in->f = stdin;
  out->f = stdout;
  err->f = stderr;

  (void)sem_handle_install(&rt->handles, 0, (sem_handle_entry_t){.ops = &stdio_ops, .ctx = in, .hflags = ZI_H_READABLE | ZI_H_ENDABLE});
  (void)sem_handle_install(&rt->handles, 1, (sem_handle_entry_t){.ops = &stdio_ops, .ctx = out, .hflags = ZI_H_WRITABLE | ZI_H_ENDABLE});
  (void)sem_handle_install(&rt->handles, 2, (sem_handle_entry_t){.ops = &stdio_ops, .ctx = err, .hflags = ZI_H_WRITABLE | ZI_H_ENDABLE});

  return true;
}

void semrt_dispose(semrt_t* rt) {
  if (!rt) return;

  // Free the stdio ctx objects (not the FILE*).
  sem_handle_entry_t e;
  if (sem_handle_lookup(&rt->handles, 0, &e)) free(e.ctx);
  if (sem_handle_lookup(&rt->handles, 1, &e)) free(e.ctx);
  if (sem_handle_lookup(&rt->handles, 2, &e)) free(e.ctx);

  sem_handles_dispose(&rt->handles);
  sem_guest_mem_dispose(&rt->mem);
  memset(rt, 0, sizeof(*rt));
}

uint32_t semrt_zi_abi_version(const semrt_t* rt) {
  return rt ? rt->abi_version : 0x00020005u;
}

zi_ptr_t semrt_zi_alloc(semrt_t* rt, zi_size32_t size) {
  if (!rt) return 0;
  return sem_guest_alloc(&rt->mem, size, 16);
}

int32_t semrt_zi_free(semrt_t* rt, zi_ptr_t ptr) {
  if (!rt) return SEMRT_ZI_E_INTERNAL;
  return sem_guest_free(&rt->mem, ptr);
}

int32_t semrt_zi_read(semrt_t* rt, zi_handle_t h, zi_ptr_t dst_ptr, zi_size32_t cap) {
  if (!rt) return SEMRT_ZI_E_INTERNAL;
  sem_handle_entry_t e;
  if (!sem_handle_lookup(&rt->handles, h, &e) || !e.ops || !e.ops->read) return SEMRT_ZI_E_NOSYS;
  if ((e.hflags & ZI_H_READABLE) == 0) return SEMRT_ZI_E_NOSYS;
  return e.ops->read(e.ctx, &rt->mem, dst_ptr, cap);
}

int32_t semrt_zi_write(semrt_t* rt, zi_handle_t h, zi_ptr_t src_ptr, zi_size32_t len) {
  if (!rt) return SEMRT_ZI_E_INTERNAL;
  sem_handle_entry_t e;
  if (!sem_handle_lookup(&rt->handles, h, &e) || !e.ops || !e.ops->write) return SEMRT_ZI_E_NOSYS;
  if ((e.hflags & ZI_H_WRITABLE) == 0) return SEMRT_ZI_E_NOSYS;
  return e.ops->write(e.ctx, &rt->mem, src_ptr, len);
}

int32_t semrt_zi_end(semrt_t* rt, zi_handle_t h) {
  if (!rt) return SEMRT_ZI_E_INTERNAL;
  sem_handle_entry_t e;
  if (!sem_handle_lookup(&rt->handles, h, &e) || !e.ops) return SEMRT_ZI_E_NOSYS;
  int32_t r = 0;
  if (e.ops->end) r = e.ops->end(e.ctx, &rt->mem);
  if (h >= 3) (void)sem_handle_release(&rt->handles, h);
  return r;
}

int32_t semrt_zi_telemetry(semrt_t* rt, zi_ptr_t topic_ptr, zi_size32_t topic_len, zi_ptr_t msg_ptr, zi_size32_t msg_len) {
  if (!rt) return SEMRT_ZI_E_INTERNAL;
  const uint8_t* topic = NULL;
  const uint8_t* msg = NULL;
  if (topic_len && (!sem_guest_mem_map_ro(&rt->mem, topic_ptr, topic_len, &topic) || !topic)) return SEMRT_ZI_E_BOUNDS;
  if (msg_len && (!sem_guest_mem_map_ro(&rt->mem, msg_ptr, msg_len, &msg) || !msg)) return SEMRT_ZI_E_BOUNDS;

  fprintf(stderr, "telemetry[%.*s]: %.*s\n", (int)topic_len, (const char*)topic, (int)msg_len, (const char*)msg);
  return 0;
}

// Hosted zi_ctl: read request bytes from guest memory, run host-pointer handler, write response bytes to guest memory.
int32_t semrt_zi_ctl(semrt_t* rt, zi_ptr_t req_ptr, zi_size32_t req_len, zi_ptr_t resp_ptr, zi_size32_t resp_cap) {
  if (!rt) return SEMRT_ZI_E_INTERNAL;
  if (req_len < ZCL1_HDR_SIZE) return SEMRT_ZI_E_INVALID;

  const uint8_t* req = NULL;
  if (!sem_guest_mem_map_ro(&rt->mem, req_ptr, req_len, &req) || !req) return SEMRT_ZI_E_BOUNDS;

  uint8_t* resp = NULL;
  if (!sem_guest_mem_map_rw(&rt->mem, resp_ptr, resp_cap, &resp) || !resp) return SEMRT_ZI_E_BOUNDS;

  // Route to the existing host-pointer handler.
  const int32_t n = sem_zi_ctl(&rt->ctl_host, req, req_len, resp, resp_cap);
  return n;
}

