#include "zi_caps.h"
#include "zi_handles25.h"
#include "zi_net_tcp25.h"
#include "zi_runtime25.h"
#include "zi_sysabi25.h"

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

  int conn = accept(srv, NULL, NULL);
  if (conn < 0) {
    perror("accept");
    (void)zi_end(h);
    close(srv);
    return 1;
  }

  const char ping[] = "ping";
  int32_t wn = zi_write(h, (zi_ptr_t)(uintptr_t)ping, (zi_size32_t)sizeof(ping) - 1);
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
