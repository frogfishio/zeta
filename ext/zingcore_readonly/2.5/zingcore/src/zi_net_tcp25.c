#include "zi_net_tcp25.h"

#include "zi_handles25.h"
#include "zi_runtime25.h"

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <netinet/tcp.h>
#endif

typedef struct {
  int fd;
} zi_tcp_stream;

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
    default:
      return ZI_E_IO;
  }
}

static int32_t tcp_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  zi_tcp_stream *s = (zi_tcp_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (cap == 0) return 0;

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

  // params: u64 host_ptr, u32 host_len, u32 port, u32 flags
  if (params_len < 20u) return (zi_handle_t)ZI_E_INVALID;

  const uint8_t *p = NULL;
  if (!mem->map_ro(mem->ctx, params_ptr, params_len, &p) || !p) return (zi_handle_t)ZI_E_BOUNDS;

  zi_ptr_t host_ptr = (zi_ptr_t)u64le(p + 0);
  uint32_t host_len = u32le(p + 8);
  uint32_t port = u32le(p + 12);
  uint32_t flags = u32le(p + 16);

  if (flags != 0) return (zi_handle_t)ZI_E_INVALID;
  if (host_len == 0 || host_len > 255u) return (zi_handle_t)ZI_E_INVALID;
  if (port == 0 || port > 65535u) return (zi_handle_t)ZI_E_INVALID;

  const uint8_t *host_bytes = NULL;
  if (!mem->map_ro(mem->ctx, host_ptr, (zi_size32_t)host_len, &host_bytes) || !host_bytes) {
    return (zi_handle_t)ZI_E_BOUNDS;
  }
  if (has_embedded_nul(host_bytes, host_len)) return (zi_handle_t)ZI_E_INVALID;

  char host[256];
  memcpy(host, host_bytes, host_len);
  host[host_len] = '\0';

  const char *allow = getenv("ZI_NET_ALLOW");
  if (!allowlist_allows(allow, host, port)) {
    return (zi_handle_t)ZI_E_DENIED;
  }

  char service[16];
  snprintf(service, sizeof(service), "%u", port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  hints.ai_flags = AI_NUMERICSERV;

  struct addrinfo *ai = NULL;
  int ga = getaddrinfo(host, service, &hints, &ai);
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

#if defined(__APPLE__) && defined(SO_NOSIGPIPE)
    {
      int one = 1;
      (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, (socklen_t)sizeof(one));
    }
#endif

    if (connect(fd, cur->ai_addr, cur->ai_addrlen) == 0) {
      last_zi = 0;
      break;
    }

    last_zi = map_errno_to_zi(errno);
    (void)close(fd);
    fd = -1;
  }

  freeaddrinfo(ai);

  if (fd < 0) {
    return (zi_handle_t)last_zi;
  }

  zi_tcp_stream *s = (zi_tcp_stream *)calloc(1, sizeof(*s));
  if (!s) {
    (void)close(fd);
    return (zi_handle_t)ZI_E_OOM;
  }
  s->fd = fd;

  zi_handle_t h = zi_handle25_alloc(&tcp_ops, s, ZI_H_READABLE | ZI_H_WRITABLE | ZI_H_ENDABLE);
  if (h == 0) {
    (void)close(fd);
    free(s);
    return (zi_handle_t)ZI_E_OOM;
  }
  return h;
}
