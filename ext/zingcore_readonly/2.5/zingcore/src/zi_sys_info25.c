#include "zi_sys_info25.h"

#include "zi_handles25.h"
#include "zi_runtime25.h"
#include "zi_sysabi25.h"
#include "zi_zcl1.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/time.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#if defined(__linux__)
#include <sys/random.h>
#endif

// ---- cap descriptor ----

static const zi_cap_v1 CAP = {
    .kind = ZI_CAP_KIND_SYS,
    .name = ZI_CAP_NAME_INFO,
    .version = 1,
    .cap_flags = ZI_CAP_CAN_OPEN | ZI_CAP_MAY_BLOCK,
    .meta = NULL,
    .meta_len = 0,
};

const zi_cap_v1 *zi_sys_info25_cap(void) { return &CAP; }

int zi_sys_info25_register(void) { return zi_cap_register(&CAP); }

// ---- helpers ----

static void write_u32le(uint8_t *p, uint32_t v) { zi_zcl1_write_u32(p, v); }

static void write_u64le(uint8_t *p, uint64_t v) {
  write_u32le(p + 0, (uint32_t)(v & 0xFFFFFFFFu));
  write_u32le(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static uint64_t now_realtime_ns(void) {
#if defined(CLOCK_REALTIME)
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  }
#endif
#if defined(__unix__) || defined(__APPLE__)
  struct timeval tv;
  if (gettimeofday(&tv, NULL) == 0) {
    return (uint64_t)tv.tv_sec * 1000000000ull + (uint64_t)tv.tv_usec * 1000ull;
  }
#endif
  return 0;
}

static uint64_t now_monotonic_ns(void) {
#if defined(CLOCK_MONOTONIC)
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  }
#endif
  return 0;
}

static uint32_t sys_cpu_count(void) {
#if defined(__unix__) || defined(__APPLE__)
#ifdef _SC_NPROCESSORS_ONLN
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n > 0 && n < (long)0x7FFFFFFF) return (uint32_t)n;
#endif
#endif
  return 1;
}

static uint32_t sys_page_size(void) {
#if defined(__unix__) || defined(__APPLE__)
#ifdef _SC_PAGESIZE
  long n = sysconf(_SC_PAGESIZE);
  if (n > 0 && n < (long)0x7FFFFFFF) return (uint32_t)n;
#endif
#ifdef _SC_PAGE_SIZE
  long n2 = sysconf(_SC_PAGE_SIZE);
  if (n2 > 0 && n2 < (long)0x7FFFFFFF) return (uint32_t)n2;
#endif
#endif
  return 0;
}

static uint64_t sys_mem_total_bytes(void) {
#if defined(__unix__) || defined(__APPLE__)
#ifdef _SC_PHYS_PAGES
#ifdef _SC_PAGESIZE
  long pages = sysconf(_SC_PHYS_PAGES);
  long ps = sysconf(_SC_PAGESIZE);
  if (pages > 0 && ps > 0) return (uint64_t)pages * (uint64_t)ps;
#endif
#endif
#endif
  return 0;
}

static uint64_t sys_mem_avail_bytes(void) {
#if defined(__unix__) || defined(__APPLE__)
#ifdef _SC_AVPHYS_PAGES
#ifdef _SC_PAGESIZE
  long pages = sysconf(_SC_AVPHYS_PAGES);
  long ps = sysconf(_SC_PAGESIZE);
  if (pages > 0 && ps > 0) return (uint64_t)pages * (uint64_t)ps;
#endif
#endif
#endif
  return 0;
}

static uint32_t clamp_u32(uint64_t v) {
  if (v > 0xFFFFFFFFull) return 0xFFFFFFFFu;
  return (uint32_t)v;
}

static uint32_t mem_pressure_milli(uint64_t total, uint64_t avail) {
  if (total == 0) return 0;
  if (avail > total) avail = total;
  uint64_t used = total - avail;
  // pressure = used/total * 1000
  uint64_t milli = (used * 1000ull) / total;
  if (milli > 1000ull) milli = 1000ull;
  return (uint32_t)milli;
}

static int fill_entropy(uint8_t *out, uint32_t n) {
  if (!out || n == 0) return 0;

#if defined(__APPLE__)
  // arc4random_buf is available on modern macOS.
  arc4random_buf(out, (size_t)n);
  return 1;
#endif

#if defined(__linux__)
  // getrandom is available on Linux.
  ssize_t r = getrandom(out, (size_t)n, 0);
  if (r == (ssize_t)n) return 1;
#endif

  FILE *f = fopen("/dev/urandom", "rb");
  if (!f) return 0;
  size_t got = fread(out, 1, (size_t)n, f);
  fclose(f);
  return got == (size_t)n;
}

static uint32_t write_str(uint8_t *out, uint32_t cap, uint32_t off, const char *s, uint32_t *io_flags, uint32_t flag_bit) {
  uint32_t len = s ? (uint32_t)strlen(s) : 0;
  if (off + 4u + len > cap) return 0;
  write_u32le(out + off, len);
  off += 4;
  if (len) memcpy(out + off, s, len);
  off += len;
  if (len && io_flags) *io_flags |= flag_bit;
  return off;
}

// ---- handle implementation ----

typedef struct {
  uint8_t inbuf[65536];
  uint32_t in_len;

  uint8_t outbuf[65536];
  uint32_t out_len;
  uint32_t out_off;

  int closed;
} zi_sys_info_handle_ctx;

static int32_t sys_info_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  zi_sys_info_handle_ctx *h = (zi_sys_info_handle_ctx *)ctx;
  if (!h || h->closed) return 0;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_rw) return ZI_E_NOSYS;

  if (h->out_off >= h->out_len) return ZI_E_AGAIN;

  uint8_t *dst = NULL;
  if (!mem->map_rw(mem->ctx, dst_ptr, cap, &dst) || !dst) return ZI_E_BOUNDS;

  uint32_t avail = h->out_len - h->out_off;
  uint32_t n = (avail < (uint32_t)cap) ? avail : (uint32_t)cap;
  memcpy(dst, h->outbuf + h->out_off, n);
  h->out_off += n;
  if (h->out_off == h->out_len) {
    h->out_off = 0;
    h->out_len = 0;
  }
  return (int32_t)n;
}

static int append_out(zi_sys_info_handle_ctx *h, const uint8_t *data, uint32_t n) {
  if (!h) return 0;
  if (n == 0) return 1;
  if (h->out_len + n > (uint32_t)sizeof(h->outbuf)) return 0;
  memcpy(h->outbuf + h->out_len, data, n);
  h->out_len += n;
  return 1;
}

static int handle_req(zi_sys_info_handle_ctx *h, const zi_zcl1_frame *z) {
  if (!h || !z) return 0;

  uint8_t fr[8192];
  int n = -1;

  if (z->op == ZI_SYS_INFO_OP_INFO) {
    uint8_t pl[4096];
    uint32_t off = 0;
    write_u32le(pl + off, 1u);
    off += 4;

    uint32_t flags = 0;
    uint32_t flags_off = off;
    write_u32le(pl + off, 0u);
    off += 4;

    write_u32le(pl + off, sys_cpu_count());
    off += 4;
    write_u32le(pl + off, sys_page_size());
    off += 4;

    char os[128] = {0};
    char arch[128] = {0};
    char model[128] = {0};
    char host[128] = {0};

#if defined(__unix__) || defined(__APPLE__)
    struct utsname u;
    if (uname(&u) == 0) {
      // Best-effort: os="sysname release".
      snprintf(os, sizeof(os), "%s %s", u.sysname, u.release);
      snprintf(arch, sizeof(arch), "%s", u.machine);
      snprintf(host, sizeof(host), "%s", u.nodename);
    }
#endif

#if defined(__APPLE__)
    // hw.model best-effort.
    size_t msz = sizeof(model);
    if (sysctlbyname("hw.model", model, &msz, NULL, 0) != 0) {
      model[0] = '\0';
    } else {
      model[sizeof(model) - 1] = '\0';
    }
#endif

    uint32_t next = 0;
    next = write_str(pl, (uint32_t)sizeof(pl), off, os[0] ? os : NULL, &flags, 0x1u);
    if (!next) goto err_bounds;
    off = next;

    next = write_str(pl, (uint32_t)sizeof(pl), off, arch[0] ? arch : NULL, &flags, 0x2u);
    if (!next) goto err_bounds;
    off = next;

    next = write_str(pl, (uint32_t)sizeof(pl), off, model[0] ? model : NULL, &flags, 0x4u);
    if (!next) goto err_bounds;
    off = next;

    // Hostname may be redacted by setting empty; for now expose best-effort.
    next = write_str(pl, (uint32_t)sizeof(pl), off, host[0] ? host : NULL, &flags, 0x8u);
    if (!next) goto err_bounds;
    off = next;

    write_u32le(pl + flags_off, flags);

    n = zi_zcl1_write_ok(fr, (uint32_t)sizeof(fr), (uint16_t)z->op, z->rid, pl, off);
    if (n < 0) return 0;
    return append_out(h, fr, (uint32_t)n);
  }

  if (z->op == ZI_SYS_INFO_OP_TIME_NOW) {
    uint8_t pl[4 + 8 + 8];
    write_u32le(pl + 0, 1u);
    write_u64le(pl + 4, now_realtime_ns());
    write_u64le(pl + 12, now_monotonic_ns());
    n = zi_zcl1_write_ok(fr, (uint32_t)sizeof(fr), (uint16_t)z->op, z->rid, pl, (uint32_t)sizeof(pl));
    if (n < 0) return 0;
    return append_out(h, fr, (uint32_t)n);
  }

  if (z->op == ZI_SYS_INFO_OP_RANDOM_SEED) {
    uint8_t seed[32];
    if (!fill_entropy(seed, (uint32_t)sizeof(seed))) {
      n = zi_zcl1_write_error(fr, (uint32_t)sizeof(fr), (uint16_t)z->op, z->rid, "sys.info", "entropy unavailable");
      if (n < 0) return 0;
      return append_out(h, fr, (uint32_t)n);
    }

    uint8_t pl[4 + 4 + 32];
    write_u32le(pl + 0, 1u);
    write_u32le(pl + 4, 32u);
    memcpy(pl + 8, seed, 32u);
    n = zi_zcl1_write_ok(fr, (uint32_t)sizeof(fr), (uint16_t)z->op, z->rid, pl, (uint32_t)sizeof(pl));
    if (n < 0) return 0;
    return append_out(h, fr, (uint32_t)n);
  }

  if (z->op == ZI_SYS_INFO_OP_STATS) {
    uint8_t pl[4 + 4 + 8 + 12 + 8 + 8 + 4];
    uint32_t off = 0;
    write_u32le(pl + off, 1u);
    off += 4;
    uint32_t flags = 0;
    uint32_t flags_off = off;
    write_u32le(pl + off, 0u);
    off += 4;

    write_u64le(pl + off, now_realtime_ns());
    off += 8;

    // load averages (best-effort)
#if defined(__unix__) || defined(__APPLE__)
    double loads[3];
    int got = getloadavg(loads, 3);
    if (got == 3) {
      flags |= 0x1u;
      uint32_t l1 = clamp_u32((uint64_t)(loads[0] * 1000.0 + 0.5));
      uint32_t l5 = clamp_u32((uint64_t)(loads[1] * 1000.0 + 0.5));
      uint32_t l15 = clamp_u32((uint64_t)(loads[2] * 1000.0 + 0.5));
      write_u32le(pl + off + 0, l1);
      write_u32le(pl + off + 4, l5);
      write_u32le(pl + off + 8, l15);
      off += 12;
    }
#endif

    uint64_t total = sys_mem_total_bytes();
    uint64_t avail = sys_mem_avail_bytes();
    if (total > 0 && avail > 0) {
      flags |= 0x2u;
      write_u64le(pl + off, total);
      off += 8;
      write_u64le(pl + off, avail);
      off += 8;
      write_u32le(pl + off, mem_pressure_milli(total, avail));
      off += 4;
    }

    write_u32le(pl + flags_off, flags);

    n = zi_zcl1_write_ok(fr, (uint32_t)sizeof(fr), (uint16_t)z->op, z->rid, pl, off);
    if (n < 0) return 0;
    return append_out(h, fr, (uint32_t)n);
  }

  // Unknown op.
  n = zi_zcl1_write_error(fr, (uint32_t)sizeof(fr), (uint16_t)z->op, z->rid, "sys.info", "unknown op");
  if (n < 0) return 0;
  return append_out(h, fr, (uint32_t)n);

err_bounds:
  n = zi_zcl1_write_error(fr, (uint32_t)sizeof(fr), (uint16_t)z->op, z->rid, "sys.info", "response too large");
  if (n < 0) return 0;
  return append_out(h, fr, (uint32_t)n);
}

static int32_t sys_info_write(void *ctx, zi_ptr_t src_ptr, zi_size32_t len) {
  zi_sys_info_handle_ctx *h = (zi_sys_info_handle_ctx *)ctx;
  if (!h || h->closed) return 0;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_ro) return ZI_E_NOSYS;

  const uint8_t *src = NULL;
  if (!mem->map_ro(mem->ctx, src_ptr, len, &src) || !src) return ZI_E_BOUNDS;

  if (len > (zi_size32_t)(sizeof(h->inbuf) - h->in_len)) return ZI_E_BOUNDS;
  memcpy(h->inbuf + h->in_len, src, (size_t)len);
  h->in_len += (uint32_t)len;

  // Process as many full frames as present.
  uint32_t off = 0;
  while (h->in_len - off >= 24u) {
    uint32_t payload_len = zi_zcl1_read_u32(h->inbuf + off + 20);
    uint32_t frame_len = 24u + payload_len;
    if (h->in_len - off < frame_len) break;

    zi_zcl1_frame z;
    if (!zi_zcl1_parse(h->inbuf + off, frame_len, &z)) {
      // Consume one byte to avoid infinite loops on garbage.
      off += 1;
      continue;
    }

    // Requests should have empty payload for v1 operations.
    if (z.payload_len != 0) {
      uint8_t fr[256];
      int n = zi_zcl1_write_error(fr, (uint32_t)sizeof(fr), (uint16_t)z.op, z.rid, "sys.info", "payload must be empty");
      if (n > 0) (void)append_out(h, fr, (uint32_t)n);
      off += frame_len;
      continue;
    }

    (void)handle_req(h, &z);
    off += frame_len;
  }

  if (off > 0) {
    uint32_t remain = h->in_len - off;
    if (remain) memmove(h->inbuf, h->inbuf + off, remain);
    h->in_len = remain;
  }

  return (int32_t)len;
}

static int32_t sys_info_end(void *ctx) {
  zi_sys_info_handle_ctx *h = (zi_sys_info_handle_ctx *)ctx;
  if (!h) return 0;
  h->closed = 1;
  memset(h, 0, sizeof(*h));
  free(h);
  return 0;
}

static const zi_handle_ops_v1 OPS = {
    .read = sys_info_read,
    .write = sys_info_write,
    .end = sys_info_end,
};

zi_handle_t zi_sys_info25_open_from_params(zi_ptr_t params_ptr, zi_size32_t params_len) {
  (void)params_ptr;
  if (params_len != 0) return (zi_handle_t)ZI_E_INVALID;

  if (!zi_handles25_init()) {
    return (zi_handle_t)ZI_E_INTERNAL;
  }

  zi_sys_info_handle_ctx *ctx = (zi_sys_info_handle_ctx *)calloc(1u, sizeof(*ctx));
  if (!ctx) return (zi_handle_t)ZI_E_OOM;

  zi_handle_t h = zi_handle25_alloc(&OPS, ctx, ZI_H_READABLE | ZI_H_WRITABLE | ZI_H_ENDABLE);
  if (h < 3) {
    sys_info_end(ctx);
    return (zi_handle_t)ZI_E_INTERNAL;
  }
  return h;
}
