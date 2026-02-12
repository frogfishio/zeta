#include "zi_caps.h"
#include "zi_handles25.h"
#include "zi_net_http25.h"
#include "zi_runtime25.h"
#include "zi_sysabi25.h"
#include "zi_sys_loop25.h"
#include "zi_zcl1.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
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

static void build_open_req(uint8_t req[40], const char *kind, const char *name, const void *params, uint32_t params_len) {
  write_u64le(req + 0, (uint64_t)(uintptr_t)kind);
  write_u32le(req + 8, (uint32_t)strlen(kind));
  write_u64le(req + 12, (uint64_t)(uintptr_t)name);
  write_u32le(req + 20, (uint32_t)strlen(name));
  write_u32le(req + 24, 0);
  write_u64le(req + 28, (uint64_t)(uintptr_t)params);
  write_u32le(req + 36, params_len);
}

static int sys_loop_poll_once(zi_handle_t loop_h, uint32_t timeout_ms) {
  uint8_t pl[8];
  write_u32le(pl + 0, 8); // max_events
  write_u32le(pl + 4, timeout_ms);
  uint8_t fr[64];
  int fn = zi_zcl1_write_ok(fr, (uint32_t)sizeof(fr), (uint16_t)ZI_SYS_LOOP_OP_POLL, 1, pl, (uint32_t)sizeof(pl));
  if (fn <= 0) return ZI_E_INTERNAL;
  int32_t wn = zi_write(loop_h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)fn);
  if (wn != fn) return (wn < 0) ? wn : ZI_E_IO;

  uint8_t buf[512];
  int rn = 0;
  // Read a full frame (small and synchronous).
  uint32_t got = 0;
  while (got < 24u) {
    int32_t n = zi_read(loop_h, (zi_ptr_t)(uintptr_t)(buf + got), (zi_size32_t)(sizeof(buf) - got));
    if (n < 0) return n;
    if (n == 0) return ZI_E_CLOSED;
    got += (uint32_t)n;
  }
  if (!(buf[0] == 'Z' && buf[1] == 'C' && buf[2] == 'L' && buf[3] == '1')) return ZI_E_INVALID;
  uint32_t payload_len = zi_zcl1_read_u32(buf + 20);
  uint32_t need = 24u + payload_len;
  if (need > (uint32_t)sizeof(buf)) return ZI_E_BOUNDS;
  while (got < need) {
    int32_t n = zi_read(loop_h, (zi_ptr_t)(uintptr_t)(buf + got), (zi_size32_t)(need - got));
    if (n < 0) return n;
    if (n == 0) return ZI_E_CLOSED;
    got += (uint32_t)n;
  }
  rn = (int)got;
  zi_zcl1_frame z;
  if (!zi_zcl1_parse(buf, (uint32_t)rn, &z)) return ZI_E_INVALID;
  uint32_t st = zi_zcl1_read_u32(buf + 12);
  if (st != 1 || z.op != (uint16_t)ZI_SYS_LOOP_OP_POLL) return ZI_E_INVALID;
  return 0;
}

static int read_full_frame(zi_handle_t h, zi_handle_t loop_h, uint8_t *buf, uint32_t cap) {
  uint32_t got = 0;
    while (got < 24u) {
    int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(buf + got), (zi_size32_t)(cap - got));
    if (n == ZI_E_AGAIN && loop_h >= 3) {
      int pr = sys_loop_poll_once(loop_h, 1000u);
      if (pr < 0) return pr;
      continue;
    }
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
    if (n == ZI_E_AGAIN && loop_h >= 3) {
      int pr = sys_loop_poll_once(loop_h, 1000u);
      if (pr < 0) return pr;
      continue;
    }
    if (n < 0) return n;
    if (n == 0) return ZI_E_CLOSED;
    got += (uint32_t)n;
  }
  return (int)got;
}

static int write_all_handle(zi_handle_t h, const void *p, uint32_t n) {
  const uint8_t *b = (const uint8_t *)p;
  uint32_t off = 0;
  while (off < n) {
    int32_t w = zi_write(h, (zi_ptr_t)(uintptr_t)(b + off), n - off);
    if (w < 0) return w;
    if (w == 0) return ZI_E_IO;
    off += (uint32_t)w;
  }
  return 0;
}

typedef struct {
  const uint8_t *p;
  uint32_t n;
  uint32_t off;
} test_ro_body_ctx;

static int32_t test_ro_body_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  test_ro_body_ctx *c = (test_ro_body_ctx *)ctx;
  if (!c) return ZI_E_INTERNAL;
  if (c->off >= c->n) return 0;
  uint32_t remain = c->n - c->off;
  uint32_t take = (uint32_t)cap;
  if (take > remain) take = remain;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_rw) return ZI_E_INTERNAL;
  uint8_t *dst = NULL;
  if (!mem->map_rw(mem->ctx, dst_ptr, take, &dst) || !dst) return ZI_E_INTERNAL;
  memcpy(dst, c->p + c->off, take);
  c->off += take;
  return (int32_t)take;
}

static int32_t test_ro_body_end(void *ctx) {
  free(ctx);
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

  uint8_t req[40];
  build_open_req(req, ZI_CAP_KIND_NET, ZI_CAP_NAME_HTTP, NULL, 0);
  zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)req);
  if (h < 3) {
    fprintf(stderr, "expected handle, got %d\n", h);
    return 1;
  }

  // Open sys/loop and WATCH the http handle for readability.
  build_open_req(req, ZI_CAP_KIND_SYS, ZI_CAP_NAME_LOOP, NULL, 0);
  zi_handle_t loop_h = zi_cap_open((zi_ptr_t)(uintptr_t)req);
  if (loop_h < 3) {
    fprintf(stderr, "expected loop handle, got %d\n", loop_h);
    return 1;
  }
  {
    uint8_t wpl[20];
    write_u32le(wpl + 0, (uint32_t)h);
    write_u32le(wpl + 4, 0x1u); // readable
    write_u64le(wpl + 8, 1u);
    write_u32le(wpl + 16, 0u);

    uint8_t wfr[128];
    int wfn = zi_zcl1_write_ok(wfr, (uint32_t)sizeof(wfr), (uint16_t)ZI_SYS_LOOP_OP_WATCH, 1, wpl, (uint32_t)sizeof(wpl));
    if (wfn <= 0) {
      fprintf(stderr, "failed to build WATCH frame\n");
      return 1;
    }
    if (zi_write(loop_h, (zi_ptr_t)(uintptr_t)wfr, (zi_size32_t)wfn) != wfn) {
      fprintf(stderr, "WATCH write failed\n");
      return 1;
    }
    uint8_t wbuf[256];
    int wrn = read_full_frame(loop_h, 0, wbuf, (uint32_t)sizeof(wbuf));
    if (wrn < 0) {
      fprintf(stderr, "WATCH read failed: %d\n", wrn);
      return 1;
    }
    zi_zcl1_frame wz;
    uint32_t wst = zi_zcl1_read_u32(wbuf + 12);
    if (!zi_zcl1_parse(wbuf, (uint32_t)wrn, &wz) || wz.op != (uint16_t)ZI_SYS_LOOP_OP_WATCH || wst != 1) {
      fprintf(stderr, "unexpected WATCH response\n");
      return 1;
    }
  }

  // LISTEN
  uint8_t pl[12];
  write_u32le(pl + 0, 0);
  write_u32le(pl + 4, 0);
  write_u32le(pl + 8, 0);

  uint8_t fr[128];
  int fn = zi_zcl1_write_ok(fr, (uint32_t)sizeof(fr), 1, 1, pl, (uint32_t)sizeof(pl));
  if (fn <= 0) {
    fprintf(stderr, "failed to build LISTEN frame\n");
    return 1;
  }
  if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)fn) != fn) {
    fprintf(stderr, "LISTEN write failed\n");
    return 1;
  }

  uint8_t buf[4096];
  int rn = read_full_frame(h, loop_h, buf, (uint32_t)sizeof(buf));
  if (rn < 0) {
    fprintf(stderr, "LISTEN read failed: %d\n", rn);
    return 1;
  }
  zi_zcl1_frame z;
  if (!zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 1) {
    fprintf(stderr, "unexpected LISTEN response\n");
    return 1;
  }
  if (z.payload_len != 24) {
    fprintf(stderr, "unexpected LISTEN payload size\n");
    return 1;
  }
  uint32_t listener_id = zi_zcl1_read_u32(z.payload + 0);
  uint32_t bound_port = zi_zcl1_read_u32(z.payload + 4);
  if (listener_id == 0 || bound_port == 0) {
    fprintf(stderr, "invalid listener response\n");
    return 1;
  }

  // Client connects and sends request.
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    perror("socket");
    return 1;
  }
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)bound_port);
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(s, (struct sockaddr *)&sa, (socklen_t)sizeof(sa)) != 0) {
    perror("connect");
    close(s);
    return 1;
  }
  const char reqtxt[] = "GET /hello?x=1 HTTP/1.1\r\nHost: localhost\r\n\r\n";
  if (send(s, reqtxt, sizeof(reqtxt) - 1, 0) != (ssize_t)(sizeof(reqtxt) - 1)) {
    perror("send");
    close(s);
    return 1;
  }

  // Read EV_REQUEST.
  rn = read_full_frame(h, loop_h, buf, (uint32_t)sizeof(buf));
  if (rn < 0) {
    fprintf(stderr, "EV_REQUEST read failed: %d\n", rn);
    close(s);
    return 1;
  }
  if (!zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 100) {
    fprintf(stderr, "unexpected event\n");
    close(s);
    return 1;
  }
  if (zi_zcl1_read_u32(z.payload + 0) != listener_id) {
    fprintf(stderr, "listener id mismatch\n");
    close(s);
    return 1;
  }

  // RESPOND_INLINE
  // (Optional) RESPOND_START
  uint8_t rfr[512];
  uint8_t stpl[256];
  uint32_t st_off = 0;
  write_u32le(stpl + st_off, 200);
  st_off += 4;
  write_u32le(stpl + st_off, 0);
  st_off += 4;
  write_u32le(stpl + st_off, 1);
  st_off += 4;
  const char shn[] = "content-type";
  const char shv[] = "text/plain";
  write_u32le(stpl + st_off, (uint32_t)strlen(shn));
  st_off += 4;
  memcpy(stpl + st_off, shn, strlen(shn));
  st_off += (uint32_t)strlen(shn);
  write_u32le(stpl + st_off, (uint32_t)strlen(shv));
  st_off += 4;
  memcpy(stpl + st_off, shv, strlen(shv));
  st_off += (uint32_t)strlen(shv);

  fn = zi_zcl1_write_ok(rfr, (uint32_t)sizeof(rfr), 10, z.rid, stpl, st_off);
  if (fn <= 0) {
    fprintf(stderr, "failed to build RESPOND_START frame\n");
    close(s);
    return 1;
  }
  if (zi_write(h, (zi_ptr_t)(uintptr_t)rfr, (zi_size32_t)fn) != fn) {
    fprintf(stderr, "RESPOND_START write failed\n");
    close(s);
    return 1;
  }
  rn = read_full_frame(h, loop_h, buf, (uint32_t)sizeof(buf));
  if (rn < 0) {
    fprintf(stderr, "RESPOND_START ack read failed: %d\n", rn);
    close(s);
    return 1;
  }
  if (!zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 10) {
    fprintf(stderr, "unexpected RESPOND_START response\n");
    close(s);
    return 1;
  }

  uint8_t rpl[256];
  uint32_t off = 0;
  write_u32le(rpl + off, 200);
  off += 4;
  write_u32le(rpl + off, 0);
  off += 4;
  write_u32le(rpl + off, 1);
  off += 4;
  const char hn[] = "content-type";
  const char hv[] = "text/plain";
  write_u32le(rpl + off, (uint32_t)strlen(hn));
  off += 4;
  memcpy(rpl + off, hn, strlen(hn));
  off += (uint32_t)strlen(hn);
  write_u32le(rpl + off, (uint32_t)strlen(hv));
  off += 4;
  memcpy(rpl + off, hv, strlen(hv));
  off += (uint32_t)strlen(hv);
  const char body[] = "world";
  write_u32le(rpl + off, (uint32_t)strlen(body));
  off += 4;
  memcpy(rpl + off, body, strlen(body));
  off += (uint32_t)strlen(body);

  fn = zi_zcl1_write_ok(rfr, (uint32_t)sizeof(rfr), 11, z.rid, rpl, off);
  if (fn <= 0) {
    fprintf(stderr, "failed to build RESPOND_INLINE frame\n");
    close(s);
    return 1;
  }
  if (zi_write(h, (zi_ptr_t)(uintptr_t)rfr, (zi_size32_t)fn) != fn) {
    fprintf(stderr, "RESPOND_INLINE write failed\n");
    close(s);
    return 1;
  }

  rn = read_full_frame(h, loop_h, buf, (uint32_t)sizeof(buf));
  if (rn < 0) {
    fprintf(stderr, "RESPOND_INLINE ack read failed: %d\n", rn);
    close(s);
    return 1;
  }
  if (!zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 11) {
    fprintf(stderr, "unexpected RESPOND_INLINE response\n");
    close(s);
    return 1;
  }

  char respbuf[512];
  memset(respbuf, 0, sizeof(respbuf));
  ssize_t nrcv = recv(s, respbuf, sizeof(respbuf) - 1, 0);
  if (nrcv <= 0) {
    fprintf(stderr, "client recv failed\n");
    close(s);
    return 1;
  }
  if (strstr(respbuf, "HTTP/1.1 200") == NULL || strstr(respbuf, "world") == NULL) {
    fprintf(stderr, "unexpected http response: %s\n", respbuf);
    close(s);
    return 1;
  }
  close(s);

  {
    // Client sends a chunked request body; server should expose a decoded STREAM body handle.
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
      perror("socket");
      return 1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)bound_port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&sa, (socklen_t)sizeof(sa)) != 0) {
      perror("connect");
      close(s);
      return 1;
    }
  const char creq1[] = "POST /chunk HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Transfer-Encoding: chunked\r\n"
                       "Content-Type: text/plain\r\n"
                       "\r\n";
  const char cbody[] = "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
  if (send(s, creq1, sizeof(creq1) - 1, 0) != (ssize_t)(sizeof(creq1) - 1)) {
    perror("send");
    close(s);
    return 1;
  }
  if (send(s, cbody, sizeof(cbody) - 1, 0) != (ssize_t)(sizeof(cbody) - 1)) {
    perror("send");
    close(s);
    return 1;
  }

  rn = read_full_frame(h, loop_h, buf, (uint32_t)sizeof(buf));
  if (rn < 0) {
    fprintf(stderr, "EV_REQUEST(chunked) read failed: %d\n", rn);
    close(s);
    return 1;
  }
  if (!zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 100) {
    fprintf(stderr, "unexpected event for chunked\n");
    close(s);
    return 1;
  }

  // Parse body_kind + body handle from EV_REQUEST payload.
  uint32_t poff = 0;
  if (z.payload_len < 8) {
    fprintf(stderr, "bad EV_REQUEST payload (chunked)\n");
    close(s);
    return 1;
  }
  poff += 8; // listener_id + flags
  for (int i = 0; i < 4; i++) {
    if (poff + 4u > z.payload_len) {
      fprintf(stderr, "bad EV_REQUEST payload (chunked strings)\n");
      close(s);
      return 1;
    }
    uint32_t ln = zi_zcl1_read_u32(z.payload + poff);
    poff += 4;
    if (poff + ln > z.payload_len) {
      fprintf(stderr, "bad EV_REQUEST payload (chunked string len)\n");
      close(s);
      return 1;
    }
    poff += ln;
  }
  if (poff + 16u + 4u + 4u > z.payload_len) {
    fprintf(stderr, "bad EV_REQUEST payload (chunked peer)\n");
    close(s);
    return 1;
  }
  poff += 16;
  poff += 4;
  uint32_t hc = zi_zcl1_read_u32(z.payload + poff);
  poff += 4;
  for (uint32_t i = 0; i < hc; i++) {
    if (poff + 4u > z.payload_len) {
      fprintf(stderr, "bad EV_REQUEST payload (chunked hdr name len)\n");
      close(s);
      return 1;
    }
    uint32_t nlen = zi_zcl1_read_u32(z.payload + poff);
    poff += 4;
    if (poff + nlen + 4u > z.payload_len) {
      fprintf(stderr, "bad EV_REQUEST payload (chunked hdr name)\n");
      close(s);
      return 1;
    }
    poff += nlen;
    uint32_t vlen = zi_zcl1_read_u32(z.payload + poff);
    poff += 4;
    if (poff + vlen > z.payload_len) {
      fprintf(stderr, "bad EV_REQUEST payload (chunked hdr val)\n");
      close(s);
      return 1;
    }
    poff += vlen;
  }
  if (poff + 4u > z.payload_len) {
    fprintf(stderr, "bad EV_REQUEST payload (chunked body_kind)\n");
    close(s);
    return 1;
  }
  uint32_t body_kind = zi_zcl1_read_u32(z.payload + poff);
  poff += 4;
  if (body_kind != 2) {
    fprintf(stderr, "expected chunked body_kind=2, got %u\n", body_kind);
    close(s);
    return 1;
  }
  if (poff + 4u > z.payload_len) {
    fprintf(stderr, "bad EV_REQUEST payload (chunked body_handle)\n");
    close(s);
    return 1;
  }
  zi_handle_t body_h = (zi_handle_t)(int32_t)zi_zcl1_read_u32(z.payload + poff);
  if (body_h < 3) {
    fprintf(stderr, "expected chunked body handle, got %d\n", body_h);
    close(s);
    return 1;
  }

  // WATCH body handle for readability so sys_loop_poll_once can block.
  {
    uint8_t wpl[20];
    write_u32le(wpl + 0, (uint32_t)body_h);
    write_u32le(wpl + 4, 0x1u); // readable
    write_u64le(wpl + 8, 2u);
    write_u32le(wpl + 16, 0u);

    uint8_t wfr[128];
    int wfn = zi_zcl1_write_ok(wfr, (uint32_t)sizeof(wfr), (uint16_t)ZI_SYS_LOOP_OP_WATCH, 2, wpl, (uint32_t)sizeof(wpl));
    if (wfn <= 0 || zi_write(loop_h, (zi_ptr_t)(uintptr_t)wfr, (zi_size32_t)wfn) != wfn) {
      fprintf(stderr, "WATCH(body) write failed\n");
      close(s);
      return 1;
    }
    uint8_t wbuf[256];
    int wrn = read_full_frame(loop_h, 0, wbuf, (uint32_t)sizeof(wbuf));
    if (wrn < 0) {
      fprintf(stderr, "WATCH(body) read failed: %d\n", wrn);
      close(s);
      return 1;
    }
    zi_zcl1_frame wz;
    uint32_t wst = zi_zcl1_read_u32(wbuf + 12);
    if (!zi_zcl1_parse(wbuf, (uint32_t)wrn, &wz) || wz.op != (uint16_t)ZI_SYS_LOOP_OP_WATCH || wst != 1) {
      fprintf(stderr, "unexpected WATCH(body) response\n");
      close(s);
      return 1;
    }
  }

  char got_body[64];
  memset(got_body, 0, sizeof(got_body));
  uint32_t got_n = 0;
  for (;;) {
    int32_t n = zi_read(body_h, (zi_ptr_t)(uintptr_t)(got_body + got_n), (zi_size32_t)(sizeof(got_body) - 1 - got_n));
    if (n == ZI_E_AGAIN) {
      int pr = sys_loop_poll_once(loop_h, 1000u);
      if (pr < 0) {
        fprintf(stderr, "POLL(body) failed: %d\n", pr);
        close(s);
        return 1;
      }
      continue;
    }
    if (n < 0) {
      fprintf(stderr, "body read failed: %d\n", n);
      close(s);
      return 1;
    }
    if (n == 0) break;
    got_n += (uint32_t)n;
    if (got_n >= sizeof(got_body) - 1) break;
  }
  if (strcmp(got_body, "hello world") != 0) {
    fprintf(stderr, "unexpected decoded chunked body: '%s'\n", got_body);
    close(s);
    return 1;
  }
  (void)zi_end(body_h);

  // RESPOND_INLINE
  uint8_t rfr2[256];
  uint8_t rpl2[64];
  uint32_t roff2 = 0;
  write_u32le(rpl2 + roff2, 200);
  roff2 += 4;
  write_u32le(rpl2 + roff2, 0);
  roff2 += 4;
  write_u32le(rpl2 + roff2, 0); // header_count
  roff2 += 4;
  const char okb[] = "ok";
  write_u32le(rpl2 + roff2, (uint32_t)strlen(okb));
  roff2 += 4;
  memcpy(rpl2 + roff2, okb, strlen(okb));
  roff2 += (uint32_t)strlen(okb);
  fn = zi_zcl1_write_ok(rfr2, (uint32_t)sizeof(rfr2), 11, z.rid, rpl2, roff2);
  if (fn <= 0 || zi_write(h, (zi_ptr_t)(uintptr_t)rfr2, (zi_size32_t)fn) != fn) {
    fprintf(stderr, "RESPOND_INLINE(chunked) write failed\n");
    close(s);
    return 1;
  }
  rn = read_full_frame(h, loop_h, buf, (uint32_t)sizeof(buf));
  if (rn < 0 || !zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 11) {
    fprintf(stderr, "RESPOND_INLINE(chunked) ack read failed\n");
    close(s);
    return 1;
  }

  memset(respbuf, 0, sizeof(respbuf));
  nrcv = recv(s, respbuf, sizeof(respbuf) - 1, 0);
  if (nrcv <= 0) {
    fprintf(stderr, "client recv(chunked) failed\n");
    close(s);
    return 1;
  }
  if (strstr(respbuf, "HTTP/1.1 200") == NULL || strstr(respbuf, "ok") == NULL) {
    fprintf(stderr, "unexpected http response (chunked): %s\n", respbuf);
    close(s);
    return 1;
  }
    close(s);
  }

  // FETCH: spin up a tiny local HTTP server in a child.
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) {
    perror("socket");
    return 1;
  }
  struct sockaddr_in faddr;
  memset(&faddr, 0, sizeof(faddr));
  faddr.sin_family = AF_INET;
  faddr.sin_port = htons(0);
  faddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(srv, (struct sockaddr *)&faddr, (socklen_t)sizeof(faddr)) != 0) {
    perror("bind");
    close(srv);
    return 1;
  }
  if (listen(srv, 1) != 0) {
    perror("listen");
    close(srv);
    return 1;
  }
  struct sockaddr_in fbound;
  socklen_t fblen = (socklen_t)sizeof(fbound);
  if (getsockname(srv, (struct sockaddr *)&fbound, &fblen) != 0) {
    perror("getsockname");
    close(srv);
    return 1;
  }
  uint32_t fport = (uint32_t)ntohs(fbound.sin_port);

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    close(srv);
    return 1;
  }
  if (pid == 0) {
    int c = accept(srv, NULL, NULL);
    if (c < 0) _exit(2);
    char rb[1024];
    (void)recv(c, rb, sizeof(rb), 0);
    const char body2[] = "fetchok";
    char resp[256];
    int rn = snprintf(resp, sizeof(resp),
                      "HTTP/1.1 200 OK\r\nContent-Length: %u\r\nContent-Type: text/plain\r\n\r\n%s",
                      (unsigned)(sizeof(body2) - 1), body2);
    if (rn > 0) (void)send(c, resp, (size_t)rn, 0);
    close(c);
    close(srv);
    _exit(0);
  }

  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%u/x", fport);
  uint8_t fpl[512];
  off = 0;
  const char mget[] = "GET";
  write_u32le(fpl + off, (uint32_t)strlen(mget));
  off += 4;
  memcpy(fpl + off, mget, strlen(mget));
  off += (uint32_t)strlen(mget);
  write_u32le(fpl + off, (uint32_t)strlen(url));
  off += 4;
  memcpy(fpl + off, url, strlen(url));
  off += (uint32_t)strlen(url);
  write_u32le(fpl + off, 0); // header_count
  off += 4;
  write_u32le(fpl + off, 0); // body_kind none
  off += 4;

  fn = zi_zcl1_write_ok(rfr, (uint32_t)sizeof(rfr), 3, 123, fpl, off);
  if (fn <= 0) {
    fprintf(stderr, "failed to build FETCH frame\n");
    close(srv);
    return 1;
  }
  if (zi_write(h, (zi_ptr_t)(uintptr_t)rfr, (zi_size32_t)fn) != fn) {
    fprintf(stderr, "FETCH write failed\n");
    close(srv);
    return 1;
  }

  rn = read_full_frame(h, loop_h, buf, (uint32_t)sizeof(buf));
  if (rn < 0) {
    fprintf(stderr, "FETCH read failed: %d\n", rn);
    close(srv);
    return 1;
  }
  if (!zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 3) {
    fprintf(stderr, "unexpected FETCH response\n");
    close(srv);
    return 1;
  }
  uint32_t st = zi_zcl1_read_u32(z.payload + 0);
  if (st != 200) {
    fprintf(stderr, "unexpected fetch status: %u\n", st);
    close(srv);
    return 1;
  }
  uint32_t hoff = 8;
  uint32_t hcnt = zi_zcl1_read_u32(z.payload + 4);
  for (uint32_t i = 0; i < hcnt; i++) {
    uint32_t nl = zi_zcl1_read_u32(z.payload + hoff);
    hoff += 4 + nl;
    uint32_t vl = zi_zcl1_read_u32(z.payload + hoff);
    hoff += 4 + vl;
    if (hoff > z.payload_len) {
      fprintf(stderr, "bad fetch headers\n");
      close(srv);
      return 1;
    }
  }
  if (hoff + 4 > z.payload_len) {
    fprintf(stderr, "missing fetch body_kind\n");
    close(srv);
    return 1;
  }
  uint32_t bk = zi_zcl1_read_u32(z.payload + hoff);
  hoff += 4;
  if (bk != 1) {
    fprintf(stderr, "expected inline fetch body\n");
    close(srv);
    return 1;
  }
  if (hoff + 4 > z.payload_len) {
    fprintf(stderr, "missing fetch body_len\n");
    close(srv);
    return 1;
  }
  uint32_t bl = zi_zcl1_read_u32(z.payload + hoff);
  hoff += 4;
  if (hoff + bl != z.payload_len) {
    fprintf(stderr, "bad fetch body length\n");
    close(srv);
    return 1;
  }
  if (bl != 7 || memcmp(z.payload + hoff, "fetchok", 7) != 0) {
    fprintf(stderr, "bad fetch body\n");
    close(srv);
    return 1;
  }

  int wstatus = 0;
  (void)waitpid(pid, &wstatus, 0);
  close(srv);

  // FETCH: chunked response body should be exposed as a STREAM (decoded).
  {
    int srv3 = socket(AF_INET, SOCK_STREAM, 0);
    if (srv3 < 0) {
      perror("socket");
      return 1;
    }
    struct sockaddr_in faddr3;
    memset(&faddr3, 0, sizeof(faddr3));
    faddr3.sin_family = AF_INET;
    faddr3.sin_port = htons(0);
    faddr3.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv3, (struct sockaddr *)&faddr3, (socklen_t)sizeof(faddr3)) != 0) {
      perror("bind");
      close(srv3);
      return 1;
    }
    if (listen(srv3, 1) != 0) {
      perror("listen");
      close(srv3);
      return 1;
    }
    struct sockaddr_in fbound3;
    socklen_t fblen3 = (socklen_t)sizeof(fbound3);
    if (getsockname(srv3, (struct sockaddr *)&fbound3, &fblen3) != 0) {
      perror("getsockname");
      close(srv3);
      return 1;
    }
    uint32_t fport3 = (uint32_t)ntohs(fbound3.sin_port);

    pid_t pid3 = fork();
    if (pid3 < 0) {
      perror("fork");
      close(srv3);
      return 1;
    }
    if (pid3 == 0) {
      int c = accept(srv3, NULL, NULL);
      if (c < 0) _exit(2);
      char rb[1024];
      (void)recv(c, rb, sizeof(rb), 0);
      const char hdr[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n";
      const char body[] = "7\r\nchunked\r\n0\r\n\r\n";
      (void)send(c, hdr, (size_t)(sizeof(hdr) - 1), 0);
      (void)send(c, body, (size_t)(sizeof(body) - 1), 0);
      close(c);
      close(srv3);
      _exit(0);
    }

    char url3[128];
    snprintf(url3, sizeof(url3), "http://127.0.0.1:%u/x", fport3);
    uint8_t fpl3[512];
    off = 0;
    const char mget3[] = "GET";
    write_u32le(fpl3 + off, (uint32_t)strlen(mget3));
    off += 4;
    memcpy(fpl3 + off, mget3, strlen(mget3));
    off += (uint32_t)strlen(mget3);
    write_u32le(fpl3 + off, (uint32_t)strlen(url3));
    off += 4;
    memcpy(fpl3 + off, url3, strlen(url3));
    off += (uint32_t)strlen(url3);
    write_u32le(fpl3 + off, 0); // header_count
    off += 4;
    write_u32le(fpl3 + off, 0); // body_kind none
    off += 4;

    fn = zi_zcl1_write_ok(rfr, (uint32_t)sizeof(rfr), 3, 124, fpl3, off);
    if (fn <= 0) {
      fprintf(stderr, "failed to build FETCH(chunked) frame\n");
      close(srv3);
      return 1;
    }
    if (zi_write(h, (zi_ptr_t)(uintptr_t)rfr, (zi_size32_t)fn) != fn) {
      fprintf(stderr, "FETCH(chunked) write failed\n");
      close(srv3);
      return 1;
    }

    rn = read_full_frame(h, loop_h, buf, (uint32_t)sizeof(buf));
    if (rn < 0) {
      fprintf(stderr, "FETCH(chunked) read failed: %d\n", rn);
      close(srv3);
      return 1;
    }
    if (!zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 3) {
      fprintf(stderr, "unexpected FETCH(chunked) response\n");
      close(srv3);
      return 1;
    }
    uint32_t st3 = zi_zcl1_read_u32(z.payload + 0);
    if (st3 != 200) {
      fprintf(stderr, "unexpected fetch(chunked) status: %u\n", st3);
      close(srv3);
      return 1;
    }
    uint32_t hoff3 = 8;
    uint32_t hcnt3 = zi_zcl1_read_u32(z.payload + 4);
    for (uint32_t i = 0; i < hcnt3; i++) {
      uint32_t nl = zi_zcl1_read_u32(z.payload + hoff3);
      hoff3 += 4 + nl;
      uint32_t vl = zi_zcl1_read_u32(z.payload + hoff3);
      hoff3 += 4 + vl;
      if (hoff3 > z.payload_len) {
        fprintf(stderr, "bad fetch(chunked) headers\n");
        close(srv3);
        return 1;
      }
    }
    if (hoff3 + 4 > z.payload_len) {
      fprintf(stderr, "missing fetch(chunked) body_kind\n");
      close(srv3);
      return 1;
    }
    uint32_t bk3 = zi_zcl1_read_u32(z.payload + hoff3);
    hoff3 += 4;
    if (bk3 != 2) {
      fprintf(stderr, "expected stream fetch(chunked) body\n");
      close(srv3);
      return 1;
    }
    if (hoff3 + 4 > z.payload_len) {
      fprintf(stderr, "missing fetch(chunked) body_handle\n");
      close(srv3);
      return 1;
    }
    zi_handle_t fb = (zi_handle_t)(int32_t)zi_zcl1_read_u32(z.payload + hoff3);
    if (fb < 3) {
      fprintf(stderr, "bad fetch(chunked) body_handle\n");
      close(srv3);
      return 1;
    }

    // Watch body handle so sys_loop_poll_once can block.
    {
      uint8_t wpl[20];
      write_u32le(wpl + 0, (uint32_t)fb);
      write_u32le(wpl + 4, 0x1u); // readable
      write_u64le(wpl + 8, 100u);
      write_u32le(wpl + 16, 0u);
      uint8_t wfr[128];
      int wfn = zi_zcl1_write_ok(wfr, (uint32_t)sizeof(wfr), (uint16_t)ZI_SYS_LOOP_OP_WATCH, 100, wpl, (uint32_t)sizeof(wpl));
      if (wfn <= 0 || zi_write(loop_h, (zi_ptr_t)(uintptr_t)wfr, (zi_size32_t)wfn) != wfn) {
        fprintf(stderr, "WATCH(fetch body) write failed\n");
        close(srv3);
        return 1;
      }
      uint8_t wbuf[256];
      int wrn = read_full_frame(loop_h, 0, wbuf, (uint32_t)sizeof(wbuf));
      if (wrn < 0) {
        fprintf(stderr, "WATCH(fetch body) read failed: %d\n", wrn);
        close(srv3);
        return 1;
      }
      zi_zcl1_frame wz;
      uint32_t wst = zi_zcl1_read_u32(wbuf + 12);
      if (!zi_zcl1_parse(wbuf, (uint32_t)wrn, &wz) || wz.op != (uint16_t)ZI_SYS_LOOP_OP_WATCH || wst != 1) {
        fprintf(stderr, "unexpected WATCH(fetch body) response\n");
        close(srv3);
        return 1;
      }
    }

    char fbtxt[64];
    memset(fbtxt, 0, sizeof(fbtxt));
    uint32_t fbo = 0;
    for (;;) {
      int32_t nread = zi_read(fb, (zi_ptr_t)(uintptr_t)(fbtxt + fbo), (zi_size32_t)(sizeof(fbtxt) - 1 - fbo));
      if (nread == ZI_E_AGAIN) {
        int pr = sys_loop_poll_once(loop_h, 1000u);
        if (pr < 0) {
          fprintf(stderr, "POLL(fetch body) failed: %d\n", pr);
          close(srv3);
          return 1;
        }
        continue;
      }
      if (nread < 0) {
        fprintf(stderr, "fetch body read failed: %d\n", nread);
        close(srv3);
        return 1;
      }
      if (nread == 0) break;
      fbo += (uint32_t)nread;
      if (fbo >= sizeof(fbtxt) - 1) break;
    }
    if (strcmp(fbtxt, "chunked") != 0) {
      fprintf(stderr, "unexpected fetch(chunked) body: '%s'\n", fbtxt);
      close(srv3);
      return 1;
    }
    (void)zi_end(fb);

    int wstatus3 = 0;
    (void)waitpid(pid3, &wstatus3, 0);
    close(srv3);
  }

  // FETCH with streaming request body (body_kind=2).
  int srv2 = socket(AF_INET, SOCK_STREAM, 0);
  if (srv2 < 0) {
    perror("socket");
    return 1;
  }
  struct sockaddr_in faddr2;
  memset(&faddr2, 0, sizeof(faddr2));
  faddr2.sin_family = AF_INET;
  faddr2.sin_port = htons(0);
  faddr2.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(srv2, (struct sockaddr *)&faddr2, (socklen_t)sizeof(faddr2)) != 0) {
    perror("bind");
    close(srv2);
    return 1;
  }
  if (listen(srv2, 1) != 0) {
    perror("listen");
    close(srv2);
    return 1;
  }
  struct sockaddr_in fbound2;
  socklen_t fblen2 = (socklen_t)sizeof(fbound2);
  if (getsockname(srv2, (struct sockaddr *)&fbound2, &fblen2) != 0) {
    perror("getsockname");
    close(srv2);
    return 1;
  }
  uint32_t fport2 = (uint32_t)ntohs(fbound2.sin_port);

  pid_t pid2 = fork();
  if (pid2 < 0) {
    perror("fork");
    close(srv2);
    return 1;
  }
  if (pid2 == 0) {
    int c = accept(srv2, NULL, NULL);
    if (c < 0) _exit(2);
    uint8_t rb[8192];
    size_t got2 = 0;
    int ok2 = 0;
    for (int i = 0; i < 32 && got2 + 1 < sizeof(rb); i++) {
      ssize_t n2 = recv(c, rb + got2, sizeof(rb) - 1 - got2, 0);
      if (n2 <= 0) break;
      got2 += (size_t)n2;
      if (memmem(rb, got2, "streambody", 10) != NULL) {
        ok2 = 1;
        break;
      }
    }
    const char body3[] = "ok";
    const char *status = ok2 ? "200 OK" : "400 Bad Request";
    char resp3[256];
    int rn3 = snprintf(resp3, sizeof(resp3),
                       "HTTP/1.1 %s\r\nContent-Length: %u\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n%s",
                       status, (unsigned)(sizeof(body3) - 1), body3);
    if (rn3 > 0) (void)send(c, resp3, (size_t)rn3, 0);
    close(c);
    close(srv2);
    _exit(ok2 ? 0 : 4);
  }

  char url2[128];
  snprintf(url2, sizeof(url2), "http://127.0.0.1:%u/post", fport2);

  const uint8_t post_body[] = "streambody";
  test_ro_body_ctx *ctx = (test_ro_body_ctx *)calloc(1, sizeof(*ctx));
  if (!ctx) {
    fprintf(stderr, "calloc failed\n");
    close(srv2);
    return 1;
  }
  ctx->p = post_body;
  ctx->n = (uint32_t)(sizeof(post_body) - 1);

  zi_handle_ops_v1 hops;
  memset(&hops, 0, sizeof(hops));
  hops.read = test_ro_body_read;
  hops.end = test_ro_body_end;
  zi_handle_t post_body_h = zi_handle25_alloc(&hops, ctx, ZI_H_READABLE | ZI_H_ENDABLE);
  if (post_body_h < 3) {
    fprintf(stderr, "failed to alloc post body handle\n");
    free(ctx);
    close(srv2);
    return 1;
  }

  uint8_t fpl2[512];
  off = 0;
  const char mpost[] = "POST";
  write_u32le(fpl2 + off, (uint32_t)strlen(mpost));
  off += 4;
  memcpy(fpl2 + off, mpost, strlen(mpost));
  off += (uint32_t)strlen(mpost);
  write_u32le(fpl2 + off, (uint32_t)strlen(url2));
  off += 4;
  memcpy(fpl2 + off, url2, strlen(url2));
  off += (uint32_t)strlen(url2);
  write_u32le(fpl2 + off, 1); // header_count
  off += 4;

  const char hcl[] = "Content-Length";
  const char vcl[] = "10";
  write_u32le(fpl2 + off, (uint32_t)strlen(hcl));
  off += 4;
  memcpy(fpl2 + off, hcl, strlen(hcl));
  off += (uint32_t)strlen(hcl);
  write_u32le(fpl2 + off, (uint32_t)strlen(vcl));
  off += 4;
  memcpy(fpl2 + off, vcl, strlen(vcl));
  off += (uint32_t)strlen(vcl);

  write_u32le(fpl2 + off, 2); // body_kind stream
  off += 4;
  write_u32le(fpl2 + off, (uint32_t)(int32_t)post_body_h);
  off += 4;

  fn = zi_zcl1_write_ok(rfr, (uint32_t)sizeof(rfr), 3, 124, fpl2, off);
  if (fn <= 0) {
    fprintf(stderr, "failed to build FETCH(stream) frame\n");
    (void)zi_end(post_body_h);
    close(srv2);
    return 1;
  }
  if (zi_write(h, (zi_ptr_t)(uintptr_t)rfr, (zi_size32_t)fn) != fn) {
    fprintf(stderr, "FETCH(stream) write failed\n");
    (void)zi_end(post_body_h);
    close(srv2);
    return 1;
  }
  rn = read_full_frame(h, loop_h, buf, (uint32_t)sizeof(buf));
  if (rn < 0) {
    fprintf(stderr, "FETCH(stream) read failed: %d\n", rn);
    (void)zi_end(post_body_h);
    close(srv2);
    return 1;
  }
  if (!zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 3) {
    fprintf(stderr, "unexpected FETCH(stream) response\n");
    (void)zi_end(post_body_h);
    close(srv2);
    return 1;
  }
  uint32_t zst = zi_zcl1_read_u32(buf + 12);
  if (zst == 0) {
    if (z.payload_len >= 8) {
      uint32_t tlen = zi_zcl1_read_u32(z.payload + 0);
      if (4u + tlen + 4u <= z.payload_len) {
        const char *t = (const char *)(z.payload + 4);
        uint32_t mlen = zi_zcl1_read_u32(z.payload + 4u + tlen);
        if (4u + tlen + 4u + mlen <= z.payload_len) {
          const char *m = (const char *)(z.payload + 8u + tlen);
          fprintf(stderr, "FETCH(stream) error: %.*s: %.*s\n", (int)tlen, t, (int)mlen, m);
        }
      }
    }
    (void)zi_end(post_body_h);
    close(srv2);
    return 1;
  }
  st = zi_zcl1_read_u32(z.payload + 0);
  if (st != 200) {
    fprintf(stderr, "unexpected fetch(stream) status: %u\n", st);
    (void)zi_end(post_body_h);
    close(srv2);
    return 1;
  }

  (void)zi_end(post_body_h);
  int wstatus2 = 0;
  (void)waitpid(pid2, &wstatus2, 0);
  if (!(WIFEXITED(wstatus2) && WEXITSTATUS(wstatus2) == 0)) {
    fprintf(stderr, "server did not see streamed body\n");
    close(srv2);
    return 1;
  }
  close(srv2);

  // Second request uses RESPOND_STREAM.
  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    perror("socket");
    return 1;
  }
  if (connect(s, (struct sockaddr *)&sa, (socklen_t)sizeof(sa)) != 0) {
    perror("connect");
    close(s);
    return 1;
  }
  const char req2[] = "GET /stream HTTP/1.1\r\nHost: localhost\r\n\r\n";
  if (send(s, req2, sizeof(req2) - 1, 0) != (ssize_t)(sizeof(req2) - 1)) {
    perror("send");
    close(s);
    return 1;
  }

  rn = read_full_frame(h, loop_h, buf, (uint32_t)sizeof(buf));
  if (rn < 0) {
    fprintf(stderr, "EV_REQUEST read failed: %d\n", rn);
    close(s);
    return 1;
  }
  if (!zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 100) {
    fprintf(stderr, "unexpected event\n");
    close(s);
    return 1;
  }

  uint8_t spl[256];
  off = 0;
  write_u32le(spl + off, 200);
  off += 4;
  write_u32le(spl + off, 0);
  off += 4;
  write_u32le(spl + off, 1);
  off += 4;
  write_u32le(spl + off, (uint32_t)strlen(hn));
  off += 4;
  memcpy(spl + off, hn, strlen(hn));
  off += (uint32_t)strlen(hn);
  write_u32le(spl + off, (uint32_t)strlen(hv));
  off += 4;
  memcpy(spl + off, hv, strlen(hv));
  off += (uint32_t)strlen(hv);

  fn = zi_zcl1_write_ok(rfr, (uint32_t)sizeof(rfr), 12, z.rid, spl, off);
  if (fn <= 0) {
    fprintf(stderr, "failed to build RESPOND_STREAM frame\n");
    close(s);
    return 1;
  }
  if (zi_write(h, (zi_ptr_t)(uintptr_t)rfr, (zi_size32_t)fn) != fn) {
    fprintf(stderr, "RESPOND_STREAM write failed\n");
    close(s);
    return 1;
  }

  rn = read_full_frame(h, loop_h, buf, (uint32_t)sizeof(buf));
  if (rn < 0) {
    fprintf(stderr, "RESPOND_STREAM resp read failed: %d\n", rn);
    close(s);
    return 1;
  }
  if (!zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 12 || z.payload_len != 4) {
    fprintf(stderr, "unexpected RESPOND_STREAM response\n");
    close(s);
    return 1;
  }
  zi_handle_t body_h = (zi_handle_t)(int32_t)zi_zcl1_read_u32(z.payload);
  if (body_h < 3) {
    fprintf(stderr, "bad body handle\n");
    close(s);
    return 1;
  }
  if (write_all_handle(body_h, body, (uint32_t)strlen(body)) != 0) {
    fprintf(stderr, "write to body handle failed\n");
    close(s);
    return 1;
  }
  if (zi_end(body_h) != 0) {
    fprintf(stderr, "zi_end(body_h) failed\n");
    close(s);
    return 1;
  }

  memset(respbuf, 0, sizeof(respbuf));
  size_t roff = 0;
  while (roff + 1 < sizeof(respbuf)) {
    nrcv = recv(s, respbuf + roff, sizeof(respbuf) - 1 - roff, 0);
    if (nrcv < 0) {
      fprintf(stderr, "client recv failed\n");
      close(s);
      return 1;
    }
    if (nrcv == 0) break;
    roff += (size_t)nrcv;
  }
  respbuf[sizeof(respbuf) - 1] = 0;
  if (strstr(respbuf, "HTTP/1.1 200") == NULL || strstr(respbuf, "world") == NULL) {
    fprintf(stderr, "unexpected http response: %s\n", respbuf);
    close(s);
    return 1;
  }
  close(s);

  // Multipart request + MULTIPART_* iteration.
  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    perror("socket");
    return 1;
  }
  if (connect(s, (struct sockaddr *)&sa, (socklen_t)sizeof(sa)) != 0) {
    perror("connect");
    close(s);
    return 1;
  }
  const char bnd[] = "XBOUND";
  const char mpbody[] =
      "--XBOUND\r\n"
      "Content-Disposition: form-data; name=\"a\"\r\n"
      "\r\n"
      "hello\r\n"
      "--XBOUND\r\n"
      "Content-Disposition: form-data; name=\"b\"; filename=\"x.txt\"\r\n"
      "Content-Type: text/plain\r\n"
      "\r\n"
      "world\r\n"
      "--XBOUND--\r\n";
  char mp_req[2048];
  int mp_len = snprintf(mp_req, sizeof(mp_req),
                        "POST /mp HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "Content-Type: multipart/form-data; boundary=%s\r\n"
                        "Content-Length: %zu\r\n"
                        "\r\n"
                        "%s",
                        bnd, strlen(mpbody), mpbody);
  if (mp_len <= 0 || (size_t)mp_len >= sizeof(mp_req)) {
    fprintf(stderr, "failed to build multipart request\n");
    close(s);
    return 1;
  }

  if (send(s, mp_req, (size_t)mp_len, 0) != (ssize_t)mp_len) {
    perror("send");
    close(s);
    return 1;
  }

  rn = read_full_frame(h, loop_h, buf, (uint32_t)sizeof(buf));
  if (rn < 0) {
    fprintf(stderr, "EV_REQUEST(multipart) read failed: %d\n", rn);
    close(s);
    return 1;
  }
  if (!zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 100) {
    fprintf(stderr, "unexpected event for multipart\n");
    close(s);
    return 1;
  }

  // Parse body_kind + body handle from EV_REQUEST payload.
  uint32_t poff = 0;
  if (z.payload_len < 8) {
    fprintf(stderr, "bad EV_REQUEST payload\n");
    close(s);
    return 1;
  }
  poff += 8; // listener_id + flags
  for (int i = 0; i < 4; i++) {
    if (poff + 4u > z.payload_len) {
      fprintf(stderr, "bad EV_REQUEST payload (strings)\n");
      close(s);
      return 1;
    }
    uint32_t ln = zi_zcl1_read_u32(z.payload + poff);
    poff += 4;
    if (poff + ln > z.payload_len) {
      fprintf(stderr, "bad EV_REQUEST payload (string len)\n");
      close(s);
      return 1;
    }
    poff += ln;
  }
  if (poff + 16u + 4u + 4u > z.payload_len) {
    fprintf(stderr, "bad EV_REQUEST payload (peer)\n");
    close(s);
    return 1;
  }
  poff += 16; // remote addr
  poff += 4;  // remote port
  uint32_t hc = zi_zcl1_read_u32(z.payload + poff);
  poff += 4;
  for (uint32_t i = 0; i < hc; i++) {
    if (poff + 4u > z.payload_len) {
      fprintf(stderr, "bad EV_REQUEST payload (hdr name len)\n");
      close(s);
      return 1;
    }
    uint32_t nlen = zi_zcl1_read_u32(z.payload + poff);
    poff += 4;
    if (poff + nlen + 4u > z.payload_len) {
      fprintf(stderr, "bad EV_REQUEST payload (hdr name)\n");
      close(s);
      return 1;
    }

    poff += nlen;
    uint32_t vlen = zi_zcl1_read_u32(z.payload + poff);
    poff += 4;
    if (poff + vlen > z.payload_len) {
      fprintf(stderr, "bad EV_REQUEST payload (hdr val)\n");
      close(s);
      return 1;
    }

    poff += vlen;
  }
  if (poff + 4u > z.payload_len) {
    fprintf(stderr, "bad EV_REQUEST payload (body_kind)\n");
    close(s);
    return 1;
  }
  uint32_t body_kind = zi_zcl1_read_u32(z.payload + poff);
  poff += 4;
  if (body_kind != 3) {
    fprintf(stderr, "expected multipart body_kind=3, got %u\n", body_kind);
    close(s);
    return 1;
  }
  if (poff + 4u > z.payload_len) {
    fprintf(stderr, "bad EV_REQUEST payload (body_handle)\n");
    close(s);
    return 1;
  }
  (void)zi_zcl1_read_u32(z.payload + poff); // raw body handle (allowed but unused in this test)

  // MULTIPART_BEGIN
  fn = zi_zcl1_write_ok(rfr, (uint32_t)sizeof(rfr), 20, z.rid, NULL, 0);
  if (fn <= 0 || zi_write(h, (zi_ptr_t)(uintptr_t)rfr, (zi_size32_t)fn) != fn) {
    fprintf(stderr, "MULTIPART_BEGIN write failed\n");
    close(s);
    return 1;
  }
  rn = read_full_frame(h, loop_h, buf, (uint32_t)sizeof(buf));
  if (rn < 0 || !zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 20) {
    fprintf(stderr, "MULTIPART_BEGIN read failed\n");
    close(s);
    return 1;
  }
  if (zi_zcl1_read_u32(buf + 12) == 0) {
    fprintf(stderr, "MULTIPART_BEGIN returned error\n");
    close(s);
    return 1;
  }

  int parts_seen = 0;
  for (;;) {
    fn = zi_zcl1_write_ok(rfr, (uint32_t)sizeof(rfr), 21, z.rid, NULL, 0);
    if (fn <= 0 || zi_write(h, (zi_ptr_t)(uintptr_t)rfr, (zi_size32_t)fn) != fn) {
      fprintf(stderr, "MULTIPART_NEXT write failed\n");
      close(s);
      return 1;
    }
    rn = read_full_frame(h, loop_h, buf, (uint32_t)sizeof(buf));
    if (rn < 0 || !zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 21) {
      fprintf(stderr, "MULTIPART_NEXT read failed\n");
      close(s);
      return 1;
    }
    if (zi_zcl1_read_u32(buf + 12) == 0) {
      fprintf(stderr, "MULTIPART_NEXT returned error\n");
      close(s);
      return 1;
    }

    uint32_t o = 0;
    if (z.payload_len < 4) {
      fprintf(stderr, "bad MULTIPART_NEXT payload\n");
      close(s);
      return 1;
    }
    uint32_t done = zi_zcl1_read_u32(z.payload + o);
    o += 4;
    if (done) break;

    if (o + 4u > z.payload_len) return 1;
    uint32_t nlen = zi_zcl1_read_u32(z.payload + o);
    o += 4;
    const char *pname = (const char *)(z.payload + o);
    o += nlen;

    if (o + 4u > z.payload_len) return 1;
    uint32_t flen = zi_zcl1_read_u32(z.payload + o);
    o += 4;
    const char *pfile = (const char *)(z.payload + o);
    o += flen;

    if (o + 4u > z.payload_len) return 1;
    uint32_t clen = zi_zcl1_read_u32(z.payload + o);
    o += 4;
    const char *pctype = (const char *)(z.payload + o);
    o += clen;

    if (o + 4u > z.payload_len) return 1;
    uint32_t phc = zi_zcl1_read_u32(z.payload + o);
    o += 4;
    for (uint32_t i = 0; i < phc; i++) {
      if (o + 4u > z.payload_len) return 1;
      uint32_t hnlen = zi_zcl1_read_u32(z.payload + o);
      o += 4 + hnlen;
      if (o + 4u > z.payload_len) return 1;
      uint32_t hvlen = zi_zcl1_read_u32(z.payload + o);
      o += 4 + hvlen;
      if (o > z.payload_len) return 1;
    }

    if (o + 4u > z.payload_len) return 1;
    zi_handle_t part_h = (zi_handle_t)(int32_t)zi_zcl1_read_u32(z.payload + o);
    if (part_h < 3) {
      fprintf(stderr, "bad part handle\n");
      close(s);
      return 1;
    }

    char gotpart[64];
    uint32_t gp = 0;
    for (;;) {
      int32_t nr = zi_read(part_h, (zi_ptr_t)(uintptr_t)(gotpart + gp), (zi_size32_t)(sizeof(gotpart) - 1 - gp));
      if (nr < 0) {
        fprintf(stderr, "part read failed\n");
        close(s);
        return 1;
      }
      if (nr == 0) break;
      gp += (uint32_t)nr;
      if (gp + 1 >= sizeof(gotpart)) break;
    }
    gotpart[gp] = 0;

    if (parts_seen == 0) {
      if (!(nlen == 1 && memcmp(pname, "a", 1) == 0)) {
        fprintf(stderr, "unexpected first part name\n");
        close(s);
        return 1;
      }
      if (flen != 0) {
        fprintf(stderr, "unexpected first part filename\n");
        close(s);
        return 1;
      }
      if (strcmp(gotpart, "hello") != 0) {
        fprintf(stderr, "unexpected first part body: %s\n", gotpart);
        close(s);
        return 1;
      }
    } else if (parts_seen == 1) {
      if (!(nlen == 1 && memcmp(pname, "b", 1) == 0)) {
        fprintf(stderr, "unexpected second part name\n");
        close(s);
        return 1;
      }
      if (!(flen == 5 && memcmp(pfile, "x.txt", 5) == 0)) {
        fprintf(stderr, "unexpected second part filename\n");
        close(s);
        return 1;
      }
      if (!(clen == 10 && memcmp(pctype, "text/plain", 10) == 0)) {
        fprintf(stderr, "unexpected second part content-type\n");
        close(s);
        return 1;
      }
      if (strcmp(gotpart, "world") != 0) {
        fprintf(stderr, "unexpected second part body: %s\n", gotpart);
        close(s);
        return 1;
      }
    }

    parts_seen++;
    if (zi_end(part_h) != 0) {
      fprintf(stderr, "part handle end failed\n");
      close(s);
      return 1;
    }
  }

  // MULTIPART_END
  fn = zi_zcl1_write_ok(rfr, (uint32_t)sizeof(rfr), 22, z.rid, NULL, 0);
  if (fn <= 0 || zi_write(h, (zi_ptr_t)(uintptr_t)rfr, (zi_size32_t)fn) != fn) {
    fprintf(stderr, "MULTIPART_END write failed\n");
    close(s);
    return 1;
  }
  rn = read_full_frame(h, loop_h, buf, (uint32_t)sizeof(buf));
  if (rn < 0 || !zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 22) {
    fprintf(stderr, "MULTIPART_END read failed\n");
    close(s);
    return 1;
  }
  if (zi_zcl1_read_u32(buf + 12) == 0) {
    fprintf(stderr, "MULTIPART_END returned error\n");
    close(s);
    return 1;
  }
  if (parts_seen != 2) {
    fprintf(stderr, "expected 2 parts, saw %d\n", parts_seen);
    close(s);
    return 1;
  }

  // RESPOND_INLINE to finish request.
  uint8_t mppl[256];
  off = 0;
  write_u32le(mppl + off, 200);
  off += 4;
  write_u32le(mppl + off, 0);
  off += 4;
  write_u32le(mppl + off, 1);
  off += 4;
  write_u32le(mppl + off, (uint32_t)strlen(hn));
  off += 4;
  memcpy(mppl + off, hn, strlen(hn));
  off += (uint32_t)strlen(hn);
  write_u32le(mppl + off, (uint32_t)strlen(hv));
  off += 4;
  memcpy(mppl + off, hv, strlen(hv));
  off += (uint32_t)strlen(hv);
  const char okb[] = "ok";
  write_u32le(mppl + off, (uint32_t)sizeof(okb) - 1);
  off += 4;
  memcpy(mppl + off, okb, sizeof(okb) - 1);
  off += (uint32_t)sizeof(okb) - 1;
  fn = zi_zcl1_write_ok(rfr, (uint32_t)sizeof(rfr), 11, z.rid, mppl, off);
  if (fn <= 0 || zi_write(h, (zi_ptr_t)(uintptr_t)rfr, (zi_size32_t)fn) != fn) {
    fprintf(stderr, "RESPOND_INLINE(multipart) write failed\n");
    close(s);
    return 1;
  }
  rn = read_full_frame(h, loop_h, buf, (uint32_t)sizeof(buf));
  if (rn < 0 || !zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 11) {
    fprintf(stderr, "RESPOND_INLINE(multipart) response failed\n");
    close(s);
    return 1;
  }

  memset(respbuf, 0, sizeof(respbuf));
  roff = 0;
  while (roff + 1 < sizeof(respbuf)) {
    nrcv = recv(s, respbuf + roff, sizeof(respbuf) - 1 - roff, 0);
    if (nrcv < 0) {
      fprintf(stderr, "client recv failed\n");
      close(s);
      return 1;
    }
    if (nrcv == 0) break;
    roff += (size_t)nrcv;
  }
  respbuf[sizeof(respbuf) - 1] = 0;
  if (strstr(respbuf, "HTTP/1.1 200") == NULL || strstr(respbuf, "ok") == NULL) {
    fprintf(stderr, "unexpected multipart http response: %s\n", respbuf);
    close(s);
    return 1;
  }
  close(s);

  // CLOSE_LISTENER
  uint8_t cpl[4];
  write_u32le(cpl + 0, listener_id);
  fn = zi_zcl1_write_ok(fr, (uint32_t)sizeof(fr), 2, 7, cpl, 4);
  if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)fn) != fn) {
    fprintf(stderr, "CLOSE_LISTENER write failed\n");
    return 1;
  }
  rn = read_full_frame(h, loop_h, buf, (uint32_t)sizeof(buf));
  if (rn < 0) {
    fprintf(stderr, "CLOSE_LISTENER read failed\n");
    return 1;
  }
  if (!zi_zcl1_parse(buf, (uint32_t)rn, &z) || z.op != 2) {
    fprintf(stderr, "unexpected CLOSE_LISTENER response\n");
    return 1;
  }

  if (zi_end(h) != 0) {
    fprintf(stderr, "zi_end failed\n");
    return 1;
  }

  printf("ok\n");
  return 0;
}
