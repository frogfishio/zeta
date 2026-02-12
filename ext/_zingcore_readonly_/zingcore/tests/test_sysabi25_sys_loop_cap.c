#include "zi_caps.h"
#include "zi_handles25.h"
#include "zi_net_tcp25.h"
#include "zi_runtime25.h"
#include "zi_sys_loop25.h"
#include "zi_sysabi25.h"
#include "zi_zcl1.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void write_u16le(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void write_u32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void write_u64le(uint8_t *p, uint64_t v) {
  write_u32le(p + 0, (uint32_t)(v & 0xFFFFFFFFu));
  write_u32le(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static uint32_t read_u32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64le(const uint8_t *p) {
  uint64_t lo = (uint64_t)read_u32le(p);
  uint64_t hi = (uint64_t)read_u32le(p + 4);
  return lo | (hi << 32);
}

static void build_open_req(uint8_t req[40], const char *kind, const char *name, const void *params, uint32_t params_len) {
  write_u64le(req + 0, (uint64_t)(uintptr_t)kind);
  write_u32le(req + 8, (uint32_t)strlen(kind));
  write_u64le(req + 12, (uint64_t)(uintptr_t)name);
  write_u32le(req + 20, (uint32_t)strlen(name));
  write_u32le(req + 24, 0);
  write_u64le(req + 28, (uint64_t)(uintptr_t)params);
  write_u32le(req + 36, params_len);
}

static void build_zcl1_req(uint8_t *out, uint16_t op, uint32_t rid, const uint8_t *payload, uint32_t payload_len) {
  memcpy(out + 0, "ZCL1", 4);
  write_u16le(out + 4, 1);
  write_u16le(out + 6, op);
  write_u32le(out + 8, rid);
  write_u32le(out + 12, 0);
  write_u32le(out + 16, 0);
  write_u32le(out + 20, payload_len);
  if (payload_len && payload) memcpy(out + 24, payload, payload_len);
}

static int write_all_handle(zi_handle_t h, const void *p, uint32_t n) {
  const uint8_t *b = (const uint8_t *)p;
  uint32_t off = 0;
  while (off < n) {
    int32_t w = zi_write(h, (zi_ptr_t)(uintptr_t)(b + off), (zi_size32_t)(n - off));
    if (w < 0) return w;
    if (w == 0) return ZI_E_IO;
    off += (uint32_t)w;
  }
  return 0;
}

static int read_full_frame(zi_handle_t h, uint8_t *buf, uint32_t cap) {
  uint32_t got = 0;
  while (got < 24u) {
    int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(buf + got), (zi_size32_t)(cap - got));
    if (n < 0) return n;
    if (n == 0) return ZI_E_CLOSED;
    got += (uint32_t)n;
  }
  if (!(buf[0] == 'Z' && buf[1] == 'C' && buf[2] == 'L' && buf[3] == '1')) return ZI_E_INVALID;
  uint32_t pl = zi_zcl1_read_u32(buf + 20);
  uint32_t need = 24u + pl;
  if (need > cap) return ZI_E_BOUNDS;
  while (got < need) {
    int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(buf + got), (zi_size32_t)(need - got));
    if (n < 0) return n;
    if (n == 0) return ZI_E_CLOSED;
    got += (uint32_t)n;
  }
  return (int)got;
}

static int expect_ok_frame(const uint8_t *fr, uint32_t fr_len, uint16_t op, uint32_t rid, const uint8_t **out_pl, uint32_t *out_pl_len) {
  zi_zcl1_frame z;
  if (!zi_zcl1_parse(fr, fr_len, &z)) return 0;
  if (z.op != op) return 0;
  if (z.rid != rid) return 0;
  if (out_pl) *out_pl = z.payload;
  if (out_pl_len) *out_pl_len = z.payload_len;
  return 1;
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
  if (!zi_net_tcp25_register()) {
    fprintf(stderr, "zi_net_tcp25_register failed\n");
    return 1;
  }
  if (!zi_sys_loop25_register()) {
    fprintf(stderr, "zi_sys_loop25_register failed\n");
    return 1;
  }

  if (setenv("ZI_NET_ALLOW", "loopback", 1) != 0) {
    perror("setenv");
    return 1;
  }

  // Start local server.
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) {
    perror("socket");
    return 1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(0);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (bind(srv, (struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0) {
    perror("bind");
    close(srv);
    return 1;
  }
  if (listen(srv, 1) != 0) {
    perror("listen");
    close(srv);
    return 1;
  }

  struct sockaddr_in bound;
  socklen_t blen = (socklen_t)sizeof(bound);
  if (getsockname(srv, (struct sockaddr *)&bound, &blen) != 0) {
    perror("getsockname");
    close(srv);
    return 1;
  }
  uint32_t port = (uint32_t)ntohs(bound.sin_port);

  // Open TCP client cap.
  uint8_t tcp_params[20];
  uint8_t open_req[40];
  write_u64le(tcp_params + 0, (uint64_t)(uintptr_t)"127.0.0.1");
  write_u32le(tcp_params + 8, (uint32_t)strlen("127.0.0.1"));
  write_u32le(tcp_params + 12, port);
  write_u32le(tcp_params + 16, 0);
  build_open_req(open_req, ZI_CAP_KIND_NET, ZI_CAP_NAME_TCP, tcp_params, (uint32_t)sizeof(tcp_params));

  zi_handle_t tcp_h = zi_cap_open((zi_ptr_t)(uintptr_t)open_req);
  if (tcp_h < 3) {
    fprintf(stderr, "tcp open failed: %d\n", tcp_h);
    close(srv);
    return 1;
  }

  int conn = accept(srv, NULL, NULL);
  if (conn < 0) {
    perror("accept");
    (void)zi_end(tcp_h);
    close(srv);
    return 1;
  }

  // Open sys/loop cap.
  build_open_req(open_req, ZI_CAP_KIND_SYS, ZI_CAP_NAME_LOOP, NULL, 0);
  zi_handle_t loop_h = zi_cap_open((zi_ptr_t)(uintptr_t)open_req);
  if (loop_h < 3) {
    fprintf(stderr, "loop open failed: %d\n", loop_h);
    (void)zi_end(tcp_h);
    close(conn);
    close(srv);
    return 1;
  }

  // WATCH tcp handle for readable.
  uint8_t watch_pl[20];
  write_u32le(watch_pl + 0, (uint32_t)tcp_h);
  write_u32le(watch_pl + 4, 0x1u); // readable
  write_u64le(watch_pl + 8, 0xABCDEF01ull);
  write_u32le(watch_pl + 16, 0u);

  uint8_t req[64];
  build_zcl1_req(req, (uint16_t)ZI_SYS_LOOP_OP_WATCH, 1u, watch_pl, (uint32_t)sizeof(watch_pl));
  if (write_all_handle(loop_h, req, 24u + (uint32_t)sizeof(watch_pl)) != 0) {
    fprintf(stderr, "WATCH write failed\n");
    return 1;
  }

  uint8_t fr[4096];
  int frn = read_full_frame(loop_h, fr, (uint32_t)sizeof(fr));
  if (frn < 0) {
    fprintf(stderr, "WATCH read failed: %d\n", frn);
    return 1;
  }
  const uint8_t *pl = NULL;
  uint32_t pl_len = 0;
  if (!expect_ok_frame(fr, (uint32_t)frn, (uint16_t)ZI_SYS_LOOP_OP_WATCH, 1u, &pl, &pl_len) || pl_len != 0) {
    fprintf(stderr, "WATCH response invalid\n");
    return 1;
  }

  // Send data from server and POLL.
  const char msg[] = "hello";
  if (send(conn, msg, sizeof(msg) - 1, 0) != (ssize_t)(sizeof(msg) - 1)) {
    perror("send");
    return 1;
  }

  uint8_t poll_pl[8];
  write_u32le(poll_pl + 0, 8u);
  write_u32le(poll_pl + 4, 1000u);
  build_zcl1_req(req, (uint16_t)ZI_SYS_LOOP_OP_POLL, 2u, poll_pl, (uint32_t)sizeof(poll_pl));
  if (write_all_handle(loop_h, req, 24u + (uint32_t)sizeof(poll_pl)) != 0) {
    fprintf(stderr, "POLL write failed\n");
    return 1;
  }

  frn = read_full_frame(loop_h, fr, (uint32_t)sizeof(fr));
  if (frn < 0) {
    fprintf(stderr, "POLL read failed: %d\n", frn);
    return 1;
  }

  if (!expect_ok_frame(fr, (uint32_t)frn, (uint16_t)ZI_SYS_LOOP_OP_POLL, 2u, &pl, &pl_len) || pl_len < 16u) {
    fprintf(stderr, "POLL response invalid\n");
    return 1;
  }
  uint32_t ver = read_u32le(pl + 0);
  uint32_t count = read_u32le(pl + 8);
  if (ver != 1u || count == 0u) {
    fprintf(stderr, "POLL payload invalid ver=%u count=%u\n", ver, count);
    return 1;
  }

  int saw_ready = 0;
  const uint8_t *evp = pl + 16;
  uint32_t remain = pl_len - 16u;
  while (remain >= 32u) {
    uint32_t kind = read_u32le(evp + 0);
    uint32_t events = read_u32le(evp + 4);
    uint32_t handle = read_u32le(evp + 8);
    uint64_t id = read_u64le(evp + 16);

    if (kind == 1u && handle == (uint32_t)tcp_h && id == 0xABCDEF01ull && (events & 0x1u)) {
      saw_ready = 1;
      break;
    }

    evp += 32u;
    remain -= 32u;
  }

  if (!saw_ready) {
    fprintf(stderr, "did not see READY for tcp handle\n");
    return 1;
  }

  char rbuf[16];
  memset(rbuf, 0, sizeof(rbuf));
  int32_t rn = zi_read(tcp_h, (zi_ptr_t)(uintptr_t)rbuf, (zi_size32_t)sizeof(rbuf));
  if (rn != (int32_t)(sizeof(msg) - 1) || memcmp(rbuf, msg, (size_t)rn) != 0) {
    fprintf(stderr, "tcp read mismatch (n=%d)\n", rn);
    return 1;
  }

  // TIMER: arm a relative one-shot and poll.
  uint8_t timer_pl[28];
  write_u64le(timer_pl + 0, 0x11111111ull);
  write_u64le(timer_pl + 8, 50ull * 1000ull * 1000ull); // 50ms in ns
  write_u64le(timer_pl + 16, 0ull);
  write_u32le(timer_pl + 24, 0x1u); // relative

  build_zcl1_req(req, (uint16_t)ZI_SYS_LOOP_OP_TIMER_ARM, 3u, timer_pl, (uint32_t)sizeof(timer_pl));
  if (write_all_handle(loop_h, req, 24u + (uint32_t)sizeof(timer_pl)) != 0) {
    fprintf(stderr, "TIMER_ARM write failed\n");
    return 1;
  }
  frn = read_full_frame(loop_h, fr, (uint32_t)sizeof(fr));
  if (frn < 0) {
    fprintf(stderr, "TIMER_ARM read failed: %d\n", frn);
    return 1;
  }
  if (!expect_ok_frame(fr, (uint32_t)frn, (uint16_t)ZI_SYS_LOOP_OP_TIMER_ARM, 3u, &pl, &pl_len) || pl_len != 0) {
    fprintf(stderr, "TIMER_ARM response invalid\n");
    return 1;
  }

  write_u32le(poll_pl + 0, 8u);
  write_u32le(poll_pl + 4, 1000u);
  build_zcl1_req(req, (uint16_t)ZI_SYS_LOOP_OP_POLL, 4u, poll_pl, (uint32_t)sizeof(poll_pl));
  if (write_all_handle(loop_h, req, 24u + (uint32_t)sizeof(poll_pl)) != 0) {
    fprintf(stderr, "POLL write failed\n");
    return 1;
  }
  frn = read_full_frame(loop_h, fr, (uint32_t)sizeof(fr));
  if (frn < 0) {
    fprintf(stderr, "POLL read failed: %d\n", frn);
    return 1;
  }
  if (!expect_ok_frame(fr, (uint32_t)frn, (uint16_t)ZI_SYS_LOOP_OP_POLL, 4u, &pl, &pl_len) || pl_len < 16u) {
    fprintf(stderr, "POLL response invalid\n");
    return 1;
  }

  ver = read_u32le(pl + 0);
  count = read_u32le(pl + 8);
  if (ver != 1u || count == 0u) {
    fprintf(stderr, "POLL payload invalid for timer\n");
    return 1;
  }

  int saw_timer = 0;
  evp = pl + 16;
  remain = pl_len - 16u;
  while (remain >= 32u) {
    uint32_t kind = read_u32le(evp + 0);
    uint64_t id = read_u64le(evp + 16);
    if (kind == 2u && id == 0x11111111ull) {
      saw_timer = 1;
      break;
    }
    evp += 32u;
    remain -= 32u;
  }

  if (!saw_timer) {
    fprintf(stderr, "did not see TIMER event\n");
    return 1;
  }

  (void)zi_end(loop_h);
  (void)zi_end(tcp_h);
  close(conn);
  close(srv);

  printf("ok\n");
  return 0;
}
