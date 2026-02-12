#include "zi_caps.h"
#include "zi_handles25.h"
#include "zi_net_tcp25.h"
#include "zi_runtime25.h"
#include "zi_sys_loop25.h"
#include "zi_sysabi25.h"
#include "zi_zcl1.h"

#include <errno.h>
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

static int loop_poll_once(zi_handle_t loop_h, uint32_t timeout_ms, uint8_t *out_fr, uint32_t out_cap, uint32_t *out_pl_len,
                          const uint8_t **out_pl) {
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

static void build_tcp_listen_params(uint8_t params[32], const char *host, uint32_t port, uint32_t flags, uint32_t backlog,
                                   uint32_t *out_bound_port) {
  write_u64le(params + 0, (uint64_t)(uintptr_t)host);
  write_u32le(params + 8, (uint32_t)strlen(host));
  write_u32le(params + 12, port);
  write_u32le(params + 16, flags);
  write_u32le(params + 20, backlog);
  write_u64le(params + 24, (uint64_t)(uintptr_t)out_bound_port);
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

  // Bind a listener on an ephemeral port (port=0), and read back the chosen port.
  zi_handle_t listen_h = 0;
  uint32_t port = 0;
  const char *host = "127.0.0.1";
  uint32_t bound_port = 0;
  uint8_t params[32];
  uint8_t req[40];
  build_tcp_listen_params(params, host, 0u, (ZI_TCP_OPEN_LISTEN | ZI_TCP_OPEN_NODELAY), 128u, &bound_port);
  build_open_req(req, ZI_CAP_KIND_NET, ZI_CAP_NAME_TCP, params, (uint32_t)sizeof(params));
  listen_h = zi_cap_open((zi_ptr_t)(uintptr_t)req);
  if (listen_h < 3) {
    fprintf(stderr, "failed to bind ephemeral listener: %d\n", listen_h);
    (void)zi_end(loop_h);
    return 1;
  }
  port = bound_port;
  if (port == 0) {
    fprintf(stderr, "listener did not report bound port\n");
    (void)zi_end(listen_h);
    (void)zi_end(loop_h);
    return 1;
  }

  const uint64_t WATCH_LISTEN = 0xA1A2A3A4A5A6A7A8ull;
  if (!loop_watch(loop_h, listen_h, 0x1u, WATCH_LISTEN)) {
    fprintf(stderr, "WATCH listener failed\n");
    (void)zi_end(listen_h);
    (void)zi_end(loop_h);
    return 1;
  }

  // Connect multiple clients.
  enum { NCLIENT = 3 };
  zi_handle_t client_h[NCLIENT];
  uint64_t watch_client[NCLIENT];
  uint8_t payload[NCLIENT];
  uint8_t expect_ack[NCLIENT];
  memset(client_h, 0, sizeof(client_h));
  for (int i = 0; i < NCLIENT; i++) {
    uint8_t cparams[20];
    uint8_t creq[40];
    build_tcp_params(cparams, host, port, 0);
    build_open_req(creq, ZI_CAP_KIND_NET, ZI_CAP_NAME_TCP, cparams, (uint32_t)sizeof(cparams));
    client_h[i] = zi_cap_open((zi_ptr_t)(uintptr_t)creq);
    if (client_h[i] < 3) {
      fprintf(stderr, "client open failed: %d\n", client_h[i]);
      return 1;
    }
    watch_client[i] = 0xB1B2B3B4B5B6B7B8ull + (uint64_t)i;
    if (!loop_watch(loop_h, client_h[i], 0x3u, watch_client[i])) {
      fprintf(stderr, "WATCH client failed\n");
      return 1;
    }
    payload[i] = (uint8_t)('a' + i);
    expect_ack[i] = (uint8_t)('A' + i);

    int32_t wn = zi_write(client_h[i], (zi_ptr_t)(uintptr_t)&payload[i], (zi_size32_t)1);
    if (wn == ZI_E_AGAIN) {
      if (!loop_wait_ready(loop_h, watch_client[i], 0x2u, 1000u)) {
        fprintf(stderr, "timeout waiting client writable\n");
        return 1;
      }
      wn = zi_write(client_h[i], (zi_ptr_t)(uintptr_t)&payload[i], (zi_size32_t)1);
    }
    if (wn != 1) {
      fprintf(stderr, "client write failed: %d\n", wn);
      return 1;
    }
  }

  // Wait for listener readability and accept records. Confirm batching by
  // observing a zi_read() that returns >32 bytes (multiple records).
  zi_handle_t server_h[NCLIENT];
  uint64_t watch_server[NCLIENT];
  memset(server_h, 0, sizeof(server_h));
  memset(watch_server, 0, sizeof(watch_server));

  int accepted = 0;
  int saw_batched = 0;
  for (int tries = 0; tries < 20 && accepted < NCLIENT; tries++) {
    if (!loop_wait_ready(loop_h, WATCH_LISTEN, 0x1u, 1000u)) {
      fprintf(stderr, "timeout waiting listener readable\n");
      return 1;
    }

    uint8_t acc[32u * NCLIENT];
    int32_t arn = zi_read(listen_h, (zi_ptr_t)(uintptr_t)acc, (zi_size32_t)sizeof(acc));
    if (arn == ZI_E_AGAIN) continue;
    if (arn < 0) {
      fprintf(stderr, "accept read failed: %d\n", arn);
      return 1;
    }
    if ((arn % 32) != 0) {
      fprintf(stderr, "accept returned non-multiple-of-32: %d\n", arn);
      return 1;
    }
    if (arn > 32) saw_batched = 1;

    int recs = arn / 32;
    for (int r = 0; r < recs && accepted < NCLIENT; r++) {
      zi_handle_t sh = (zi_handle_t)read_u32le(acc + (uint32_t)r * 32u + 0);
      if (sh < 3) {
        fprintf(stderr, "bad accepted handle: %d\n", sh);
        return 1;
      }
      server_h[accepted] = sh;
      watch_server[accepted] = 0xC1C2C3C4C5C6C7C8ull + (uint64_t)accepted;
      if (!loop_watch(loop_h, sh, 0x3u, watch_server[accepted])) {
        fprintf(stderr, "WATCH accepted stream failed\n");
        return 1;
      }
      accepted++;
    }
  }

  if (accepted != NCLIENT) {
    fprintf(stderr, "expected %d accepts, got %d\n", NCLIENT, accepted);
    return 1;
  }
  if (!saw_batched) {
    fprintf(stderr, "expected batched accept (zi_read > 32), did not observe\n");
    return 1;
  }

  // For each accepted server stream, read 1 byte from the client and reply with 1 byte.
  for (int i = 0; i < NCLIENT; i++) {
    uint8_t b = 0;
    int32_t rn = zi_read(server_h[i], (zi_ptr_t)(uintptr_t)&b, (zi_size32_t)1);
    if (rn == ZI_E_AGAIN) {
      if (!loop_wait_ready(loop_h, watch_server[i], 0x1u, 1000u)) {
        fprintf(stderr, "timeout waiting server readable\n");
        return 1;
      }
      rn = zi_read(server_h[i], (zi_ptr_t)(uintptr_t)&b, (zi_size32_t)1);
    }
    if (rn != 1 || b != payload[i]) {
      fprintf(stderr, "server read mismatch on %d: rn=%d b=%u\n", i, rn, (unsigned)b);
      return 1;
    }

    int32_t sw = zi_write(server_h[i], (zi_ptr_t)(uintptr_t)&expect_ack[i], (zi_size32_t)1);
    if (sw == ZI_E_AGAIN) {
      if (!loop_wait_ready(loop_h, watch_server[i], 0x2u, 1000u)) {
        fprintf(stderr, "timeout waiting server writable\n");
        return 1;
      }
      sw = zi_write(server_h[i], (zi_ptr_t)(uintptr_t)&expect_ack[i], (zi_size32_t)1);
    }
    if (sw != 1) {
      fprintf(stderr, "server write failed on %d: %d\n", i, sw);
      return 1;
    }

    uint8_t ack = 0;
    int32_t cr = zi_read(client_h[i], (zi_ptr_t)(uintptr_t)&ack, (zi_size32_t)1);
    if (cr == ZI_E_AGAIN) {
      if (!loop_wait_ready(loop_h, watch_client[i], 0x1u, 1000u)) {
        fprintf(stderr, "timeout waiting client readable\n");
        return 1;
      }
      cr = zi_read(client_h[i], (zi_ptr_t)(uintptr_t)&ack, (zi_size32_t)1);
    }
    if (cr != 1 || ack != expect_ack[i]) {
      fprintf(stderr, "client read mismatch on %d: cr=%d ack=%u\n", i, cr, (unsigned)ack);
      return 1;
    }
  }

  for (int i = 0; i < NCLIENT; i++) {
    (void)zi_end(server_h[i]);
    (void)zi_end(client_h[i]);
  }
  (void)zi_end(listen_h);
  (void)zi_end(loop_h);

  printf("ok\n");
  return 0;
}
