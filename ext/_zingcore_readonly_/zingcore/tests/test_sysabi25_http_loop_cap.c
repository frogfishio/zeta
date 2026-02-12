#include "zi_caps.h"
#include "zi_handles25.h"
#include "zi_net_http25.h"
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

static zi_ptr_t test_host_alloc(void *ctx, zi_size32_t size) {
  (void)ctx;
  void *p = malloc((size_t)size);
  return (zi_ptr_t)(uintptr_t)p;
}

static int32_t test_host_free(void *ctx, zi_ptr_t ptr) {
  (void)ctx;
  free((void *)(uintptr_t)ptr);
  return 0;
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
    if (w == 0) return ZI_E_IO;
    off += (uint32_t)w;
  }
  return 0;
}

static int read_full_frame_blocking(zi_handle_t h, uint8_t *buf, uint32_t cap) {
  uint32_t got = 0;
  while (got < 24u) {
    int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(buf + got), (zi_size32_t)(cap - got));
    if (n < 0) return (int)n;
    if (n == 0) return ZI_E_CLOSED;
    got += (uint32_t)n;
  }
  if (!(buf[0] == 'Z' && buf[1] == 'C' && buf[2] == 'L' && buf[3] == '1')) return ZI_E_INVALID;
  uint32_t pl = zi_zcl1_read_u32(buf + 20);
  uint32_t need = 24u + pl;
  if (need > cap) return ZI_E_BOUNDS;
  while (got < need) {
    int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(buf + got), (zi_size32_t)(need - got));
    if (n < 0) return (int)n;
    if (n == 0) return ZI_E_CLOSED;
    got += (uint32_t)n;
  }
  return (int)got;
}

static int poll_until_ready(zi_handle_t loop_h, uint64_t watch_id, uint32_t expected_handle, uint32_t timeout_ms) {
  uint8_t pl[8];
  write_u32le(pl + 0, 8u);
  write_u32le(pl + 4, timeout_ms);

  uint8_t fr[64];
  int fn = zi_zcl1_write_ok(fr, (uint32_t)sizeof(fr), (uint16_t)ZI_SYS_LOOP_OP_POLL, 2u, pl, (uint32_t)sizeof(pl));
  if (fn <= 0) return ZI_E_INTERNAL;
  if (write_all_handle(loop_h, fr, (uint32_t)fn) != 0) return ZI_E_IO;

  uint8_t buf[1024];
  int rn = read_full_frame_blocking(loop_h, buf, (uint32_t)sizeof(buf));
  if (rn < 0) return rn;

  zi_zcl1_frame z;
  if (!zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != (uint16_t)ZI_SYS_LOOP_OP_POLL || z.rid != 2u) return ZI_E_INVALID;
  if (zi_zcl1_read_u32(buf + 12) != 1u) return ZI_E_INTERNAL;

  if (z.payload_len < 16u) return ZI_E_INVALID;
  uint32_t ver = read_u32le(z.payload + 0);
  uint32_t count = read_u32le(z.payload + 8);
  if (ver != 1u) return ZI_E_INVALID;
  if (count == 0u) return ZI_E_AGAIN;

  const uint8_t *ev = z.payload + 16;
  uint32_t need = 16u + count * 32u;
  if (z.payload_len < need) return ZI_E_INVALID;

  for (uint32_t i = 0; i < count; i++) {
    const uint8_t *e = ev + i * 32u;
    uint32_t kind = read_u32le(e + 0);
    uint32_t events = read_u32le(e + 4);
    uint32_t handle = read_u32le(e + 8);
    uint64_t id = (uint64_t)read_u32le(e + 16) | ((uint64_t)read_u32le(e + 20) << 32);
    if (kind == 1u && id == watch_id && handle == expected_handle && (events & 0x1u)) {
      return 0;
    }
  }

  return ZI_E_AGAIN;
}

static int read_full_frame_via_loop(zi_handle_t h, zi_handle_t loop_h, uint64_t watch_id, uint8_t *buf, uint32_t cap) {
  uint32_t got = 0;

  while (got < 24u) {
    int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(buf + got), (zi_size32_t)(cap - got));
    if (n == ZI_E_AGAIN) {
      int pr = poll_until_ready(loop_h, watch_id, (uint32_t)h, 1000u);
      if (pr < 0 && pr != ZI_E_AGAIN) return pr;
      continue;
    }
    if (n < 0) return (int)n;
    if (n == 0) return ZI_E_CLOSED;
    got += (uint32_t)n;
  }

  if (!(buf[0] == 'Z' && buf[1] == 'C' && buf[2] == 'L' && buf[3] == '1')) return ZI_E_INVALID;
  uint32_t pl = zi_zcl1_read_u32(buf + 20);
  uint32_t need = 24u + pl;
  if (need > cap) return ZI_E_BOUNDS;

  while (got < need) {
    int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(buf + got), (zi_size32_t)(need - got));
    if (n == ZI_E_AGAIN) {
      int pr = poll_until_ready(loop_h, watch_id, (uint32_t)h, 1000u);
      if (pr < 0 && pr != ZI_E_AGAIN) return pr;
      continue;
    }
    if (n < 0) return (int)n;
    if (n == 0) return ZI_E_CLOSED;
    got += (uint32_t)n;
  }

  return (int)got;
}

static int send_respond_inline(zi_handle_t http_h, zi_handle_t loop_h, uint64_t watch_id, uint32_t rid, const char *body) {
  uint8_t pl[512];
  uint32_t off = 0;

  // status, flags, header_count
  write_u32le(pl + off, 200u);
  off += 4;
  write_u32le(pl + off, 0u);
  off += 4;
  write_u32le(pl + off, 1u);
  off += 4;

  const char hn[] = "content-type";
  const char hv[] = "text/plain";
  write_u32le(pl + off, (uint32_t)strlen(hn));
  off += 4;
  memcpy(pl + off, hn, strlen(hn));
  off += (uint32_t)strlen(hn);
  write_u32le(pl + off, (uint32_t)strlen(hv));
  off += 4;
  memcpy(pl + off, hv, strlen(hv));
  off += (uint32_t)strlen(hv);

  uint32_t blen = (uint32_t)strlen(body);
  write_u32le(pl + off, blen);
  off += 4;
  memcpy(pl + off, body, blen);
  off += blen;

  uint8_t fr[1024];
  int fn = zi_zcl1_write_ok(fr, (uint32_t)sizeof(fr), 11u, rid, pl, off);
  if (fn <= 0) return ZI_E_INTERNAL;
  if (write_all_handle(http_h, fr, (uint32_t)fn) != 0) return ZI_E_IO;

  uint8_t rbuf[1024];
  int rn = read_full_frame_via_loop(http_h, loop_h, watch_id, rbuf, (uint32_t)sizeof(rbuf));
  if (rn < 0) return rn;
  zi_zcl1_frame z;
  if (!zi_zcl1_parse(rbuf, (uint32_t)rn, &z) || z.op != 11u || z.rid != rid) return ZI_E_INVALID;
  if (zi_zcl1_read_u32(rbuf + 12) != 1u) return ZI_E_INTERNAL;
  return 0;
}

static int recv_all_str(int fd, char *out, size_t cap) {
  size_t off = 0;
  while (off + 1 < cap) {
    ssize_t n = recv(fd, out + off, cap - 1 - off, 0);
    if (n < 0) return -1;
    if (n == 0) break;
    off += (size_t)n;
  }
  out[off] = 0;
  return 0;
}

int main(void) {
  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);

  zi_host_v1 host;
  memset(&host, 0, sizeof(host));
  host.alloc = test_host_alloc;
  host.free = test_host_free;
  zi_runtime25_set_host(&host);

  zi_caps_reset_for_test();
  zi_handles25_reset_for_test();

  if (!zi_caps_init()) {
    fprintf(stderr, "zi_caps_init failed\n");
    return 1;
  }
  if (!zi_net_http25_register()) {
    fprintf(stderr, "zi_net_http25_register failed\n");
    return 1;
  }
  if (!zi_sys_loop25_register()) {
    fprintf(stderr, "zi_sys_loop25_register failed\n");
    return 1;
  }

  if (setenv("ZI_NET_LISTEN_ALLOW", "loopback", 1) != 0) {
    perror("setenv");
    return 1;
  }
  if (setenv("ZI_NET_ALLOW", "loopback", 1) != 0) {
    perror("setenv");
    return 1;
  }

  // Open HTTP cap.
  uint8_t open_req[40];
  build_open_req(open_req, ZI_CAP_KIND_NET, ZI_CAP_NAME_HTTP, NULL, 0);
  zi_handle_t http_h = zi_cap_open((zi_ptr_t)(uintptr_t)open_req);
  if (http_h < 3) {
    fprintf(stderr, "http open failed: %d\n", http_h);
    return 1;
  }

  // Open sys/loop cap.
  build_open_req(open_req, ZI_CAP_KIND_SYS, ZI_CAP_NAME_LOOP, NULL, 0);
  zi_handle_t loop_h = zi_cap_open((zi_ptr_t)(uintptr_t)open_req);
  if (loop_h < 3) {
    fprintf(stderr, "loop open failed: %d\n", loop_h);
    return 1;
  }

  // WATCH http handle for readable.
  const uint64_t watch_id = 0x1111u;
  uint8_t wpl[20];
  write_u32le(wpl + 0, (uint32_t)http_h);
  write_u32le(wpl + 4, 0x1u);
  write_u64le(wpl + 8, watch_id);
  write_u32le(wpl + 16, 0u);

  uint8_t wfr[128];
  int wfn = zi_zcl1_write_ok(wfr, (uint32_t)sizeof(wfr), (uint16_t)ZI_SYS_LOOP_OP_WATCH, 1u, wpl, (uint32_t)sizeof(wpl));
  if (wfn <= 0 || write_all_handle(loop_h, wfr, (uint32_t)wfn) != 0) {
    fprintf(stderr, "WATCH write failed\n");
    return 1;
  }

  uint8_t buf[4096];
  int rn = read_full_frame_blocking(loop_h, buf, (uint32_t)sizeof(buf));
  if (rn < 0) {
    fprintf(stderr, "WATCH read failed: %d\n", rn);
    return 1;
  }
  zi_zcl1_frame z;
  if (!zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != (uint16_t)ZI_SYS_LOOP_OP_WATCH || z.rid != 1u || zi_zcl1_read_u32(buf + 12) != 1u) {
    fprintf(stderr, "WATCH response invalid\n");
    return 1;
  }

  // LISTEN (op=1)
  uint8_t lpl[12];
  write_u32le(lpl + 0, 0);
  write_u32le(lpl + 4, 0);
  write_u32le(lpl + 8, 0);

  uint8_t lfr[128];
  int lfn = zi_zcl1_write_ok(lfr, (uint32_t)sizeof(lfr), 1u, 1u, lpl, (uint32_t)sizeof(lpl));
  if (lfn <= 0 || write_all_handle(http_h, lfr, (uint32_t)lfn) != 0) {
    fprintf(stderr, "LISTEN write failed\n");
    return 1;
  }

  rn = read_full_frame_via_loop(http_h, loop_h, watch_id, buf, (uint32_t)sizeof(buf));
  if (rn < 0) {
    fprintf(stderr, "LISTEN read failed: %d\n", rn);
    return 1;
  }
  if (!zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 1u) {
    fprintf(stderr, "LISTEN response invalid\n");
    return 1;
  }
  if (z.payload_len != 24u) {
    fprintf(stderr, "LISTEN payload invalid\n");
    return 1;
  }
  uint32_t listener_id = zi_zcl1_read_u32(z.payload + 0);
  uint32_t bound_port = zi_zcl1_read_u32(z.payload + 4);
  if (listener_id == 0u || bound_port == 0u) {
    fprintf(stderr, "LISTEN returned invalid listener/port\n");
    return 1;
  }

  // Two clients connect and send requests back-to-back, before reading any events.
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)bound_port);
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  int s1 = socket(AF_INET, SOCK_STREAM, 0);
  if (s1 < 0) {
    perror("socket");
    return 1;
  }
  if (connect(s1, (struct sockaddr *)&sa, (socklen_t)sizeof(sa)) != 0) {
    perror("connect");
    close(s1);
    return 1;
  }

  int s2 = socket(AF_INET, SOCK_STREAM, 0);
  if (s2 < 0) {
    perror("socket");
    close(s1);
    return 1;
  }
  if (connect(s2, (struct sockaddr *)&sa, (socklen_t)sizeof(sa)) != 0) {
    perror("connect");
    close(s2);
    close(s1);
    return 1;
  }

  const char req1[] = "GET /a HTTP/1.1\r\nHost: localhost\r\n\r\n";
  const char req2[] = "GET /b HTTP/1.1\r\nHost: localhost\r\n\r\n";
  if (send(s1, req1, sizeof(req1) - 1, 0) != (ssize_t)(sizeof(req1) - 1)) {
    perror("send");
    close(s2);
    close(s1);
    return 1;
  }
  if (send(s2, req2, sizeof(req2) - 1, 0) != (ssize_t)(sizeof(req2) - 1)) {
    perror("send");
    close(s2);
    close(s1);
    return 1;
  }

  // Wait for two EV_REQUEST frames using sys/loop readiness.
  uint32_t rid1 = 0, rid2 = 0;
  rn = read_full_frame_via_loop(http_h, loop_h, watch_id, buf, (uint32_t)sizeof(buf));
  if (rn < 0) {
    fprintf(stderr, "EV_REQUEST(1) read failed: %d\n", rn);
    close(s2);
    close(s1);
    return 1;
  }
  if (!zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 100u) {
    fprintf(stderr, "unexpected event(1)\n");
    close(s2);
    close(s1);
    return 1;
  }
  rid1 = z.rid;
  if (zi_zcl1_read_u32(z.payload + 0) != listener_id) {
    fprintf(stderr, "listener id mismatch(1)\n");
    close(s2);
    close(s1);
    return 1;
  }

  rn = read_full_frame_via_loop(http_h, loop_h, watch_id, buf, (uint32_t)sizeof(buf));
  if (rn < 0) {
    fprintf(stderr, "EV_REQUEST(2) read failed: %d\n", rn);
    close(s2);
    close(s1);
    return 1;
  }
  if (!zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 100u) {
    fprintf(stderr, "unexpected event(2)\n");
    close(s2);
    close(s1);
    return 1;
  }
  rid2 = z.rid;
  if (zi_zcl1_read_u32(z.payload + 0) != listener_id) {
    fprintf(stderr, "listener id mismatch(2)\n");
    close(s2);
    close(s1);
    return 1;
  }
  if (rid1 == 0u || rid2 == 0u || rid1 == rid2) {
    fprintf(stderr, "unexpected request ids rid1=%u rid2=%u\n", rid1, rid2);
    close(s2);
    close(s1);
    return 1;
  }

  // RESPOND_INLINE to both requests.
  if (send_respond_inline(http_h, loop_h, watch_id, rid1, "one") != 0) {
    fprintf(stderr, "RESPOND_INLINE(1) failed\n");
    close(s2);
    close(s1);
    return 1;
  }
  if (send_respond_inline(http_h, loop_h, watch_id, rid2, "two") != 0) {
    fprintf(stderr, "RESPOND_INLINE(2) failed\n");
    close(s2);
    close(s1);
    return 1;
  }

  // Both clients should receive 200 + the expected body.
  char resp1[512];
  char resp2[512];
  if (recv_all_str(s1, resp1, sizeof(resp1)) != 0) {
    fprintf(stderr, "client(1) recv failed\n");
    close(s2);
    close(s1);
    return 1;
  }
  if (recv_all_str(s2, resp2, sizeof(resp2)) != 0) {
    fprintf(stderr, "client(2) recv failed\n");
    close(s2);
    close(s1);
    return 1;
  }
  if (strstr(resp1, "HTTP/1.1 200") == NULL || strstr(resp1, "one") == NULL) {
    fprintf(stderr, "unexpected client(1) response: %s\n", resp1);
    close(s2);
    close(s1);
    return 1;
  }
  if (strstr(resp2, "HTTP/1.1 200") == NULL || strstr(resp2, "two") == NULL) {
    fprintf(stderr, "unexpected client(2) response: %s\n", resp2);
    close(s2);
    close(s1);
    return 1;
  }

  close(s2);
  close(s1);
  (void)zi_end(loop_h);
  (void)zi_end(http_h);

  printf("ok\n");
  return 0;
}
