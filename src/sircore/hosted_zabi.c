#include "hosted_zabi.h"

#include "hosted_file_fs.h"
#include "zcl1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  ZI_E_INVALID = -1,
  ZI_E_BOUNDS = -2,
  ZI_E_NOENT = -3,
  ZI_E_DENIED = -4,
  ZI_E_CLOSED = -5,
  ZI_E_AGAIN = -6,
  ZI_E_NOSYS = -7,
  ZI_E_OOM = -8,
  ZI_E_IO = -9,
  ZI_E_INTERNAL = -10,
};

static uint32_t u32_len(const char* s) {
  if (!s) return 0;
  const size_t n = strlen(s);
  return (n > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)n;
}

static uint32_t cap_count(const sir_hosted_zabi_t* rt) {
  return rt ? rt->ctl_host.cfg.cap_count : 0;
}

static const sem_cap_t* cap_at(const sir_hosted_zabi_t* rt, uint32_t i) {
  if (!rt) return NULL;
  if (i >= rt->ctl_host.cfg.cap_count) return NULL;
  return &rt->ctl_host.cfg.caps[i];
}

int32_t sir_zi_cap_count(sir_hosted_zabi_t* rt) {
  (void)rt;
  return (int32_t)cap_count(rt);
}

int32_t sir_zi_cap_get_size(sir_hosted_zabi_t* rt, int32_t index) {
  if (!rt) return ZI_E_INTERNAL;
  if (index < 0) return ZI_E_NOENT;
  const sem_cap_t* c = cap_at(rt, (uint32_t)index);
  if (!c || !c->kind || !c->name) return ZI_E_NOENT;
  const uint32_t kind_len = u32_len(c->kind);
  const uint32_t name_len = u32_len(c->name);
  const uint64_t need = 4ull + kind_len + 4ull + name_len + 4ull;
  if (need > 0x7FFFFFFFull) return ZI_E_INTERNAL;
  return (int32_t)need;
}

int32_t sir_zi_cap_get(sir_hosted_zabi_t* rt, int32_t index, zi_ptr_t out_ptr, zi_size32_t out_cap) {
  if (!rt) return ZI_E_INTERNAL;
  if (index < 0) return ZI_E_NOENT;
  const sem_cap_t* c = cap_at(rt, (uint32_t)index);
  if (!c || !c->kind || !c->name) return ZI_E_NOENT;

  const uint32_t need = (uint32_t)sir_zi_cap_get_size(rt, index);
  if (need > out_cap) return ZI_E_BOUNDS;

  uint8_t* out = NULL;
  if (!sem_guest_mem_map_rw(rt->mem, out_ptr, out_cap, &out) || !out) return ZI_E_BOUNDS;

  uint32_t off = 0;
  const uint32_t kind_len = u32_len(c->kind);
  const uint32_t name_len = u32_len(c->name);

  zcl1_write_u32le(out + off, kind_len);
  off += 4;
  memcpy(out + off, c->kind, kind_len);
  off += kind_len;
  zcl1_write_u32le(out + off, name_len);
  off += 4;
  memcpy(out + off, c->name, name_len);
  off += name_len;
  zcl1_write_u32le(out + off, c->flags);
  off += 4;

  if (off != need) return ZI_E_INTERNAL;
  return (int32_t)need;
}

uint32_t sir_zi_handle_hflags(sir_hosted_zabi_t* rt, zi_handle_t h) {
  if (!rt) return 0;
  return sem_handle_hflags(&rt->handles, h);
}

typedef struct sir_stdio_stream {
  FILE* f;
} sir_stdio_stream_t;

static int32_t stdio_read(void* ctx, sem_guest_mem_t* mem, zi_ptr_t dst_ptr, zi_size32_t cap) {
  sir_stdio_stream_t* s = (sir_stdio_stream_t*)ctx;
  if (!s || !s->f) return ZI_E_INTERNAL;
  if (cap == 0) return 0;

  uint8_t* dst = NULL;
  if (!sem_guest_mem_map_rw(mem, dst_ptr, cap, &dst) || !dst) return ZI_E_BOUNDS;
  const size_t n = fread(dst, 1, (size_t)cap, s->f);
  if (n == 0 && ferror(s->f)) return ZI_E_INTERNAL;
  return (int32_t)n;
}

static int32_t stdio_write(void* ctx, sem_guest_mem_t* mem, zi_ptr_t src_ptr, zi_size32_t len) {
  sir_stdio_stream_t* s = (sir_stdio_stream_t*)ctx;
  if (!s || !s->f) return ZI_E_INTERNAL;
  if (len == 0) return 0;

  const uint8_t* src = NULL;
  if (!sem_guest_mem_map_ro(mem, src_ptr, len, &src) || !src) return ZI_E_BOUNDS;
  const size_t n = fwrite(src, 1, (size_t)len, s->f);
  if (n < (size_t)len) return ZI_E_INTERNAL;
  (void)fflush(s->f);
  return (int32_t)n;
}

static int32_t stdio_end(void* ctx, sem_guest_mem_t* mem) {
  (void)mem;
  sir_stdio_stream_t* s = (sir_stdio_stream_t*)ctx;
  if (!s) return 0;
  if (s->f) (void)fflush(s->f);
  return 0;
}

static const sem_handle_ops_t stdio_ops = {
    .read = stdio_read,
    .write = stdio_write,
    .end = stdio_end,
};

typedef struct sir_blob_stream {
  uint8_t* buf;
  uint32_t len;
  uint32_t off;
} sir_blob_stream_t;

static int32_t blob_read(void* ctx, sem_guest_mem_t* mem, zi_ptr_t dst_ptr, zi_size32_t cap) {
  sir_blob_stream_t* s = (sir_blob_stream_t*)ctx;
  if (!s || (!s->buf && s->len)) return ZI_E_INTERNAL;
  if (cap == 0) return 0;
  if (s->off >= s->len) return 0;

  uint8_t* dst = NULL;
  if (!sem_guest_mem_map_rw(mem, dst_ptr, cap, &dst) || !dst) return ZI_E_BOUNDS;

  const uint32_t remain = s->len - s->off;
  const uint32_t n = (cap < remain) ? cap : remain;
  memcpy(dst, s->buf + s->off, n);
  s->off += n;
  return (int32_t)n;
}

static int32_t blob_end(void* ctx, sem_guest_mem_t* mem) {
  (void)mem;
  sir_blob_stream_t* s = (sir_blob_stream_t*)ctx;
  if (!s) return 0;
  free(s->buf);
  free(s);
  return 0;
}

static const sem_handle_ops_t blob_ops = {
    .read = blob_read,
    .write = NULL,
    .end = blob_end,
};

static bool str_eq_bytes(const char* s, const uint8_t* b, uint32_t n) {
  if (!s || !b) return false;
  const size_t sl = strlen(s);
  if (sl != (size_t)n) return false;
  return memcmp(s, b, n) == 0;
}

bool sir_hosted_zabi_init(sir_hosted_zabi_t* rt, sir_hosted_zabi_cfg_t cfg) {
  if (!rt) return false;
  sem_guest_mem_t* mem = (sem_guest_mem_t*)calloc(1, sizeof(*mem));
  if (!mem) return false;
  if (!sem_guest_mem_init(mem, cfg.guest_mem_cap ? cfg.guest_mem_cap : (16u * 1024u * 1024u),
                          cfg.guest_mem_base ? cfg.guest_mem_base : 0x10000ull)) {
    free(mem);
    return false;
  }
  if (!sir_hosted_zabi_init_with_mem(rt, mem, cfg)) {
    sem_guest_mem_dispose(mem);
    free(mem);
    return false;
  }
  rt->owns_mem = true;
  return true;
}

void sir_hosted_zabi_dispose(sir_hosted_zabi_t* rt) {
  if (!rt) return;

  // Best-effort close of any outstanding handles.
  for (zi_handle_t h = 0; h < (zi_handle_t)rt->handles.cap; h++) {
    sem_handle_entry_t e;
    if (!sem_handle_lookup(&rt->handles, h, &e)) continue;
    if (e.ops && e.ops->end) (void)e.ops->end(e.ctx, rt->mem);
    if (h == 0 || h == 1 || h == 2) {
      free(e.ctx);
    } else {
      if (!e.ops || !e.ops->end) free(e.ctx);
      (void)sem_handle_release(&rt->handles, h);
    }
  }

  sem_handles_dispose(&rt->handles);
  if (rt->owns_mem && rt->mem) {
    sem_guest_mem_dispose(rt->mem);
    free(rt->mem);
  }
  memset(rt, 0, sizeof(*rt));
}

uint32_t sir_zi_abi_version(const sir_hosted_zabi_t* rt) {
  return rt ? rt->abi_version : 0x00020005u;
}

zi_ptr_t sir_zi_alloc(sir_hosted_zabi_t* rt, zi_size32_t size) {
  if (!rt) return 0;
  return sem_guest_alloc(rt->mem, size, 16);
}

int32_t sir_zi_free(sir_hosted_zabi_t* rt, zi_ptr_t ptr) {
  if (!rt) return ZI_E_INTERNAL;
  return sem_guest_free(rt->mem, ptr);
}

int32_t sir_zi_read(sir_hosted_zabi_t* rt, zi_handle_t h, zi_ptr_t dst_ptr, zi_size32_t cap) {
  if (!rt) return ZI_E_INTERNAL;
  sem_handle_entry_t e;
  if (!sem_handle_lookup(&rt->handles, h, &e) || !e.ops || !e.ops->read) return ZI_E_NOSYS;
  if ((e.hflags & ZI_H_READABLE) == 0) return ZI_E_NOSYS;
  return e.ops->read(e.ctx, rt->mem, dst_ptr, cap);
}

int32_t sir_zi_write(sir_hosted_zabi_t* rt, zi_handle_t h, zi_ptr_t src_ptr, zi_size32_t len) {
  if (!rt) return ZI_E_INTERNAL;
  sem_handle_entry_t e;
  if (!sem_handle_lookup(&rt->handles, h, &e) || !e.ops || !e.ops->write) return ZI_E_NOSYS;
  if ((e.hflags & ZI_H_WRITABLE) == 0) return ZI_E_NOSYS;
  return e.ops->write(e.ctx, rt->mem, src_ptr, len);
}

int32_t sir_zi_end(sir_hosted_zabi_t* rt, zi_handle_t h) {
  if (!rt) return ZI_E_INTERNAL;
  sem_handle_entry_t e;
  if (!sem_handle_lookup(&rt->handles, h, &e) || !e.ops) return ZI_E_NOSYS;
  int32_t r = 0;
  if (e.ops->end) r = e.ops->end(e.ctx, rt->mem);
  if (h >= 3) (void)sem_handle_release(&rt->handles, h);
  return r;
}

int32_t sir_zi_telemetry(sir_hosted_zabi_t* rt, zi_ptr_t topic_ptr, zi_size32_t topic_len, zi_ptr_t msg_ptr, zi_size32_t msg_len) {
  if (!rt) return ZI_E_INTERNAL;
  const uint8_t* topic = NULL;
  const uint8_t* msg = NULL;
  if (topic_len && (!sem_guest_mem_map_ro(rt->mem, topic_ptr, topic_len, &topic) || !topic)) return ZI_E_BOUNDS;
  if (msg_len && (!sem_guest_mem_map_ro(rt->mem, msg_ptr, msg_len, &msg) || !msg)) return ZI_E_BOUNDS;
  fprintf(stderr, "telemetry[%.*s]: %.*s\n", (int)topic_len, (const char*)topic, (int)msg_len, (const char*)msg);
  return 0;
}

int32_t sir_zi_ctl(sir_hosted_zabi_t* rt, zi_ptr_t req_ptr, zi_size32_t req_len, zi_ptr_t resp_ptr, zi_size32_t resp_cap) {
  if (!rt) return ZI_E_INTERNAL;
  if (req_len < ZCL1_HDR_SIZE) return ZI_E_INVALID;

  const uint8_t* req = NULL;
  if (!sem_guest_mem_map_ro(rt->mem, req_ptr, req_len, &req) || !req) return ZI_E_BOUNDS;

  uint8_t* resp = NULL;
  if (!sem_guest_mem_map_rw(rt->mem, resp_ptr, resp_cap, &resp) || !resp) return ZI_E_BOUNDS;

  return sem_zi_ctl(&rt->ctl_host, req, req_len, resp, resp_cap);
}

zi_handle_t sir_zi_cap_open(sir_hosted_zabi_t* rt, zi_ptr_t req_ptr) {
  if (!rt) return (zi_handle_t)ZI_E_INTERNAL;

  const uint32_t REQ_LEN = 40u;
  const uint8_t* req = NULL;
  if (!sem_guest_mem_map_ro(rt->mem, req_ptr, REQ_LEN, &req) || !req) return (zi_handle_t)ZI_E_BOUNDS;

  const uint32_t kind_len = zcl1_read_u32le(req + 8);
  const uint32_t name_len = zcl1_read_u32le(req + 20);
  const uint32_t mode = zcl1_read_u32le(req + 24);
  const uint32_t params_len = zcl1_read_u32le(req + 36);

  const uint64_t kind_ptr = (uint64_t)zcl1_read_u32le(req + 0) | ((uint64_t)zcl1_read_u32le(req + 4) << 32);
  const uint64_t name_ptr = (uint64_t)zcl1_read_u32le(req + 12) | ((uint64_t)zcl1_read_u32le(req + 16) << 32);
  const uint64_t params_ptr = (uint64_t)zcl1_read_u32le(req + 28) | ((uint64_t)zcl1_read_u32le(req + 32) << 32);

  if (mode != 0) return (zi_handle_t)ZI_E_INVALID;
  if (kind_len == 0 || name_len == 0) return (zi_handle_t)ZI_E_INVALID;

  const uint8_t* kind = NULL;
  const uint8_t* name = NULL;
  if (!sem_guest_mem_map_ro(rt->mem, (zi_ptr_t)kind_ptr, (zi_size32_t)kind_len, &kind) || !kind) return (zi_handle_t)ZI_E_BOUNDS;
  if (!sem_guest_mem_map_ro(rt->mem, (zi_ptr_t)name_ptr, (zi_size32_t)name_len, &name) || !name) return (zi_handle_t)ZI_E_BOUNDS;

  const sem_cap_t* found = NULL;
  for (uint32_t i = 0; i < cap_count(rt); i++) {
    const sem_cap_t* c = cap_at(rt, i);
    if (!c || !c->kind || !c->name) continue;
    if (!str_eq_bytes(c->kind, kind, kind_len)) continue;
    if (!str_eq_bytes(c->name, name, name_len)) continue;
    found = c;
    break;
  }
  if (!found) return (zi_handle_t)ZI_E_NOENT;
  if ((found->flags & SEM_ZI_CAP_CAN_OPEN) == 0) return (zi_handle_t)ZI_E_DENIED;

  if (strcmp(found->kind, "file") == 0 && strcmp(found->name, "fs") == 0) {
    sir_hosted_file_fs_t fs;
    sir_hosted_file_fs_init(&fs, (sir_hosted_file_fs_cfg_t){.fs_root = rt->fs_root});
    return sir_hosted_file_fs_open_from_params(&fs, &rt->handles, rt->mem, (zi_ptr_t)params_ptr, (zi_size32_t)params_len);
  }

  if (strcmp(found->kind, "proc") == 0 && strcmp(found->name, "argv") == 0) {
    if (!rt->ctl_host.cfg.argv_enabled) return (zi_handle_t)ZI_E_DENIED;
    if (params_len != 0) return (zi_handle_t)ZI_E_INVALID;

    const uint32_t argc = rt->ctl_host.cfg.argv_count;
    uint64_t need = 8;
    for (uint32_t i = 0; i < argc; i++) {
      const char* s = rt->ctl_host.cfg.argv ? rt->ctl_host.cfg.argv[i] : NULL;
      const uint32_t n = s ? u32_len(s) : 0;
      need += 4ull + n;
    }
    if (need > 0x7FFFFFFFull) return (zi_handle_t)ZI_E_INTERNAL;

    sir_blob_stream_t* bs = (sir_blob_stream_t*)calloc(1, sizeof(*bs));
    if (!bs) return (zi_handle_t)ZI_E_OOM;
    bs->buf = (uint8_t*)calloc(1, (size_t)need);
    if (!bs->buf) {
      free(bs);
      return (zi_handle_t)ZI_E_OOM;
    }
    bs->len = (uint32_t)need;

    uint32_t off = 0;
    zcl1_write_u32le(bs->buf + off, 1);
    off += 4;
    zcl1_write_u32le(bs->buf + off, argc);
    off += 4;
    for (uint32_t i = 0; i < argc; i++) {
      const char* s = rt->ctl_host.cfg.argv ? rt->ctl_host.cfg.argv[i] : NULL;
      const uint32_t n = s ? u32_len(s) : 0;
      zcl1_write_u32le(bs->buf + off, n);
      off += 4;
      if (n) memcpy(bs->buf + off, s, n);
      off += n;
    }
    if (off != bs->len) {
      free(bs->buf);
      free(bs);
      return (zi_handle_t)ZI_E_INTERNAL;
    }

    const zi_handle_t h = sem_handle_alloc(&rt->handles,
                                           (sem_handle_entry_t){.ops = &blob_ops, .ctx = bs, .hflags = ZI_H_READABLE | ZI_H_ENDABLE});
    if (h < 3) {
      free(bs->buf);
      free(bs);
      return h;
    }
    return h;
  }

  if (strcmp(found->kind, "proc") == 0 && strcmp(found->name, "env") == 0) {
    if (!rt->ctl_host.cfg.env_enabled) return (zi_handle_t)ZI_E_DENIED;
    if (params_len != 0) return (zi_handle_t)ZI_E_INVALID;

    const uint32_t envc = rt->ctl_host.cfg.env_count;
    uint64_t need = 8;
    for (uint32_t i = 0; i < envc; i++) {
      const sem_env_kv_t* kv = rt->ctl_host.cfg.env ? &rt->ctl_host.cfg.env[i] : NULL;
      const uint32_t klen = (kv && kv->key) ? u32_len(kv->key) : 0;
      const uint32_t vlen = (kv && kv->val) ? u32_len(kv->val) : 0;
      const uint64_t entry_len = (uint64_t)klen + 1ull + (uint64_t)vlen;
      if (entry_len > 0xFFFFFFFFull) return (zi_handle_t)ZI_E_INTERNAL;
      need += 4ull + entry_len;
    }
    if (need > 0x7FFFFFFFull) return (zi_handle_t)ZI_E_INTERNAL;

    sir_blob_stream_t* bs = (sir_blob_stream_t*)calloc(1, sizeof(*bs));
    if (!bs) return (zi_handle_t)ZI_E_OOM;
    bs->buf = (uint8_t*)calloc(1, (size_t)need);
    if (!bs->buf) {
      free(bs);
      return (zi_handle_t)ZI_E_OOM;
    }
    bs->len = (uint32_t)need;

    uint32_t off = 0;
    zcl1_write_u32le(bs->buf + off, 1);
    off += 4;
    zcl1_write_u32le(bs->buf + off, envc);
    off += 4;
    for (uint32_t i = 0; i < envc; i++) {
      const sem_env_kv_t* kv = rt->ctl_host.cfg.env ? &rt->ctl_host.cfg.env[i] : NULL;
      const char* k = (kv && kv->key) ? kv->key : "";
      const char* v = (kv && kv->val) ? kv->val : "";
      const uint32_t klen = u32_len(k);
      const uint32_t vlen = u32_len(v);
      const uint32_t n = klen + 1u + vlen;
      zcl1_write_u32le(bs->buf + off, n);
      off += 4;
      if (klen) memcpy(bs->buf + off, k, klen);
      off += klen;
      bs->buf[off++] = (uint8_t)'=';
      if (vlen) memcpy(bs->buf + off, v, vlen);
      off += vlen;
    }
    if (off != bs->len) {
      free(bs->buf);
      free(bs);
      return (zi_handle_t)ZI_E_INTERNAL;
    }

    const zi_handle_t h = sem_handle_alloc(&rt->handles,
                                           (sem_handle_entry_t){.ops = &blob_ops, .ctx = bs, .hflags = ZI_H_READABLE | ZI_H_ENDABLE});
    if (h < 3) {
      free(bs->buf);
      free(bs);
      return h;
    }
    return h;
  }

  (void)params_ptr;
  (void)params_len;
  return (zi_handle_t)ZI_E_DENIED;
}

bool sir_hosted_zabi_init_with_mem(sir_hosted_zabi_t* rt, sem_guest_mem_t* mem, sir_hosted_zabi_cfg_t cfg) {
  if (!rt || !mem) return false;
  memset(rt, 0, sizeof(*rt));

  rt->mem = mem;
  rt->owns_mem = false;

  if (!sem_handles_init(&rt->handles, 4096)) {
    return false;
  }

  rt->abi_version = cfg.abi_version ? cfg.abi_version : 0x00020005u;
  rt->fs_root = (cfg.fs_root && cfg.fs_root[0] != '\0') ? cfg.fs_root : NULL;

  sem_host_init(&rt->ctl_host, (sem_host_cfg_t){
                                 .caps = cfg.caps,
                                 .cap_count = cfg.cap_count,
                                 .argv_enabled = cfg.argv_enabled,
                                 .argv = cfg.argv,
                                 .argv_count = cfg.argv_count,
                                 .env_enabled = cfg.env_enabled,
                                 .env = cfg.env,
                                 .env_count = cfg.env_count,
                             });

  sir_stdio_stream_t* in = (sir_stdio_stream_t*)calloc(1, sizeof(*in));
  sir_stdio_stream_t* out = (sir_stdio_stream_t*)calloc(1, sizeof(*out));
  sir_stdio_stream_t* err = (sir_stdio_stream_t*)calloc(1, sizeof(*err));
  if (!in || !out || !err) {
    free(in);
    free(out);
    free(err);
    sem_handles_dispose(&rt->handles);
    return false;
  }
  in->f = stdin;
  out->f = stdout;
  err->f = stderr;

  (void)sem_handle_install(&rt->handles, 0,
                           (sem_handle_entry_t){.ops = &stdio_ops, .ctx = in, .hflags = ZI_H_READABLE | ZI_H_ENDABLE});
  (void)sem_handle_install(&rt->handles, 1,
                           (sem_handle_entry_t){.ops = &stdio_ops, .ctx = out, .hflags = ZI_H_WRITABLE | ZI_H_ENDABLE});
  (void)sem_handle_install(&rt->handles, 2,
                           (sem_handle_entry_t){.ops = &stdio_ops, .ctx = err, .hflags = ZI_H_WRITABLE | ZI_H_ENDABLE});

  return true;
}
