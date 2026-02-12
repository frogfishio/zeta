#include "zingcore25.h"

#include "zi_caps.h"
#include "zi_async_default25.h"
#include "zi_event_bus25.h"
#include "zi_file_fs25.h"
#include "zi_handles25.h"
#include "zi_net_tcp25.h"
#include "zi_proc_argv25.h"
#include "zi_proc_env25.h"
#include "zi_proc_hopper25.h"
#include "zi_runtime25.h"
#include "zi_sys_info25.h"
#include "zi_sysabi25.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// This example is the "kitchen sink" embedding.
//
// Keep `stdio_caps_demo.c` minimal as the bare template.
// This file registers *all* currently-implemented golden caps and runs a small
// end-to-end smoke:
// - CAPS_LIST via zi_ctl
// - open proc/argv and read its packed stream
// - open file/fs, write+read a file (uses ZI_FS_ROOT if set; else writes in /tmp)

typedef struct {
  int fd;
  int close_on_end;
} fd_stream;

static int32_t map_errno_to_zi(int e) {
  switch (e) {
    case EAGAIN:
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
    case EWOULDBLOCK:
#endif
      return ZI_E_AGAIN;
    case EBADF:
      return ZI_E_CLOSED;
    case EACCES:
    case EPERM:
      return ZI_E_DENIED;
    case ENOMEM:
      return ZI_E_OOM;
    default:
      return ZI_E_IO;
  }
}

static int32_t fd_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  fd_stream *s = (fd_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (cap == 0) return 0;
  if (dst_ptr == 0) return ZI_E_BOUNDS;

  void *dst = (void *)(uintptr_t)dst_ptr; // native-guest mode
  ssize_t n = read(s->fd, dst, (size_t)cap);
  if (n < 0) return map_errno_to_zi(errno);
  return (int32_t)n;
}

static int32_t fd_write(void *ctx, zi_ptr_t src_ptr, zi_size32_t len) {
  fd_stream *s = (fd_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (len == 0) return 0;
  if (src_ptr == 0) return ZI_E_BOUNDS;

  const void *src = (const void *)(uintptr_t)src_ptr; // native-guest mode
  ssize_t n = write(s->fd, src, (size_t)len);
  if (n < 0) return map_errno_to_zi(errno);
  return (int32_t)n;
}

static int32_t fd_end(void *ctx) {
  fd_stream *s = (fd_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (!s->close_on_end) return 0;
  if (close(s->fd) != 0) return map_errno_to_zi(errno);
  s->fd = -1;
  return 0;
}

static const zi_handle_ops_v1 fd_ops = {
    .read = fd_read,
    .write = fd_write,
    .end = fd_end,
};

static int32_t host_telemetry(void *ctx, zi_ptr_t topic_ptr, zi_size32_t topic_len, zi_ptr_t msg_ptr,
                             zi_size32_t msg_len) {
  (void)ctx;
  const uint8_t *topic = (const uint8_t *)(uintptr_t)topic_ptr;
  const uint8_t *msg = (const uint8_t *)(uintptr_t)msg_ptr;

  write(2, "telemetry:", 10);
  if (topic && topic_len) {
    write(2, " ", 1);
    write(2, topic, (size_t)topic_len);
  }
  if (msg && msg_len) {
    write(2, " ", 1);
    write(2, msg, (size_t)msg_len);
  }
  write(2, "\n", 1);
  return 0;
}

static void zcl1_write_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void zcl1_write_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t zcl1_read_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t zcl1_read_i32(const uint8_t *p) {
  return (int32_t)zcl1_read_u32(p);
}

static void build_caps_list_req(uint8_t req[24], uint32_t rid) {
  memcpy(req + 0, "ZCL1", 4);
  zcl1_write_u16(req + 4, 1);
  zcl1_write_u16(req + 6, (uint16_t)ZI_CTL_OP_CAPS_LIST);
  zcl1_write_u32(req + 8, rid);
  zcl1_write_u32(req + 12, 0);
  zcl1_write_u32(req + 16, 0);
  zcl1_write_u32(req + 20, 0);
}

static uint32_t zcl1_status(const uint8_t *fr) {
  return zcl1_read_u32(fr + 12);
}

static void build_zcl1_req(uint8_t *out, uint16_t op, uint32_t rid, const uint8_t *payload, uint32_t payload_len) {
  memcpy(out + 0, "ZCL1", 4);
  zcl1_write_u16(out + 4, 1);
  zcl1_write_u16(out + 6, op);
  zcl1_write_u32(out + 8, rid);
  zcl1_write_u32(out + 12, 0);
  zcl1_write_u32(out + 16, 0);
  zcl1_write_u32(out + 20, payload_len);
  if (payload_len && payload) memcpy(out + 24, payload, payload_len);
}

static void build_open_req(uint8_t req[40], const char *kind, const char *name, const void *params, uint32_t params_len);

static int hopper_smoke(void) {
  // Open proc/hopper with small buffers.
  uint8_t params[12];
  zcl1_write_u32(params + 0, 1);
  zcl1_write_u32(params + 4, 256);
  zcl1_write_u32(params + 8, 8);

  uint8_t req[40];
  build_open_req(req, ZI_CAP_KIND_PROC, ZI_CAP_NAME_HOPPER, params, (uint32_t)sizeof(params));

  zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)req);
  if (h < 3) {
    fprintf(stderr, "proc/hopper open failed: %d\n", h);
    return 0;
  }

  // Helper: round-trip a single request and read back the whole response.
  uint8_t resp[4096];

  // INFO
  {
    uint8_t fr[24];
    build_zcl1_req(fr, (uint16_t)ZI_HOPPER_OP_INFO, 1, NULL, 0);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "hopper INFO write failed\n");
      (void)zi_end(h);
      return 0;
    }
    uint32_t off = 0;
    for (;;) {
      int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(resp + off), (zi_size32_t)(sizeof(resp) - off));
      if (n == ZI_E_AGAIN) continue;
      if (n < 0) {
        fprintf(stderr, "hopper INFO read failed: %d\n", n);
        (void)zi_end(h);
        return 0;
      }
      off += (uint32_t)n;
      if (n == 0 || off >= 24) break;
    }
    if (off < 24 || zcl1_status(resp) != 1) {
      fprintf(stderr, "hopper INFO bad response\n");
      (void)zi_end(h);
      return 0;
    }
  }

  // RECORD layout_id=1
  int32_t ref = -1;
  {
    uint8_t payload[4];
    zcl1_write_u32(payload + 0, 1);
    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_HOPPER_OP_RECORD, 2, payload, (uint32_t)sizeof(payload));
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "hopper RECORD write failed\n");
      (void)zi_end(h);
      return 0;
    }
    uint32_t off = 0;
    for (;;) {
      int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(resp + off), (zi_size32_t)(sizeof(resp) - off));
      if (n == ZI_E_AGAIN) continue;
      if (n < 0) {
        fprintf(stderr, "hopper RECORD read failed: %d\n", n);
        (void)zi_end(h);
        return 0;
      }
      off += (uint32_t)n;
      if (n == 0 || off >= 32) break;
    }
    if (off < 32 || zcl1_status(resp) != 1) {
      fprintf(stderr, "hopper RECORD bad response\n");
      (void)zi_end(h);
      return 0;
    }
    const uint8_t *pl = resp + 24;
    uint32_t herr = zcl1_read_u32(pl + 0);
    ref = zcl1_read_i32(pl + 4);
    if (herr != 0 || ref < 0) {
      fprintf(stderr, "hopper RECORD failed herr=%u ref=%d\n", herr, ref);
      (void)zi_end(h);
      return 0;
    }
  }

  // SET_BYTES field 0 = "hi"
  {
    const char *msg = "hi";
    uint8_t payload[12 + 2];
    zcl1_write_u32(payload + 0, (uint32_t)ref);
    zcl1_write_u32(payload + 4, 0);
    zcl1_write_u32(payload + 8, 2);
    memcpy(payload + 12, msg, 2);
    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_HOPPER_OP_FIELD_SET_BYTES, 3, payload, (uint32_t)sizeof(payload));
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "hopper SET_BYTES write failed\n");
      (void)zi_end(h);
      return 0;
    }
    uint32_t off = 0;
    for (;;) {
      int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(resp + off), (zi_size32_t)(sizeof(resp) - off));
      if (n == ZI_E_AGAIN) continue;
      if (n < 0) {
        fprintf(stderr, "hopper SET_BYTES read failed: %d\n", n);
        (void)zi_end(h);
        return 0;
      }
      off += (uint32_t)n;
      if (n == 0 || off >= 28) break;
    }
    if (off < 28 || zcl1_status(resp) != 1) {
      fprintf(stderr, "hopper SET_BYTES bad response\n");
      (void)zi_end(h);
      return 0;
    }
    if (zcl1_read_u32(resp + 24) != 0) {
      fprintf(stderr, "hopper SET_BYTES failed\n");
      (void)zi_end(h);
      return 0;
    }
  }

  // SET_I32 field 1 = 123
  {
    uint8_t payload[12];
    zcl1_write_u32(payload + 0, (uint32_t)ref);
    zcl1_write_u32(payload + 4, 1);
    zcl1_write_u32(payload + 8, 123);
    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_HOPPER_OP_FIELD_SET_I32, 4, payload, (uint32_t)sizeof(payload));
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "hopper SET_I32 write failed\n");
      (void)zi_end(h);
      return 0;
    }
    uint32_t off = 0;
    for (;;) {
      int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(resp + off), (zi_size32_t)(sizeof(resp) - off));
      if (n == ZI_E_AGAIN) continue;
      if (n < 0) {
        fprintf(stderr, "hopper SET_I32 read failed: %d\n", n);
        (void)zi_end(h);
        return 0;
      }
      off += (uint32_t)n;
      if (n == 0 || off >= 28) break;
    }
    if (off < 28 || zcl1_status(resp) != 1) {
      fprintf(stderr, "hopper SET_I32 bad response\n");
      (void)zi_end(h);
      return 0;
    }
    if (zcl1_read_u32(resp + 24) != 0) {
      fprintf(stderr, "hopper SET_I32 failed\n");
      (void)zi_end(h);
      return 0;
    }
  }

  // GET_BYTES field 0 -> expect "hi  "
  {
    uint8_t payload[8];
    zcl1_write_u32(payload + 0, (uint32_t)ref);
    zcl1_write_u32(payload + 4, 0);
    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_HOPPER_OP_FIELD_GET_BYTES, 5, payload, (uint32_t)sizeof(payload));
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "hopper GET_BYTES write failed\n");
      (void)zi_end(h);
      return 0;
    }
    uint32_t off = 0;
    for (;;) {
      int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(resp + off), (zi_size32_t)(sizeof(resp) - off));
      if (n == ZI_E_AGAIN) continue;
      if (n < 0) {
        fprintf(stderr, "hopper GET_BYTES read failed: %d\n", n);
        (void)zi_end(h);
        return 0;
      }
      off += (uint32_t)n;
      if (n == 0) break;
      if (off >= 24) {
        uint32_t pl = zcl1_read_u32(resp + 20);
        if (off >= 24u + pl) break;
      }
    }
    if (off < 24 || zcl1_status(resp) != 1) {
      fprintf(stderr, "hopper GET_BYTES bad response\n");
      (void)zi_end(h);
      return 0;
    }
    const uint8_t *pl = resp + 24;
    uint32_t herr = zcl1_read_u32(pl + 0);
    uint32_t blen = zcl1_read_u32(pl + 4);
    if (herr != 0 || blen != 4 || memcmp(pl + 8, "hi  ", 4) != 0) {
      fprintf(stderr, "hopper GET_BYTES mismatch herr=%u blen=%u\n", herr, blen);
      (void)zi_end(h);
      return 0;
    }
  }

  // GET_I32 field 1 -> expect 123
  {
    uint8_t payload[8];
    zcl1_write_u32(payload + 0, (uint32_t)ref);
    zcl1_write_u32(payload + 4, 1);
    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_HOPPER_OP_FIELD_GET_I32, 6, payload, (uint32_t)sizeof(payload));
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "hopper GET_I32 write failed\n");
      (void)zi_end(h);
      return 0;
    }
    uint32_t off = 0;
    for (;;) {
      int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(resp + off), (zi_size32_t)(sizeof(resp) - off));
      if (n == ZI_E_AGAIN) continue;
      if (n < 0) {
        fprintf(stderr, "hopper GET_I32 read failed: %d\n", n);
        (void)zi_end(h);
        return 0;
      }
      off += (uint32_t)n;
      if (n == 0 || off >= 32) break;
    }
    if (off < 32 || zcl1_status(resp) != 1) {
      fprintf(stderr, "hopper GET_I32 bad response\n");
      (void)zi_end(h);
      return 0;
    }
    const uint8_t *pl = resp + 24;
    uint32_t herr = zcl1_read_u32(pl + 0);
    int32_t v = zcl1_read_i32(pl + 4);
    if (herr != 0 || v != 123) {
      fprintf(stderr, "hopper GET_I32 mismatch herr=%u v=%d\n", herr, v);
      (void)zi_end(h);
      return 0;
    }
  }

  (void)zi_end(h);
  return 1;
}

static void write_u64le(uint8_t *p, uint64_t v) {
  zcl1_write_u32(p + 0, (uint32_t)(v & 0xFFFFFFFFu));
  zcl1_write_u32(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static void build_open_req(uint8_t req[40], const char *kind, const char *name, const void *params, uint32_t params_len) {
  // Packed open request (see zi_syscalls_caps25.c):
  // u64 kind_ptr, u32 kind_len, u64 name_ptr, u32 name_len, u32 mode, u64 params_ptr, u32 params_len
  write_u64le(req + 0, (uint64_t)(uintptr_t)kind);
  zcl1_write_u32(req + 8, (uint32_t)strlen(kind));
  write_u64le(req + 12, (uint64_t)(uintptr_t)name);
  zcl1_write_u32(req + 20, (uint32_t)strlen(name));
  zcl1_write_u32(req + 24, 0);
  write_u64le(req + 28, (uint64_t)(uintptr_t)params);
  zcl1_write_u32(req + 36, params_len);
}

static void build_fs_params(uint8_t params[20], const char *path, uint32_t oflags, uint32_t create_mode) {
  // u64 path_ptr, u32 path_len, u32 oflags, u32 create_mode
  write_u64le(params + 0, (uint64_t)(uintptr_t)path);
  zcl1_write_u32(params + 8, (uint32_t)strlen(path));
  zcl1_write_u32(params + 12, oflags);
  zcl1_write_u32(params + 16, create_mode);
}

static void dump_caps_list(void) {
  uint8_t req[24];
  uint8_t resp[4096];
  memset(resp, 0, sizeof(resp));
  build_caps_list_req(req, 1);

  int32_t r = zi_ctl((zi_ptr_t)(uintptr_t)req, (zi_size32_t)sizeof(req), (zi_ptr_t)(uintptr_t)resp, (zi_size32_t)sizeof(resp));
  if (r < 0) {
    fprintf(stderr, "ctl CAPS_LIST failed: %d\n", r);
    return;
  }

  // ZCL1 response header is 24 bytes; payload begins at 24.
  uint32_t payload_len = zcl1_read_u32(resp + 20);
  if (24u + payload_len > sizeof(resp)) {
    fprintf(stderr, "ctl CAPS_LIST: payload too large\n");
    return;
  }

  const uint8_t *p = resp + 24;
  if (payload_len < 8) {
    fprintf(stderr, "ctl CAPS_LIST: short payload\n");
    return;
  }

  uint32_t ver = zcl1_read_u32(p + 0);
  uint32_t n = zcl1_read_u32(p + 4);
  fprintf(stderr, "caps_list v%u: %u caps\n", ver, n);

  uint32_t off = 8;
  for (uint32_t i = 0; i < n; i++) {
    if (off + 4 > payload_len) break;
    uint32_t kind_len = zcl1_read_u32(p + off);
    off += 4;
    if (off + kind_len + 4 > payload_len) break;
    const char *kind = (const char *)(p + off);
    off += kind_len;

    uint32_t name_len = zcl1_read_u32(p + off);
    off += 4;
    if (off + name_len + 4 > payload_len) break;
    const char *name = (const char *)(p + off);
    off += name_len;

    uint32_t flags = zcl1_read_u32(p + off);
    off += 4;

    if (off + 4 > payload_len) break;
    uint32_t meta_len = zcl1_read_u32(p + off);
    off += 4;
    if (off + meta_len > payload_len) break;
    off += meta_len;

    fprintf(stderr, "  - %.*s/%.*s flags=0x%08x\n", (int)kind_len, kind, (int)name_len, name, flags);
  }
}

static int dump_argv_via_cap(int argc, char **argv) {
  zi_runtime25_set_argv(argc, (const char *const *)argv);

  uint8_t req[40];
  build_open_req(req, ZI_CAP_KIND_PROC, ZI_CAP_NAME_ARGV, NULL, 0);

  zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)req);
  if (h < 3) {
    fprintf(stderr, "proc/argv open failed: %d\n", h);
    return 0;
  }

  uint8_t buf[2048];
  uint32_t off = 0;
  for (;;) {
    int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(buf + off), (zi_size32_t)(sizeof(buf) - off));
    if (n < 0) {
      fprintf(stderr, "proc/argv read failed: %d\n", n);
      (void)zi_end(h);
      return 0;
    }
    if (n == 0) break;
    off += (uint32_t)n;
    if (off == sizeof(buf)) break;
  }

  if (off < 8) {
    fprintf(stderr, "proc/argv: short\n");
    (void)zi_end(h);
    return 0;
  }

  uint32_t ver = zcl1_read_u32(buf + 0);
  uint32_t ac = zcl1_read_u32(buf + 4);
  fprintf(stderr, "argv v%u argc=%u\n", ver, ac);

  uint32_t p = 8;
  for (uint32_t i = 0; i < ac; i++) {
    if (p + 4 > off) break;
    uint32_t sl = zcl1_read_u32(buf + p);
    p += 4;
    if (p + sl > off) break;
    fprintf(stderr, "  argv[%u]=%.*s\n", i, (int)sl, (const char *)(buf + p));
    p += sl;
  }

  (void)zi_end(h);
  return 1;
}

static int dump_env_via_cap(const char *const *envp) {
  int envc = 0;
  if (envp) {
    while (envp[envc]) envc++;
  }
  zi_runtime25_set_env(envc, envp);

  uint8_t req[40];
  build_open_req(req, ZI_CAP_KIND_PROC, ZI_CAP_NAME_ENV, NULL, 0);

  zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)req);
  if (h < 3) {
    fprintf(stderr, "proc/env open failed: %d\n", h);
    return 0;
  }

  uint8_t buf[4096];
  uint32_t off = 0;
  for (;;) {
    int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(buf + off), (zi_size32_t)(sizeof(buf) - off));
    if (n < 0) {
      fprintf(stderr, "proc/env read failed: %d\n", n);
      (void)zi_end(h);
      return 0;
    }
    if (n == 0) break;
    off += (uint32_t)n;
    if (off == sizeof(buf)) break;
  }

  if (off >= 8) {
    uint32_t ver = zcl1_read_u32(buf + 0);
    uint32_t ec = zcl1_read_u32(buf + 4);
    fprintf(stderr, "env v%u envc=%u\n", ver, ec);
  }

  (void)zi_end(h);
  return 1;
}

static void zcl1_write_u64(uint8_t *p, uint64_t v) {
  zcl1_write_u32(p + 0, (uint32_t)(v & 0xFFFFFFFFu));
  zcl1_write_u32(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static uint64_t zcl1_read_u64(const uint8_t *p) {
  uint64_t lo = (uint64_t)zcl1_read_u32(p + 0);
  uint64_t hi = (uint64_t)zcl1_read_u32(p + 4);
  return lo | (hi << 32);
}

static void print_load_milli(const char *label, uint32_t milli) {
  fprintf(stderr, "%s=%u.%03u", label, milli / 1000u, milli % 1000u);
}

static int sys_info_smoke(void) {
  uint8_t req[40];
  build_open_req(req, ZI_CAP_KIND_SYS, ZI_CAP_NAME_INFO, NULL, 0);

  zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)req);
  if (h < 3) {
    fprintf(stderr, "sys/info open failed: %d\n", h);
    return 0;
  }

  uint8_t resp[4096];

  // INFO
  uint32_t info_flags = 0;
  uint32_t info_cpu = 0;
  uint32_t info_ps = 0;
  {
    uint8_t fr[24];
    build_zcl1_req(fr, (uint16_t)ZI_SYS_INFO_OP_INFO, 30, NULL, 0);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "sys/info INFO write failed\n");
      (void)zi_end(h);
      return 0;
    }

    uint32_t got = 0;
    for (;;) {
      int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(resp + got), (zi_size32_t)(sizeof(resp) - got));
      if (n == ZI_E_AGAIN) continue;
      if (n < 0) {
        fprintf(stderr, "sys/info INFO read failed: %d\n", n);
        (void)zi_end(h);
        return 0;
      }
      if (n == 0) break;
      got += (uint32_t)n;
      if (got >= 24) {
        uint32_t pl = zcl1_read_u32(resp + 20);
        if (got >= 24u + pl) break;
      }
    }
    if (got < 24 || zcl1_status(resp) != 1) {
      fprintf(stderr, "sys/info INFO bad response\n");
      (void)zi_end(h);
      return 0;
    }
    uint32_t pl = zcl1_read_u32(resp + 20);
    if (pl < 16) {
      fprintf(stderr, "sys/info INFO payload too small\n");
      (void)zi_end(h);
      return 0;
    }
    if (zcl1_read_u32(resp + 24 + 0) != 1u) {
      fprintf(stderr, "sys/info INFO version mismatch\n");
      (void)zi_end(h);
      return 0;
    }
    info_flags = zcl1_read_u32(resp + 24 + 4);
    info_cpu = zcl1_read_u32(resp + 24 + 8);
    info_ps = zcl1_read_u32(resp + 24 + 12);
  }

  // TIME_NOW
  uint64_t realtime_ns = 0;
  uint64_t monotonic_ns = 0;
  {
    uint8_t fr[24];
    build_zcl1_req(fr, (uint16_t)ZI_SYS_INFO_OP_TIME_NOW, 31, NULL, 0);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "sys/info TIME_NOW write failed\n");
      (void)zi_end(h);
      return 0;
    }

    uint32_t got = 0;
    for (;;) {
      int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(resp + got), (zi_size32_t)(sizeof(resp) - got));
      if (n == ZI_E_AGAIN) continue;
      if (n < 0) {
        fprintf(stderr, "sys/info TIME_NOW read failed: %d\n", n);
        (void)zi_end(h);
        return 0;
      }
      if (n == 0) break;
      got += (uint32_t)n;
      if (got >= 24) {
        uint32_t pl = zcl1_read_u32(resp + 20);
        if (got >= 24u + pl) break;
      }
    }
    if (got < 24 || zcl1_status(resp) != 1) {
      fprintf(stderr, "sys/info TIME_NOW bad response\n");
      (void)zi_end(h);
      return 0;
    }
    uint32_t pl = zcl1_read_u32(resp + 20);
    if (pl != 20) {
      fprintf(stderr, "sys/info TIME_NOW payload size mismatch\n");
      (void)zi_end(h);
      return 0;
    }
    if (zcl1_read_u32(resp + 24 + 0) != 1u) {
      fprintf(stderr, "sys/info TIME_NOW version mismatch\n");
      (void)zi_end(h);
      return 0;
    }
    realtime_ns = zcl1_read_u64(resp + 24 + 4);
    monotonic_ns = zcl1_read_u64(resp + 24 + 12);
  }

  // RANDOM_SEED
  {
    uint8_t fr[24];
    build_zcl1_req(fr, (uint16_t)ZI_SYS_INFO_OP_RANDOM_SEED, 32, NULL, 0);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "sys/info RANDOM_SEED write failed\n");
      (void)zi_end(h);
      return 0;
    }

    uint32_t got = 0;
    for (;;) {
      int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(resp + got), (zi_size32_t)(sizeof(resp) - got));
      if (n == ZI_E_AGAIN) continue;
      if (n < 0) {
        fprintf(stderr, "sys/info RANDOM_SEED read failed: %d\n", n);
        (void)zi_end(h);
        return 0;
      }
      if (n == 0) break;
      got += (uint32_t)n;
      if (got >= 24) {
        uint32_t pl = zcl1_read_u32(resp + 20);
        if (got >= 24u + pl) break;
      }
    }
    if (got < 24 || zcl1_status(resp) != 1) {
      fprintf(stderr, "sys/info RANDOM_SEED bad response\n");
      (void)zi_end(h);
      return 0;
    }
    uint32_t pl = zcl1_read_u32(resp + 20);
    if (pl != 40) {
      fprintf(stderr, "sys/info RANDOM_SEED payload size mismatch\n");
      (void)zi_end(h);
      return 0;
    }
    if (zcl1_read_u32(resp + 24 + 0) != 1u) {
      fprintf(stderr, "sys/info RANDOM_SEED version mismatch\n");
      (void)zi_end(h);
      return 0;
    }
    uint32_t seed_len = zcl1_read_u32(resp + 24 + 4);
    if (seed_len != 32u) {
      fprintf(stderr, "sys/info RANDOM_SEED seed_len mismatch\n");
      (void)zi_end(h);
      return 0;
    }
    const uint8_t *seed = resp + 24 + 8;
    uint8_t acc = 0;
    for (uint32_t i = 0; i < 32; i++) acc |= seed[i];
    if (acc == 0) {
      fprintf(stderr, "sys/info RANDOM_SEED all-zero seed\n");
      (void)zi_end(h);
      return 0;
    }
  }

  // STATS
  {
    uint8_t fr[24];
    build_zcl1_req(fr, (uint16_t)ZI_SYS_INFO_OP_STATS, 33, NULL, 0);
    if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "sys/info STATS write failed\n");
      (void)zi_end(h);
      return 0;
    }

    uint32_t got = 0;
    for (;;) {
      int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(resp + got), (zi_size32_t)(sizeof(resp) - got));
      if (n == ZI_E_AGAIN) continue;
      if (n < 0) {
        fprintf(stderr, "sys/info STATS read failed: %d\n", n);
        (void)zi_end(h);
        return 0;
      }
      if (n == 0) break;
      got += (uint32_t)n;
      if (got >= 24) {
        uint32_t pl = zcl1_read_u32(resp + 20);
        if (got >= 24u + pl) break;
      }
    }
    if (got < 24 || zcl1_status(resp) != 1) {
      fprintf(stderr, "sys/info STATS bad response\n");
      (void)zi_end(h);
      return 0;
    }

    uint32_t pl = zcl1_read_u32(resp + 20);
    if (pl < 16) {
      fprintf(stderr, "sys/info STATS payload too small\n");
      (void)zi_end(h);
      return 0;
    }
    if (zcl1_read_u32(resp + 24 + 0) != 1u) {
      fprintf(stderr, "sys/info STATS version mismatch\n");
      (void)zi_end(h);
      return 0;
    }

    uint32_t flags = zcl1_read_u32(resp + 24 + 4);
    uint64_t stats_realtime_ns = zcl1_read_u64(resp + 24 + 8);
    uint32_t off = 24u + 16u;

    fprintf(stderr, "sys/stats v1 flags=0x%08x realtime_ns=%llu", flags, (unsigned long long)stats_realtime_ns);

    if (flags & 0x1u) {
      if (off + 12u > 24u + pl) {
        fprintf(stderr, "\n");
        fprintf(stderr, "sys/info STATS load section truncated\n");
        (void)zi_end(h);
        return 0;
      }
      uint32_t l1 = zcl1_read_u32(resp + off + 0);
      uint32_t l5 = zcl1_read_u32(resp + off + 4);
      uint32_t l15 = zcl1_read_u32(resp + off + 8);
      off += 12u;
      fprintf(stderr, " ");
      print_load_milli("load1", l1);
      fprintf(stderr, " ");
      print_load_milli("load5", l5);
      fprintf(stderr, " ");
      print_load_milli("load15", l15);
    }

    if (flags & 0x2u) {
      if (off + 20u > 24u + pl) {
        fprintf(stderr, "\n");
        fprintf(stderr, "sys/info STATS mem section truncated\n");
        (void)zi_end(h);
        return 0;
      }
      uint64_t mem_total = zcl1_read_u64(resp + off + 0);
      uint64_t mem_avail = zcl1_read_u64(resp + off + 8);
      uint32_t pressure = zcl1_read_u32(resp + off + 16);
      off += 20u;
      fprintf(stderr, " mem_total=%llu mem_avail=%llu pressure=%u.%03u", (unsigned long long)mem_total,
              (unsigned long long)mem_avail, pressure / 1000u, pressure % 1000u);
    }

    fprintf(stderr, "\n");
  }

  fprintf(stderr, "sys/info v1 cpu_count=%u page_size=%u flags=0x%08x realtime_ns=%llu monotonic_ns=%llu\n", info_cpu,
          info_ps, info_flags, (unsigned long long)realtime_ns, (unsigned long long)monotonic_ns);

  (void)zi_end(h);
  return 1;
}

static int event_bus_smoke(void) {
  // Open two event/bus handles: subscriber + publisher.
  uint8_t req_sub[40];
  uint8_t req_pub[40];
  build_open_req(req_sub, ZI_CAP_KIND_EVENT, ZI_CAP_NAME_BUS, NULL, 0);
  build_open_req(req_pub, ZI_CAP_KIND_EVENT, ZI_CAP_NAME_BUS, NULL, 0);

  zi_handle_t h_sub = zi_cap_open((zi_ptr_t)(uintptr_t)req_sub);
  zi_handle_t h_pub = zi_cap_open((zi_ptr_t)(uintptr_t)req_pub);
  if (h_sub < 3 || h_pub < 3) {
    fprintf(stderr, "event/bus open failed: sub=%d pub=%d\n", h_sub, h_pub);
    return 0;
  }

  const char *topic = "ui.click";
  const char *data = "left";
  uint32_t topic_len = (uint32_t)strlen(topic);
  uint32_t data_len = (uint32_t)strlen(data);

  // SUBSCRIBE on subscriber.
  uint32_t sub_id = 0;
  {
    uint8_t payload[128];
    uint32_t off = 0;
    zcl1_write_u32(payload + off, topic_len);
    off += 4;
    memcpy(payload + off, topic, topic_len);
    off += topic_len;
    zcl1_write_u32(payload + off, 0u);
    off += 4;

    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_EVENT_BUS_OP_SUBSCRIBE, 20, payload, off);
    if (zi_write(h_sub, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)(24u + off)) != (int32_t)(24u + off)) {
      fprintf(stderr, "event/bus SUBSCRIBE write failed\n");
      (void)zi_end(h_sub);
      (void)zi_end(h_pub);
      return 0;
    }

    uint8_t resp[256];
    uint32_t got = 0;
    for (;;) {
      int32_t n = zi_read(h_sub, (zi_ptr_t)(uintptr_t)(resp + got), (zi_size32_t)(sizeof(resp) - got));
      if (n == ZI_E_AGAIN) continue;
      if (n < 0) {
        fprintf(stderr, "event/bus SUBSCRIBE read failed: %d\n", n);
        (void)zi_end(h_sub);
        (void)zi_end(h_pub);
        return 0;
      }
      if (n == 0) break;
      got += (uint32_t)n;
      if (got >= 24) {
        uint32_t pl = zcl1_read_u32(resp + 20);
        if (got >= 24u + pl) break;
      }
    }

    if (got < 28 || zcl1_status(resp) != 1) {
      fprintf(stderr, "event/bus SUBSCRIBE bad response\n");
      (void)zi_end(h_sub);
      (void)zi_end(h_pub);
      return 0;
    }
    sub_id = zcl1_read_u32(resp + 24);
    if (sub_id == 0) {
      fprintf(stderr, "event/bus SUBSCRIBE returned sub_id=0\n");
      (void)zi_end(h_sub);
      (void)zi_end(h_pub);
      return 0;
    }
  }

  // PUBLISH on publisher (rid=22); expect delivered=1.
  {
    uint8_t payload[256];
    uint32_t off = 0;
    zcl1_write_u32(payload + off, topic_len);
    off += 4;
    memcpy(payload + off, topic, topic_len);
    off += topic_len;
    zcl1_write_u32(payload + off, data_len);
    off += 4;
    memcpy(payload + off, data, data_len);
    off += data_len;

    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_EVENT_BUS_OP_PUBLISH, 22, payload, off);
    if (zi_write(h_pub, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)(24u + off)) != (int32_t)(24u + off)) {
      fprintf(stderr, "event/bus PUBLISH write failed\n");
      (void)zi_end(h_sub);
      (void)zi_end(h_pub);
      return 0;
    }

    uint8_t resp[256];
    uint32_t got = 0;
    for (;;) {
      int32_t n = zi_read(h_pub, (zi_ptr_t)(uintptr_t)(resp + got), (zi_size32_t)(sizeof(resp) - got));
      if (n == ZI_E_AGAIN) continue;
      if (n < 0) {
        fprintf(stderr, "event/bus PUBLISH read failed: %d\n", n);
        (void)zi_end(h_sub);
        (void)zi_end(h_pub);
        return 0;
      }
      if (n == 0) break;
      got += (uint32_t)n;
      if (got >= 24) {
        uint32_t pl = zcl1_read_u32(resp + 20);
        if (got >= 24u + pl) break;
      }
    }
    if (got < 28 || zcl1_status(resp) != 1) {
      fprintf(stderr, "event/bus PUBLISH bad response\n");
      (void)zi_end(h_sub);
      (void)zi_end(h_pub);
      return 0;
    }
    if (zcl1_read_u32(resp + 24) != 1u) {
      fprintf(stderr, "event/bus PUBLISH expected delivered=1\n");
      (void)zi_end(h_sub);
      (void)zi_end(h_pub);
      return 0;
    }
  }

  // Subscriber must receive EVENT (op=100) with rid=22.
  {
    uint8_t ev[512];
    uint32_t got = 0;
    for (;;) {
      int32_t n = zi_read(h_sub, (zi_ptr_t)(uintptr_t)(ev + got), (zi_size32_t)(sizeof(ev) - got));
      if (n == ZI_E_AGAIN) continue;
      if (n < 0) {
        fprintf(stderr, "event/bus EVENT read failed: %d\n", n);
        (void)zi_end(h_sub);
        (void)zi_end(h_pub);
        return 0;
      }
      if (n == 0) break;
      got += (uint32_t)n;
      if (got >= 24) {
        uint32_t pl = zcl1_read_u32(ev + 20);
        if (got >= 24u + pl) break;
      }
    }

    if (got < 24 || zcl1_status(ev) != 1) {
      fprintf(stderr, "event/bus EVENT bad frame\n");
      (void)zi_end(h_sub);
      (void)zi_end(h_pub);
      return 0;
    }

    uint16_t op = (uint16_t)ev[6] | (uint16_t)((uint16_t)ev[7] << 8);
    uint32_t rid = zcl1_read_u32(ev + 8);
    if (op != (uint16_t)ZI_EVENT_BUS_EV_EVENT || rid != 22u) {
      fprintf(stderr, "event/bus EVENT op/rid mismatch\n");
      (void)zi_end(h_sub);
      (void)zi_end(h_pub);
      return 0;
    }

    uint32_t pl_len = zcl1_read_u32(ev + 20);
    if (pl_len < 16) {
      fprintf(stderr, "event/bus EVENT payload too small\n");
      (void)zi_end(h_sub);
      (void)zi_end(h_pub);
      return 0;
    }

    const uint8_t *pl = ev + 24;
    uint32_t got_sub_id = zcl1_read_u32(pl + 0);
    uint32_t got_topic_len = zcl1_read_u32(pl + 4);
    if (got_sub_id != sub_id || got_topic_len != topic_len) {
      fprintf(stderr, "event/bus EVENT sub/topic mismatch\n");
      (void)zi_end(h_sub);
      (void)zi_end(h_pub);
      return 0;
    }
    if (8u + got_topic_len + 4u > pl_len) {
      fprintf(stderr, "event/bus EVENT payload bounds mismatch\n");
      (void)zi_end(h_sub);
      (void)zi_end(h_pub);
      return 0;
    }
    if (memcmp(pl + 8, topic, topic_len) != 0) {
      fprintf(stderr, "event/bus EVENT topic bytes mismatch\n");
      (void)zi_end(h_sub);
      (void)zi_end(h_pub);
      return 0;
    }
    uint32_t off = 8u + got_topic_len;
    uint32_t got_data_len = zcl1_read_u32(pl + off);
    off += 4;
    if (off + got_data_len != pl_len || got_data_len != data_len) {
      fprintf(stderr, "event/bus EVENT data bounds mismatch\n");
      (void)zi_end(h_sub);
      (void)zi_end(h_pub);
      return 0;
    }
    if (memcmp(pl + off, data, data_len) != 0) {
      fprintf(stderr, "event/bus EVENT data mismatch\n");
      (void)zi_end(h_sub);
      (void)zi_end(h_pub);
      return 0;
    }
  }

  // UNSUBSCRIBE.
  {
    uint8_t payload[4];
    zcl1_write_u32(payload + 0, sub_id);

    uint8_t fr[24 + sizeof(payload)];
    build_zcl1_req(fr, (uint16_t)ZI_EVENT_BUS_OP_UNSUBSCRIBE, 30, payload, (uint32_t)sizeof(payload));
    if (zi_write(h_sub, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)sizeof(fr)) != (int32_t)sizeof(fr)) {
      fprintf(stderr, "event/bus UNSUBSCRIBE write failed\n");
      (void)zi_end(h_sub);
      (void)zi_end(h_pub);
      return 0;
    }

    uint8_t resp[256];
    uint32_t got = 0;
    for (;;) {
      int32_t n = zi_read(h_sub, (zi_ptr_t)(uintptr_t)(resp + got), (zi_size32_t)(sizeof(resp) - got));
      if (n == ZI_E_AGAIN) continue;
      if (n < 0) {
        fprintf(stderr, "event/bus UNSUBSCRIBE read failed: %d\n", n);
        (void)zi_end(h_sub);
        (void)zi_end(h_pub);
        return 0;
      }
      if (n == 0) break;
      got += (uint32_t)n;
      if (got >= 24) {
        uint32_t pl = zcl1_read_u32(resp + 20);
        if (got >= 24u + pl) break;
      }
    }
    if (got < 28 || zcl1_status(resp) != 1) {
      fprintf(stderr, "event/bus UNSUBSCRIBE bad response\n");
      (void)zi_end(h_sub);
      (void)zi_end(h_pub);
      return 0;
    }
    if (zcl1_read_u32(resp + 24) != 1u) {
      fprintf(stderr, "event/bus UNSUBSCRIBE expected removed=1\n");
      (void)zi_end(h_sub);
      (void)zi_end(h_pub);
      return 0;
    }
  }

  (void)zi_end(h_sub);
  (void)zi_end(h_pub);
  return 1;
}

static int async_smoke(void) {
  // Open async/default (no params).
  uint8_t req[40];
  build_open_req(req, ZI_CAP_KIND_ASYNC, ZI_CAP_NAME_DEFAULT, NULL, 0);

  zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)req);
  if (h < 3) {
    fprintf(stderr, "async/default open failed: %d\n", h);
    return 0;
  }

  // INVOKE ping.v1 (future_id=1, no params)
  uint8_t payload[128];
  uint32_t off = 0;
  const char *kind = ZI_CAP_KIND_ASYNC;
  const char *name = ZI_CAP_NAME_DEFAULT;
  const char *sel = "ping.v1";
  uint32_t klen = (uint32_t)strlen(kind);
  uint32_t nlen = (uint32_t)strlen(name);
  uint32_t slen = (uint32_t)strlen(sel);

  zcl1_write_u32(payload + off, klen);
  off += 4;
  memcpy(payload + off, kind, klen);
  off += klen;
  zcl1_write_u32(payload + off, nlen);
  off += 4;
  memcpy(payload + off, name, nlen);
  off += nlen;
  zcl1_write_u32(payload + off, slen);
  off += 4;
  memcpy(payload + off, sel, slen);
  off += slen;
  zcl1_write_u64(payload + off, 1u);
  off += 8;
  zcl1_write_u32(payload + off, 0u);
  off += 4;

  uint8_t fr[24 + sizeof(payload)];
  build_zcl1_req(fr, (uint16_t)ZI_ASYNC_OP_INVOKE, 10, payload, off);
  if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)(24u + off)) != (int32_t)(24u + off)) {
    fprintf(stderr, "async/default invoke write failed\n");
    (void)zi_end(h);
    return 0;
  }

  uint8_t buf[4096];
  uint32_t got = 0;
  for (;;) {
    int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(buf + got), (zi_size32_t)(sizeof(buf) - got));
    if (n == ZI_E_AGAIN) continue;
    if (n < 0) {
      fprintf(stderr, "async/default invoke read failed: %d\n", n);
      (void)zi_end(h);
      return 0;
    }
    if (n == 0) break;
    got += (uint32_t)n;
    if (got == sizeof(buf)) break;
    // Likely got everything; the handle returns E_AGAIN once drained.
    break;
  }

  int saw_future_ok = 0;
  uint32_t pos = 0;
  while (pos + 24 <= got) {
    uint32_t plen = zcl1_read_u32(buf + pos + 20);
    uint32_t flen = 24u + plen;
    if (pos + flen > got) break;
    uint16_t op = (uint16_t)buf[pos + 6] | (uint16_t)((uint16_t)buf[pos + 7] << 8);
    uint32_t status = zcl1_read_u32(buf + pos + 12);
    if (status != 1) {
      fprintf(stderr, "async/default frame error status\n");
      (void)zi_end(h);
      return 0;
    }
    if (op == (uint16_t)ZI_ASYNC_EV_FUTURE_OK) {
      if (plen < 12) {
        fprintf(stderr, "async/default future_ok payload too small\n");
        (void)zi_end(h);
        return 0;
      }
      uint64_t fid = (uint64_t)zcl1_read_u32(buf + pos + 24 + 0) | ((uint64_t)zcl1_read_u32(buf + pos + 24 + 4) << 32);
      uint32_t vlen = zcl1_read_u32(buf + pos + 24 + 8);
      if (fid != 1u || 12u + vlen != plen) {
        fprintf(stderr, "async/default future_ok payload mismatch\n");
        (void)zi_end(h);
        return 0;
      }
      if (vlen != 4 || memcmp(buf + pos + 24 + 12, "pong", 4) != 0) {
        fprintf(stderr, "async/default future_ok value mismatch\n");
        (void)zi_end(h);
        return 0;
      }
      saw_future_ok = 1;
    }
    pos += flen;
  }

  (void)zi_end(h);
  return saw_future_ok;
}

static int fs_smoke(void) {
  const char *root = getenv("ZI_FS_ROOT");
  const char *guest_path = "/all_caps_demo.txt";

  char tmpfile[256];
  if (!root || root[0] == '\0') {
    // No sandbox set: fall back to a concrete host path.
    // This demonstrates permissive behavior; it is *not* a sandbox.
    snprintf(tmpfile, sizeof(tmpfile), "/tmp/all_caps_demo_%ld.txt", (long)getpid());
    guest_path = tmpfile;
  }

  const char *msg = "hello from file/fs\n";

  uint8_t params[20];
  uint8_t req[40];

  // Write
  build_fs_params(params, guest_path, ZI_FILE_O_WRITE | ZI_FILE_O_CREATE | ZI_FILE_O_TRUNC, 0644);
  build_open_req(req, ZI_CAP_KIND_FILE, ZI_CAP_NAME_FS, params, (uint32_t)sizeof(params));
  zi_handle_t hw = zi_cap_open((zi_ptr_t)(uintptr_t)req);
  if (hw < 3) {
    fprintf(stderr, "file/fs open(write) failed: %d\n", hw);
    return 0;
  }
  int32_t wn = zi_write(hw, (zi_ptr_t)(uintptr_t)msg, (zi_size32_t)strlen(msg));
  if (wn != (int32_t)strlen(msg)) {
    fprintf(stderr, "file/fs write failed: %d\n", wn);
    (void)zi_end(hw);
    return 0;
  }
  (void)zi_end(hw);

  // Read
  build_fs_params(params, guest_path, ZI_FILE_O_READ, 0);
  build_open_req(req, ZI_CAP_KIND_FILE, ZI_CAP_NAME_FS, params, (uint32_t)sizeof(params));
  zi_handle_t hr = zi_cap_open((zi_ptr_t)(uintptr_t)req);
  if (hr < 3) {
    fprintf(stderr, "file/fs open(read) failed: %d\n", hr);
    return 0;
  }
  char buf[128];
  memset(buf, 0, sizeof(buf));
  int32_t rn = zi_read(hr, (zi_ptr_t)(uintptr_t)buf, (zi_size32_t)(sizeof(buf) - 1));
  (void)zi_end(hr);
  if (rn <= 0) {
    fprintf(stderr, "file/fs read failed: %d\n", rn);
    return 0;
  }
  if ((size_t)rn != strlen(msg) || memcmp(buf, msg, (size_t)rn) != 0) {
    fprintf(stderr, "file/fs content mismatch\n");
    return 0;
  }

  return 1;
}

static zi_cap_v1 cap_stdio_v1 = {
    .kind = "file",
    .name = "stdio",
    .version = 1,
    .cap_flags = 0,
    .meta = (const uint8_t *)"{\"handles\":[\"in\",\"out\",\"err\"]}",
    .meta_len = 34,
};

static zi_cap_v1 cap_demo_echo_v1 = {
    .kind = "demo",
    .name = "echo",
    .version = 1,
    .cap_flags = 0,
    .meta = NULL,
    .meta_len = 0,
};

static zi_cap_v1 cap_demo_version_v1 = {
    .kind = "demo",
    .name = "version",
    .version = 1,
    .cap_flags = 0,
    .meta = (const uint8_t *)"{\"impl\":\"all_caps_demo\"}",
    .meta_len = 25,
};

extern const char **environ;

int main(int argc, char **argv) {
  if (!zingcore25_init()) {
    fprintf(stderr, "zingcore25_init failed\n");
    return 1;
  }

  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);

  zi_host_v1 host;
  memset(&host, 0, sizeof(host));
  host.telemetry = host_telemetry;
  zi_runtime25_set_host(&host);

  // Register all known caps in this build.
  (void)zi_cap_register(&cap_stdio_v1);
  (void)zi_cap_register(&cap_demo_echo_v1);
  (void)zi_cap_register(&cap_demo_version_v1);
  (void)zi_async_default25_register();
  (void)zi_event_bus25_register();
  (void)zi_file_fs25_register();
  (void)zi_net_tcp25_register();
  (void)zi_proc_argv25_register();
  (void)zi_proc_env25_register();
  (void)zi_proc_hopper25_register();
  (void)zi_sys_info25_register();

  if (!zi_async_default25_register_selectors()) {
    fprintf(stderr, "async/default selector registration failed\n");
    return 1;
  }

  // Wire stdio handles.
  (void)zi_handles25_init();
  static fd_stream s_in = {.fd = 0, .close_on_end = 0};
  static fd_stream s_out = {.fd = 1, .close_on_end = 0};
  static fd_stream s_err = {.fd = 2, .close_on_end = 0};

  zi_handle_t h_in = zi_handle25_alloc(&fd_ops, &s_in, ZI_H_READABLE);
  zi_handle_t h_out = zi_handle25_alloc(&fd_ops, &s_out, ZI_H_WRITABLE);
  zi_handle_t h_err = zi_handle25_alloc(&fd_ops, &s_err, ZI_H_WRITABLE);

  if (h_in == 0 || h_out == 0 || h_err == 0) {
    fprintf(stderr, "failed to allocate stdio handles\n");
    return 1;
  }

  const char *banner = "all_caps_demo: caps + argv + file/fs\n";
  (void)zi_write(h_out, (zi_ptr_t)(uintptr_t)banner, (zi_size32_t)strlen(banner));

  dump_caps_list();

  if (!dump_argv_via_cap(argc, argv)) {
    fprintf(stderr, "argv cap failed\n");
    return 1;
  }

  if (!dump_env_via_cap((const char *const *)environ)) {
    fprintf(stderr, "env cap failed\n");
    return 1;
  }

  if (!async_smoke()) {
    fprintf(stderr, "async/default smoke failed\n");
    return 1;
  }

  if (!event_bus_smoke()) {
    fprintf(stderr, "event/bus smoke failed\n");
    return 1;
  }

  if (!sys_info_smoke()) {
    fprintf(stderr, "sys/info smoke failed\n");
    return 1;
  }

  if (!fs_smoke()) {
    fprintf(stderr, "file/fs smoke failed\n");
    return 1;
  }

  if (!hopper_smoke()) {
    fprintf(stderr, "hopper smoke failed\n");
    return 1;
  }

  (void)zi_write(h_err, (zi_ptr_t)(uintptr_t)"ok\n", 3);
  return 0;
}
