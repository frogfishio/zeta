#include "zi_caps.h"
#include "zi_file_aio25.h"
#include "zi_handles25.h"
#include "zi_runtime25.h"
#include "zi_sys_loop25.h"
#include "zi_sysabi25.h"
#include "zi_zcl1.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_u32le(uint8_t *p, uint32_t v) { zi_zcl1_write_u32(p, v); }

static void write_u64le(uint8_t *p, uint64_t v) {
  write_u32le(p + 0, (uint32_t)(v & 0xFFFFFFFFu));
  write_u32le(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static uint64_t read_u64le(const uint8_t *p);

static void build_open_req(uint8_t req[40], const char *kind, const char *name, const void *params, uint32_t params_len) {
  write_u64le(req + 0, (uint64_t)(uintptr_t)kind);
  write_u32le(req + 8, (uint32_t)strlen(kind));
  write_u64le(req + 12, (uint64_t)(uintptr_t)name);
  write_u32le(req + 20, (uint32_t)strlen(name));
  write_u32le(req + 24, 0);
  write_u64le(req + 28, (uint64_t)(uintptr_t)params);
  write_u32le(req + 36, params_len);
}

static int write_all_handle(zi_handle_t h, const void *p, uint32_t n) {
  const uint8_t *b = (const uint8_t *)p;
  uint32_t off = 0;
  while (off < n) {
    int32_t w = zi_write(h, (zi_ptr_t)(uintptr_t)(b + off), (zi_size32_t)(n - off));
    if (w < 0) return (int)w;
    if (w == 0) return -1;
    off += (uint32_t)w;
  }
  return 0;
}

static int read_some(zi_handle_t h, uint8_t *buf, uint32_t cap, uint32_t *inout_have) {
  if (!buf || !inout_have || *inout_have >= cap) return 0;
  int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(buf + *inout_have), (zi_size32_t)(cap - *inout_have));
  if (n < 0) return (int)n;
  if (n == 0) return -1;
  *inout_have += (uint32_t)n;
  return 1;
}

static void build_zcl1_req(uint8_t *out, uint16_t op, uint32_t rid, const uint8_t *payload, uint32_t payload_len) {
  memcpy(out + 0, "ZCL1", 4);
  zi_zcl1_write_u16(out + 4, 1);
  zi_zcl1_write_u16(out + 6, op);
  zi_zcl1_write_u32(out + 8, rid);
  zi_zcl1_write_u32(out + 12, 0);
  zi_zcl1_write_u32(out + 16, 0);
  zi_zcl1_write_u32(out + 20, payload_len);
  if (payload_len && payload) memcpy(out + 24, payload, payload_len);
}

static int expect_ok_frame(const uint8_t *fr, uint32_t fr_len, uint16_t op, uint32_t rid) {
  zi_zcl1_frame z;
  if (!zi_zcl1_parse(fr, fr_len, &z)) return 0;
  if (z.op != op || z.rid != rid) return 0;
  if (zi_zcl1_read_u32(fr + 12) != 1u) return 0;
  return 1;
}

static int loop_watch(zi_handle_t loop_h, zi_handle_t target_h, uint32_t events, uint64_t watch_id) {
  uint8_t watch_pl[20];
  zi_zcl1_write_u32(watch_pl + 0, (uint32_t)target_h);
  zi_zcl1_write_u32(watch_pl + 4, events);
  write_u64le(watch_pl + 8, watch_id);
  zi_zcl1_write_u32(watch_pl + 16, 0);

  uint8_t req[24 + 20];
  build_zcl1_req(req, (uint16_t)ZI_SYS_LOOP_OP_WATCH, 1u, watch_pl, (uint32_t)sizeof(watch_pl));
  if (write_all_handle(loop_h, req, (uint32_t)sizeof(req)) != 0) return 0;

  uint8_t fr[256];
  uint32_t have = 0;
  for (;;) {
    int r = read_some(loop_h, fr, (uint32_t)sizeof(fr), &have);
    if (r == ZI_E_AGAIN) continue;
    if (r <= 0) return 0;
    if (have >= 24) {
      uint32_t pl = zi_zcl1_read_u32(fr + 20);
      if (have >= 24u + pl) break;
    }
  }
  return expect_ok_frame(fr, have, (uint16_t)ZI_SYS_LOOP_OP_WATCH, 1u);
}

static int loop_unwatch(zi_handle_t loop_h, uint64_t watch_id) {
  uint8_t unwatch_pl[8];
  write_u64le(unwatch_pl + 0, watch_id);

  uint8_t req[24 + sizeof(unwatch_pl)];
  build_zcl1_req(req, (uint16_t)ZI_SYS_LOOP_OP_UNWATCH, 3u, unwatch_pl, (uint32_t)sizeof(unwatch_pl));
  if (write_all_handle(loop_h, req, (uint32_t)sizeof(req)) != 0) return 0;

  uint8_t fr[256];
  uint32_t have = 0;
  for (;;) {
    int r = read_some(loop_h, fr, (uint32_t)sizeof(fr), &have);
    if (r == ZI_E_AGAIN) continue;
    if (r <= 0) return 0;
    if (have >= 24) {
      uint32_t pl = zi_zcl1_read_u32(fr + 20);
      if (have >= 24u + pl) break;
    }
  }
  return expect_ok_frame(fr, have, (uint16_t)ZI_SYS_LOOP_OP_UNWATCH, 3u);
}

static int loop_wait_readable(zi_handle_t loop_h, zi_handle_t target_h, uint64_t watch_id, uint32_t timeout_ms) {
  uint8_t poll_pl[8];
  zi_zcl1_write_u32(poll_pl + 0, 8u); // max_events
  zi_zcl1_write_u32(poll_pl + 4, timeout_ms);

  uint8_t req[24 + sizeof(poll_pl)];
  build_zcl1_req(req, (uint16_t)ZI_SYS_LOOP_OP_POLL, 2u, poll_pl, (uint32_t)sizeof(poll_pl));
  if (write_all_handle(loop_h, req, (uint32_t)sizeof(req)) != 0) return 0;

  uint8_t fr[65536];
  uint32_t have = 0;
  for (;;) {
    int r = read_some(loop_h, fr, (uint32_t)sizeof(fr), &have);
    if (r == ZI_E_AGAIN) continue;
    if (r <= 0) return 0;
    if (have >= 24) {
      uint32_t pl = zi_zcl1_read_u32(fr + 20);
      if (have >= 24u + pl) break;
    }
  }

  zi_zcl1_frame z;
  if (!zi_zcl1_parse(fr, have, &z)) return 0;
  if (z.op != (uint16_t)ZI_SYS_LOOP_OP_POLL || z.rid != 2u) return 0;
  if (zi_zcl1_read_u32(fr + 12) != 1u) return 0;

  // payload:
  //   u32 ver
  //   u32 flags
  //   u32 count
  //   u32 reserved
  //   events[count] each 32 bytes
  if (z.payload_len < 16u) return 0;
  uint32_t count = zi_zcl1_read_u32(z.payload + 8);
  const uint8_t *p = z.payload + 16;
  uint32_t left = z.payload_len - 16u;
  for (uint32_t i = 0; i < count; i++) {
    if (left < 32u) return 0;
    uint32_t kind = zi_zcl1_read_u32(p + 0);
    uint32_t events = zi_zcl1_read_u32(p + 4);
    uint32_t handle = zi_zcl1_read_u32(p + 8);
    uint64_t id = read_u64le(p + 16);
    if (kind == 1u && handle == (uint32_t)target_h && id == watch_id && (events & 0x1u)) {
      return 1;
    }
    p += 32u;
    left -= 32u;
  }

  return 0;
}

static int loop_wait_writable(zi_handle_t loop_h, zi_handle_t target_h, uint64_t watch_id, uint32_t timeout_ms) {
  uint8_t poll_pl[8];
  zi_zcl1_write_u32(poll_pl + 0, 8u); // max_events
  zi_zcl1_write_u32(poll_pl + 4, timeout_ms);

  uint8_t req[24 + sizeof(poll_pl)];
  build_zcl1_req(req, (uint16_t)ZI_SYS_LOOP_OP_POLL, 2u, poll_pl, (uint32_t)sizeof(poll_pl));
  if (write_all_handle(loop_h, req, (uint32_t)sizeof(req)) != 0) return 0;

  uint8_t fr[65536];
  uint32_t have = 0;
  for (;;) {
    int r = read_some(loop_h, fr, (uint32_t)sizeof(fr), &have);
    if (r == ZI_E_AGAIN) continue;
    if (r <= 0) return 0;
    if (have >= 24) {
      uint32_t pl = zi_zcl1_read_u32(fr + 20);
      if (have >= 24u + pl) break;
    }
  }

  zi_zcl1_frame z;
  if (!zi_zcl1_parse(fr, have, &z)) return 0;
  if (z.op != (uint16_t)ZI_SYS_LOOP_OP_POLL || z.rid != 2u) return 0;
  if (zi_zcl1_read_u32(fr + 12) != 1u) return 0;
  if (z.payload_len < 16u) return 0;

  const uint8_t *pl = z.payload;
  uint32_t ver = zi_zcl1_read_u32(pl + 0);
  uint32_t n = zi_zcl1_read_u32(pl + 8);
  if (ver != 1u) return 0;

  const uint8_t *ev = pl + 16;
  uint32_t left = z.payload_len - 16u;
  for (uint32_t i = 0; i < n; i++) {
    if (left < 32u) break;
    uint32_t kind = zi_zcl1_read_u32(ev + 0);
    uint32_t events = zi_zcl1_read_u32(ev + 4);
    uint32_t handle = zi_zcl1_read_u32(ev + 8);
    uint64_t id = read_u64le(ev + 16);
    if (kind == 1u && handle == (uint32_t)target_h && id == watch_id && (events & 0x2u)) {
      return 1;
    }
    ev += 32u;
    left -= 32u;
  }
  return 0;
}

static int read_full_frame_wait(zi_handle_t loop_h, zi_handle_t h, uint64_t watch_id, uint8_t *out, uint32_t cap, uint32_t timeout_ms) {
  uint32_t have = 0;
  while (have < 24) {
    uint32_t want = 24u - have;
    if (want > (cap - have)) want = cap - have;
    int32_t r = zi_read(h, (zi_ptr_t)(uintptr_t)(out + have), (zi_size32_t)want);
    if (r == ZI_E_AGAIN) {
      if (!loop_wait_readable(loop_h, h, watch_id, timeout_ms)) return 0;
      continue;
    }
    if (r <= 0) return 0;
    have += (uint32_t)r;
  }
  uint32_t pl = zi_zcl1_read_u32(out + 20);
  uint32_t need = 24u + pl;
  if (need > cap) return 0;
  while (have < need) {
    int32_t r = zi_read(h, (zi_ptr_t)(uintptr_t)(out + have), (zi_size32_t)(need - have));
    if (r == ZI_E_AGAIN) {
      if (!loop_wait_readable(loop_h, h, watch_id, timeout_ms)) return 0;
      continue;
    }
    if (r <= 0) return 0;
    have += (uint32_t)r;
  }
  return (int)need;
}

static uint64_t read_u64le(const uint8_t *p) {
  uint64_t lo = (uint64_t)zi_zcl1_read_u32(p + 0);
  uint64_t hi = (uint64_t)zi_zcl1_read_u32(p + 4);
  return lo | (hi << 32);
}

int main(void) {
  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);

  zi_caps_reset_for_test();
  zi_handles25_reset_for_test();

  if (!zi_caps_init()) {
    fprintf(stderr, "zi_caps_init failed\n");
    return 1;
  }
  if (!zi_file_aio25_register()) {
    fprintf(stderr, "zi_file_aio25_register failed\n");
    return 1;
  }
  if (!zi_sys_loop25_register()) {
    fprintf(stderr, "zi_sys_loop25_register failed\n");
    return 1;
  }

  char root_template[] = "/tmp/zi_fs_root_XXXXXX";
  char *root = mkdtemp(root_template);
  if (!root) {
    perror("mkdtemp");
    return 1;
  }
  if (setenv("ZI_FS_ROOT", root, 1) != 0) {
    perror("setenv");
    return 1;
  }

  // Used by the backpressure readiness smoke test (kept open until after zi_end(aio_h)
  // so the file/aio worker can't re-block on FIFO opens during shutdown).
  int fifo_wfd = -1;
  char fifo_host_keep[512];
  int have_fifo_keep = 0;

  // Open file/aio
  uint8_t open_req[40];
  build_open_req(open_req, ZI_CAP_KIND_FILE, ZI_CAP_NAME_AIO, NULL, 0);
  zi_handle_t aio_h = zi_cap_open((zi_ptr_t)(uintptr_t)open_req);
  if (aio_h < 3) {
    fprintf(stderr, "file/aio open failed: %d\n", aio_h);
    return 1;
  }

  // Open sys/loop
  build_open_req(open_req, ZI_CAP_KIND_SYS, ZI_CAP_NAME_LOOP, NULL, 0);
  zi_handle_t loop_h = zi_cap_open((zi_ptr_t)(uintptr_t)open_req);
  if (loop_h < 3) {
    fprintf(stderr, "sys/loop open failed: %d\n", loop_h);
    (void)zi_end(aio_h);
    return 1;
  }

  const uint64_t WATCH_AIO = 0xA10A10A1ull;
  if (!loop_watch(loop_h, aio_h, 0x1u, WATCH_AIO)) {
    fprintf(stderr, "loop WATCH aio failed\n");
    (void)zi_end(loop_h);
    (void)zi_end(aio_h);
    return 1;
  }

  const char *guest_path = "/hello.txt";
  const char msg[] = "hello aio\n";

  // Submit OPEN (rid=1)
  uint8_t open_pl[20];
  write_u64le(open_pl + 0, (uint64_t)(uintptr_t)guest_path);
  write_u32le(open_pl + 8, (uint32_t)strlen(guest_path));
  write_u32le(open_pl + 12, ZI_FILE_O_READ | ZI_FILE_O_WRITE | ZI_FILE_O_CREATE | ZI_FILE_O_TRUNC);
  write_u32le(open_pl + 16, 0644);

  uint8_t req[24 + 64];
  build_zcl1_req(req, (uint16_t)ZI_FILE_AIO_OP_OPEN, 1u, open_pl, (uint32_t)sizeof(open_pl));
  if (write_all_handle(aio_h, req, 24u + (uint32_t)sizeof(open_pl)) != 0) {
    fprintf(stderr, "aio OPEN write failed\n");
    return 1;
  }

  uint8_t fr[65536];
  int n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !expect_ok_frame(fr, (uint32_t)n, (uint16_t)ZI_FILE_AIO_OP_OPEN, 1u)) {
    fprintf(stderr, "aio OPEN ack failed\n");
    return 1;
  }

  // Completion
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0) {
    fprintf(stderr, "aio OPEN done missing\n");
    return 1;
  }
  zi_zcl1_frame z;
  if (!zi_zcl1_parse(fr, (uint32_t)n, &z) || z.op != (uint16_t)ZI_FILE_AIO_EV_DONE || z.rid != 1u) {
    fprintf(stderr, "aio OPEN done bad frame\n");
    return 1;
  }
  if (zi_zcl1_read_u32(fr + 12) != 1u || z.payload_len != 16u) {
    fprintf(stderr, "aio OPEN done bad status/payload\n");
    return 1;
  }
  if (zi_zcl1_read_u16(z.payload + 0) != (uint16_t)ZI_FILE_AIO_OP_OPEN) {
    fprintf(stderr, "aio OPEN done orig_op mismatch\n");
    return 1;
  }
  uint64_t file_id = read_u64le(z.payload + 8);
  if (file_id == 0) {
    fprintf(stderr, "aio OPEN got file_id=0\n");
    return 1;
  }

  // Submit WRITE (rid=2)
  uint8_t write_pl[32];
  write_u64le(write_pl + 0, file_id);
  write_u64le(write_pl + 8, 0);
  write_u64le(write_pl + 16, (uint64_t)(uintptr_t)msg);
  write_u32le(write_pl + 24, (uint32_t)strlen(msg));
  write_u32le(write_pl + 28, 0);
  build_zcl1_req(req, (uint16_t)ZI_FILE_AIO_OP_WRITE, 2u, write_pl, (uint32_t)sizeof(write_pl));
  if (write_all_handle(aio_h, req, 24u + (uint32_t)sizeof(write_pl)) != 0) {
    fprintf(stderr, "aio WRITE write failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !expect_ok_frame(fr, (uint32_t)n, (uint16_t)ZI_FILE_AIO_OP_WRITE, 2u)) {
    fprintf(stderr, "aio WRITE ack failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !zi_zcl1_parse(fr, (uint32_t)n, &z) || z.op != (uint16_t)ZI_FILE_AIO_EV_DONE || z.rid != 2u) {
    fprintf(stderr, "aio WRITE done bad\n");
    return 1;
  }
  if (zi_zcl1_read_u32(fr + 12) != 1u || z.payload_len != 8u) {
    fprintf(stderr, "aio WRITE done bad status/payload\n");
    return 1;
  }
  if (zi_zcl1_read_u16(z.payload + 0) != (uint16_t)ZI_FILE_AIO_OP_WRITE) {
    fprintf(stderr, "aio WRITE done orig_op mismatch\n");
    return 1;
  }
  if (zi_zcl1_read_u32(z.payload + 4) != (uint32_t)strlen(msg)) {
    fprintf(stderr, "aio WRITE done result mismatch\n");
    return 1;
  }

  // Submit READ (rid=3)
  uint8_t read_pl[24];
  write_u64le(read_pl + 0, file_id);
  write_u64le(read_pl + 8, 0);
  write_u32le(read_pl + 16, 64u);
  write_u32le(read_pl + 20, 0u);
  build_zcl1_req(req, (uint16_t)ZI_FILE_AIO_OP_READ, 3u, read_pl, (uint32_t)sizeof(read_pl));
  if (write_all_handle(aio_h, req, 24u + (uint32_t)sizeof(read_pl)) != 0) {
    fprintf(stderr, "aio READ write failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !expect_ok_frame(fr, (uint32_t)n, (uint16_t)ZI_FILE_AIO_OP_READ, 3u)) {
    fprintf(stderr, "aio READ ack failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !zi_zcl1_parse(fr, (uint32_t)n, &z) || z.op != (uint16_t)ZI_FILE_AIO_EV_DONE || z.rid != 3u) {
    fprintf(stderr, "aio READ done bad\n");
    return 1;
  }
  if (zi_zcl1_read_u32(fr + 12) != 1u || z.payload_len < 8u) {
    fprintf(stderr, "aio READ done bad status/payload\n");
    return 1;
  }
  if (zi_zcl1_read_u16(z.payload + 0) != (uint16_t)ZI_FILE_AIO_OP_READ) {
    fprintf(stderr, "aio READ done orig_op mismatch\n");
    return 1;
  }
  uint32_t got = zi_zcl1_read_u32(z.payload + 4);
  if (got != (uint32_t)strlen(msg) || z.payload_len != 8u + got) {
    fprintf(stderr, "aio READ done length mismatch\n");
    return 1;
  }
  if (memcmp(z.payload + 8, msg, got) != 0) {
    fprintf(stderr, "aio READ content mismatch\n");
    return 1;
  }

  // Submit CLOSE (rid=4)
  uint8_t close_pl[8];
  write_u64le(close_pl, file_id);
  build_zcl1_req(req, (uint16_t)ZI_FILE_AIO_OP_CLOSE, 4u, close_pl, (uint32_t)sizeof(close_pl));
  if (write_all_handle(aio_h, req, 24u + (uint32_t)sizeof(close_pl)) != 0) {
    fprintf(stderr, "aio CLOSE write failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !expect_ok_frame(fr, (uint32_t)n, (uint16_t)ZI_FILE_AIO_OP_CLOSE, 4u)) {
    fprintf(stderr, "aio CLOSE ack failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !zi_zcl1_parse(fr, (uint32_t)n, &z) || z.op != (uint16_t)ZI_FILE_AIO_EV_DONE || z.rid != 4u) {
    fprintf(stderr, "aio CLOSE done bad\n");
    return 1;
  }
  if (zi_zcl1_read_u32(fr + 12) != 1u || z.payload_len != 8u) {
    fprintf(stderr, "aio CLOSE done bad status/payload\n");
    return 1;
  }

  // Directory ops: MKDIR + OPEN + WRITE + STAT + READDIR + UNLINK + RMDIR
  const char *dir_path = "/dir1";
  const char *inner_path = "/dir1/inner.txt";
  const char inner_msg[] = "inner\n";

  // MKDIR (rid=6)
  uint8_t mkdir_pl[20];
  write_u64le(mkdir_pl + 0, (uint64_t)(uintptr_t)dir_path);
  write_u32le(mkdir_pl + 8, (uint32_t)strlen(dir_path));
  write_u32le(mkdir_pl + 12, 0755u);
  write_u32le(mkdir_pl + 16, 0u);
  build_zcl1_req(req, (uint16_t)ZI_FILE_AIO_OP_MKDIR, 6u, mkdir_pl, (uint32_t)sizeof(mkdir_pl));
  if (write_all_handle(aio_h, req, 24u + (uint32_t)sizeof(mkdir_pl)) != 0) {
    fprintf(stderr, "aio MKDIR write failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !expect_ok_frame(fr, (uint32_t)n, (uint16_t)ZI_FILE_AIO_OP_MKDIR, 6u)) {
    fprintf(stderr, "aio MKDIR ack failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !zi_zcl1_parse(fr, (uint32_t)n, &z) || z.op != (uint16_t)ZI_FILE_AIO_EV_DONE || z.rid != 6u) {
    fprintf(stderr, "aio MKDIR done bad\n");
    return 1;
  }
  if (zi_zcl1_read_u32(fr + 12) != 1u || z.payload_len != 8u || zi_zcl1_read_u16(z.payload + 0) != (uint16_t)ZI_FILE_AIO_OP_MKDIR) {
    fprintf(stderr, "aio MKDIR done payload mismatch\n");
    return 1;
  }

  // OPEN inner (rid=7)
  uint8_t open2_pl[20];
  write_u64le(open2_pl + 0, (uint64_t)(uintptr_t)inner_path);
  write_u32le(open2_pl + 8, (uint32_t)strlen(inner_path));
  write_u32le(open2_pl + 12, ZI_FILE_O_READ | ZI_FILE_O_WRITE | ZI_FILE_O_CREATE | ZI_FILE_O_TRUNC);
  write_u32le(open2_pl + 16, 0644u);
  build_zcl1_req(req, (uint16_t)ZI_FILE_AIO_OP_OPEN, 7u, open2_pl, (uint32_t)sizeof(open2_pl));
  if (write_all_handle(aio_h, req, 24u + (uint32_t)sizeof(open2_pl)) != 0) {
    fprintf(stderr, "aio OPEN(inner) write failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !expect_ok_frame(fr, (uint32_t)n, (uint16_t)ZI_FILE_AIO_OP_OPEN, 7u)) {
    fprintf(stderr, "aio OPEN(inner) ack failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !zi_zcl1_parse(fr, (uint32_t)n, &z) || z.op != (uint16_t)ZI_FILE_AIO_EV_DONE || z.rid != 7u) {
    fprintf(stderr, "aio OPEN(inner) done bad\n");
    return 1;
  }
  if (zi_zcl1_read_u32(fr + 12) != 1u || z.payload_len != 16u || zi_zcl1_read_u16(z.payload + 0) != (uint16_t)ZI_FILE_AIO_OP_OPEN) {
    fprintf(stderr, "aio OPEN(inner) done payload mismatch\n");
    return 1;
  }
  uint64_t inner_id = read_u64le(z.payload + 8);
  if (inner_id == 0) {
    fprintf(stderr, "aio OPEN(inner) got file_id=0\n");
    return 1;
  }

  // WRITE inner (rid=8)
  uint8_t write2_pl[32];
  write_u64le(write2_pl + 0, inner_id);
  write_u64le(write2_pl + 8, 0);
  write_u64le(write2_pl + 16, (uint64_t)(uintptr_t)inner_msg);
  write_u32le(write2_pl + 24, (uint32_t)strlen(inner_msg));
  write_u32le(write2_pl + 28, 0u);
  build_zcl1_req(req, (uint16_t)ZI_FILE_AIO_OP_WRITE, 8u, write2_pl, (uint32_t)sizeof(write2_pl));
  if (write_all_handle(aio_h, req, 24u + (uint32_t)sizeof(write2_pl)) != 0) {
    fprintf(stderr, "aio WRITE(inner) write failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !expect_ok_frame(fr, (uint32_t)n, (uint16_t)ZI_FILE_AIO_OP_WRITE, 8u)) {
    fprintf(stderr, "aio WRITE(inner) ack failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !zi_zcl1_parse(fr, (uint32_t)n, &z) || z.op != (uint16_t)ZI_FILE_AIO_EV_DONE || z.rid != 8u) {
    fprintf(stderr, "aio WRITE(inner) done bad\n");
    return 1;
  }
  if (zi_zcl1_read_u32(fr + 12) != 1u || z.payload_len != 8u || zi_zcl1_read_u16(z.payload + 0) != (uint16_t)ZI_FILE_AIO_OP_WRITE) {
    fprintf(stderr, "aio WRITE(inner) done payload mismatch\n");
    return 1;
  }

  // STAT inner (rid=9)
  uint8_t stat_pl[16];
  write_u64le(stat_pl + 0, (uint64_t)(uintptr_t)inner_path);
  write_u32le(stat_pl + 8, (uint32_t)strlen(inner_path));
  write_u32le(stat_pl + 12, 0u);
  build_zcl1_req(req, (uint16_t)ZI_FILE_AIO_OP_STAT, 9u, stat_pl, (uint32_t)sizeof(stat_pl));
  if (write_all_handle(aio_h, req, 24u + (uint32_t)sizeof(stat_pl)) != 0) {
    fprintf(stderr, "aio STAT write failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !expect_ok_frame(fr, (uint32_t)n, (uint16_t)ZI_FILE_AIO_OP_STAT, 9u)) {
    fprintf(stderr, "aio STAT ack failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !zi_zcl1_parse(fr, (uint32_t)n, &z) || z.op != (uint16_t)ZI_FILE_AIO_EV_DONE || z.rid != 9u) {
    fprintf(stderr, "aio STAT done bad\n");
    return 1;
  }
  if (zi_zcl1_read_u32(fr + 12) != 1u || z.payload_len != 40u || zi_zcl1_read_u16(z.payload + 0) != (uint16_t)ZI_FILE_AIO_OP_STAT) {
    fprintf(stderr, "aio STAT done payload mismatch\n");
    return 1;
  }
  uint64_t st_size = read_u64le(z.payload + 8);
  uint32_t st_mode = zi_zcl1_read_u32(z.payload + 8 + 16);
  if (st_size != (uint64_t)strlen(inner_msg)) {
    fprintf(stderr, "aio STAT size mismatch\n");
    return 1;
  }
  if ((st_mode & S_IFMT) != S_IFREG) {
    fprintf(stderr, "aio STAT mode not regular file\n");
    return 1;
  }

  // READDIR dir (rid=10)
  uint8_t readdir_pl[20];
  write_u64le(readdir_pl + 0, (uint64_t)(uintptr_t)dir_path);
  write_u32le(readdir_pl + 8, (uint32_t)strlen(dir_path));
  write_u32le(readdir_pl + 12, 4096u);
  write_u32le(readdir_pl + 16, 0u);
  build_zcl1_req(req, (uint16_t)ZI_FILE_AIO_OP_READDIR, 10u, readdir_pl, (uint32_t)sizeof(readdir_pl));
  if (write_all_handle(aio_h, req, 24u + (uint32_t)sizeof(readdir_pl)) != 0) {
    fprintf(stderr, "aio READDIR write failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !expect_ok_frame(fr, (uint32_t)n, (uint16_t)ZI_FILE_AIO_OP_READDIR, 10u)) {
    fprintf(stderr, "aio READDIR ack failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !zi_zcl1_parse(fr, (uint32_t)n, &z) || z.op != (uint16_t)ZI_FILE_AIO_EV_DONE || z.rid != 10u) {
    fprintf(stderr, "aio READDIR done bad\n");
    return 1;
  }
  if (zi_zcl1_read_u32(fr + 12) != 1u || z.payload_len < 12u || zi_zcl1_read_u16(z.payload + 0) != (uint16_t)ZI_FILE_AIO_OP_READDIR) {
    fprintf(stderr, "aio READDIR done payload mismatch\n");
    return 1;
  }
  uint32_t ent_count = zi_zcl1_read_u32(z.payload + 4);
  const uint8_t *p = z.payload + 8;
  uint32_t left = z.payload_len - 8u;
  if (left < 4u) {
    fprintf(stderr, "aio READDIR extra too small\n");
    return 1;
  }
  (void)zi_zcl1_read_u32(p + 0); // flags
  p += 4u;
  left -= 4u;
  int found_inner = 0;
  for (uint32_t i = 0; i < ent_count; i++) {
    if (left < 8u) {
      fprintf(stderr, "aio READDIR entry truncated\n");
      return 1;
    }
    uint32_t dtype = zi_zcl1_read_u32(p + 0);
    uint32_t name_len = zi_zcl1_read_u32(p + 4);
    p += 8u;
    left -= 8u;
    if (left < name_len) {
      fprintf(stderr, "aio READDIR name truncated\n");
      return 1;
    }
    if (name_len == strlen("inner.txt") && memcmp(p, "inner.txt", name_len) == 0) {
      if (dtype != (uint32_t)ZI_FILE_AIO_DTYPE_FILE && dtype != (uint32_t)ZI_FILE_AIO_DTYPE_UNKNOWN) {
        fprintf(stderr, "aio READDIR dtype mismatch\n");
        return 1;
      }
      found_inner = 1;
    }
    p += name_len;
    left -= name_len;
  }
  if (!found_inner) {
    fprintf(stderr, "aio READDIR did not find inner.txt\n");
    return 1;
  }

  // CLOSE inner (rid=11)
  write_u64le(close_pl, inner_id);
  build_zcl1_req(req, (uint16_t)ZI_FILE_AIO_OP_CLOSE, 11u, close_pl, (uint32_t)sizeof(close_pl));
  if (write_all_handle(aio_h, req, 24u + (uint32_t)sizeof(close_pl)) != 0) {
    fprintf(stderr, "aio CLOSE(inner) write failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !expect_ok_frame(fr, (uint32_t)n, (uint16_t)ZI_FILE_AIO_OP_CLOSE, 11u)) {
    fprintf(stderr, "aio CLOSE(inner) ack failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !zi_zcl1_parse(fr, (uint32_t)n, &z) || z.op != (uint16_t)ZI_FILE_AIO_EV_DONE || z.rid != 11u || zi_zcl1_read_u32(fr + 12) != 1u) {
    fprintf(stderr, "aio CLOSE(inner) done bad\n");
    return 1;
  }

  // UNLINK inner (rid=12)
  uint8_t unlink_pl[16];
  write_u64le(unlink_pl + 0, (uint64_t)(uintptr_t)inner_path);
  write_u32le(unlink_pl + 8, (uint32_t)strlen(inner_path));
  write_u32le(unlink_pl + 12, 0u);
  build_zcl1_req(req, (uint16_t)ZI_FILE_AIO_OP_UNLINK, 12u, unlink_pl, (uint32_t)sizeof(unlink_pl));
  if (write_all_handle(aio_h, req, 24u + (uint32_t)sizeof(unlink_pl)) != 0) {
    fprintf(stderr, "aio UNLINK write failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !expect_ok_frame(fr, (uint32_t)n, (uint16_t)ZI_FILE_AIO_OP_UNLINK, 12u)) {
    fprintf(stderr, "aio UNLINK ack failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !zi_zcl1_parse(fr, (uint32_t)n, &z) || z.op != (uint16_t)ZI_FILE_AIO_EV_DONE || z.rid != 12u || zi_zcl1_read_u32(fr + 12) != 1u) {
    fprintf(stderr, "aio UNLINK done bad\n");
    return 1;
  }

  // RMDIR dir (rid=13)
  uint8_t rmdir_pl[16];
  write_u64le(rmdir_pl + 0, (uint64_t)(uintptr_t)dir_path);
  write_u32le(rmdir_pl + 8, (uint32_t)strlen(dir_path));
  write_u32le(rmdir_pl + 12, 0u);
  build_zcl1_req(req, (uint16_t)ZI_FILE_AIO_OP_RMDIR, 13u, rmdir_pl, (uint32_t)sizeof(rmdir_pl));
  if (write_all_handle(aio_h, req, 24u + (uint32_t)sizeof(rmdir_pl)) != 0) {
    fprintf(stderr, "aio RMDIR write failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !expect_ok_frame(fr, (uint32_t)n, (uint16_t)ZI_FILE_AIO_OP_RMDIR, 13u)) {
    fprintf(stderr, "aio RMDIR ack failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !zi_zcl1_parse(fr, (uint32_t)n, &z) || z.op != (uint16_t)ZI_FILE_AIO_EV_DONE || z.rid != 13u || zi_zcl1_read_u32(fr + 12) != 1u) {
    fprintf(stderr, "aio RMDIR done bad\n");
    return 1;
  }

  // Sandbox escape should fail (completion error)
  const char *bad_path = "/../escape.txt";
  write_u64le(open_pl + 0, (uint64_t)(uintptr_t)bad_path);
  write_u32le(open_pl + 8, (uint32_t)strlen(bad_path));
  write_u32le(open_pl + 12, ZI_FILE_O_READ);
  write_u32le(open_pl + 16, 0);
  build_zcl1_req(req, (uint16_t)ZI_FILE_AIO_OP_OPEN, 5u, open_pl, (uint32_t)sizeof(open_pl));
  if (write_all_handle(aio_h, req, 24u + (uint32_t)sizeof(open_pl)) != 0) {
    fprintf(stderr, "aio OPEN(bad) write failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !expect_ok_frame(fr, (uint32_t)n, (uint16_t)ZI_FILE_AIO_OP_OPEN, 5u)) {
    fprintf(stderr, "aio OPEN(bad) ack failed\n");
    return 1;
  }
  n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
  if (n <= 0 || !zi_zcl1_parse(fr, (uint32_t)n, &z) || z.op != (uint16_t)ZI_FILE_AIO_EV_DONE || z.rid != 5u) {
    fprintf(stderr, "aio OPEN(bad) done bad\n");
    return 1;
  }
  if (zi_zcl1_read_u32(fr + 12) != 0u) {
    fprintf(stderr, "expected aio OPEN(bad) completion error\n");
    return 1;
  }

  // ---- backpressure readiness smoke: writable means queue has space ----
  {
    const uint64_t WATCH_AIO_W = 0xA10A10A2ull;
    if (!loop_watch(loop_h, aio_h, 0x2u, WATCH_AIO_W)) {
      fprintf(stderr, "loop WATCH aio(writable) failed\n");
      return 1;
    }

    char fifo_host[512];
    snprintf(fifo_host, sizeof(fifo_host), "%s/fifo", root);
    (void)unlink(fifo_host);
    if (mkfifo(fifo_host, 0600) != 0) {
      perror("mkfifo");
      return 1;
    }

    if (!have_fifo_keep) {
      strncpy(fifo_host_keep, fifo_host, sizeof(fifo_host_keep));
      fifo_host_keep[sizeof(fifo_host_keep) - 1] = '\0';
      have_fifo_keep = 1;
    }

    const char *fifo_guest = "/fifo";
    uint8_t fifo_open_pl[20];
    write_u64le(fifo_open_pl + 0, (uint64_t)(uintptr_t)fifo_guest);
    write_u32le(fifo_open_pl + 8, (uint32_t)strlen(fifo_guest));
    write_u32le(fifo_open_pl + 12, ZI_FILE_O_READ);
    write_u32le(fifo_open_pl + 16, 0);

    uint32_t rid = 1000u;

    // Prime the worker: enqueue one FIFO OPEN, then give the worker a moment to
    // dequeue it and block on opening the FIFO (no writer yet). This makes the
    // subsequent queue-full + not-writable state stable instead of racy.
    build_zcl1_req(req, (uint16_t)ZI_FILE_AIO_OP_OPEN, rid, fifo_open_pl, (uint32_t)sizeof(fifo_open_pl));
    if (write_all_handle(aio_h, req, 24u + (uint32_t)sizeof(fifo_open_pl)) != 0) {
      fprintf(stderr, "aio FIFO OPEN write failed\n");
      return 1;
    }

    n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
    if (n <= 0) {
      fprintf(stderr, "aio FIFO OPEN ack missing\n");
      return 1;
    }
    if (!zi_zcl1_parse(fr, (uint32_t)n, &z) || z.op != (uint16_t)ZI_FILE_AIO_OP_OPEN || z.rid != rid) {
      fprintf(stderr, "aio FIFO OPEN ack bad frame\n");
      return 1;
    }
    if (zi_zcl1_read_u32(fr + 12) == 0u) {
      fprintf(stderr, "aio FIFO OPEN unexpected error\n");
      return 1;
    }
    rid++;
    usleep(50 * 1000);

    int saw_full = 0;
    for (int i = 0; i < 5000; i++) {
      build_zcl1_req(req, (uint16_t)ZI_FILE_AIO_OP_OPEN, rid, fifo_open_pl, (uint32_t)sizeof(fifo_open_pl));
      if (write_all_handle(aio_h, req, 24u + (uint32_t)sizeof(fifo_open_pl)) != 0) {
        fprintf(stderr, "aio FIFO OPEN write failed\n");
        return 1;
      }

      // Ack should be immediately available after zi_write.
      n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
      if (n <= 0) {
        fprintf(stderr, "aio FIFO OPEN ack missing\n");
        return 1;
      }

      if (!zi_zcl1_parse(fr, (uint32_t)n, &z) || z.op != (uint16_t)ZI_FILE_AIO_OP_OPEN || z.rid != rid) {
        fprintf(stderr, "aio FIFO OPEN ack bad frame\n");
        return 1;
      }

      if (zi_zcl1_read_u32(fr + 12) == 0u) {
        // ERROR: parse trace/msg
        const uint8_t *ep = z.payload;
        if (z.payload_len < 12u) {
          fprintf(stderr, "aio FIFO OPEN error short\n");
          return 1;
        }
        uint32_t tlen = zi_zcl1_read_u32(ep + 0);
        if (4u + tlen + 4u > z.payload_len) {
          fprintf(stderr, "aio FIFO OPEN error bad trace\n");
          return 1;
        }
        const char *trace = (const char *)(ep + 4);
        uint32_t mlen = zi_zcl1_read_u32(ep + 4u + tlen);
        if (4u + tlen + 4u + mlen + 4u > z.payload_len) {
          fprintf(stderr, "aio FIFO OPEN error bad msg\n");
          return 1;
        }
        const char *msgp = (const char *)(ep + 4u + tlen + 4u);

        if (tlen == (uint32_t)strlen("file.aio") && mlen == (uint32_t)strlen("queue full") &&
            memcmp(trace, "file.aio", tlen) == 0 && memcmp(msgp, "queue full", mlen) == 0) {
          saw_full = 1;
          break;
        }

        fprintf(stderr, "aio FIFO OPEN unexpected error\n");
        return 1;
      }

      rid++;
    }

    if (!saw_full) {
      fprintf(stderr, "aio did not reach queue full\n");
      return 1;
    }

    // When full, writable readiness should not be reported.
    if (loop_wait_writable(loop_h, aio_h, WATCH_AIO_W, 50u)) {
      fprintf(stderr, "aio reported writable while full\n");
      return 1;
    }

    // Unblock worker by opening the FIFO for write on the host side.
    // Keep this writer open until after zi_end(aio_h) so the worker can't re-block
    // processing queued FIFO OPEN jobs during shutdown.
    if (fifo_wfd < 0) {
      for (int attempt = 0; attempt < 200; attempt++) {
        fifo_wfd = open(fifo_host, O_WRONLY | O_NONBLOCK);
        if (fifo_wfd >= 0) break;
        if (errno != ENXIO) break;
        usleep(5 * 1000);
      }
    }
    if (fifo_wfd < 0) {
      perror("open fifo writer");
      return 1;
    }

    // Drain some frames to let the worker dequeue and make progress.
    for (int k = 0; k < 32; k++) {
      n = read_full_frame_wait(loop_h, aio_h, WATCH_AIO, fr, (uint32_t)sizeof(fr), 1000u);
      if (n <= 0) break;
    }

    // After progress, aio should become writable again.
    if (!loop_wait_writable(loop_h, aio_h, WATCH_AIO_W, 1000u)) {
      fprintf(stderr, "aio did not become writable after progress\n");
      return 1;
    }

    // Note: do not close/unlink the FIFO here; keep it until after zi_end(aio_h).

    if (!loop_unwatch(loop_h, WATCH_AIO_W)) {
      fprintf(stderr, "loop UNWATCH aio(writable) failed\n");
      return 1;
    }
  }

  (void)zi_end(loop_h);
  (void)zi_end(aio_h);

  if (fifo_wfd >= 0) (void)close(fifo_wfd);
  if (have_fifo_keep) (void)unlink(fifo_host_keep);

  printf("ok\n");
  return 0;
}
