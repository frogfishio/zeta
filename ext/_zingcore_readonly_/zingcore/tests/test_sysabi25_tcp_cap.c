#include "zi_caps.h"
#include "zi_handles25.h"
#include "zi_net_tcp25.h"
#include "zi_runtime25.h"
#include "zi_sys_loop25.h"
#include "zi_sysabi25.h"
#include "zi_zcl1.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void write_u32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void write_u16le(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void write_u64le(uint8_t *p, uint64_t v) {
  write_u32le(p + 0, (uint32_t)(v & 0xFFFFFFFFu));
  write_u32le(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static uint32_t read_u32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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

static int expect_ok_frame(const uint8_t *fr, uint32_t fr_len, uint16_t op, uint32_t rid) {
  zi_zcl1_frame z;
  if (!zi_zcl1_parse(fr, fr_len, &z)) return 0;
  if (z.op != op) return 0;
  if (z.rid != rid) return 0;
  return 1;
}

static int loop_watch(zi_handle_t loop_h, zi_handle_t target_h, uint32_t events, uint64_t watch_id) {
  uint8_t watch_pl[20];
  write_u32le(watch_pl + 0, (uint32_t)target_h);
  write_u32le(watch_pl + 4, events);
  write_u64le(watch_pl + 8, watch_id);
  write_u32le(watch_pl + 16, 0u);

  uint8_t req[64];
  build_zcl1_req(req, (uint16_t)ZI_SYS_LOOP_OP_WATCH, 1u, watch_pl, (uint32_t)sizeof(watch_pl));
  int w = write_all_handle(loop_h, req, 24u + (uint32_t)sizeof(watch_pl));
  if (w != 0) return 0;
  uint8_t fr[256];
  int n = read_full_frame(loop_h, fr, (uint32_t)sizeof(fr));
  if (n < 0) return 0;
  return expect_ok_frame(fr, (uint32_t)n, (uint16_t)ZI_SYS_LOOP_OP_WATCH, 1u);
}

static int loop_poll_once(zi_handle_t loop_h, uint32_t timeout_ms, uint8_t *out_fr, uint32_t out_cap, uint32_t *out_pl_len, const uint8_t **out_pl) {
  uint8_t poll_pl[8];
  write_u32le(poll_pl + 0, 16u);
  write_u32le(poll_pl + 4, timeout_ms);
  uint8_t req[64];
  build_zcl1_req(req, (uint16_t)ZI_SYS_LOOP_OP_POLL, 2u, poll_pl, (uint32_t)sizeof(poll_pl));
  if (write_all_handle(loop_h, req, 24u + (uint32_t)sizeof(poll_pl)) != 0) return 0;
  int n = read_full_frame(loop_h, out_fr, out_cap);
  if (n < 0) return 0;
  zi_zcl1_frame z;
  if (!zi_zcl1_parse(out_fr, (uint32_t)n, &z)) return 0;
  if (z.op != (uint16_t)ZI_SYS_LOOP_OP_POLL || z.rid != 2u) return 0;
  if (out_pl_len) *out_pl_len = z.payload_len;
  if (out_pl) *out_pl = z.payload;
  return 1;
}

static int loop_wait_ready(zi_handle_t loop_h, uint64_t watch_id, uint32_t want_events, uint32_t timeout_ms) {
  uint8_t fr[4096];
  for (;;) {
    uint32_t pl_len = 0;
    const uint8_t *pl = NULL;
    if (!loop_poll_once(loop_h, timeout_ms, fr, (uint32_t)sizeof(fr), &pl_len, &pl)) return 0;
    if (!pl || pl_len < 16u) return 0;
    uint32_t count = read_u32le(pl + 8);
    const uint8_t *ev = pl + 16;
    for (uint32_t i = 0; i < count; i++) {
      if (16u + (i + 1u) * 32u > pl_len) return 0;
      uint32_t kind = read_u32le(ev + i * 32u + 0);
      uint32_t events = read_u32le(ev + i * 32u + 4);
      uint64_t id = (uint64_t)read_u32le(ev + i * 32u + 16) | ((uint64_t)read_u32le(ev + i * 32u + 20) << 32);
      if (kind == 1u && id == watch_id && (events & want_events) != 0u) return 1;
    }
  }
}

static void build_tcp_params(uint8_t params[20], const char *host, uint32_t port, uint32_t flags) {
  write_u64le(params + 0, (uint64_t)(uintptr_t)host);
  write_u32le(params + 8, (uint32_t)strlen(host));
  write_u32le(params + 12, port);
  write_u32le(params + 16, flags);
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

  // Sandbox: only loopback.
  if (setenv("ZI_NET_ALLOW", "loopback", 1) != 0) {
    perror("setenv");
    return 1;
  }

  // Negative: non-loopback host denied.
  {
    uint8_t params[20];
    uint8_t req[40];
    build_tcp_params(params, "example.com", 80, 0);
    build_open_req(req, ZI_CAP_KIND_NET, ZI_CAP_NAME_TCP, params, (uint32_t)sizeof(params));
    zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)req);
    if (h != ZI_E_DENIED) {
      fprintf(stderr, "expected denied for example.com, got %d\n", h);
      return 1;
    }
  }

  // Negative: invalid port rejected.
  {
    uint8_t params[20];
    uint8_t req[40];
    build_tcp_params(params, "127.0.0.1", 0, 0);
    build_open_req(req, ZI_CAP_KIND_NET, ZI_CAP_NAME_TCP, params, (uint32_t)sizeof(params));
    zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)req);
    if (h != ZI_E_INVALID) {
      fprintf(stderr, "expected invalid for port 0, got %d\n", h);
      return 1;
    }
  }

  // Positive: spin up a local TCP server and connect using the cap.
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

  uint8_t params[20];
  uint8_t req[40];
  build_tcp_params(params, "127.0.0.1", port, 0);
  build_open_req(req, ZI_CAP_KIND_NET, ZI_CAP_NAME_TCP, params, (uint32_t)sizeof(params));

  zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)req);
  if (h < 3) {
    fprintf(stderr, "expected handle, got %d\n", h);
    close(srv);
    return 1;
  }

  // Open sys/loop cap.
  uint8_t loop_req[40];
  build_open_req(loop_req, ZI_CAP_KIND_SYS, ZI_CAP_NAME_LOOP, NULL, 0);
  zi_handle_t loop_h = zi_cap_open((zi_ptr_t)(uintptr_t)loop_req);
  if (loop_h < 3) {
    fprintf(stderr, "loop open failed: %d\n", loop_h);
    (void)zi_end(h);
    close(srv);
    return 1;
  }

  // Watch the tcp stream for read/write readiness.
  const uint64_t WATCH_RW = 0x1111222233334444ull;
  if (!loop_watch(loop_h, h, 0x3u, WATCH_RW)) {
    fprintf(stderr, "loop WATCH failed\n");
    (void)zi_end(loop_h);
    (void)zi_end(h);
    close(srv);
    return 1;
  }

  int conn = accept(srv, NULL, NULL);
  if (conn < 0) {
    perror("accept");
    (void)zi_end(loop_h);
    (void)zi_end(h);
    close(srv);
    return 1;
  }

  const char ping[] = "ping";
  int32_t wn = zi_write(h, (zi_ptr_t)(uintptr_t)ping, (zi_size32_t)sizeof(ping) - 1);
  if (wn == ZI_E_AGAIN) {
    if (!loop_wait_ready(loop_h, WATCH_RW, 0x2u, 1000u)) {
      fprintf(stderr, "timeout waiting for tcp writable\n");
      close(conn);
      close(srv);
      return 1;
    }
    wn = zi_write(h, (zi_ptr_t)(uintptr_t)ping, (zi_size32_t)sizeof(ping) - 1);
  }
  if (wn != (int32_t)(sizeof(ping) - 1)) {
    fprintf(stderr, "zi_write failed: %d\n", wn);
    close(conn);
    close(srv);
    return 1;
  }

  char buf[16];
  memset(buf, 0, sizeof(buf));
  ssize_t rn = recv(conn, buf, sizeof(buf), 0);
  if (rn != (ssize_t)(sizeof(ping) - 1) || memcmp(buf, ping, (size_t)rn) != 0) {
    fprintf(stderr, "server recv mismatch\n");
    close(conn);
    close(srv);
    return 1;
  }

  const char pong[] = "pong";
  if (send(conn, pong, sizeof(pong) - 1, 0) != (ssize_t)(sizeof(pong) - 1)) {
    perror("send");
    close(conn);
    close(srv);
    return 1;
  }

  memset(buf, 0, sizeof(buf));
  int32_t gn = zi_read(h, (zi_ptr_t)(uintptr_t)buf, (zi_size32_t)sizeof(buf));
  if (gn == ZI_E_AGAIN) {
    if (!loop_wait_ready(loop_h, WATCH_RW, 0x1u, 1000u)) {
      fprintf(stderr, "timeout waiting for tcp readable\n");
      close(conn);
      close(srv);
      return 1;
    }
    gn = zi_read(h, (zi_ptr_t)(uintptr_t)buf, (zi_size32_t)sizeof(buf));
  }
  if (gn != (int32_t)(sizeof(pong) - 1) || memcmp(buf, pong, (size_t)gn) != 0) {
    fprintf(stderr, "zi_read mismatch (n=%d)\n", gn);
    close(conn);
    close(srv);
    return 1;
  }

  if (zi_end(h) != 0) {
    fprintf(stderr, "zi_end failed\n");
    close(conn);
    close(srv);
    return 1;
  }

  (void)zi_end(loop_h);

  if (zi_write(h, (zi_ptr_t)(uintptr_t)ping, (zi_size32_t)sizeof(ping) - 1) != ZI_E_NOSYS) {
    fprintf(stderr, "expected ended handle to be invalid\n");
    close(conn);
    close(srv);
    return 1;
  }

  close(conn);
  close(srv);

  printf("ok\n");
  return 0;
}
