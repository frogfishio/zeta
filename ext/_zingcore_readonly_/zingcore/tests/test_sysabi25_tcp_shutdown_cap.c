#include "zi_caps.h"
#include "zi_handles25.h"
#include "zi_net_tcp25.h"
#include "zi_runtime25.h"
#include "zi_sys_loop25.h"
#include "zi_sysabi25.h"
#include "zi_zcl1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void build_tcp_listen_params(uint8_t params[32], const char *host, uint32_t port, uint32_t flags, uint32_t backlog,
                                   uint32_t *out_bound_port) {
  write_u64le(params + 0, (uint64_t)(uintptr_t)host);
  write_u32le(params + 8, (uint32_t)strlen(host));
  write_u32le(params + 12, port);
  write_u32le(params + 16, flags);
  write_u32le(params + 20, backlog);
  write_u64le(params + 24, (uint64_t)(uintptr_t)out_bound_port);
}

static void build_tcp_params(uint8_t params[20], const char *host, uint32_t port, uint32_t flags) {
  write_u64le(params + 0, (uint64_t)(uintptr_t)host);
  write_u32le(params + 8, (uint32_t)strlen(host));
  write_u32le(params + 12, port);
  write_u32le(params + 16, flags);
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

static int loop_watch(zi_handle_t loop_h, zi_handle_t target_h, uint32_t events, uint64_t watch_id) {
  uint8_t watch_pl[20];
  write_u32le(watch_pl + 0, (uint32_t)target_h);
  write_u32le(watch_pl + 4, events);
  write_u64le(watch_pl + 8, watch_id);
  write_u32le(watch_pl + 16, 0u);

  uint8_t req[64];
  build_zcl1_req(req, (uint16_t)ZI_SYS_LOOP_OP_WATCH, 1u, watch_pl, (uint32_t)sizeof(watch_pl));
  // loop is a handle; use zi_write/zi_read framing like other tests.
  uint32_t off = 0;
  while (off < 24u + (uint32_t)sizeof(watch_pl)) {
    int32_t w = zi_write(loop_h, (zi_ptr_t)(uintptr_t)(req + off), (zi_size32_t)(24u + (uint32_t)sizeof(watch_pl) - off));
    if (w < 0) return 0;
    if (w == 0) return 0;
    off += (uint32_t)w;
  }

  uint8_t fr[256];
  uint32_t got = 0;
  while (got < 24u) {
    int32_t n = zi_read(loop_h, (zi_ptr_t)(uintptr_t)(fr + got), (zi_size32_t)(sizeof(fr) - got));
    if (n < 0) return 0;
    if (n == 0) return 0;
    got += (uint32_t)n;
  }
  zi_zcl1_frame z;
  if (!zi_zcl1_parse(fr, got, &z)) return 0;
  // Might not have read full payload yet; but WATCH ok has 0 payload.
  if (z.op != (uint16_t)ZI_SYS_LOOP_OP_WATCH || z.rid != 1u) return 0;
  return 1;
}

static int loop_wait_ready(zi_handle_t loop_h, uint64_t watch_id, uint32_t want_events, uint32_t timeout_ms) {
  for (;;) {
    uint8_t poll_pl[8];
    write_u32le(poll_pl + 0, 16u);
    write_u32le(poll_pl + 4, timeout_ms);
    uint8_t req[64];
    build_zcl1_req(req, (uint16_t)ZI_SYS_LOOP_OP_POLL, 2u, poll_pl, (uint32_t)sizeof(poll_pl));

    uint32_t off = 0;
    while (off < 24u + (uint32_t)sizeof(poll_pl)) {
      int32_t w = zi_write(loop_h, (zi_ptr_t)(uintptr_t)(req + off), (zi_size32_t)(24u + (uint32_t)sizeof(poll_pl) - off));
      if (w < 0) return 0;
      if (w == 0) return 0;
      off += (uint32_t)w;
    }

    uint8_t fr[4096];
    uint32_t got = 0;
    while (got < 24u) {
      int32_t n = zi_read(loop_h, (zi_ptr_t)(uintptr_t)(fr + got), (zi_size32_t)(sizeof(fr) - got));
      if (n < 0) return 0;
      if (n == 0) return 0;
      got += (uint32_t)n;
    }

    if (!(fr[0] == 'Z' && fr[1] == 'C' && fr[2] == 'L' && fr[3] == '1')) return 0;
    uint32_t pl = zi_zcl1_read_u32(fr + 20);
    uint32_t need = 24u + pl;
    if (need > (uint32_t)sizeof(fr)) return 0;
    while (got < need) {
      int32_t n = zi_read(loop_h, (zi_ptr_t)(uintptr_t)(fr + got), (zi_size32_t)(need - got));
      if (n < 0) return 0;
      if (n == 0) return 0;
      got += (uint32_t)n;
    }

    zi_zcl1_frame z;
    if (!zi_zcl1_parse(fr, got, &z)) return 0;
    if (z.op != (uint16_t)ZI_SYS_LOOP_OP_POLL || z.rid != 2u) return 0;
    if (!z.payload || z.payload_len < 16u) return 0;
    uint32_t count = read_u32le(z.payload + 8);
    const uint8_t *ev = z.payload + 16;
    for (uint32_t i = 0; i < count; i++) {
      if (16u + (i + 1u) * 32u > z.payload_len) return 0;
      uint32_t kind = read_u32le(ev + i * 32u + 0);
      uint32_t events = read_u32le(ev + i * 32u + 4);
      uint64_t id = (uint64_t)read_u32le(ev + i * 32u + 16) | ((uint64_t)read_u32le(ev + i * 32u + 20) << 32);
      if (kind == 1u && id == watch_id && (events & want_events) != 0u) return 1;
    }
  }
}

static int ctl_handle_shutdown_write(zi_handle_t h) {
  uint8_t pl[16];
  write_u32le(pl + 0, 1u);
  write_u32le(pl + 4, (uint32_t)h);
  write_u32le(pl + 8, (uint32_t)ZI_HANDLE_OP_SHUT_WR);
  write_u32le(pl + 12, 0u);

  uint8_t req[64];
  build_zcl1_req(req, (uint16_t)ZI_CTL_OP_HANDLE_OP, 99u, pl, (uint32_t)sizeof(pl));
  uint8_t resp[256];
  int32_t n = zi_ctl((zi_ptr_t)(uintptr_t)req, 24u + (uint32_t)sizeof(pl), (zi_ptr_t)(uintptr_t)resp, (zi_size32_t)sizeof(resp));
  if (n < 0) return 0;
  zi_zcl1_frame z;
  if (!zi_zcl1_parse(resp, (uint32_t)n, &z)) return 0;
  return (z.op == (uint16_t)ZI_CTL_OP_HANDLE_OP && z.rid == 99u);
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

  if (setenv("ZI_NET_LISTEN_ALLOW", "loopback", 1) != 0) {
    perror("setenv listen allow");
    return 1;
  }
  if (setenv("ZI_NET_ALLOW", "loopback", 1) != 0) {
    perror("setenv net allow");
    return 1;
  }

  // Open sys/loop.
  uint8_t loop_req[40];
  build_open_req(loop_req, ZI_CAP_KIND_SYS, ZI_CAP_NAME_LOOP, NULL, 0);
  zi_handle_t loop_h = zi_cap_open((zi_ptr_t)(uintptr_t)loop_req);
  if (loop_h < 3) {
    fprintf(stderr, "loop open failed: %d\n", loop_h);
    return 1;
  }

  // Open listener on ephemeral port and learn the port.
  const char *host = "127.0.0.1";
  uint32_t bound_port = 0;
  uint8_t lparams[32];
  uint8_t lreq[40];
  build_tcp_listen_params(lparams, host, 0u, (ZI_TCP_OPEN_LISTEN | ZI_TCP_OPEN_NODELAY), 128u, &bound_port);
  build_open_req(lreq, ZI_CAP_KIND_NET, ZI_CAP_NAME_TCP, lparams, (uint32_t)sizeof(lparams));
  zi_handle_t listen_h = zi_cap_open((zi_ptr_t)(uintptr_t)lreq);
  if (listen_h < 3 || bound_port == 0) {
    fprintf(stderr, "listen open failed: %d bound_port=%u\n", listen_h, bound_port);
    return 1;
  }

  const uint64_t WATCH_LISTEN = 0x11111111ull;
  if (!loop_watch(loop_h, listen_h, 0x1u, WATCH_LISTEN)) {
    fprintf(stderr, "WATCH listen failed\n");
    return 1;
  }

  // Connect client.
  uint8_t cparams[20];
  uint8_t creq[40];
  build_tcp_params(cparams, host, bound_port, 0);
  build_open_req(creq, ZI_CAP_KIND_NET, ZI_CAP_NAME_TCP, cparams, (uint32_t)sizeof(cparams));
  zi_handle_t client_h = zi_cap_open((zi_ptr_t)(uintptr_t)creq);
  if (client_h < 3) {
    fprintf(stderr, "client open failed: %d\n", client_h);
    return 1;
  }
  const uint64_t WATCH_CLIENT = 0x22222222ull;
  if (!loop_watch(loop_h, client_h, 0x3u, WATCH_CLIENT)) {
    fprintf(stderr, "WATCH client failed\n");
    return 1;
  }

  // Accept one connection.
  if (!loop_wait_ready(loop_h, WATCH_LISTEN, 0x1u, 1000u)) {
    fprintf(stderr, "timeout waiting listen readable\n");
    return 1;
  }
  uint8_t acc[32];
  int32_t arn = zi_read(listen_h, (zi_ptr_t)(uintptr_t)acc, (zi_size32_t)sizeof(acc));
  if (arn == ZI_E_AGAIN) {
    if (!loop_wait_ready(loop_h, WATCH_LISTEN, 0x1u, 1000u)) return 1;
    arn = zi_read(listen_h, (zi_ptr_t)(uintptr_t)acc, (zi_size32_t)sizeof(acc));
  }
  if (arn != 32) {
    fprintf(stderr, "accept failed: %d\n", arn);
    return 1;
  }
  zi_handle_t server_h = (zi_handle_t)read_u32le(acc + 0);
  if (server_h < 3) {
    fprintf(stderr, "bad server handle: %d\n", server_h);
    return 1;
  }
  const uint64_t WATCH_SERVER = 0x33333333ull;
  if (!loop_watch(loop_h, server_h, 0x3u, WATCH_SERVER)) {
    fprintf(stderr, "WATCH server failed\n");
    return 1;
  }

  // Client sends one byte.
  uint8_t x = (uint8_t)'x';
  int32_t cw = zi_write(client_h, (zi_ptr_t)(uintptr_t)&x, 1);
  if (cw == ZI_E_AGAIN) {
    if (!loop_wait_ready(loop_h, WATCH_CLIENT, 0x2u, 1000u)) return 1;
    cw = zi_write(client_h, (zi_ptr_t)(uintptr_t)&x, 1);
  }
  if (cw != 1) {
    fprintf(stderr, "client write failed: %d\n", cw);
    return 1;
  }

  // Server reads it.
  uint8_t b = 0;
  int32_t sr = zi_read(server_h, (zi_ptr_t)(uintptr_t)&b, 1);
  if (sr == ZI_E_AGAIN) {
    if (!loop_wait_ready(loop_h, WATCH_SERVER, 0x1u, 1000u)) return 1;
    sr = zi_read(server_h, (zi_ptr_t)(uintptr_t)&b, 1);
  }
  if (sr != 1 || b != (uint8_t)'x') {
    fprintf(stderr, "server read mismatch: %d b=%u\n", sr, (unsigned)b);
    return 1;
  }

  // Half-close server write side.
  if (!ctl_handle_shutdown_write(server_h)) {
    fprintf(stderr, "shutdown-write ctl failed\n");
    return 1;
  }

  // After shutdown-write, server writes should fail.
  uint8_t y = (uint8_t)'y';
  int32_t sw = zi_write(server_h, (zi_ptr_t)(uintptr_t)&y, 1);
  if (sw >= 0) {
    fprintf(stderr, "expected server write to fail after shutdown, got %d\n", sw);
    return 1;
  }

  // Client should observe EOF (read returns 0) eventually.
  for (int tries = 0; tries < 20; tries++) {
    uint8_t tmp = 0;
    int32_t cr = zi_read(client_h, (zi_ptr_t)(uintptr_t)&tmp, 1);
    if (cr == 0) break;
    if (cr == ZI_E_AGAIN) {
      if (!loop_wait_ready(loop_h, WATCH_CLIENT, 0x1u, 1000u)) return 1;
      continue;
    }
    if (cr < 0) {
      fprintf(stderr, "client read error: %d\n", cr);
      return 1;
    }
    // If we got unexpected data, fail.
    fprintf(stderr, "expected EOF, got %d bytes\n", cr);
    return 1;
  }

  // Client can still write after observing EOF from peer's write side.
  uint8_t q = (uint8_t)'q';
  cw = zi_write(client_h, (zi_ptr_t)(uintptr_t)&q, 1);
  if (cw == ZI_E_AGAIN) {
    if (!loop_wait_ready(loop_h, WATCH_CLIENT, 0x2u, 1000u)) return 1;
    cw = zi_write(client_h, (zi_ptr_t)(uintptr_t)&q, 1);
  }
  if (cw != 1) {
    fprintf(stderr, "client write2 failed: %d\n", cw);
    return 1;
  }

  // Server can still read.
  b = 0;
  sr = zi_read(server_h, (zi_ptr_t)(uintptr_t)&b, 1);
  if (sr == ZI_E_AGAIN) {
    if (!loop_wait_ready(loop_h, WATCH_SERVER, 0x1u, 1000u)) return 1;
    sr = zi_read(server_h, (zi_ptr_t)(uintptr_t)&b, 1);
  }
  if (sr != 1 || b != (uint8_t)'q') {
    fprintf(stderr, "server read2 mismatch: %d b=%u\n", sr, (unsigned)b);
    return 1;
  }

  (void)zi_end(server_h);
  (void)zi_end(client_h);
  (void)zi_end(listen_h);
  (void)zi_end(loop_h);

  printf("ok\n");
  return 0;
}
