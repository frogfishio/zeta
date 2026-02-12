#include "zi_net_tcp25.h"

#include "zi_handles25.h"
#include "zi_runtime25.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#if !defined(_WIN32)
#include <netinet/tcp.h>
#endif

typedef struct {
  int fd;
  int connecting;
  int write_shutdown;
} zi_tcp_stream;

typedef struct {
  int fd;
  uint32_t open_flags;
} zi_tcp_listener;

static void set_nonblocking_best_effort(int fd) {
  if (fd < 0) return;
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return;
  (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void set_cloexec_best_effort(int fd) {
  if (fd < 0) return;
  int flags = fcntl(fd, F_GETFD, 0);
  if (flags < 0) return;
  (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int tcp_get_fd(void *ctx, int *out_fd) {
  zi_tcp_stream *s = (zi_tcp_stream *)ctx;
  if (!s) return 0;
  if (s->fd < 0) return 0;
  if (out_fd) *out_fd = s->fd;
  return 1;
}

static int listener_get_fd(void *ctx, int *out_fd) {
  zi_tcp_listener *l = (zi_tcp_listener *)ctx;
  if (!l) return 0;
  if (l->fd < 0) return 0;
  if (out_fd) *out_fd = l->fd;
  return 1;
}

static int32_t map_errno_to_zi(int e);

static int32_t tcp_ensure_connected(zi_tcp_stream *s) {
  if (!s) return ZI_E_INTERNAL;
  if (!s->connecting) return 0;
  if (s->fd < 0) return ZI_E_CLOSED;

  int so_err = 0;
  socklen_t len = (socklen_t)sizeof(so_err);
  if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &so_err, &len) != 0) {
    return map_errno_to_zi(errno);
  }
  if (so_err == 0) {
    // Some platforms may report SO_ERROR=0 before the connection is fully established.
    // Confirm connectivity via getpeername.
    struct sockaddr_storage ss;
    socklen_t slen = (socklen_t)sizeof(ss);
    if (getpeername(s->fd, (struct sockaddr *)&ss, &slen) == 0) {
      s->connecting = 0;
      return 0;
    }
    if (errno == ENOTCONN) return ZI_E_AGAIN;
    return map_errno_to_zi(errno);
  }

  // Still in progress.
  if (so_err == EINPROGRESS
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EINPROGRESS)
      || so_err == EWOULDBLOCK
#endif
  ) {
    return ZI_E_AGAIN;
  }

  return map_errno_to_zi(so_err);
}

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
    case ENOENT:
      return ZI_E_NOENT;
    case ENOMEM:
      return ZI_E_OOM;
    case EINVAL:
      return ZI_E_INVALID;
    case EADDRINUSE:
      return ZI_E_AGAIN;
    case EADDRNOTAVAIL:
      return ZI_E_INVALID;
    default:
      return ZI_E_IO;
  }
}

static void addr_to_ipv6_mapped(const struct sockaddr *sa, uint8_t out16[16], uint32_t *out_port) {
  if (out16) memset(out16, 0, 16);
  if (out_port) *out_port = 0;
  if (!sa) return;

  if (sa->sa_family == AF_INET) {
    const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
    if (out_port) *out_port = (uint32_t)ntohs(in->sin_port);
    if (out16) {
      // ::ffff:a.b.c.d
      out16[10] = 0xFF;
      out16[11] = 0xFF;
      memcpy(out16 + 12, &in->sin_addr, 4);
    }
    return;
  }

  if (sa->sa_family == AF_INET6) {
    const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
    if (out_port) *out_port = (uint32_t)ntohs(in6->sin6_port);
    if (out16) memcpy(out16, &in6->sin6_addr, 16);
    return;
  }
}

static void apply_stream_opts_best_effort(int fd, uint32_t open_flags) {
  if (fd < 0) return;

#if defined(__APPLE__) && defined(SO_NOSIGPIPE)
  {
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, (socklen_t)sizeof(one));
  }
#endif

  if (open_flags & ZI_TCP_OPEN_NODELAY) {
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, (socklen_t)sizeof(one));
  }
  if (open_flags & ZI_TCP_OPEN_KEEPALIVE) {
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, (socklen_t)sizeof(one));
  }
}

static int32_t tcp_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  zi_tcp_stream *s = (zi_tcp_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (cap == 0) return 0;

  int32_t cr = tcp_ensure_connected(s);
  if (cr != 0) return cr;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_rw) return ZI_E_NOSYS;
  if (dst_ptr == 0) return ZI_E_BOUNDS;

  uint8_t *dst = NULL;
  if (!mem->map_rw(mem->ctx, dst_ptr, cap, &dst) || !dst) return ZI_E_BOUNDS;

  ssize_t n = recv(s->fd, dst, (size_t)cap, 0);
  if (n < 0) return map_errno_to_zi(errno);
  return (int32_t)n;
}

static int32_t tcp_write(void *ctx, zi_ptr_t src_ptr, zi_size32_t len) {
  zi_tcp_stream *s = (zi_tcp_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (len == 0) return 0;

  if (s->write_shutdown) return ZI_E_CLOSED;

  int32_t cr = tcp_ensure_connected(s);
  if (cr != 0) return cr;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_ro) return ZI_E_NOSYS;
  if (src_ptr == 0) return ZI_E_BOUNDS;

  const uint8_t *src = NULL;
  if (!mem->map_ro(mem->ctx, src_ptr, len, &src) || !src) return ZI_E_BOUNDS;

  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
  ssize_t n = send(s->fd, src, (size_t)len, flags);
  if (n < 0) return map_errno_to_zi(errno);
  return (int32_t)n;
}

static int32_t tcp_ctl(void *ctx, uint32_t op, zi_ptr_t arg_ptr, zi_size32_t arg_len) {
  (void)arg_ptr;
  (void)arg_len;
  zi_tcp_stream *s = (zi_tcp_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (s->fd < 0) return ZI_E_CLOSED;

  if (op == (uint32_t)ZI_HANDLE_OP_SHUT_WR) {
    if (s->write_shutdown) return 0;
    if (shutdown(s->fd, SHUT_WR) != 0) return map_errno_to_zi(errno);
    s->write_shutdown = 1;
    return 0;
  }

  return ZI_E_NOSYS;
}

static int32_t tcp_end(void *ctx) {
  zi_tcp_stream *s = (zi_tcp_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (s->fd >= 0) {
    (void)close(s->fd);
    s->fd = -1;
  }
  free(s);
  return 0;
}

static const zi_handle_ops_v1 tcp_ops = {
    .read = tcp_read,
    .write = tcp_write,
    .end = tcp_end,
  .ctl = tcp_ctl,
};

static const zi_handle_poll_ops_v1 tcp_poll_ops = {
  .get_fd = tcp_get_fd,
};

static int32_t listener_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  zi_tcp_listener *l = (zi_tcp_listener *)ctx;
  if (!l) return ZI_E_INTERNAL;
  if (l->fd < 0) return ZI_E_CLOSED;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_rw) return ZI_E_NOSYS;

  // Accept record(s). Each record is 32 bytes.
  if (cap < 32u) return ZI_E_BOUNDS;
  if (dst_ptr == 0) return ZI_E_BOUNDS;

  uint8_t *dst = NULL;
  if (!mem->map_rw(mem->ctx, dst_ptr, cap, &dst) || !dst) return ZI_E_BOUNDS;

  const uint32_t max_recs = (uint32_t)(cap / 32u);
  uint32_t wrote = 0;

  for (uint32_t i = 0; i < max_recs; i++) {
    struct sockaddr_storage ss;
    socklen_t slen = (socklen_t)sizeof(ss);

    int cfd = -1;
#if defined(__linux__) && defined(SOCK_CLOEXEC)
    cfd = accept4(l->fd, (struct sockaddr *)&ss, &slen, SOCK_CLOEXEC);
#else
    cfd = accept(l->fd, (struct sockaddr *)&ss, &slen);
#endif
    if (cfd < 0) {
      int32_t zr = map_errno_to_zi(errno);
      if (zr == ZI_E_AGAIN) break;
      if (wrote != 0) break;
      return zr;
    }

    set_nonblocking_best_effort(cfd);
    set_cloexec_best_effort(cfd);
    apply_stream_opts_best_effort(cfd, l->open_flags);

    zi_tcp_stream *s = (zi_tcp_stream *)calloc(1, sizeof(*s));
    if (!s) {
      (void)close(cfd);
      if (wrote != 0) break;
      return ZI_E_OOM;
    }
    s->fd = cfd;
    s->connecting = 0;

    zi_handle_t h = zi_handle25_alloc_with_poll(&tcp_ops, &tcp_poll_ops, s, ZI_H_READABLE | ZI_H_WRITABLE | ZI_H_ENDABLE);
    if (h == 0) {
      (void)close(cfd);
      free(s);
      if (wrote != 0) break;
      return ZI_E_OOM;
    }

    uint8_t peer_addr[16];
    uint32_t peer_port = 0;
    addr_to_ipv6_mapped((const struct sockaddr *)&ss, peer_addr, &peer_port);

    struct sockaddr_storage lss;
    socklen_t lslen = (socklen_t)sizeof(lss);
    uint32_t local_port = 0;
    if (getsockname(cfd, (struct sockaddr *)&lss, &lslen) == 0) {
      addr_to_ipv6_mapped((const struct sockaddr *)&lss, NULL, &local_port);
    }

    uint8_t *rec = dst + wrote;
    // u32 conn_handle
    rec[0] = (uint8_t)((uint32_t)h & 0xFF);
    rec[1] = (uint8_t)(((uint32_t)h >> 8) & 0xFF);
    rec[2] = (uint8_t)(((uint32_t)h >> 16) & 0xFF);
    rec[3] = (uint8_t)(((uint32_t)h >> 24) & 0xFF);
    // u32 peer_port
    rec[4] = (uint8_t)(peer_port & 0xFF);
    rec[5] = (uint8_t)((peer_port >> 8) & 0xFF);
    rec[6] = (uint8_t)((peer_port >> 16) & 0xFF);
    rec[7] = (uint8_t)((peer_port >> 24) & 0xFF);
    // peer_addr[16]
    memcpy(rec + 8, peer_addr, 16);
    // u32 local_port
    rec[24] = (uint8_t)(local_port & 0xFF);
    rec[25] = (uint8_t)((local_port >> 8) & 0xFF);
    rec[26] = (uint8_t)((local_port >> 16) & 0xFF);
    rec[27] = (uint8_t)((local_port >> 24) & 0xFF);
    // reserved
    rec[28] = 0;
    rec[29] = 0;
    rec[30] = 0;
    rec[31] = 0;

    wrote += 32u;
  }

  if (wrote == 0) return ZI_E_AGAIN;
  return (int32_t)wrote;
}

static int32_t listener_write(void *ctx, zi_ptr_t src_ptr, zi_size32_t len) {
  (void)ctx;
  (void)src_ptr;
  (void)len;
  return ZI_E_NOSYS;
}

static int32_t listener_end(void *ctx) {
  zi_tcp_listener *l = (zi_tcp_listener *)ctx;
  if (!l) return ZI_E_INTERNAL;
  if (l->fd >= 0) {
    (void)close(l->fd);
    l->fd = -1;
  }
  free(l);
  return 0;
}

static const zi_handle_ops_v1 listener_ops = {
    .read = listener_read,
    .write = listener_write,
    .end = listener_end,
};

static const zi_handle_poll_ops_v1 listener_poll_ops = {
  .get_fd = listener_get_fd,
};

static uint32_t u32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t u64le(const uint8_t *p) {
  uint64_t lo = u32le(p);
  uint64_t hi = u32le(p + 4);
  return lo | (hi << 32);
}

static int has_embedded_nul(const uint8_t *p, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    if (p[i] == 0) return 1;
  }
  return 0;
}

static int streq_nocase(const char *a, const char *b) {
  if (a == b) return 1;
  if (!a || !b) return 0;
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static void host_strip_brackets(const char *in, char *out, size_t out_cap) {
  if (!out || out_cap == 0) return;
  out[0] = '\0';
  if (!in) return;

  size_t n = strlen(in);
  if (n >= 2 && in[0] == '[' && in[n - 1] == ']') {
    size_t inner = n - 2;
    if (inner + 1 > out_cap) inner = out_cap - 1;
    memcpy(out, in + 1, inner);
    out[inner] = '\0';
    return;
  }

  strncpy(out, in, out_cap - 1);
  out[out_cap - 1] = '\0';
}

static int is_loopback_host(const char *host) {
  if (!host || host[0] == '\0') return 0;
  char h[256];
  host_strip_brackets(host, h, sizeof(h));
  if (streq_nocase(h, "localhost")) return 1;
  if (strcmp(h, "127.0.0.1") == 0) return 1;
  if (strcmp(h, "::1") == 0) return 1;
  return 0;
}

static const char *skip_ws(const char *p) {
  while (p && *p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  return p;
}

static int allowlist_allows(const char *allow, const char *host, uint32_t port) {
  if (!host || host[0] == '\0') return 0;

  if (!allow || allow[0] == '\0') {
    return is_loopback_host(host);
  }

  if (streq_nocase(allow, "any")) return 1;

  const char *p = allow;
  // Special case: port=0 is used for ephemeral bind in listener mode.
  // Treat it as "any port" for allowlist matching.
  const int want_any_port = (port == 0);

  while (*p) {
    p = skip_ws(p);
    if (!*p) break;

    // Extract token up to comma.
    const char *start = p;
    while (*p && *p != ',') p++;
    const char *end = p;
    if (*p == ',') p++;

    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) end--;
    size_t tok_len = (size_t)(end - start);
    if (tok_len == 0) continue;

    char tok[512];
    if (tok_len >= sizeof(tok)) tok_len = sizeof(tok) - 1;
    memcpy(tok, start, tok_len);
    tok[tok_len] = '\0';

    if (streq_nocase(tok, "loopback")) {
      if (is_loopback_host(host)) return 1;
      continue;
    }

    // token forms: host:* or host:port
    char *colon = strrchr(tok, ':');
    if (!colon) continue;
    *colon = '\0';
    const char *entry_host = tok;
    const char *entry_port = colon + 1;

    // host match (supports "*")
    int host_ok = 0;
    if (strcmp(entry_host, "*") == 0) host_ok = 1;
    else {
      char hn[256];
      char en[256];
      host_strip_brackets(host, hn, sizeof(hn));
      host_strip_brackets(entry_host, en, sizeof(en));
      host_ok = streq_nocase(hn, en);
    }
    if (!host_ok) continue;

    if (strcmp(entry_port, "*") == 0) return 1;

    if (want_any_port) continue;

    char *ep = NULL;
    long v = strtol(entry_port, &ep, 10);
    if (!ep || *ep != '\0') continue;
    if (v <= 0 || v > 65535) continue;
    if ((uint32_t)v == port) return 1;
  }

  return 0;
}

static int gai_to_zi(int e) {
  switch (e) {
    case EAI_MEMORY:
      return ZI_E_OOM;
#if defined(EAI_NODATA)
    case EAI_NODATA:
#endif
    case EAI_NONAME:
      return ZI_E_NOENT;
    default:
      return ZI_E_IO;
  }
}

static const zi_cap_v1 CAP = {
    .kind = ZI_CAP_KIND_NET,
    .name = ZI_CAP_NAME_TCP,
    .version = 1,
    .cap_flags = ZI_CAP_CAN_OPEN | ZI_CAP_MAY_BLOCK,
    .meta = NULL,
    .meta_len = 0,
};

const zi_cap_v1 *zi_net_tcp25_cap(void) { return &CAP; }

int zi_net_tcp25_register(void) { return zi_cap_register(&CAP); }

zi_handle_t zi_net_tcp25_open_from_params(zi_ptr_t params_ptr, zi_size32_t params_len) {
  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_ro) return (zi_handle_t)ZI_E_NOSYS;

  // params: u64 host_ptr, u32 host_len, u32 port, u32 flags,
  //         [u32 backlog], [u64 out_port_ptr]
  if (params_len < 20u) return (zi_handle_t)ZI_E_INVALID;

  const uint8_t *p = NULL;
  if (!mem->map_ro(mem->ctx, params_ptr, params_len, &p) || !p) return (zi_handle_t)ZI_E_BOUNDS;

  zi_ptr_t host_ptr = (zi_ptr_t)u64le(p + 0);
  uint32_t host_len = u32le(p + 8);
  uint32_t port = u32le(p + 12);
  uint32_t flags = u32le(p + 16);

  const uint32_t known_flags = ZI_TCP_OPEN_LISTEN | ZI_TCP_OPEN_REUSEADDR | ZI_TCP_OPEN_REUSEPORT | ZI_TCP_OPEN_IPV6ONLY |
                               ZI_TCP_OPEN_NODELAY | ZI_TCP_OPEN_KEEPALIVE;
  if ((flags & ~known_flags) != 0) return (zi_handle_t)ZI_E_INVALID;

  if (host_len == 0 || host_len > 255u) return (zi_handle_t)ZI_E_INVALID;
  if (port > 65535u) return (zi_handle_t)ZI_E_INVALID;

  const uint8_t *host_bytes = NULL;
  if (!mem->map_ro(mem->ctx, host_ptr, (zi_size32_t)host_len, &host_bytes) || !host_bytes) {
    return (zi_handle_t)ZI_E_BOUNDS;
  }
  if (has_embedded_nul(host_bytes, host_len)) return (zi_handle_t)ZI_E_INVALID;

  char host[256];
  memcpy(host, host_bytes, host_len);
  host[host_len] = '\0';

  // Normalize bracketed IPv6 literal form ("[::1]") into "::1".
  // This matches allowlist semantics and avoids getaddrinfo() failures.
  char host_norm[256];
  host_strip_brackets(host, host_norm, sizeof(host_norm));

  const int want_listen = (flags & ZI_TCP_OPEN_LISTEN) != 0;
  if (want_listen) {
    const char *allow = getenv("ZI_NET_LISTEN_ALLOW");
    if (!allowlist_allows(allow, host_norm, port)) {
      return (zi_handle_t)ZI_E_DENIED;
    }
  } else {
    if (port == 0) return (zi_handle_t)ZI_E_INVALID;
    const char *allow = getenv("ZI_NET_ALLOW");
    if (!allowlist_allows(allow, host_norm, port)) {
      return (zi_handle_t)ZI_E_DENIED;
    }
  }

  // Optional: out_port_ptr (write bound port for listeners, or 0 for none).
  // If params_len >= 24: backlog is present at +20.
  // If params_len >= 32: out_port_ptr is present at +24.
  zi_ptr_t out_port_ptr = 0;
  if (params_len >= 32u) out_port_ptr = (zi_ptr_t)u64le(p + 24);

  char service[16];
  snprintf(service, sizeof(service), "%u", port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  hints.ai_flags = AI_NUMERICSERV;

  const char *gai_host = host_norm;
  if (want_listen) {
    // "*" means wildcard bind.
    if (strcmp(host, "*") == 0) {
      gai_host = NULL;
      hints.ai_flags |= AI_PASSIVE;
    }
  }

  struct addrinfo *ai = NULL;
  int ga = getaddrinfo(gai_host, service, &hints, &ai);
  if (ga != 0 || !ai) {
    if (ai) freeaddrinfo(ai);
    return (zi_handle_t)gai_to_zi(ga);
  }

  int fd = -1;
  int last_zi = ZI_E_IO;

  for (struct addrinfo *cur = ai; cur; cur = cur->ai_next) {
    fd = socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
    if (fd < 0) {
      last_zi = map_errno_to_zi(errno);
      continue;
    }

    set_nonblocking_best_effort(fd);
    set_cloexec_best_effort(fd);

    if (want_listen) {
      int one = 1;
      // nginx-style default: allow quick restarts.
      (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, (socklen_t)sizeof(one));
#if defined(SO_REUSEPORT)
      if (flags & ZI_TCP_OPEN_REUSEPORT) {
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, (socklen_t)sizeof(one));
      }
#endif

      if (cur->ai_family == AF_INET6 && (flags & ZI_TCP_OPEN_IPV6ONLY)) {
#if defined(IPV6_V6ONLY)
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, (socklen_t)sizeof(one));
#endif
      }

      if (bind(fd, cur->ai_addr, cur->ai_addrlen) != 0) {
        last_zi = map_errno_to_zi(errno);
        (void)close(fd);
        fd = -1;
        continue;
      }

      uint32_t backlog = 128u;
      if (params_len >= 24u) {
        backlog = u32le(p + 20);
        if (backlog == 0) backlog = 128u;
        if (backlog > 65535u) backlog = 65535u;
      }

      if (listen(fd, (int)backlog) != 0) {
        last_zi = map_errno_to_zi(errno);
        (void)close(fd);
        fd = -1;
        continue;
      }

      if (out_port_ptr != 0) {
        // Write the actual bound port (useful for port=0 ephemeral binds).
        if (!mem->map_rw) {
          last_zi = ZI_E_NOSYS;
          (void)close(fd);
          fd = -1;
          continue;
        }
        struct sockaddr_storage bss;
        socklen_t bslen = (socklen_t)sizeof(bss);
        uint32_t bound_port = 0;
        if (getsockname(fd, (struct sockaddr *)&bss, &bslen) == 0) {
          addr_to_ipv6_mapped((const struct sockaddr *)&bss, NULL, &bound_port);
        }
        uint8_t *bp = NULL;
        if (!mem->map_rw(mem->ctx, out_port_ptr, 4u, &bp) || !bp) {
          last_zi = ZI_E_BOUNDS;
          (void)close(fd);
          fd = -1;
          continue;
        }
        bp[0] = (uint8_t)(bound_port & 0xFF);
        bp[1] = (uint8_t)((bound_port >> 8) & 0xFF);
        bp[2] = (uint8_t)((bound_port >> 16) & 0xFF);
        bp[3] = (uint8_t)((bound_port >> 24) & 0xFF);
      }

      last_zi = 0;
      break;
    }

    // Connect mode.
    apply_stream_opts_best_effort(fd, flags);

    for (;;) {
      if (connect(fd, cur->ai_addr, cur->ai_addrlen) == 0) {
        last_zi = 0;
        break;
      }

      if (errno == EINTR) continue;
      if (errno == EINPROGRESS
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EINPROGRESS)
          || errno == EWOULDBLOCK
#endif
      ) {
        last_zi = ZI_E_AGAIN;
        break;
      }

      last_zi = map_errno_to_zi(errno);
      (void)close(fd);
      fd = -1;
      break;
    }

    if (fd >= 0) break;
  }

  freeaddrinfo(ai);

  if (fd < 0) return (zi_handle_t)last_zi;

  if (want_listen) {
    zi_tcp_listener *l = (zi_tcp_listener *)calloc(1, sizeof(*l));
    if (!l) {
      (void)close(fd);
      return (zi_handle_t)ZI_E_OOM;
    }
    l->fd = fd;
    l->open_flags = flags;
    zi_handle_t h = zi_handle25_alloc_with_poll(&listener_ops, &listener_poll_ops, l, ZI_H_READABLE | ZI_H_ENDABLE);
    if (h == 0) {
      (void)close(fd);
      free(l);
      return (zi_handle_t)ZI_E_OOM;
    }
    return h;
  }

  zi_tcp_stream *s = (zi_tcp_stream *)calloc(1, sizeof(*s));
  if (!s) {
    (void)close(fd);
    return (zi_handle_t)ZI_E_OOM;
  }
  s->fd = fd;
  s->connecting = (last_zi == ZI_E_AGAIN) ? 1 : 0;

  zi_handle_t h = zi_handle25_alloc_with_poll(&tcp_ops, &tcp_poll_ops, s, ZI_H_READABLE | ZI_H_WRITABLE | ZI_H_ENDABLE);
  if (h == 0) {
    (void)close(fd);
    free(s);
    return (zi_handle_t)ZI_E_OOM;
  }
  return h;
}
