#include "zi_net_http25.h"

#include "zi_handles25.h"
#include "zi_runtime25.h"
#include "zi_zcl1.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <netinet/tcp.h>
#endif

// ---- protocol ops (must match HTTP_PROTOCOL.md) ----
enum {
  ZI_HTTP_OP_LISTEN = 1,
  ZI_HTTP_OP_CLOSE_LISTENER = 2,
  ZI_HTTP_OP_FETCH = 3,

  ZI_HTTP_OP_RESPOND_START = 10,
  ZI_HTTP_OP_RESPOND_INLINE = 11,
  ZI_HTTP_OP_RESPOND_STREAM = 12,

  ZI_HTTP_OP_MULTIPART_BEGIN = 20,
  ZI_HTTP_OP_MULTIPART_NEXT = 21,
  ZI_HTTP_OP_MULTIPART_END = 22,

  ZI_HTTP_EV_REQUEST = 100,
};

enum {
  ZI_HTTP_BODY_NONE = 0,
  ZI_HTTP_BODY_INLINE = 1,
  ZI_HTTP_BODY_STREAM = 2,
  ZI_HTTP_BODY_MULTIPART = 3,
};

// ---- limits (defaults mirrored from HTTP_PROTOCOL.md) ----

static uint32_t env_u32(const char *name, uint32_t def, uint32_t minv, uint32_t maxv) {
  const char *s = getenv(name);
  if (!s || s[0] == '\0') return def;
  char *end = NULL;
  unsigned long v = strtoul(s, &end, 10);
  if (!end || *end != '\0') return def;
  if (v < (unsigned long)minv) return minv;
  if (v > (unsigned long)maxv) return maxv;
  return (uint32_t)v;
}

typedef struct {
  uint32_t max_req_line_bytes;
  uint32_t max_header_bytes;
  uint32_t max_header_count;
  uint32_t max_inline_body_bytes;
  uint32_t max_inflight_requests;

  uint32_t max_fetch_url_bytes;

  // Multipart (Option A) bounds.
  uint32_t mp_max_parts;
  uint32_t mp_max_header_bytes;
  uint32_t mp_max_header_count;
  uint32_t mp_max_name_bytes;
  uint32_t mp_max_filename_bytes;
} zi_http_limits;

static zi_http_limits load_limits(void) {
  zi_http_limits lim;
  lim.max_req_line_bytes = env_u32("ZI_HTTP_MAX_REQ_LINE_BYTES", 8192u, 512u, 65536u);
  lim.max_header_bytes = env_u32("ZI_HTTP_MAX_HEADER_BYTES", 65536u, 1024u, 1024u * 1024u);
  lim.max_header_count = env_u32("ZI_HTTP_MAX_HEADER_COUNT", 128u, 1u, 4096u);
  lim.max_inline_body_bytes = env_u32("ZI_HTTP_MAX_INLINE_BODY_BYTES", 1024u * 1024u, 0u, 64u * 1024u * 1024u);
  lim.max_inflight_requests = env_u32("ZI_HTTP_MAX_INFLIGHT_REQUESTS", 256u, 1u, 4096u);

  lim.max_fetch_url_bytes = env_u32("ZI_HTTP_MAX_FETCH_URL_BYTES", 8192u, 256u, 1024u * 1024u);

  lim.mp_max_parts = env_u32("ZI_HTTP_MAX_MULTIPART_PARTS", 128u, 1u, 65535u);
  lim.mp_max_header_bytes = env_u32("ZI_HTTP_MAX_MULTIPART_HEADER_BYTES", 16384u, 256u, 1024u * 1024u);
  lim.mp_max_header_count = env_u32("ZI_HTTP_MAX_MULTIPART_HEADER_COUNT", 64u, 1u, 4096u);
  lim.mp_max_name_bytes = env_u32("ZI_HTTP_MAX_MULTIPART_NAME_BYTES", 256u, 1u, 65535u);
  lim.mp_max_filename_bytes = env_u32("ZI_HTTP_MAX_MULTIPART_FILENAME_BYTES", 1024u, 1u, 1024u * 1024u);
  return lim;
}

// ---- sandbox allowlist (mirrors net/tcp semantics) ----

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

static int listen_allowlist_allows(const char *allow, const char *host, uint32_t port) {
  if (!allow || allow[0] == '\0') {
    if (!host || host[0] == '\0') return 1;
    return is_loopback_host(host);
  }
  if (streq_nocase(allow, "any")) return 1;

  int ephemeral = (port == 0);
  const char *p = allow;
  while (*p) {
    p = skip_ws(p);
    if (!*p) break;

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
      if (!host || host[0] == '\0') return 1;
      if (is_loopback_host(host)) return 1;
      continue;
    }

    char *colon = strrchr(tok, ':');
    if (!colon) continue;
    *colon = '\0';
    const char *entry_host = tok;
    const char *entry_port = colon + 1;

    int host_ok = 0;
    if (strcmp(entry_host, "*") == 0) host_ok = 1;
    else {
      char hn[256];
      char en[256];
      host_strip_brackets(host ? host : "", hn, sizeof(hn));
      host_strip_brackets(entry_host, en, sizeof(en));
      host_ok = streq_nocase(hn, en);
    }
    if (!host_ok) continue;

    if (strcmp(entry_port, "*") == 0) return 1;
    if (ephemeral) continue;

    char *ep = NULL;
    long v = strtol(entry_port, &ep, 10);
    if (!ep || *ep != '\0') continue;
    if (v <= 0 || v > 65535) continue;
    if ((uint32_t)v == port) return 1;
  }
  return 0;
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
    default:
      return ZI_E_IO;
  }
}

static void set_nonblocking_best_effort(int fd) {
  if (fd < 0) return;
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return;
  (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void drain_fd_best_effort(int fd) {
  if (fd < 0) return;
  uint8_t tmp[64];
  for (;;) {
    ssize_t n = read(fd, tmp, sizeof(tmp));
    if (n > 0) continue;
    if (n == 0) return;
    if (errno == EINTR) continue;
    return;
  }
}

// ---- state ----

#ifndef ZI_HTTP_MAX_LISTENERS
#define ZI_HTTP_MAX_LISTENERS 16
#endif

typedef struct {
  int in_use;
  uint32_t id;
  int fd;
  uint16_t bound_port;
  uint8_t bound_addr[16];
} zi_http_listener;

typedef struct {
  int fd;
  uint64_t remaining;
  uint8_t *pre;
  uint32_t pre_len;
  uint32_t pre_off;
  int close_on_end;
} zi_http_body_stream;

static uint8_t *find_crlf(uint8_t *p, uint8_t *end);

// Chunked-transfer decoding stream (server-side request bodies).
// This decodes Transfer-Encoding: chunked into a flat byte stream.
typedef struct {
  int fd;
  uint8_t *buf;
  uint32_t buf_off;
  uint32_t buf_len;
  uint32_t buf_cap;

  uint64_t chunk_rem;
  uint32_t trailer_bytes;
  uint32_t trailer_limit;

  int state;  // 0=size line, 1=data, 2=data_crlf, 3=trailers, 4=done
  int close_on_end;
} zi_http_chunked_stream;

static int chunked_poll_get_fd(void *ctx, int *out_fd) {
  zi_http_chunked_stream *s = (zi_http_chunked_stream *)ctx;
  if (!s) return 0;
  if (s->fd < 0) return 0;
  if (out_fd) *out_fd = s->fd;
  return 1;
}

static const zi_handle_poll_ops_v1 CHUNKED_BODY_POLL_OPS = {
    .get_fd = chunked_poll_get_fd,
};

static void chunked_buf_compact(zi_http_chunked_stream *s) {
  if (!s || !s->buf) return;
  if (s->buf_off == 0) return;
  if (s->buf_off >= s->buf_len) {
    s->buf_off = 0;
    s->buf_len = 0;
    return;
  }
  uint32_t avail = s->buf_len - s->buf_off;
  memmove(s->buf, s->buf + s->buf_off, avail);
  s->buf_off = 0;
  s->buf_len = avail;
}

static int32_t chunked_fill(zi_http_chunked_stream *s, uint32_t min_avail) {
  if (!s) return ZI_E_INTERNAL;
  if (min_avail == 0) min_avail = 1;
  while (s->buf_len - s->buf_off < min_avail) {
    if (s->buf_cap - s->buf_len < 1024u) {
      chunked_buf_compact(s);
    }
    if (s->buf_cap - s->buf_len < 1024u) {
      uint32_t ncap = s->buf_cap ? (s->buf_cap * 2u) : 4096u;
      if (ncap < s->buf_cap) return ZI_E_OOM;
      if (ncap > (1024u * 1024u)) ncap = 1024u * 1024u;
      uint8_t *nb = (uint8_t *)realloc(s->buf, (size_t)ncap);
      if (!nb) return ZI_E_OOM;
      s->buf = nb;
      s->buf_cap = ncap;
    }

    ssize_t n = recv(s->fd, s->buf + s->buf_len, (size_t)(s->buf_cap - s->buf_len), 0);
    if (n < 0) return map_errno_to_zi(errno);
    if (n == 0) return ZI_E_IO;
    s->buf_len += (uint32_t)n;
  }
  return 0;
}

static int parse_chunk_size_line(const uint8_t *p, uint32_t n, uint64_t *out_size) {
  if (!p || !out_size) return 0;
  uint64_t v = 0;
  int any = 0;
  for (uint32_t i = 0; i < n; i++) {
    uint8_t ch = p[i];
    if (ch == ';' || ch == ' ' || ch == '\t') break;
    uint8_t c = (uint8_t)tolower((unsigned char)ch);
    uint8_t d;
    if (c >= '0' && c <= '9') d = (uint8_t)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint8_t)(10u + (c - 'a'));
    else return 0;
    any = 1;
    if (v > (UINT64_MAX >> 4)) return 0;
    v = (v << 4) | (uint64_t)d;
  }
  if (!any) return 0;
  *out_size = v;
  return 1;
}

static int32_t chunked_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  zi_http_chunked_stream *s = (zi_http_chunked_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (cap == 0) return 0;
  if (s->state == 4) return 0;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_rw) return ZI_E_NOSYS;
  if (dst_ptr == 0) return ZI_E_BOUNDS;

  uint8_t *dst = NULL;
  if (!mem->map_rw(mem->ctx, dst_ptr, cap, &dst) || !dst) return ZI_E_BOUNDS;

  for (;;) {
    if (s->state == 0) {
      // Need a full chunk-size line ending in CRLF.
      uint8_t *start = s->buf ? (s->buf + s->buf_off) : NULL;
      uint8_t *end = s->buf ? (s->buf + s->buf_len) : NULL;
      uint8_t *eol = (start && end) ? find_crlf(start, end) : NULL;
      if (!eol) {
        if (s->buf && (s->buf_len - s->buf_off) > 1024u) return ZI_E_INVALID;
        int32_t fr = chunked_fill(s, 1u);
        if (fr != 0) return fr;
        continue;
      }
      uint32_t line_len = (uint32_t)(eol - start);
      uint64_t sz = 0;
      if (!parse_chunk_size_line(start, line_len, &sz)) return ZI_E_INVALID;
      s->buf_off += line_len + 2u;
      s->chunk_rem = sz;
      if (sz == 0) {
        s->state = 3;
      } else {
        s->state = 1;
      }
      continue;
    }

    if (s->state == 1) {
      if (s->chunk_rem == 0) {
        s->state = 2;
        continue;
      }
      uint32_t avail = s->buf_len - s->buf_off;
      if (avail == 0) {
        int32_t fr = chunked_fill(s, 1u);
        if (fr != 0) return fr;
        continue;
      }
      uint32_t take = cap;
      if ((uint64_t)take > s->chunk_rem) take = (uint32_t)s->chunk_rem;
      if (take > avail) take = avail;
      memcpy(dst, s->buf + s->buf_off, take);
      s->buf_off += take;
      s->chunk_rem -= (uint64_t)take;
      return (int32_t)take;
    }

    if (s->state == 2) {
      int32_t fr = chunked_fill(s, 2u);
      if (fr != 0) return fr;
      if (!(s->buf[s->buf_off] == '\r' && s->buf[s->buf_off + 1u] == '\n')) return ZI_E_INVALID;
      s->buf_off += 2u;
      s->state = 0;
      continue;
    }

    if (s->state == 3) {
      // Trailers: read lines until an empty line.
      uint8_t *start = s->buf ? (s->buf + s->buf_off) : NULL;
      uint8_t *end = s->buf ? (s->buf + s->buf_len) : NULL;
      uint8_t *eol = (start && end) ? find_crlf(start, end) : NULL;
      if (!eol) {
        if (s->trailer_bytes > s->trailer_limit) return ZI_E_INVALID;
        int32_t fr = chunked_fill(s, 1u);
        if (fr != 0) return fr;
        continue;
      }
      uint32_t line_len = (uint32_t)(eol - start);
      s->buf_off += line_len + 2u;
      s->trailer_bytes += line_len + 2u;
      if (line_len == 0) {
        s->state = 4;
        return 0;
      }
      continue;
    }

    return 0;
  }
}

static int32_t chunked_write(void *ctx, zi_ptr_t src_ptr, zi_size32_t len) {
  (void)ctx;
  (void)src_ptr;
  (void)len;
  return ZI_E_DENIED;
}

static int32_t chunked_end(void *ctx) {
  zi_http_chunked_stream *s = (zi_http_chunked_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (s->close_on_end && s->fd >= 0) {
    (void)close(s->fd);
    s->fd = -1;
  }
  free(s->buf);
  s->buf = NULL;
  free(s);
  return 0;
}

static const zi_handle_ops_v1 CHUNKED_BODY_OPS = {
    .read = chunked_read,
    .write = chunked_write,
    .end = chunked_end,
};

static zi_http_chunked_stream *chunked_stream_new(int fd, const uint8_t *pre, uint32_t pre_len, uint32_t trailer_limit,
                                                  int close_on_end) {
  zi_http_chunked_stream *s = (zi_http_chunked_stream *)calloc(1u, sizeof(*s));
  if (!s) return NULL;
  s->fd = fd;
  s->state = 0;
  s->close_on_end = close_on_end ? 1 : 0;
  s->trailer_limit = trailer_limit;
  if (pre_len) {
    s->buf = (uint8_t *)malloc((size_t)pre_len);
    if (!s->buf) {
      free(s);
      return NULL;
    }
    memcpy(s->buf, pre, pre_len);
    s->buf_off = 0;
    s->buf_len = pre_len;
    s->buf_cap = pre_len;
  }
  return s;
}

static int body_poll_get_fd(void *ctx, int *out_fd) {
  zi_http_body_stream *s = (zi_http_body_stream *)ctx;
  if (!s) return 0;
  if (s->fd < 0) return 0;
  if (out_fd) *out_fd = s->fd;
  return 1;
}

static const zi_handle_poll_ops_v1 BODY_POLL_OPS = {
    .get_fd = body_poll_get_fd,
};

typedef struct zi_http_cap_ctx zi_http_cap_ctx;
typedef struct zi_http_multipart_iter zi_http_multipart_iter;

static int32_t body_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  zi_http_body_stream *s = (zi_http_body_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (cap == 0) return 0;
  if (s->remaining == 0) return 0;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_rw) return ZI_E_NOSYS;
  if (dst_ptr == 0) return ZI_E_BOUNDS;

  uint32_t want = cap;
  if ((uint64_t)want > s->remaining) want = (uint32_t)s->remaining;

  uint8_t *dst = NULL;
  if (!mem->map_rw(mem->ctx, dst_ptr, want, &dst) || !dst) return ZI_E_BOUNDS;

  // Serve any prebuffered bytes first.
  if (s->pre && s->pre_off < s->pre_len) {
    uint32_t avail = s->pre_len - s->pre_off;
    uint32_t take = want < avail ? want : avail;
    memcpy(dst, s->pre + s->pre_off, take);
    s->pre_off += take;
    if ((uint64_t)take > s->remaining) s->remaining = 0;
    else s->remaining -= (uint64_t)take;
    if (s->pre_off == s->pre_len) {
      free(s->pre);
      s->pre = NULL;
      s->pre_len = 0;
      s->pre_off = 0;
    }
    return (int32_t)take;
  }

  ssize_t n = recv(s->fd, dst, (size_t)want, 0);
  if (n < 0) return map_errno_to_zi(errno);
  if (n == 0) {
    s->remaining = 0;
    return 0;
  }
  if ((uint64_t)n > s->remaining) s->remaining = 0;
  else s->remaining -= (uint64_t)n;
  return (int32_t)n;
}

static int32_t body_write(void *ctx, zi_ptr_t src_ptr, zi_size32_t len) {
  (void)ctx;
  (void)src_ptr;
  (void)len;
  return ZI_E_DENIED;
}

static int32_t body_end(void *ctx) {
  zi_http_body_stream *s = (zi_http_body_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (s->close_on_end && s->fd >= 0) {
    (void)close(s->fd);
    s->fd = -1;
  }
  free(s->pre);
  s->pre = NULL;
  memset(s, 0, sizeof(*s));
  free(s);
  return 0;
}

static const zi_handle_ops_v1 BODY_OPS = {
    .read = body_read,
    .write = body_write,
    .end = body_end,
};

static zi_http_body_stream *body_stream_new(int fd, uint64_t remaining, const uint8_t *pre, uint32_t pre_len, int close_on_end) {
  zi_http_body_stream *bs = (zi_http_body_stream *)calloc(1u, sizeof(*bs));
  if (!bs) return NULL;
  bs->fd = fd;
  bs->remaining = remaining;
  bs->close_on_end = close_on_end ? 1 : 0;
  if (pre_len) {
    bs->pre = (uint8_t *)malloc((size_t)pre_len);
    if (!bs->pre) {
      free(bs);
      return NULL;
    }
    memcpy(bs->pre, pre, pre_len);
    bs->pre_len = pre_len;
    bs->pre_off = 0;
  }
  return bs;
}

static int body_stream_read_host(zi_http_body_stream *s, uint8_t *dst, uint32_t cap) {
  if (!s || !dst) return -1;
  if (cap == 0) return 0;
  if (s->pre && s->pre_off < s->pre_len) {
    uint32_t avail = s->pre_len - s->pre_off;
    uint32_t take = cap < avail ? cap : avail;
    memcpy(dst, s->pre + s->pre_off, take);
    s->pre_off += take;
    if (s->pre_off == s->pre_len) {
      free(s->pre);
      s->pre = NULL;
      s->pre_len = 0;
      s->pre_off = 0;
    }
    return (int)take;
  }

  if (s->remaining == 0) return 0;
  if (s->fd < 0) return 0;

  uint32_t want = cap;
  if ((uint64_t)want > s->remaining) want = (uint32_t)s->remaining;

    ssize_t n = recv(s->fd, dst, (size_t)want, 0);
  if (n < 0) return -1;
  if (n == 0) {
    s->remaining = 0;
    return 0;
  }
  if ((uint64_t)n > s->remaining) s->remaining = 0;
  else s->remaining -= (uint64_t)n;
  return (int)n;
}

typedef struct {
  char *name;
  uint32_t name_len;
  char *val;
  uint32_t val_len;
} zi_http_mp_hdr;

static void lower_ascii(uint8_t *p, uint32_t n);
static int find_seq(const uint8_t *p, size_t n, const uint8_t *seq, size_t seqn);

struct zi_http_multipart_iter {
  uint32_t rid;
  zi_http_body_stream *bs;

  uint32_t max_parts;
  uint32_t max_header_bytes;
  uint32_t max_header_count;
  uint32_t max_name_bytes;
  uint32_t max_filename_bytes;
  uint32_t parts_emitted;

  uint8_t *boundary;
  uint32_t boundary_len;

  uint8_t *delim;      // "\r\n--" + boundary
  uint32_t delim_len;

  uint8_t *buf;
  uint32_t buf_off;
  uint32_t buf_len;
  uint32_t buf_cap;

  int started;
  int done;
  int part_open;
  int need_boundary;
};

typedef struct {
  zi_http_multipart_iter *it;
  int closed;
} zi_http_mp_part;

static int mp_part_poll_get_fd(void *ctx, int *out_fd) {
  zi_http_mp_part *p = (zi_http_mp_part *)ctx;
  if (!p || !p->it || !p->it->bs) return 0;
  if (p->it->bs->fd < 0) return 0;
  if (out_fd) *out_fd = p->it->bs->fd;
  return 1;
}

static const zi_handle_poll_ops_v1 MP_PART_POLL_OPS = {
    .get_fd = mp_part_poll_get_fd,
};

static void mp_free(zi_http_multipart_iter *it) {
  if (!it) return;
  free(it->boundary);
  free(it->delim);
  free(it->buf);
  memset(it, 0, sizeof(*it));
  free(it);
}

static int mp_ensure(zi_http_multipart_iter *it, uint32_t need) {
  if (!it) return 0;
  if (it->done) return 1;
  if (it->buf_len - it->buf_off >= need) return 1;

  if (it->buf_off > 0 && it->buf_off == it->buf_len) {
    it->buf_off = 0;
    it->buf_len = 0;
  } else if (it->buf_off > 0 && it->buf_off > (it->buf_cap / 2u)) {
    memmove(it->buf, it->buf + it->buf_off, it->buf_len - it->buf_off);
    it->buf_len -= it->buf_off;
    it->buf_off = 0;
  }

  while (it->buf_len - it->buf_off < need) {
    if (!it->bs) return 0;
    if (it->bs->remaining == 0 && (!it->bs->pre || it->bs->pre_off >= it->bs->pre_len)) return 0;
    if (it->buf_cap - it->buf_len < 4096u) {
      uint32_t ncap = it->buf_cap ? it->buf_cap * 2u : 8192u;
      if (ncap < it->buf_cap) return 0;
      uint32_t lim = it->max_header_bytes ? it->max_header_bytes : (1024u * 1024u);
      if (ncap > lim) ncap = lim;
      if (ncap <= it->buf_cap) return 0;
      uint8_t *nb = (uint8_t *)realloc(it->buf, (size_t)ncap);
      if (!nb) return 0;
      it->buf = nb;
      it->buf_cap = ncap;
    }
    int n = body_stream_read_host(it->bs, it->buf + it->buf_len, it->buf_cap - it->buf_len);
    if (n <= 0) return 0;
    it->buf_len += (uint32_t)n;
  }
  return 1;
}

static int mp_starts_with(const uint8_t *p, uint32_t n, const uint8_t *lit, uint32_t ln) {
  if (!p || !lit) return 0;
  if (n < ln) return 0;
  return memcmp(p, lit, ln) == 0;
}

static int mp_consume_boundary(zi_http_multipart_iter *it, int first) {
  if (!it || it->done) return 0;

  // Accept either:
  //  first:  "--boundary\r\n" OR "\r\n--boundary\r\n"
  //  next:   "\r\n--boundary\r\n" or final "\r\n--boundary--\r\n"
  const uint8_t *p = it->buf + it->buf_off;
  uint32_t avail = it->buf_len - it->buf_off;

  uint32_t prefix = first ? 2u : 4u;
  // Ensure we can match the longer prefix path.
  if (!mp_ensure(it, 4u + it->boundary_len + 2u)) return 0;
  p = it->buf + it->buf_off;
  avail = it->buf_len - it->buf_off;

  if (first) {
    if (avail >= 4u + it->boundary_len && mp_starts_with(p, avail, (const uint8_t *)"\r\n--", 4u)) {
      prefix = 4u;
    } else if (avail >= 2u + it->boundary_len && mp_starts_with(p, avail, (const uint8_t *)"--", 2u)) {
      prefix = 2u;
    } else {
      return 0;
    }
  } else {
    if (!mp_starts_with(p, avail, (const uint8_t *)"\r\n--", 4u)) return 0;
    prefix = 4u;
  }

  if (!mp_starts_with(p + prefix, avail - prefix, it->boundary, it->boundary_len)) return 0;
  uint32_t off = prefix + it->boundary_len;
  if (!mp_ensure(it, off + 2u)) return 0;
  p = it->buf + it->buf_off;

  // Final boundary: "--"
  if (p[off] == '-' && p[off + 1] == '-') {
    // Consume "--boundary--" and optional trailing CRLF
    if (!mp_ensure(it, off + 4u)) {
      // At least consume what we have.
      it->buf_off += off + 2u;
      it->done = 1;
      return 1;
    }
    it->buf_off += off + 2u;
    // Optional CRLF
    if (it->buf_len - it->buf_off >= 2u) {
      if (it->buf[it->buf_off] == '\r' && it->buf[it->buf_off + 1] == '\n') it->buf_off += 2u;
    }
    it->done = 1;
    return 1;
  }

  // Normal boundary: must end with CRLF.
  if (p[off] != '\r' || p[off + 1] != '\n') return 0;
  it->buf_off += off + 2u;
  return 1;
}

static int mp_find_dcrlf(zi_http_multipart_iter *it, uint32_t *out_idx) {
  if (!it || !out_idx) return 0;
  // Find "\r\n\r\n" in buffer.
  for (;;) {
    uint8_t *p = it->buf + it->buf_off;
    uint32_t n = it->buf_len - it->buf_off;
    for (uint32_t i = 0; i + 3u < n; i++) {
      if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n') {
        *out_idx = it->buf_off + i;
        return 1;
      }
    }
    if (!mp_ensure(it, n + 4096u)) return 0;
  }
}

static int mp_parse_headers(zi_http_multipart_iter *it, zi_http_mp_hdr **out_hdrs, uint32_t *out_hcnt,
                            char **out_name, uint32_t *out_name_len,
                            char **out_filename, uint32_t *out_filename_len,
                            char **out_ctype, uint32_t *out_ctype_len) {
  if (!it || !out_hdrs || !out_hcnt) return 0;
  *out_hdrs = NULL;
  *out_hcnt = 0;
  if (out_name) *out_name = NULL;
  if (out_name_len) *out_name_len = 0;
  if (out_filename) *out_filename = NULL;
  if (out_filename_len) *out_filename_len = 0;
  if (out_ctype) *out_ctype = NULL;
  if (out_ctype_len) *out_ctype_len = 0;

  uint32_t hdr_end = 0;
  if (!mp_find_dcrlf(it, &hdr_end)) return 0;
  uint32_t start = it->buf_off;
  // hdr_end points at the CR of the final header line's CRLF within the "\r\n\r\n" terminator.
  // Include that CRLF so we can parse the last header line.
  uint32_t end = hdr_end + 2u;
  if (end < start) return 0;
  uint32_t total = end - start;
  if (it->max_header_bytes && total > it->max_header_bytes) return 0;

  uint32_t maxh = it->max_header_count ? it->max_header_count : 128u;
  if (maxh > 4096u) maxh = 4096u;
  zi_http_mp_hdr *hdrs = (zi_http_mp_hdr *)calloc((size_t)maxh, sizeof(*hdrs));
  if (!hdrs) return 0;

  uint32_t hcnt = 0;
  uint32_t pos = start;
  while (pos < end) {
    // line ends at CRLF
    uint32_t line_end = pos;
    while (line_end + 1u < end) {
      if (it->buf[line_end] == '\r' && it->buf[line_end + 1] == '\n') break;
      line_end++;
    }
    if (line_end + 1u >= end) break;
    if (line_end == pos) {
      pos = line_end + 2u;
      continue;
    }
    uint8_t *line = it->buf + pos;
    uint32_t line_len = line_end - pos;
    uint8_t *colon = (uint8_t *)memchr(line, ':', (size_t)line_len);
    if (!colon) {
      free(hdrs);
      return 0;
    }
    uint32_t name_len = (uint32_t)(colon - line);
    uint8_t *val = colon + 1;
    uint8_t *val_end = line + line_len;
    while (val < val_end && (*val == ' ' || *val == '\t')) val++;
    while (val_end > val && (val_end[-1] == ' ' || val_end[-1] == '\t')) val_end--;
    uint32_t val_len = (uint32_t)(val_end - val);
    if (name_len == 0) {
      free(hdrs);
      return 0;
    }
    lower_ascii(line, name_len);

    if (hcnt >= maxh) {
      free(hdrs);
      return 0;
    }
    hdrs[hcnt].name = (char *)malloc((size_t)name_len + 1u);
    hdrs[hcnt].val = (char *)malloc((size_t)val_len + 1u);
    if (!hdrs[hcnt].name || !hdrs[hcnt].val) {
      free(hdrs[hcnt].name);
      free(hdrs[hcnt].val);
      for (uint32_t j = 0; j < hcnt; j++) {
        free(hdrs[j].name);
        free(hdrs[j].val);
      }
      free(hdrs);
      return 0;
    }
    memcpy(hdrs[hcnt].name, line, name_len);
    hdrs[hcnt].name[name_len] = 0;
    hdrs[hcnt].name_len = name_len;
    memcpy(hdrs[hcnt].val, val, val_len);
    hdrs[hcnt].val[val_len] = 0;
    hdrs[hcnt].val_len = val_len;

    if (out_ctype && out_ctype_len && name_len == 12 && memcmp(hdrs[hcnt].name, "content-type", 12) == 0) {
      *out_ctype = hdrs[hcnt].val;
      *out_ctype_len = hdrs[hcnt].val_len;
    }

    if (out_name && out_name_len && out_filename && out_filename_len && name_len == 19 &&
        memcmp(hdrs[hcnt].name, "content-disposition", 19) == 0) {
      // Very small parser for: form-data; name="x"; filename="y"
      const char *v = hdrs[hcnt].val;
      uint32_t vn = hdrs[hcnt].val_len;
      for (uint32_t k = 0; k + 5u < vn; k++) {
        int at_param = (k == 0) || (v[k - 1] == ';') || (v[k - 1] == ' ') || (v[k - 1] == '\t');
        if (at_param && (k + 5u <= vn) && strncasecmp(v + k, "name=", 5) == 0) {
          if (*out_name != NULL) continue;
          const char *q = v + k + 5;
          char quote = 0;
          if (*q == '"' || *q == '\'') { quote = *q; q++; }
          const char *s = q;
          while ((uint32_t)(q - v) < vn) {
            if (quote) {
              if (*q == quote) break;
            } else {
              if (*q == ';' || *q == ' ' || *q == '\t') break;
            }
            q++;
          }
          uint32_t ln = (uint32_t)(q - s);
          if (ln) {
            if (it->max_name_bytes && ln > it->max_name_bytes) {
              for (uint32_t j = 0; j <= hcnt; j++) {
                free(hdrs[j].name);
                free(hdrs[j].val);
              }
              free(hdrs);
              return 0;
            }
            *out_name = (char *)malloc((size_t)ln + 1u);
            if (*out_name) {
              memcpy(*out_name, s, ln);
              (*out_name)[ln] = 0;
              *out_name_len = ln;
            }
          }
        }
        if (at_param && (k + 9u <= vn) && strncasecmp(v + k, "filename=", 9) == 0) {
          if (*out_filename != NULL) continue;
          const char *q = v + k + 9;
          char quote = 0;
          if (*q == '"' || *q == '\'') { quote = *q; q++; }
          const char *s = q;
          while ((uint32_t)(q - v) < vn) {
            if (quote) {
              if (*q == quote) break;
            } else {
              if (*q == ';' || *q == ' ' || *q == '\t') break;
            }
            q++;
          }
          uint32_t ln = (uint32_t)(q - s);
          if (ln) {
            if (it->max_filename_bytes && ln > it->max_filename_bytes) {
              for (uint32_t j = 0; j <= hcnt; j++) {
                free(hdrs[j].name);
                free(hdrs[j].val);
              }
              free(hdrs);
              return 0;
            }
            *out_filename = (char *)malloc((size_t)ln + 1u);
            if (*out_filename) {
              memcpy(*out_filename, s, ln);
              (*out_filename)[ln] = 0;
              *out_filename_len = ln;
            }
          }
        }
      }
    }

    hcnt++;
    pos = line_end + 2u;
  }

  // consume header section + CRLFCRLF
  it->buf_off = hdr_end + 4u;
  *out_hdrs = hdrs;
  *out_hcnt = hcnt;
  return 1;
}

static int mp_find_delim(zi_http_multipart_iter *it, uint32_t *out_idx) {
  if (!it || !out_idx) return 0;
  for (;;) {
    uint32_t n = it->buf_len - it->buf_off;
    int idx = find_seq(it->buf + it->buf_off, n, it->delim, it->delim_len);
    if (idx >= 0) {
      *out_idx = it->buf_off + (uint32_t)idx;
      return 1;
    }
    // need more data
    if (!mp_ensure(it, n + 4096u)) return 0;
  }
}

static int32_t mp_part_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  zi_http_mp_part *p = (zi_http_mp_part *)ctx;
  if (!p || !p->it) return ZI_E_INTERNAL;
  if (p->closed) return ZI_E_CLOSED;
  if (cap == 0) return 0;
  zi_http_multipart_iter *it = p->it;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_rw) return ZI_E_NOSYS;
  if (dst_ptr == 0) return ZI_E_BOUNDS;

  // If delimiter is at current position, EOF.
  if (!mp_ensure(it, it->delim_len)) return 0;
  uint32_t avail = it->buf_len - it->buf_off;
  if (avail == 0) return 0;

  // Compute how many bytes we can emit without crossing the delimiter.
  uint32_t out_avail = 0;
  int dpos = find_seq(it->buf + it->buf_off, (size_t)avail, it->delim, (size_t)it->delim_len);
  if (dpos >= 0) {
    if ((uint32_t)dpos == 0) return 0;
    out_avail = (uint32_t)dpos;
  } else {
    // Hold back enough bytes to match a delimiter spanning the boundary.
    uint32_t hold = (it->delim_len > 0) ? (it->delim_len - 1u) : 0u;
    out_avail = avail;
    if (out_avail > hold) out_avail -= hold;
    else out_avail = 0;
    if (out_avail == 0) {
      if (!mp_ensure(it, avail + 4096u)) return 0;
      avail = it->buf_len - it->buf_off;
      dpos = find_seq(it->buf + it->buf_off, (size_t)avail, it->delim, (size_t)it->delim_len);
      if (dpos >= 0) {
        if ((uint32_t)dpos == 0) return 0;
        out_avail = (uint32_t)dpos;
      } else {
        out_avail = avail;
        if (out_avail > hold) out_avail -= hold;
        else out_avail = 0;
        if (out_avail == 0) return 0;
      }
    }
  }

  uint32_t n = (uint32_t)cap;
  if (n > out_avail) n = out_avail;
  uint8_t *dst = NULL;
  if (!mem->map_rw(mem->ctx, dst_ptr, n, &dst) || !dst) return ZI_E_BOUNDS;
  memcpy(dst, it->buf + it->buf_off, n);
  it->buf_off += n;
  return (int32_t)n;
}

static void mp_free_hdrs(zi_http_mp_hdr *hdrs, uint32_t hcnt) {
  if (!hdrs) return;
  for (uint32_t i = 0; i < hcnt; i++) {
    free(hdrs[i].name);
    free(hdrs[i].val);
  }
  free(hdrs);
}

static void mp_drain_part_to_delim(zi_http_multipart_iter *it) {
  if (!it) return;
  for (;;) {
    if (!mp_ensure(it, it->delim_len)) return;
    uint32_t idx = 0;
    if (mp_find_delim(it, &idx) && idx >= it->buf_off) {
      it->buf_off = idx;
      return;
    }
    // discard most of the buffer but keep some tail
    uint32_t avail = it->buf_len - it->buf_off;
    uint32_t hold = (it->delim_len > 0) ? (it->delim_len - 1u) : 0u;
    if (avail > hold) {
      it->buf_off += (avail - hold);
    } else {
      (void)mp_ensure(it, avail + 4096u);
    }
  }
}

static int32_t mp_part_end(void *ctx) {
  zi_http_mp_part *p = (zi_http_mp_part *)ctx;
  if (!p) return ZI_E_INTERNAL;
  if (p->closed) {
    free(p);
    return 0;
  }
  p->closed = 1;
  if (p->it) {
    mp_drain_part_to_delim(p->it);
    p->it->part_open = 0;
    p->it->need_boundary = 1;
  }
  free(p);
  return 0;
}

static const zi_handle_ops_v1 MP_PART_OPS = {
    .read = mp_part_read,
    .write = body_write,
    .end = mp_part_end,
};

typedef struct {
  int in_use;
  uint32_t rid;
  uint32_t listener_id;
  int fd;
  uint64_t body_remaining;
  zi_handle_t body_handle;
  zi_http_body_stream *body_stream;
  zi_handle_t resp_body_handle;

  int is_multipart;
  uint8_t *mp_boundary;
  uint32_t mp_boundary_len;
  zi_http_multipart_iter *mp;
} zi_http_req;

typedef struct zi_http_cap_ctx {
  int closed;

  pthread_mutex_t mu;
  pthread_cond_t cv;

  // Wakeup pipe for sys/loop readiness (readable when c->out has data).
  int notify_r;
  int notify_w;
  int notify_pending;

  pthread_t srv_thr;
  int srv_thr_started;

  uint8_t *in;
  uint32_t in_len;
  uint32_t in_cap;

  uint8_t *out;
  uint32_t out_len;
  uint32_t out_off;

  zi_http_limits lim;

  zi_http_listener listeners[ZI_HTTP_MAX_LISTENERS];
  uint32_t next_listener_id;

  zi_http_req *reqs;
  uint32_t reqs_cap;
  uint32_t next_rid;
} zi_http_cap_ctx;

static int http_poll_get_fd(void *ctx, int *out_fd) {
  zi_http_cap_ctx *c = (zi_http_cap_ctx *)ctx;
  if (!c) return 0;
  if (c->notify_r < 0) return 0;
  if (out_fd) *out_fd = c->notify_r;
  return 1;
}

static const zi_handle_poll_ops_v1 HTTP_POLL_OPS = {
    .get_fd = http_poll_get_fd,
};

static void free_out_locked(zi_http_cap_ctx *c) {
  if (!c) return;
  free(c->out);
  c->out = NULL;
  c->out_len = 0;
  c->out_off = 0;

  // If there's no more readable data, clear the notify pipe.
  if (c->notify_r >= 0) {
    drain_fd_best_effort(c->notify_r);
  }
  c->notify_pending = 0;
  pthread_cond_broadcast(&c->cv);
}

static void free_out(zi_http_cap_ctx *c) {
  if (!c) return;
  pthread_mutex_lock(&c->mu);
  free_out_locked(c);
  pthread_mutex_unlock(&c->mu);
}

static void free_in(zi_http_cap_ctx *c) {
  if (!c) return;
  free(c->in);
  c->in = NULL;
  c->in_len = 0;
  c->in_cap = 0;
}

static int ensure_in_cap(zi_http_cap_ctx *c, uint32_t need) {
  if (!c) return 0;
  if (need <= c->in_cap) return 1;
  uint32_t cap = c->in_cap ? c->in_cap : 4096u;
  while (cap < need) {
    uint32_t next = cap * 2u;
    if (next < cap) return 0;
    cap = next;
  }
  uint64_t hard = 24ull + (uint64_t)c->lim.max_header_bytes + (uint64_t)c->lim.max_inline_body_bytes + 4096ull;
  if ((uint64_t)cap > hard) cap = (uint32_t)hard;
  if ((uint64_t)need > (uint64_t)cap) return 0;
  uint8_t *p = (uint8_t *)realloc(c->in, (size_t)cap);
  if (!p) return 0;
  c->in = p;
  c->in_cap = cap;
  return 1;
}

static int try_set_out_frame_ok(zi_http_cap_ctx *c, uint16_t op, uint32_t rid, const uint8_t *payload, uint32_t payload_len) {
  if (!c) return 0;
  pthread_mutex_lock(&c->mu);
  if (c->out && c->out_len && c->out_off < c->out_len) {
    pthread_mutex_unlock(&c->mu);
    return 0;
  }
  free_out_locked(c);
  uint32_t cap = 24u + payload_len;
  c->out = (uint8_t *)malloc((size_t)cap);
  if (!c->out) {
    pthread_mutex_unlock(&c->mu);
    return 0;
  }
  int n = zi_zcl1_write_ok(c->out, cap, op, rid, payload, payload_len);
  if (n <= 0) {
    free_out_locked(c);
    pthread_mutex_unlock(&c->mu);
    return 0;
  }
  c->out_len = (uint32_t)n;
  c->out_off = 0;

  if (c->notify_w >= 0 && !c->notify_pending) {
    uint8_t b = 1;
    (void)write(c->notify_w, &b, 1);
    c->notify_pending = 1;
  }
  pthread_mutex_unlock(&c->mu);
  return 1;
}

static int wait_set_out_frame_ok(zi_http_cap_ctx *c, uint16_t op, uint32_t rid, const uint8_t *payload, uint32_t payload_len) {
  if (!c) return 0;
  pthread_mutex_lock(&c->mu);
  while (!c->closed && c->out && c->out_len && c->out_off < c->out_len) {
    pthread_cond_wait(&c->cv, &c->mu);
  }
  if (c->closed) {
    pthread_mutex_unlock(&c->mu);
    return 0;
  }
  free_out_locked(c);
  uint32_t cap = 24u + payload_len;
  c->out = (uint8_t *)malloc((size_t)cap);
  if (!c->out) {
    pthread_mutex_unlock(&c->mu);
    return 0;
  }
  int n = zi_zcl1_write_ok(c->out, cap, op, rid, payload, payload_len);
  if (n <= 0) {
    free_out_locked(c);
    pthread_mutex_unlock(&c->mu);
    return 0;
  }
  c->out_len = (uint32_t)n;
  c->out_off = 0;
  if (c->notify_w >= 0 && !c->notify_pending) {
    uint8_t b = 1;
    (void)write(c->notify_w, &b, 1);
    c->notify_pending = 1;
  }
  pthread_mutex_unlock(&c->mu);
  return 1;
}

static int try_set_out_frame_err(zi_http_cap_ctx *c, uint16_t op, uint32_t rid, const char *trace, const char *msg) {
  if (!c) return 0;
  pthread_mutex_lock(&c->mu);
  if (c->out && c->out_len && c->out_off < c->out_len) {
    pthread_mutex_unlock(&c->mu);
    return 0;
  }
  free_out_locked(c);
  uint32_t cap = 4096u;
  c->out = (uint8_t *)malloc((size_t)cap);
  if (!c->out) {
    pthread_mutex_unlock(&c->mu);
    return 0;
  }
  int n = zi_zcl1_write_error(c->out, cap, op, rid, trace, msg);
  if (n <= 0) {
    free_out_locked(c);
    pthread_mutex_unlock(&c->mu);
    return 0;
  }
  c->out_len = (uint32_t)n;
  c->out_off = 0;
  if (c->notify_w >= 0 && !c->notify_pending) {
    uint8_t b = 1;
    (void)write(c->notify_w, &b, 1);
    c->notify_pending = 1;
  }
  pthread_mutex_unlock(&c->mu);
  return 1;
}

static int wait_set_out_frame_err(zi_http_cap_ctx *c, uint16_t op, uint32_t rid, const char *trace, const char *msg) {
  if (!c) return 0;
  pthread_mutex_lock(&c->mu);
  while (!c->closed && c->out && c->out_len && c->out_off < c->out_len) {
    pthread_cond_wait(&c->cv, &c->mu);
  }
  if (c->closed) {
    pthread_mutex_unlock(&c->mu);
    return 0;
  }
  free_out_locked(c);
  uint32_t cap = 4096u;
  c->out = (uint8_t *)malloc((size_t)cap);
  if (!c->out) {
    pthread_mutex_unlock(&c->mu);
    return 0;
  }
  int n = zi_zcl1_write_error(c->out, cap, op, rid, trace, msg);
  if (n <= 0) {
    free_out_locked(c);
    pthread_mutex_unlock(&c->mu);
    return 0;
  }
  c->out_len = (uint32_t)n;
  c->out_off = 0;
  if (c->notify_w >= 0 && !c->notify_pending) {
    uint8_t b = 1;
    (void)write(c->notify_w, &b, 1);
    c->notify_pending = 1;
  }
  pthread_mutex_unlock(&c->mu);
  return 1;
}

// Most dispatch paths are synchronous and must not block waiting for a reader;
// they rely on http_write returning ZI_E_AGAIN if an unread frame exists.
#define set_out_frame_ok  try_set_out_frame_ok
#define set_out_frame_err try_set_out_frame_err

static zi_http_listener *listener_by_id(zi_http_cap_ctx *c, uint32_t id) {
  if (!c) return NULL;
  for (uint32_t i = 0; i < ZI_HTTP_MAX_LISTENERS; i++) {
    if (c->listeners[i].in_use && c->listeners[i].id == id) return &c->listeners[i];
  }
  return NULL;
}

static int alloc_listener_slot(zi_http_cap_ctx *c, zi_http_listener **out) {
  if (!c || !out) return 0;
  for (uint32_t i = 0; i < ZI_HTTP_MAX_LISTENERS; i++) {
    if (!c->listeners[i].in_use) {
      *out = &c->listeners[i];
      return 1;
    }
  }
  return 0;
}

static zi_http_req *req_by_rid(zi_http_cap_ctx *c, uint32_t rid) {
  if (!c || !c->reqs) return NULL;
  for (uint32_t i = 0; i < c->reqs_cap; i++) {
    if (c->reqs[i].in_use && c->reqs[i].rid == rid) return &c->reqs[i];
  }
  return NULL;
}

static zi_http_req *alloc_req_slot(zi_http_cap_ctx *c) {
  if (!c || !c->reqs) return NULL;
  for (uint32_t i = 0; i < c->reqs_cap; i++) {
    if (!c->reqs[i].in_use) return &c->reqs[i];
  }
  return NULL;
}

static void close_req(zi_http_req *r) {
  if (!r) return;
  if (r->fd >= 0) {
    (void)close(r->fd);
    r->fd = -1;
  }
  if (r->mp) {
    mp_free(r->mp);
    r->mp = NULL;
  }
  free(r->mp_boundary);
  r->mp_boundary = NULL;
  r->mp_boundary_len = 0;
  r->is_multipart = 0;
  if (r->body_handle >= 3) {
    (void)zi_end(r->body_handle);
    r->body_handle = 0;
  } else if (r->body_stream) {
    body_end(r->body_stream);
  }
  r->body_stream = NULL;
  memset(r, 0, sizeof(*r));
  r->fd = -1;
}

static void close_req_no_resp_handle(zi_http_req *r) {
  if (!r) return;
  zi_handle_t resp = r->resp_body_handle;
  r->resp_body_handle = 0;
  if (r->fd >= 0) {
    (void)close(r->fd);
    r->fd = -1;
  }
  if (r->mp) {
    mp_free(r->mp);
    r->mp = NULL;
  }
  free(r->mp_boundary);
  r->mp_boundary = NULL;
  r->mp_boundary_len = 0;
  r->is_multipart = 0;
  if (r->body_handle >= 3) {
    (void)zi_end(r->body_handle);
    r->body_handle = 0;
  } else if (r->body_stream) {
    body_end(r->body_stream);
  }
  r->body_stream = NULL;
  memset(r, 0, sizeof(*r));
  r->fd = -1;
  r->resp_body_handle = resp;
}

static int find_seq(const uint8_t *p, size_t n, const uint8_t *seq, size_t seqn) {
  if (!p || !seq || seqn == 0) return -1;
  if (n < seqn) return -1;
  for (size_t i = 0; i + seqn <= n; i++) {
    if (memcmp(p + i, seq, seqn) == 0) return (int)i;
  }
  return -1;
}

static int contains_ctl_or_lf(const uint8_t *p, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    uint8_t c = p[i];
    if (c == '\r' || c == '\n') return 1;
    if (c < 0x20 && c != '\t') return 1;
  }
  return 0;
}

static int send_all(int fd, const uint8_t *p, size_t n) {
  while (n > 0) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    ssize_t w = send(fd, p, n, flags);
    if (w < 0) {
      if (errno == EINTR) continue;
      return 0;
    }
    p += (size_t)w;
    n -= (size_t)w;
  }
  return 1;
}

static void send_http_error_best_effort(int fd, uint32_t code, const char *reason, const char *body) {
  if (fd < 0) return;
  if (!reason) reason = "Error";
  if (!body) body = "";
  char resp[512];
  uint32_t blen = (uint32_t)strlen(body);
  int n = snprintf(resp, sizeof(resp),
                   "HTTP/1.1 %u %s\r\n"
                   "content-type: text/plain\r\n"
                   "content-length: %u\r\n"
                   "connection: close\r\n"
                   "\r\n"
                   "%s",
                   code, reason, blen, body);
  if (n <= 0) return;
  if ((size_t)n > sizeof(resp)) n = (int)sizeof(resp);
  (void)send_all(fd, (const uint8_t *)resp, (size_t)n);
}

static const char *reason_phrase(uint32_t code) {
  switch (code) {
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 204:
      return "No Content";
    case 301:
      return "Moved Permanently";
    case 302:
      return "Found";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 413:
      return "Payload Too Large";
    case 500:
      return "Internal Server Error";
    case 501:
      return "Not Implemented";
    default:
      return "OK";
  }
}

static void lower_ascii(uint8_t *p, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    if (p[i] >= 'A' && p[i] <= 'Z') p[i] = (uint8_t)(p[i] - 'A' + 'a');
  }
}

typedef struct {
  uint8_t *name;
  uint32_t name_len;
  uint8_t *val;
  uint32_t val_len;
} hdr;

static int parse_u64_dec(const uint8_t *p, uint32_t n, uint64_t *out) {
  if (!p || n == 0 || !out) return 0;
  uint64_t v = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (p[i] < '0' || p[i] > '9') return 0;
    uint64_t nv = v * 10ull + (uint64_t)(p[i] - '0');
    if (nv < v) return 0;
    v = nv;
  }
  *out = v;
  return 1;
}

static int starts_with_nocase_bytes(const uint8_t *p, uint32_t n, const char *lit) {
  if (!p || !lit) return 0;
  size_t ln = strlen(lit);
  if ((size_t)n < ln) return 0;
  for (size_t i = 0; i < ln; i++) {
    if (tolower((unsigned char)p[i]) != tolower((unsigned char)lit[i])) return 0;
  }
  return 1;
}

static int find_nocase_substr_bytes(const uint8_t *p, uint32_t n, const char *lit, uint32_t *out_off) {
  if (!p || !lit || !out_off) return 0;
  size_t ln = strlen(lit);
  if (ln == 0 || (size_t)n < ln) return 0;
  for (uint32_t i = 0; i + (uint32_t)ln <= n; i++) {
    int ok = 1;
    for (size_t j = 0; j < ln; j++) {
      if (tolower((unsigned char)p[i + (uint32_t)j]) != tolower((unsigned char)lit[j])) {
        ok = 0;
        break;
      }
    }
    if (ok) {
      *out_off = i;
      return 1;
    }
  }
  return 0;
}

static int eq_nocase_bytes(const uint8_t *p, uint32_t n, const char *lit) {
  if (!p || !lit) return 0;
  size_t ln = strlen(lit);
  if ((size_t)n != ln) return 0;
  for (uint32_t i = 0; i < n; i++) {
    if (tolower((unsigned char)p[i]) != tolower((unsigned char)lit[i])) return 0;
  }
  return 1;
}

static int contains_nocase_token(const uint8_t *p, uint32_t n, const char *lit) {
  if (!p || !lit) return 0;
  size_t ln = strlen(lit);
  if (ln == 0) return 0;

  // Parse a comma-separated list of tokens. Tokens end at comma, whitespace, or ';'.
  // This avoids false positives like "unchunked" matching "chunked".
  uint32_t i = 0;
  while (i < n) {
    while (i < n && (p[i] == ',' || p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n')) i++;
    if (i >= n) break;
    uint32_t start = i;
    while (i < n && p[i] != ',' && p[i] != ' ' && p[i] != '\t' && p[i] != '\r' && p[i] != '\n' && p[i] != ';') i++;
    uint32_t tok_len = i - start;
    if (tok_len == (uint32_t)ln) {
      int ok = 1;
      for (size_t j = 0; j < ln; j++) {
        if (tolower((unsigned char)p[start + (uint32_t)j]) != tolower((unsigned char)lit[j])) {
          ok = 0;
          break;
        }
      }
      if (ok) return 1;
    }
    while (i < n && p[i] != ',') i++;
  }
  return 0;
}

static uint8_t *find_crlf(uint8_t *p, uint8_t *end) {
  for (uint8_t *q = p; q + 1 < end; q++) {
    if (q[0] == '\r' && q[1] == '\n') return q;
  }
  return NULL;
}

static int build_ev_request(zi_http_cap_ctx *c, uint32_t listener_id, int conn_fd, const struct sockaddr_storage *peer,
                            socklen_t peer_len) {
  if (!c) return 0;

  uint8_t *buf = NULL;
  size_t cap = 4096;
  size_t len = 0;
  buf = (uint8_t *)malloc(cap);
  if (!buf) return 0;

  const uint8_t hdr_end_seq[] = {'\r', '\n', '\r', '\n'};
  int hdr_end_off = -1;

  while (1) {
    if (len >= (size_t)c->lim.max_header_bytes + 4u) {
      free(buf);
      return 0;
    }
    if (cap - len < 2048) {
      size_t ncap = cap * 2u;
      if (ncap < cap) {
        free(buf);
        return 0;
      }
      if (ncap > (size_t)c->lim.max_header_bytes + 4096u) ncap = (size_t)c->lim.max_header_bytes + 4096u;
      uint8_t *nb = (uint8_t *)realloc(buf, ncap);
      if (!nb) {
        free(buf);
        return 0;
      }
      buf = nb;
      cap = ncap;
    }

    ssize_t n = recv(conn_fd, buf + len, cap - len, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      free(buf);
      return 0;
    }
    if (n == 0) {
      free(buf);
      return 0;
    }
    len += (size_t)n;

    hdr_end_off = find_seq(buf, len, hdr_end_seq, sizeof(hdr_end_seq));
    if (hdr_end_off >= 0) break;
  }

  size_t header_bytes = (size_t)hdr_end_off + 4u;
  if (header_bytes > (size_t)c->lim.max_header_bytes + 4u) {
    free(buf);
    return 0;
  }

  int req_line_end = find_seq(buf, header_bytes, (const uint8_t *)"\r\n", 2);
  if (req_line_end < 0) {
    free(buf);
    return 0;
  }
  if ((uint32_t)req_line_end > c->lim.max_req_line_bytes) {
    free(buf);
    return 0;
  }

  uint8_t *line = buf;
  uint32_t line_len = (uint32_t)req_line_end;
  uint32_t sp1 = 0xFFFFFFFFu, sp2 = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < line_len; i++) {
    if (line[i] == ' ') {
      if (sp1 == 0xFFFFFFFFu) sp1 = i;
      else {
        sp2 = i;
        break;
      }
    }
  }
  if (sp1 == 0xFFFFFFFFu || sp2 == 0xFFFFFFFFu) {
    free(buf);
    return 0;
  }

  uint8_t *method = line;
  uint32_t method_len = sp1;
  uint8_t *path = line + sp1 + 1u;
  uint32_t path_len = sp2 - (sp1 + 1u);
  uint8_t *ver = line + sp2 + 1u;
  uint32_t ver_len = line_len - (sp2 + 1u);
  if (method_len == 0 || path_len == 0 || ver_len == 0) {
    free(buf);
    return 0;
  }
  if (!(ver_len == 8 && memcmp(ver, "HTTP/1.1", 8) == 0)) {
    free(buf);
    return 0;
  }
  if (path[0] != '/') {
    free(buf);
    return 0;
  }
  if (contains_ctl_or_lf(method, method_len) || contains_ctl_or_lf(path, path_len)) {
    free(buf);
    return 0;
  }

  uint32_t maxh = c->lim.max_header_count;
  if (maxh > 4096u) maxh = 4096u;
  hdr *headers = (hdr *)calloc((size_t)maxh, sizeof(hdr));
  if (!headers) {
    free(buf);
    return 0;
  }

  uint32_t hcount = 0;
  uint8_t *p = buf + (uint32_t)req_line_end + 2u;
  // Parse up to (and including) the "\r\n\r\n" terminator; the loop already
  // treats the empty line as a stop marker.
  uint8_t *pend = buf + (uint32_t)header_bytes;
  uint8_t *authority = NULL;
  uint32_t authority_len = 0;
  uint64_t content_len = 0;
  int has_content_len = 0;
  int has_chunked = 0;
  uint8_t *content_type = NULL;
  uint32_t content_type_len = 0;

  while (p < pend) {
    uint8_t *eol = find_crlf(p, pend);
    if (!eol) break;
    if (eol == p) {
      p += 2;
      continue;
    }
    uint8_t *colon = (uint8_t *)memchr(p, ':', (size_t)(eol - p));
    if (!colon) {
      free(headers);
      free(buf);
      return 0;
    }

    uint8_t *name = p;
    uint32_t name_len = (uint32_t)(colon - p);
    uint8_t *val = colon + 1;
    uint8_t *val_end = eol;
    while (val < val_end && (*val == ' ' || *val == '\t')) val++;
    while (val_end > val && (val_end[-1] == ' ' || val_end[-1] == '\t')) val_end--;
    uint32_t val_len = (uint32_t)(val_end - val);

    if (name_len == 0) {
      free(headers);
      free(buf);
      return 0;
    }
    if (contains_ctl_or_lf(name, name_len) || contains_ctl_or_lf(val, val_len)) {
      free(headers);
      free(buf);
      return 0;
    }
    lower_ascii(name, name_len);

    if (hcount >= maxh) {
      free(headers);
      free(buf);
      return 0;
    }
    headers[hcount].name = name;
    headers[hcount].name_len = name_len;
    headers[hcount].val = val;
    headers[hcount].val_len = val_len;
    hcount++;

    if (authority == NULL && name_len == 4 && memcmp(name, "host", 4) == 0) {
      authority = val;
      authority_len = val_len;
    }
    if (name_len == 14 && memcmp(name, "content-length", 14) == 0) {
      uint64_t v = 0;
      if (parse_u64_dec(val, val_len, &v)) {
        content_len = v;
        has_content_len = 1;
      }
    }
    if (name_len == 17 && memcmp(name, "transfer-encoding", 17) == 0) {
      if (contains_nocase_token(val, val_len, "chunked")) has_chunked = 1;
    }

    if (name_len == 12 && memcmp(name, "content-type", 12) == 0) {
      content_type = val;
      content_type_len = val_len;
    }

    p = eol + 2;
  }

  uint32_t body_kind = ZI_HTTP_BODY_NONE;
  uint8_t *body_inline = NULL;
  uint32_t body_inline_len = 0;
  zi_handle_t body_handle = 0;
  zi_http_body_stream *bs = NULL;
  zi_http_chunked_stream *cbs = NULL;
  uint64_t body_rem = 0;

  size_t already = len - header_bytes;

  // Body handling:
  // - If Transfer-Encoding: chunked is present, ignore Content-Length and expose a decoded STREAM body.
  // - Otherwise, use Content-Length to decide NONE vs INLINE vs STREAM.
  if (has_chunked) {
    body_kind = ZI_HTTP_BODY_STREAM;
    uint32_t pre_len = 0;
    if (already > 0) {
      if (already > (size_t)UINT32_MAX) pre_len = UINT32_MAX;
      else pre_len = (uint32_t)already;
    }
    cbs = chunked_stream_new(conn_fd, buf + header_bytes, pre_len, c->lim.max_header_bytes, 0);
    if (!cbs) {
      free(headers);
      free(buf);
      return 0;
    }
    body_handle = zi_handle25_alloc_with_poll(&CHUNKED_BODY_OPS, &CHUNKED_BODY_POLL_OPS, cbs, ZI_H_READABLE | ZI_H_ENDABLE);
    if (body_handle < 3) {
      chunked_end(cbs);
      free(headers);
      free(buf);
      return 0;
    }
    body_rem = 0;
  } else {
    if (!has_content_len) content_len = 0;
    body_rem = content_len;
    if (content_len == 0) {
      body_kind = ZI_HTTP_BODY_NONE;
    } else if (content_len <= (uint64_t)c->lim.max_inline_body_bytes) {
    body_kind = ZI_HTTP_BODY_INLINE;
    body_inline_len = (uint32_t)content_len;
    body_inline = (uint8_t *)malloc((size_t)body_inline_len);
    if (!body_inline) {
      free(headers);
      free(buf);
      return 0;
    }
    size_t take = already;
    if (take > (size_t)body_inline_len) take = (size_t)body_inline_len;
    if (take) memcpy(body_inline, buf + header_bytes, take);
    size_t off = take;
    while (off < (size_t)body_inline_len) {
      ssize_t rn = recv(conn_fd, body_inline + off, (size_t)body_inline_len - off, 0);
      if (rn < 0) {
        if (errno == EINTR) continue;
        free(body_inline);
        free(headers);
        free(buf);
        return 0;
      }
      if (rn == 0) {
        free(body_inline);
        free(headers);
        free(buf);
        return 0;
      }
      off += (size_t)rn;
    }
    body_rem = 0;
    } else {
    body_kind = ZI_HTTP_BODY_STREAM;
    uint32_t pre_len = 0;
    if (already > 0) {
      if ((uint64_t)already >= content_len) pre_len = (uint32_t)content_len;
      else pre_len = (uint32_t)already;
    }
    uint64_t rem = content_len;
    if ((uint64_t)pre_len > rem) pre_len = (uint32_t)rem;
    rem -= (uint64_t)pre_len;
    bs = body_stream_new(conn_fd, rem, buf + header_bytes, pre_len, 0);
    if (!bs) {
      free(headers);
      free(buf);
      return 0;
    }
    body_rem = rem;
    }
  }

  zi_http_req *r = alloc_req_slot(c);
  if (!r) {
    if (body_inline) free(body_inline);
    if (body_handle >= 3) (void)zi_end(body_handle);
    free(headers);
    free(buf);
    return 0;
  }

  uint32_t rid = c->next_rid++;
  if (rid == 0) rid = c->next_rid++;
  r->in_use = 1;
  r->rid = rid;
  r->listener_id = listener_id;
  r->fd = conn_fd;
  r->body_remaining = body_rem;
  r->body_handle = 0;
  r->body_stream = NULL;
  r->is_multipart = 0;
  r->mp_boundary = NULL;
  r->mp_boundary_len = 0;
  r->mp = NULL;

  if (!has_chunked && body_kind == ZI_HTTP_BODY_STREAM) r->body_stream = bs;

  // If this is multipart/form-data, advertise MULTIPART and store boundary.
  // NOTE: For chunked bodies we do not advertise MULTIPART.
  if (!has_chunked && (body_kind == ZI_HTTP_BODY_STREAM || body_kind == ZI_HTTP_BODY_INLINE) && content_type && content_type_len > 0) {
    // conservative: require prefix "multipart/form-data" and a boundary parameter
    if (starts_with_nocase_bytes(content_type, content_type_len, "multipart/form-data")) {
      uint32_t boff = 0;
      if (find_nocase_substr_bytes(content_type, content_type_len, "boundary=", &boff)) {
        uint32_t pos = boff + 9u;
        if (pos < content_type_len) {
          char quote = 0;
          if (content_type[pos] == '"' || content_type[pos] == '\'') {
            quote = (char)content_type[pos];
            pos++;
          }
          uint32_t start = pos;
          while (pos < content_type_len) {
            uint8_t ch = content_type[pos];
            if (quote) {
              if (ch == (uint8_t)quote) break;
            } else {
              if (ch == ';' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') break;
            }
            pos++;
          }
          uint32_t bl = pos - start;
          if (bl > 0 && bl <= 200u) {
            r->mp_boundary = (uint8_t *)malloc((size_t)bl);
            if (r->mp_boundary) {
              memcpy(r->mp_boundary, content_type + start, bl);
              r->mp_boundary_len = bl;
              r->is_multipart = 1;

              if (body_kind == ZI_HTTP_BODY_INLINE) {
                // Convert inline body to a prebuffer-only body stream for the multipart iterator.
                zi_http_body_stream *mbs = body_stream_new(-1, 0, body_inline, body_inline_len, 0);
                if (mbs) {
                  if (body_inline) free(body_inline);
                  body_inline = NULL;
                  body_inline_len = 0;
                  r->body_stream = mbs;
                  bs = mbs;
                  body_kind = ZI_HTTP_BODY_MULTIPART;
                }
              } else {
                body_kind = ZI_HTTP_BODY_MULTIPART;
              }
            }
          }
        }
      }
    }
  }

  // If this is a normal streamed body, expose it as a readable handle.
  // For MULTIPART we intentionally do not expose a raw body handle (guests must use MULTIPART_*).
  if (body_kind == ZI_HTTP_BODY_STREAM) {
    if (body_handle >= 3) {
      // Chunked: body handle already allocated.
      r->body_handle = body_handle;
    } else {
      if (!r->body_stream) {
        free(headers);
        free(buf);
        close_req(r);
        return 0;
      }
      body_handle = zi_handle25_alloc_with_poll(&BODY_OPS, &BODY_POLL_OPS, r->body_stream, ZI_H_READABLE | ZI_H_ENDABLE);
      if (body_handle < 3) {
        free(headers);
        free(buf);
        close_req(r);
        return 0;
      }
      r->body_handle = body_handle;
    }
  } else if (body_kind == ZI_HTTP_BODY_MULTIPART) {
    // No raw body handle.
    body_handle = 0;
    r->body_handle = 0;
  } else {
    r->body_handle = body_handle;
  }

  const uint8_t scheme[] = {'h', 't', 't', 'p'};
  uint32_t scheme_len = 4;

  uint8_t remote_addr[16];
  memset(remote_addr, 0, sizeof(remote_addr));
  uint32_t remote_port = 0;
  if (peer && peer_len > 0) {
    if (peer->ss_family == AF_INET) {
      const struct sockaddr_in *sa = (const struct sockaddr_in *)peer;
      remote_addr[10] = 0xFF;
      remote_addr[11] = 0xFF;
      memcpy(remote_addr + 12, &sa->sin_addr, 4);
      remote_port = (uint32_t)ntohs(sa->sin_port);
    } else if (peer->ss_family == AF_INET6) {
      const struct sockaddr_in6 *sa6 = (const struct sockaddr_in6 *)peer;
      memcpy(remote_addr, &sa6->sin6_addr, 16);
      remote_port = (uint32_t)ntohs(sa6->sin6_port);
    }
  }

  uint64_t payload_len = 0;
  payload_len += 4; // listener_id
  payload_len += 4; // flags (reserved; 0)
  payload_len += 4 + method_len;
  payload_len += 4 + path_len;
  payload_len += 4 + scheme_len;
  payload_len += 4 + authority_len;
  payload_len += 16;
  payload_len += 4;
  payload_len += 4;
  for (uint32_t i = 0; i < hcount; i++) {
    payload_len += 4 + headers[i].name_len;
    payload_len += 4 + headers[i].val_len;
  }
  payload_len += 4;
  if (body_kind == ZI_HTTP_BODY_INLINE) {
    payload_len += 4 + body_inline_len;
  } else if (body_kind == ZI_HTTP_BODY_STREAM || body_kind == ZI_HTTP_BODY_MULTIPART) {
    payload_len += 4;
  }
  if (payload_len > 16u * 1024u * 1024u) {
    if (body_inline) free(body_inline);
    free(headers);
    free(buf);
    close_req(r);
    return 0;
  }

  uint8_t *pl = (uint8_t *)malloc((size_t)payload_len);
  if (!pl) {
    if (body_inline) free(body_inline);
    free(headers);
    free(buf);
    close_req(r);
    return 0;
  }

  uint32_t off = 0;
  zi_zcl1_write_u32(pl + off, listener_id);
  off += 4;
  zi_zcl1_write_u32(pl + off, 0u);
  off += 4;
  zi_zcl1_write_u32(pl + off, method_len);
  off += 4;
  memcpy(pl + off, method, method_len);
  off += method_len;
  zi_zcl1_write_u32(pl + off, path_len);
  off += 4;
  memcpy(pl + off, path, path_len);
  off += path_len;
  zi_zcl1_write_u32(pl + off, scheme_len);
  off += 4;
  memcpy(pl + off, scheme, scheme_len);
  off += scheme_len;
  zi_zcl1_write_u32(pl + off, authority_len);
  off += 4;
  if (authority_len) memcpy(pl + off, authority, authority_len);
  off += authority_len;
  memcpy(pl + off, remote_addr, 16);
  off += 16;
  zi_zcl1_write_u32(pl + off, remote_port);
  off += 4;
  zi_zcl1_write_u32(pl + off, hcount);
  off += 4;
  for (uint32_t i = 0; i < hcount; i++) {
    zi_zcl1_write_u32(pl + off, headers[i].name_len);
    off += 4;
    memcpy(pl + off, headers[i].name, headers[i].name_len);
    off += headers[i].name_len;
    zi_zcl1_write_u32(pl + off, headers[i].val_len);
    off += 4;
    if (headers[i].val_len) memcpy(pl + off, headers[i].val, headers[i].val_len);
    off += headers[i].val_len;
  }
  zi_zcl1_write_u32(pl + off, body_kind);
  off += 4;
  if (body_kind == ZI_HTTP_BODY_INLINE) {
    zi_zcl1_write_u32(pl + off, body_inline_len);
    off += 4;
    memcpy(pl + off, body_inline, body_inline_len);
    off += body_inline_len;
  } else if (body_kind == ZI_HTTP_BODY_STREAM || body_kind == ZI_HTTP_BODY_MULTIPART) {
    zi_zcl1_write_u32(pl + off, (uint32_t)(int32_t)body_handle);
    off += 4;
  }

  int ok = wait_set_out_frame_ok(c, (uint16_t)ZI_HTTP_EV_REQUEST, rid, pl, (uint32_t)payload_len);

  // After we've fully parsed the request headers (and any inline body), switch
  // the connection to nonblocking so stream handles can return ZI_E_AGAIN.
  set_nonblocking_best_effort(conn_fd);

  free(pl);
  free(body_inline);
  free(headers);
  free(buf);
  return ok;
}

static int pump_one_request_event(zi_http_cap_ctx *c) {
  if (!c) return 0;
  int maxfd = -1;
  fd_set rfds;
  FD_ZERO(&rfds);

  int have_listener = 0;
  for (uint32_t i = 0; i < ZI_HTTP_MAX_LISTENERS; i++) {
    if (!c->listeners[i].in_use) continue;
    have_listener = 1;
    FD_SET(c->listeners[i].fd, &rfds);
    if (c->listeners[i].fd > maxfd) maxfd = c->listeners[i].fd;
  }
  if (!have_listener) return 0;

  int rc;
  do {
    rc = select(maxfd + 1, &rfds, NULL, NULL, NULL);
  } while (rc < 0 && errno == EINTR);
  if (rc <= 0) return 0;

  for (uint32_t i = 0; i < ZI_HTTP_MAX_LISTENERS; i++) {
    if (!c->listeners[i].in_use) continue;
    int lfd = c->listeners[i].fd;
    if (!FD_ISSET(lfd, &rfds)) continue;

    struct sockaddr_storage peer;
    socklen_t peer_len = (socklen_t)sizeof(peer);
    int conn = accept(lfd, (struct sockaddr *)&peer, &peer_len);
    if (conn < 0) continue;

    int ok = build_ev_request(c, c->listeners[i].id, conn, &peer, peer_len);
    if (!ok) {
      send_http_error_best_effort(conn, 400u, "Bad Request", "bad request\n");
      (void)close(conn);
      continue;
    }
    return 1;
  }
  return 0;
}

static void *http_server_thread_main(void *arg) {
  zi_http_cap_ctx *c = (zi_http_cap_ctx *)arg;
  if (!c) return NULL;

  for (;;) {
    pthread_mutex_lock(&c->mu);
    while (!c->closed) {
      int have = 0;
      for (uint32_t i = 0; i < ZI_HTTP_MAX_LISTENERS; i++) {
        if (c->listeners[i].in_use && c->listeners[i].fd >= 0) {
          have = 1;
          break;
        }
      }
      if (have) break;
      pthread_cond_wait(&c->cv, &c->mu);
    }
    if (c->closed) {
      pthread_mutex_unlock(&c->mu);
      break;
    }

    struct pollfd pfds[ZI_HTTP_MAX_LISTENERS];
    uint32_t lids[ZI_HTTP_MAX_LISTENERS];
    nfds_t nfds = 0;
    for (uint32_t i = 0; i < ZI_HTTP_MAX_LISTENERS; i++) {
      if (!c->listeners[i].in_use) continue;
      if (c->listeners[i].fd < 0) continue;
      pfds[nfds].fd = c->listeners[i].fd;
      pfds[nfds].events = POLLIN;
      pfds[nfds].revents = 0;
      lids[nfds] = c->listeners[i].id;
      nfds++;
    }
    pthread_mutex_unlock(&c->mu);

    if (nfds == 0) continue;
    int rc = poll(pfds, nfds, 250);
    if (rc <= 0) continue;

    for (nfds_t i = 0; i < nfds; i++) {
      if (!(pfds[i].revents & POLLIN)) continue;

      struct sockaddr_storage peer;
      socklen_t peer_len = (socklen_t)sizeof(peer);
      int conn = accept(pfds[i].fd, (struct sockaddr *)&peer, &peer_len);
      if (conn < 0) continue;

      // build_ev_request emits EV_REQUEST and blocks only as needed to read the
      // request and/or wait for the guest to drain the previous frame.
      int ok = build_ev_request(c, lids[i], conn, &peer, peer_len);
      if (!ok) {
        send_http_error_best_effort(conn, 400u, "Bad Request", "bad request\n");
        (void)close(conn);
      }
    }
  }

  return NULL;
}

static int parse_listen_req(const uint8_t *p, uint32_t n, uint32_t *port, uint32_t *flags, const uint8_t **host,
                            uint32_t *host_len) {
  if (!p || n < 12u || !port || !flags || !host || !host_len) return 0;
  *port = zi_zcl1_read_u32(p + 0);
  *flags = zi_zcl1_read_u32(p + 4);
  *host_len = zi_zcl1_read_u32(p + 8);
  if (12u + *host_len != n) return 0;
  *host = p + 12;
  return 1;
}

static int dispatch_listen(zi_http_cap_ctx *c, uint32_t rid, const uint8_t *p, uint32_t n) {
  if (!c) return 0;
  uint32_t port = 0, flags = 0, host_len = 0;
  const uint8_t *host = NULL;
  if (!parse_listen_req(p, n, &port, &flags, &host, &host_len)) {
    return set_out_frame_err(c, ZI_HTTP_OP_LISTEN, rid, "t_http_invalid", "malformed LISTEN payload");
  }
  if (flags != 0) {
    return set_out_frame_err(c, ZI_HTTP_OP_LISTEN, rid, "t_http_invalid", "LISTEN flags must be 0");
  }
  if (port > 65535u) {
    return set_out_frame_err(c, ZI_HTTP_OP_LISTEN, rid, "t_http_invalid", "invalid port");
  }

  char host_str[256];
  host_str[0] = '\0';
  if (host_len > 0) {
    if (host_len >= sizeof(host_str)) {
      return set_out_frame_err(c, ZI_HTTP_OP_LISTEN, rid, "t_http_invalid", "bind_host too long");
    }
    memcpy(host_str, host, host_len);
    host_str[host_len] = '\0';
  }

  const char *allow = getenv("ZI_NET_LISTEN_ALLOW");
  if (!listen_allowlist_allows(allow, host_len ? host_str : NULL, port)) {
    return set_out_frame_err(c, ZI_HTTP_OP_LISTEN, rid, "t_http_denied", "listener bind denied by policy");
  }

  zi_http_listener *slot = NULL;
  if (!alloc_listener_slot(c, &slot) || !slot) {
    return set_out_frame_err(c, ZI_HTTP_OP_LISTEN, rid, "t_http_oom", "no listener slots");
  }

  const char *bind_host = (host_len == 0) ? "127.0.0.1" : host_str;
  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%u", port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;

  struct addrinfo *res = NULL;
  int ga = getaddrinfo(bind_host, port == 0 ? "0" : port_str, &hints, &res);
  if (ga != 0 || !res) {
    if (res) freeaddrinfo(res);
    return set_out_frame_err(c, ZI_HTTP_OP_LISTEN, rid, "t_http_io", "getaddrinfo failed");
  }

  int fd = -1;
  struct addrinfo *ai;
  for (ai = res; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#if defined(__APPLE__)
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
    if (bind(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
      (void)close(fd);
      fd = -1;
      continue;
    }
    if (listen(fd, 128) != 0) {
      (void)close(fd);
      fd = -1;
      continue;
    }
    break;
  }
  freeaddrinfo(res);
  if (fd < 0) {
    return set_out_frame_err(c, ZI_HTTP_OP_LISTEN, rid, "t_http_io", "bind/listen failed");
  }

  struct sockaddr_storage ss;
  socklen_t slen = (socklen_t)sizeof(ss);
  memset(&ss, 0, sizeof(ss));
  if (getsockname(fd, (struct sockaddr *)&ss, &slen) != 0) {
    (void)close(fd);
    return set_out_frame_err(c, ZI_HTTP_OP_LISTEN, rid, "t_http_io", "getsockname failed");
  }

  uint8_t bound_addr[16];
  memset(bound_addr, 0, sizeof(bound_addr));
  uint32_t bound_port = 0;
  if (ss.ss_family == AF_INET) {
    const struct sockaddr_in *sa = (const struct sockaddr_in *)&ss;
    bound_addr[10] = 0xFF;
    bound_addr[11] = 0xFF;
    memcpy(bound_addr + 12, &sa->sin_addr, 4);
    bound_port = (uint32_t)ntohs(sa->sin_port);
  } else if (ss.ss_family == AF_INET6) {
    const struct sockaddr_in6 *sa6 = (const struct sockaddr_in6 *)&ss;
    memcpy(bound_addr, &sa6->sin6_addr, 16);
    bound_port = (uint32_t)ntohs(sa6->sin6_port);
  }

  pthread_mutex_lock(&c->mu);
  uint32_t lid = ++c->next_listener_id;
  if (lid == 0) lid = ++c->next_listener_id;
  slot->in_use = 1;
  slot->id = lid;
  slot->fd = fd;
  slot->bound_port = (uint16_t)bound_port;
  memcpy(slot->bound_addr, bound_addr, 16);
  pthread_cond_broadcast(&c->cv);
  pthread_mutex_unlock(&c->mu);

  uint8_t payload[24];
  zi_zcl1_write_u32(payload + 0, lid);
  zi_zcl1_write_u32(payload + 4, bound_port);
  memcpy(payload + 8, bound_addr, 16);
  return set_out_frame_ok(c, ZI_HTTP_OP_LISTEN, rid, payload, (uint32_t)sizeof(payload));
}

static int dispatch_close_listener(zi_http_cap_ctx *c, uint32_t rid, const uint8_t *p, uint32_t n) {
  if (!c) return 0;
  if (!p || n != 4u) {
    return set_out_frame_err(c, ZI_HTTP_OP_CLOSE_LISTENER, rid, "t_http_invalid", "malformed CLOSE_LISTENER payload");
  }
  uint32_t lid = zi_zcl1_read_u32(p + 0);

  pthread_mutex_lock(&c->mu);
  zi_http_listener *l = listener_by_id(c, lid);
  if (!l) {
    pthread_mutex_unlock(&c->mu);
    return set_out_frame_err(c, ZI_HTTP_OP_CLOSE_LISTENER, rid, "t_http_noent", "unknown listener_id");
  }
  if (l->fd >= 0) {
    (void)close(l->fd);
    l->fd = -1;
  }
  memset(l, 0, sizeof(*l));

  pthread_cond_broadcast(&c->cv);
  pthread_mutex_unlock(&c->mu);
  return set_out_frame_ok(c, ZI_HTTP_OP_CLOSE_LISTENER, rid, NULL, 0);
}

static int dispatch_respond_start(zi_http_cap_ctx *c, uint32_t rid, const uint8_t *p, uint32_t n) {
  if (!c) return 0;
  zi_http_req *r = req_by_rid(c, rid);
  if (!r) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_START, rid, "t_http_noent", "unknown request id");
  }
  if (r->resp_body_handle >= 3) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_START, rid, "t_http_invalid", "response already streaming");
  }
  if (!p || n < 12u) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_START, rid, "t_http_invalid", "malformed RESPOND_START payload");
  }

  uint32_t flags = zi_zcl1_read_u32(p + 4);
  uint32_t hcount = zi_zcl1_read_u32(p + 8);
  if (flags != 0) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_START, rid, "t_http_invalid", "RESPOND_START flags must be 0");
  }
  if (hcount > c->lim.max_header_count) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_START, rid, "t_http_invalid", "too many headers");
  }

  uint32_t off = 12;
  for (uint32_t i = 0; i < hcount; i++) {
    if (off + 4u > n) return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_START, rid, "t_http_invalid", "bad headers");
    uint32_t name_len = zi_zcl1_read_u32(p + off);
    off += 4;
    if (off + name_len + 4u > n) return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_START, rid, "t_http_invalid", "bad headers");
    const uint8_t *name = p + off;
    off += name_len;
    uint32_t val_len = zi_zcl1_read_u32(p + off);
    off += 4;
    if (off + val_len > n) return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_START, rid, "t_http_invalid", "bad headers");
    const uint8_t *val = p + off;
    off += val_len;
    if (contains_ctl_or_lf(name, name_len) || contains_ctl_or_lf(val, val_len)) {
      return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_START, rid, "t_http_invalid", "header contains control chars");
    }
  }
  if (off != n) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_START, rid, "t_http_invalid", "trailing bytes");
  }

  // v1: RESPOND_START is an optional claim/validation step.
  // We intentionally do not emit any HTTP bytes here.
  return set_out_frame_ok(c, ZI_HTTP_OP_RESPOND_START, rid, NULL, 0);
}

static int dispatch_respond_inline(zi_http_cap_ctx *c, uint32_t rid, const uint8_t *p, uint32_t n) {
  if (!c) return 0;
  zi_http_req *r = req_by_rid(c, rid);
  if (!r) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_INLINE, rid, "t_http_noent", "unknown request id");
  }
  if (r->resp_body_handle >= 3) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_INLINE, rid, "t_http_invalid", "response already streaming");
  }
  if (!p || n < 16u) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_INLINE, rid, "t_http_invalid", "malformed RESPOND_INLINE payload");
  }
  uint32_t status = zi_zcl1_read_u32(p + 0);
  uint32_t flags = zi_zcl1_read_u32(p + 4);
  uint32_t hcount = zi_zcl1_read_u32(p + 8);
  if (flags != 0) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_INLINE, rid, "t_http_invalid", "RESPOND_INLINE flags must be 0");
  }
  if (hcount > c->lim.max_header_count) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_INLINE, rid, "t_http_invalid", "too many headers");
  }

  uint32_t off = 12;
  uint64_t hdr_bytes = 0;
  for (uint32_t i = 0; i < hcount; i++) {
    if (off + 4u > n) return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_INLINE, rid, "t_http_invalid", "bad headers");
    uint32_t name_len = zi_zcl1_read_u32(p + off);
    off += 4;
    if (off + name_len + 4u > n) return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_INLINE, rid, "t_http_invalid", "bad headers");
    const uint8_t *name = p + off;
    off += name_len;
    uint32_t val_len = zi_zcl1_read_u32(p + off);
    off += 4;
    if (off + val_len > n) return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_INLINE, rid, "t_http_invalid", "bad headers");
    const uint8_t *val = p + off;
    off += val_len;
    if (contains_ctl_or_lf(name, name_len) || contains_ctl_or_lf(val, val_len)) {
      return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_INLINE, rid, "t_http_invalid", "header contains control chars");
    }
    hdr_bytes += (uint64_t)name_len + 2ull + (uint64_t)val_len + 2ull;
  }
  if (off + 4u > n) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_INLINE, rid, "t_http_invalid", "missing body_len");
  }
  uint32_t body_len = zi_zcl1_read_u32(p + off);
  off += 4;
  if (off + body_len != n) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_INLINE, rid, "t_http_invalid", "bad body length");
  }
  if (body_len > c->lim.max_inline_body_bytes) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_INLINE, rid, "t_http_invalid", "inline body too large");
  }

  const char *reason = reason_phrase(status);
  char status_line[128];
  int sl = snprintf(status_line, sizeof(status_line), "HTTP/1.1 %u %s\r\n", status, reason);
  if (sl <= 0) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_INLINE, rid, "t_http_internal", "snprintf failed");
  }

  char cl_header[64];
  int cln = snprintf(cl_header, sizeof(cl_header), "content-length: %u\r\n", body_len);
  if (cln <= 0) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_INLINE, rid, "t_http_internal", "snprintf failed");
  }

  static const char conn_close[] = "connection: close\r\n";
  uint64_t total = 0;
  total += (uint64_t)sl;
  total += hdr_bytes;
  total += (uint64_t)cln;
  total += (uint64_t)sizeof(conn_close) - 1u;
  total += 2;
  total += (uint64_t)body_len;
  if (total > 32ull * 1024ull * 1024ull) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_INLINE, rid, "t_http_invalid", "response too large");
  }

  uint8_t *resp = (uint8_t *)malloc((size_t)total);
  if (!resp) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_INLINE, rid, "t_http_oom", "oom building response");
  }

  uint32_t woff = 0;
  memcpy(resp + woff, status_line, (size_t)sl);
  woff += (uint32_t)sl;
  off = 12;
  for (uint32_t i = 0; i < hcount; i++) {
    uint32_t name_len = zi_zcl1_read_u32(p + off);
    off += 4;
    const uint8_t *name = p + off;
    off += name_len;
    uint32_t val_len = zi_zcl1_read_u32(p + off);
    off += 4;
    const uint8_t *val = p + off;
    off += val_len;

    if (name_len == 14 && memcmp(name, "content-length", 14) == 0) continue;
    if (name_len == 10 && memcmp(name, "connection", 10) == 0) continue;

    memcpy(resp + woff, name, name_len);
    woff += name_len;
    resp[woff++] = ':';
    resp[woff++] = ' ';
    if (val_len) {
      memcpy(resp + woff, val, val_len);
      woff += val_len;
    }
    resp[woff++] = '\r';
    resp[woff++] = '\n';
  }
  memcpy(resp + woff, cl_header, (size_t)cln);
  woff += (uint32_t)cln;
  memcpy(resp + woff, conn_close, sizeof(conn_close) - 1u);
  woff += (uint32_t)(sizeof(conn_close) - 1u);
  resp[woff++] = '\r';
  resp[woff++] = '\n';
  if (body_len) {
    memcpy(resp + woff, p + off + 4u, body_len);
    woff += body_len;
  }

  int ok = send_all(r->fd, resp, (size_t)woff);
  free(resp);
  if (!ok) {
    close_req(r);
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_INLINE, rid, "t_http_io", "send failed");
  }
  close_req(r);
  return set_out_frame_ok(c, ZI_HTTP_OP_RESPOND_INLINE, rid, NULL, 0);
}

typedef struct {
  zi_http_cap_ctx *cap;
  uint32_t rid;
  int fd;
  int closed;
} zi_http_resp_stream;

static int resp_stream_poll_get_fd(void *ctx, int *out_fd) {
  zi_http_resp_stream *s = (zi_http_resp_stream *)ctx;
  if (!s) return 0;
  if (s->fd < 0) return 0;
  if (out_fd) *out_fd = s->fd;
  return 1;
}

static const zi_handle_poll_ops_v1 RESP_STREAM_POLL_OPS = {
    .get_fd = resp_stream_poll_get_fd,
};

static int32_t resp_stream_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  (void)ctx;
  (void)dst_ptr;
  (void)cap;
  return ZI_E_DENIED;
}

static int32_t resp_stream_write(void *ctx, zi_ptr_t src_ptr, zi_size32_t len) {
  zi_http_resp_stream *s = (zi_http_resp_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (s->closed) return ZI_E_CLOSED;
  if (len == 0) return 0;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_ro) return ZI_E_NOSYS;
  const uint8_t *src = NULL;
  if (!mem->map_ro(mem->ctx, src_ptr, len, &src) || !src) return ZI_E_BOUNDS;

  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
  ssize_t w = send(s->fd, src, (size_t)len, flags);
  if (w < 0) return map_errno_to_zi(errno);
  return (int32_t)w;
}

static int32_t resp_stream_end(void *ctx) {
  zi_http_resp_stream *s = (zi_http_resp_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (s->closed) {
    free(s);
    return 0;
  }
  s->closed = 1;

  int fd_to_close = -1;
  if (s->cap) {
    zi_http_req *r = req_by_rid(s->cap, s->rid);
    if (r) {
      r->resp_body_handle = 0;
      fd_to_close = r->fd;
      r->fd = -1;
      close_req_no_resp_handle(r);
      memset(r, 0, sizeof(*r));
      r->fd = -1;
    }
  }
  if (fd_to_close < 0) fd_to_close = s->fd;
  if (fd_to_close >= 0) {
    (void)shutdown(fd_to_close, SHUT_RDWR);
    (void)close(fd_to_close);
  }
  s->fd = -1;

  free(s);
  return 0;
}

static const zi_handle_ops_v1 RESP_STREAM_OPS = {
    .read = resp_stream_read,
    .write = resp_stream_write,
    .end = resp_stream_end,
};

static int dispatch_respond_stream(zi_http_cap_ctx *c, uint32_t rid, const uint8_t *p, uint32_t n) {
  if (!c) return 0;
  zi_http_req *r = req_by_rid(c, rid);
  if (!r) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_STREAM, rid, "t_http_noent", "unknown request id");
  }
  if (r->resp_body_handle >= 3) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_STREAM, rid, "t_http_invalid", "response already streaming");
  }
  if (!p || n < 12u) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_STREAM, rid, "t_http_invalid", "malformed RESPOND_STREAM payload");
  }

  uint32_t status = zi_zcl1_read_u32(p + 0);
  uint32_t flags = zi_zcl1_read_u32(p + 4);
  uint32_t hcount = zi_zcl1_read_u32(p + 8);
  if (flags != 0) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_STREAM, rid, "t_http_invalid", "RESPOND_STREAM flags must be 0");
  }
  if (hcount > c->lim.max_header_count) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_STREAM, rid, "t_http_invalid", "too many headers");
  }

  uint32_t off = 12;
  uint64_t hdr_bytes = 0;
  for (uint32_t i = 0; i < hcount; i++) {
    if (off + 4u > n) return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_STREAM, rid, "t_http_invalid", "bad headers");
    uint32_t name_len = zi_zcl1_read_u32(p + off);
    off += 4;
    if (off + name_len + 4u > n) return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_STREAM, rid, "t_http_invalid", "bad headers");
    const uint8_t *name = p + off;
    off += name_len;
    uint32_t val_len = zi_zcl1_read_u32(p + off);
    off += 4;
    if (off + val_len > n) return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_STREAM, rid, "t_http_invalid", "bad headers");
    const uint8_t *val = p + off;
    off += val_len;
    if (contains_ctl_or_lf(name, name_len) || contains_ctl_or_lf(val, val_len)) {
      return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_STREAM, rid, "t_http_invalid", "header contains control chars");
    }
    hdr_bytes += (uint64_t)name_len + 2ull + (uint64_t)val_len + 2ull;
  }
  if (off != n) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_STREAM, rid, "t_http_invalid", "trailing bytes");
  }

  const char *reason = reason_phrase(status);
  char status_line[128];
  int sl = snprintf(status_line, sizeof(status_line), "HTTP/1.1 %u %s\r\n", status, reason);
  if (sl <= 0) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_STREAM, rid, "t_http_internal", "snprintf failed");
  }

  static const char conn_close[] = "connection: close\r\n";
  uint64_t total = 0;
  total += (uint64_t)sl;
  total += hdr_bytes;
  total += (uint64_t)sizeof(conn_close) - 1u;
  total += 2;
  if (total > 1024ull * 1024ull) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_STREAM, rid, "t_http_invalid", "headers too large");
  }

  uint8_t *resp = (uint8_t *)malloc((size_t)total);
  if (!resp) {
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_STREAM, rid, "t_http_oom", "oom building response");
  }

  uint32_t woff = 0;
  memcpy(resp + woff, status_line, (size_t)sl);
  woff += (uint32_t)sl;
  off = 12;
  for (uint32_t i = 0; i < hcount; i++) {
    uint32_t name_len = zi_zcl1_read_u32(p + off);
    off += 4;
    const uint8_t *name = p + off;
    off += name_len;
    uint32_t val_len = zi_zcl1_read_u32(p + off);
    off += 4;
    const uint8_t *val = p + off;
    off += val_len;

    // For close-delimited streaming, forbid content-length/chunked/connection headers.
    if (name_len == 14 && memcmp(name, "content-length", 14) == 0) continue;
    if (name_len == 17 && memcmp(name, "transfer-encoding", 17) == 0) continue;
    if (name_len == 10 && memcmp(name, "connection", 10) == 0) continue;

    memcpy(resp + woff, name, name_len);
    woff += name_len;
    resp[woff++] = ':';
    resp[woff++] = ' ';
    if (val_len) {
      memcpy(resp + woff, val, val_len);
      woff += val_len;
    }
    resp[woff++] = '\r';
    resp[woff++] = '\n';
  }
  memcpy(resp + woff, conn_close, sizeof(conn_close) - 1u);
  woff += (uint32_t)(sizeof(conn_close) - 1u);
  resp[woff++] = '\r';
  resp[woff++] = '\n';

  int ok = send_all(r->fd, resp, (size_t)woff);
  free(resp);
  if (!ok) {
    close_req(r);
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_STREAM, rid, "t_http_io", "send failed");
  }

  zi_http_resp_stream *s = (zi_http_resp_stream *)calloc(1u, sizeof(*s));
  if (!s) {
    close_req(r);
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_STREAM, rid, "t_http_oom", "oom creating body handle");
  }
  s->cap = c;
  s->rid = rid;
  s->fd = r->fd;

  // Ensure writes don't block; caller can wait via sys/loop readiness.
  set_nonblocking_best_effort(s->fd);

  zi_handle_t body_h = zi_handle25_alloc_with_poll(&RESP_STREAM_OPS, &RESP_STREAM_POLL_OPS, s, ZI_H_WRITABLE | ZI_H_ENDABLE);
  if (body_h < 3) {
    free(s);
    close_req(r);
    return set_out_frame_err(c, ZI_HTTP_OP_RESPOND_STREAM, rid, "t_http_internal", "failed to alloc body handle");
  }
  r->resp_body_handle = body_h;

  uint8_t payload[4];
  zi_zcl1_write_u32(payload, (uint32_t)(int32_t)body_h);
  return set_out_frame_ok(c, ZI_HTTP_OP_RESPOND_STREAM, rid, payload, 4);
}

static int dispatch_fetch(zi_http_cap_ctx *c, uint32_t rid, const uint8_t *p, uint32_t n);

static int dispatch_multipart_begin(zi_http_cap_ctx *c, uint32_t rid, const uint8_t *p, uint32_t n) {
  (void)p;
  if (!c) return 0;
  if (n != 0) return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_BEGIN, rid, "t_http_invalid", "malformed MULTIPART_BEGIN payload");
  zi_http_req *r = req_by_rid(c, rid);
  if (!r) return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_BEGIN, rid, "t_http_noent", "unknown request id");
  if (!r->is_multipart || !r->mp_boundary || r->mp_boundary_len == 0) {
    return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_BEGIN, rid, "t_http_invalid", "request is not multipart");
  }
  if (!r->body_stream) {
    return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_BEGIN, rid, "t_http_internal", "missing body stream");
  }
  if (r->mp) return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_BEGIN, rid, "t_http_invalid", "multipart already begun");

  zi_http_multipart_iter *it = (zi_http_multipart_iter *)calloc(1, sizeof(*it));
  if (!it) return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_BEGIN, rid, "t_http_oom", "oom");
  it->rid = rid;
  it->bs = r->body_stream;
  it->max_parts = c->lim.mp_max_parts;
  it->max_header_bytes = c->lim.mp_max_header_bytes;
  it->max_header_count = c->lim.mp_max_header_count;
  it->max_name_bytes = c->lim.mp_max_name_bytes;
  it->max_filename_bytes = c->lim.mp_max_filename_bytes;
  it->parts_emitted = 0;

  it->boundary_len = r->mp_boundary_len;
  it->boundary = (uint8_t *)malloc((size_t)it->boundary_len);
  if (!it->boundary) {
    free(it);
    return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_BEGIN, rid, "t_http_oom", "oom");
  }
  memcpy(it->boundary, r->mp_boundary, it->boundary_len);

  it->delim_len = 4u + it->boundary_len;
  it->delim = (uint8_t *)malloc((size_t)it->delim_len);
  if (!it->delim) {
    free(it->boundary);
    free(it);
    return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_BEGIN, rid, "t_http_oom", "oom");
  }
  it->delim[0] = '\r';
  it->delim[1] = '\n';
  it->delim[2] = '-';
  it->delim[3] = '-';
  memcpy(it->delim + 4, it->boundary, it->boundary_len);

  it->started = 0;
  it->done = 0;
  it->part_open = 0;
  it->need_boundary = 0;
  r->mp = it;

  uint8_t payload[4];
  zi_zcl1_write_u32(payload, 0);
  return set_out_frame_ok(c, ZI_HTTP_OP_MULTIPART_BEGIN, rid, payload, 4);
}

static int dispatch_multipart_next(zi_http_cap_ctx *c, uint32_t rid, const uint8_t *p, uint32_t n) {
  (void)p;
  if (!c) return 0;
  if (n != 0) return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_NEXT, rid, "t_http_invalid", "malformed MULTIPART_NEXT payload");
  zi_http_req *r = req_by_rid(c, rid);
  if (!r || !r->mp) return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_NEXT, rid, "t_http_invalid", "multipart not begun");
  zi_http_multipart_iter *it = r->mp;

  if (it->done) {
    uint8_t payload[4];
    zi_zcl1_write_u32(payload, 1);
    return set_out_frame_ok(c, ZI_HTTP_OP_MULTIPART_NEXT, rid, payload, 4);
  }
  if (it->part_open) return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_NEXT, rid, "t_http_invalid", "previous part still open");
  if (it->max_parts && it->parts_emitted >= it->max_parts) {
    return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_NEXT, rid, "t_http_invalid", "too many multipart parts");
  }

  if (it->need_boundary) {
    if (!mp_consume_boundary(it, 0)) return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_NEXT, rid, "t_http_invalid", "bad boundary");
    it->need_boundary = 0;
  }
  if (!it->started) {
    if (!mp_consume_boundary(it, 1)) return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_NEXT, rid, "t_http_invalid", "bad first boundary");
    it->started = 1;
  }
  if (it->done) {
    uint8_t payload[4];
    zi_zcl1_write_u32(payload, 1);
    return set_out_frame_ok(c, ZI_HTTP_OP_MULTIPART_NEXT, rid, payload, 4);
  }

  zi_http_mp_hdr *hdrs = NULL;
  uint32_t hcnt = 0;
  char *name = NULL;
  uint32_t name_len = 0;
  char *filename = NULL;
  uint32_t filename_len = 0;
  char *ctype = NULL;
  uint32_t ctype_len = 0;

  if (!mp_parse_headers(it, &hdrs, &hcnt, &name, &name_len, &filename, &filename_len, &ctype, &ctype_len)) {
    free(name);
    free(filename);
    mp_free_hdrs(hdrs, hcnt);
    return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_NEXT, rid, "t_http_invalid", "bad part headers");
  }

  zi_http_mp_part *part = (zi_http_mp_part *)calloc(1, sizeof(*part));
  if (!part) {
    free(name);
    free(filename);
    mp_free_hdrs(hdrs, hcnt);
    return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_NEXT, rid, "t_http_oom", "oom");
  }
  part->it = it;
  part->closed = 0;
  it->part_open = 1;

  zi_handle_t part_h = zi_handle25_alloc_with_poll(&MP_PART_OPS, &MP_PART_POLL_OPS, part, ZI_H_READABLE | ZI_H_ENDABLE);
  if (part_h < 3) {
    it->part_open = 0;
    free(part);
    free(name);
    free(filename);
    mp_free_hdrs(hdrs, hcnt);
    return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_NEXT, rid, "t_http_oom", "failed to alloc part handle");
  }

  uint32_t payload_len = 4;
  payload_len += 4 + name_len;
  payload_len += 4 + filename_len;
  payload_len += 4 + ctype_len;
  payload_len += 4;
  for (uint32_t i = 0; i < hcnt; i++) {
    payload_len += 4 + hdrs[i].name_len;
    payload_len += 4 + hdrs[i].val_len;
  }
  payload_len += 4;

  uint8_t *payload = (uint8_t *)malloc((size_t)payload_len);
  if (!payload) {
    (void)zi_end(part_h);
    free(name);
    free(filename);
    mp_free_hdrs(hdrs, hcnt);
    return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_NEXT, rid, "t_http_oom", "oom");
  }

  uint32_t off = 0;
  zi_zcl1_write_u32(payload + off, 0);
  off += 4;
  zi_zcl1_write_u32(payload + off, name_len);
  off += 4;
  if (name_len) {
    memcpy(payload + off, name, name_len);
    off += name_len;
  }
  zi_zcl1_write_u32(payload + off, filename_len);
  off += 4;
  if (filename_len) {
    memcpy(payload + off, filename, filename_len);
    off += filename_len;
  }
  zi_zcl1_write_u32(payload + off, ctype_len);
  off += 4;
  if (ctype_len) {
    memcpy(payload + off, ctype, ctype_len);
    off += ctype_len;
  }
  zi_zcl1_write_u32(payload + off, hcnt);
  off += 4;
  for (uint32_t i = 0; i < hcnt; i++) {
    zi_zcl1_write_u32(payload + off, hdrs[i].name_len);
    off += 4;
    if (hdrs[i].name_len) {
      memcpy(payload + off, hdrs[i].name, hdrs[i].name_len);
      off += hdrs[i].name_len;
    }
    zi_zcl1_write_u32(payload + off, hdrs[i].val_len);
    off += 4;
    if (hdrs[i].val_len) {
      memcpy(payload + off, hdrs[i].val, hdrs[i].val_len);
      off += hdrs[i].val_len;
    }
  }
  zi_zcl1_write_u32(payload + off, (uint32_t)(int32_t)part_h);
  off += 4;

  free(name);
  free(filename);
  mp_free_hdrs(hdrs, hcnt);

  int ok = set_out_frame_ok(c, ZI_HTTP_OP_MULTIPART_NEXT, rid, payload, payload_len);
  if (ok) it->parts_emitted++;
  free(payload);
  return ok;
}

static int dispatch_multipart_end(zi_http_cap_ctx *c, uint32_t rid, const uint8_t *p, uint32_t n) {
  (void)p;
  if (!c) return 0;
  if (n != 0) return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_END, rid, "t_http_invalid", "malformed MULTIPART_END payload");
  zi_http_req *r = req_by_rid(c, rid);
  if (!r || !r->mp) return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_END, rid, "t_http_invalid", "multipart not begun");
  if (r->mp->part_open) return set_out_frame_err(c, ZI_HTTP_OP_MULTIPART_END, rid, "t_http_invalid", "part still open");
  mp_free(r->mp);
  r->mp = NULL;
  return set_out_frame_ok(c, ZI_HTTP_OP_MULTIPART_END, rid, NULL, 0);
}

static int dispatch_request(zi_http_cap_ctx *c, const zi_zcl1_frame *fr) {
  if (!c || !fr) return 0;
  if (fr->op == ZI_HTTP_OP_LISTEN) {
    return dispatch_listen(c, fr->rid, fr->payload, fr->payload_len);
  }
  if (fr->op == ZI_HTTP_OP_CLOSE_LISTENER) {
    return dispatch_close_listener(c, fr->rid, fr->payload, fr->payload_len);
  }
  if (fr->op == ZI_HTTP_OP_MULTIPART_BEGIN) {
    return dispatch_multipart_begin(c, fr->rid, fr->payload, fr->payload_len);
  }
  if (fr->op == ZI_HTTP_OP_MULTIPART_NEXT) {
    return dispatch_multipart_next(c, fr->rid, fr->payload, fr->payload_len);
  }
  if (fr->op == ZI_HTTP_OP_MULTIPART_END) {
    return dispatch_multipart_end(c, fr->rid, fr->payload, fr->payload_len);
  }
  if (fr->op == ZI_HTTP_OP_RESPOND_START) {
    return dispatch_respond_start(c, fr->rid, fr->payload, fr->payload_len);
  }
  if (fr->op == ZI_HTTP_OP_RESPOND_INLINE) {
    return dispatch_respond_inline(c, fr->rid, fr->payload, fr->payload_len);
  }
  if (fr->op == ZI_HTTP_OP_RESPOND_STREAM) {
    return dispatch_respond_stream(c, fr->rid, fr->payload, fr->payload_len);
  }
  if (fr->op == ZI_HTTP_OP_FETCH) {
    return dispatch_fetch(c, fr->rid, fr->payload, fr->payload_len);
  }
  return set_out_frame_err(c, fr->op, fr->rid, "t_http_nosys", "op not implemented");
}

static int allowlist_allows_outbound(const char *allow, const char *host, uint32_t port) {
  if (!host || host[0] == '\0') return 0;

  if (!allow || allow[0] == '\0') {
    return is_loopback_host(host);
  }
  if (streq_nocase(allow, "any")) return 1;

  const char *p = allow;
  while (*p) {
    p = skip_ws(p);
    if (!*p) break;

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

    char *colon = strrchr(tok, ':');
    if (!colon) continue;
    *colon = '\0';
    const char *entry_host = tok;
    const char *entry_port = colon + 1;

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

static int parse_http_url(const uint8_t *url, uint32_t url_len, char *out_host, size_t out_host_cap, uint32_t *out_port,
                          char *out_path, size_t out_path_cap, char *out_authority, size_t out_auth_cap) {
  if (!url || url_len == 0 || !out_host || !out_port || !out_path || !out_authority) return 0;
  if (out_host_cap == 0 || out_path_cap == 0 || out_auth_cap == 0) return 0;
  out_host[0] = '\0';
  out_path[0] = '\0';
  out_authority[0] = '\0';
  *out_port = 0;

  const char prefix[] = "http://";
  if (url_len < (uint32_t)(sizeof(prefix) - 1)) return 0;
  if (memcmp(url, prefix, sizeof(prefix) - 1) != 0) return 0;

  const uint8_t *p = url + (sizeof(prefix) - 1);
  uint32_t n = url_len - (uint32_t)(sizeof(prefix) - 1);

  // split authority and path
  uint32_t auth_len = 0;
  while (auth_len < n && p[auth_len] != '/' && p[auth_len] != '?' && p[auth_len] != '#') auth_len++;
  if (auth_len == 0) return 0;

  // Copy authority for Host header
  if ((size_t)auth_len + 1 > out_auth_cap) return 0;
  memcpy(out_authority, p, auth_len);
  out_authority[auth_len] = '\0';
  if (strchr(out_authority, '@') != NULL) return 0;

  // Parse host + optional port
  const uint8_t *auth = p;
  uint32_t host_len = 0;
  uint32_t port = 80;
  if (auth[0] == '[') {
    uint32_t i = 1;
    while (i < auth_len && auth[i] != ']') i++;
    if (i >= auth_len) return 0;
    host_len = i + 1; // keep brackets in authority, but strip for getaddrinfo below
    uint32_t rest = auth_len - host_len;
    if (rest != 0) {
      if (auth[host_len] != ':') return 0;
      const uint8_t *pp = auth + host_len + 1;
      uint32_t pn = auth_len - (host_len + 1);
      if (pn == 0) return 0;
      uint64_t pv = 0;
      if (!parse_u64_dec(pp, pn, &pv) || pv == 0 || pv > 65535) return 0;
      port = (uint32_t)pv;
    }
    // host_out without brackets
    uint32_t inner = host_len - 2;
    if ((size_t)inner + 1 > out_host_cap) return 0;
    memcpy(out_host, auth + 1, inner);
    out_host[inner] = '\0';
  } else {
    // look for :port from the right
    int colon = -1;
    for (uint32_t i = 0; i < auth_len; i++) {
      if (auth[i] == ':') colon = (int)i;
    }
    if (colon >= 0) {
      host_len = (uint32_t)colon;
      uint32_t pn = auth_len - host_len - 1;
      if (pn == 0) return 0;
      uint64_t pv = 0;
      if (!parse_u64_dec(auth + host_len + 1, pn, &pv) || pv == 0 || pv > 65535) return 0;
      port = (uint32_t)pv;
    } else {
      host_len = auth_len;
    }
    if (host_len == 0) return 0;
    if ((size_t)host_len + 1 > out_host_cap) return 0;
    memcpy(out_host, auth, host_len);
    out_host[host_len] = '\0';
  }
  *out_port = port;

  // path+query
  const uint8_t *rest = p + auth_len;
  uint32_t rest_len = n - auth_len;
  if (rest_len == 0) {
    strncpy(out_path, "/", out_path_cap - 1);
    out_path[out_path_cap - 1] = '\0';
    return 1;
  }
  if (rest[0] == '#') {
    strncpy(out_path, "/", out_path_cap - 1);
    out_path[out_path_cap - 1] = '\0';
    return 1;
  }
  if (rest[0] == '?') {
    // treat as /?query
    if (rest_len + 2 > out_path_cap) return 0;
    out_path[0] = '/';
    memcpy(out_path + 1, rest, rest_len);
    out_path[1 + rest_len] = '\0';
    return 1;
  }
  if (rest[0] != '/') return 0;
  // strip fragment
  uint32_t path_len = 0;
  while (path_len < rest_len && rest[path_len] != '#') path_len++;
  if ((size_t)path_len + 1 > out_path_cap) return 0;
  memcpy(out_path, rest, path_len);
  out_path[path_len] = '\0';
  return 1;
}

static int connect_tcp(const char *host, uint32_t port, int *out_fd) {
  if (!host || !out_fd) return 0;
  *out_fd = -1;
  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%u", port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  struct addrinfo *res = NULL;
  int ga = getaddrinfo(host, port_str, &hints, &res);
  if (ga != 0 || !res) {
    if (res) freeaddrinfo(res);
    return 0;
  }

  int fd = -1;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
#if defined(__APPLE__)
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
      (void)close(fd);
      fd = -1;
      continue;
    }
    break;
  }
  freeaddrinfo(res);
  if (fd < 0) return 0;
  *out_fd = fd;
  return 1;
}

static int dispatch_fetch(zi_http_cap_ctx *c, uint32_t rid, const uint8_t *p, uint32_t n) {
  if (!c) return 0;
  if (!p || n < 12u) {
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "malformed FETCH payload");
  }

  uint32_t off = 0;
  uint32_t method_len = zi_zcl1_read_u32(p + off);
  off += 4;
  if (method_len == 0 || method_len > 32u) {
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "bad method_len");
  }
  if (off + method_len + 4u > n) return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "bad method");
  const uint8_t *method = p + off;
  off += method_len;

  uint32_t url_len = zi_zcl1_read_u32(p + off);
  off += 4;
  if (url_len == 0 || url_len > c->lim.max_fetch_url_bytes) {
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "bad url_len");
  }
  if (off + url_len + 4u > n) return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "bad url");
  const uint8_t *url = p + off;
  off += url_len;

  if (contains_ctl_or_lf(method, method_len) || contains_ctl_or_lf(url, url_len)) {
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "invalid characters");
  }

  uint32_t hcount = zi_zcl1_read_u32(p + off);
  off += 4;
  if (hcount > c->lim.max_header_count) {
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "too many headers");
  }

  const uint8_t *hdr_start = p + off;
  uint32_t hdr_off = off;
  int has_host = 0;
  int has_conn = 0;
  int has_cl = 0;
  uint64_t cl_val = 0;
  int has_chunked_te = 0;
  for (uint32_t i = 0; i < hcount; i++) {
    if (hdr_off + 4u > n) return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "bad headers");
    uint32_t name_len = zi_zcl1_read_u32(p + hdr_off);
    hdr_off += 4;
    if (hdr_off + name_len + 4u > n) return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "bad headers");
    const uint8_t *name = p + hdr_off;
    hdr_off += name_len;
    uint32_t val_len = zi_zcl1_read_u32(p + hdr_off);
    hdr_off += 4;
    if (hdr_off + val_len > n) return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "bad headers");
    const uint8_t *val = p + hdr_off;
    hdr_off += val_len;
    (void)val;

    if (contains_ctl_or_lf(name, name_len) || contains_ctl_or_lf(val, val_len)) {
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "header contains control chars");
    }
    if (eq_nocase_bytes(name, name_len, "host")) has_host = 1;
    if (eq_nocase_bytes(name, name_len, "connection")) has_conn = 1;
    if (eq_nocase_bytes(name, name_len, "content-length")) {
      uint64_t v = 0;
      if (parse_u64_dec(val, val_len, &v)) {
        cl_val = v;
        has_cl = 1;
      }
    }
    if (eq_nocase_bytes(name, name_len, "transfer-encoding")) {
      if (contains_nocase_token(val, val_len, "chunked")) has_chunked_te = 1;
    }
  }
  off = hdr_off;
  if (off + 4u > n) return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "missing body_kind");
  uint32_t body_kind = zi_zcl1_read_u32(p + off);
  off += 4;
  uint32_t body_len = 0;
  const uint8_t *body = NULL;
  zi_handle_t body_handle = 0;
  if (body_kind == 0) {
    // ok
  } else if (body_kind == 1) {
    if (off + 4u > n) return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "missing body_len");
    body_len = zi_zcl1_read_u32(p + off);
    off += 4;
    if (off + body_len != n) return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "bad body");
    body = p + off;
    if (body_len > c->lim.max_inline_body_bytes) {
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "inline body too large");
    }
  } else if (body_kind == 2) {
    if (off + 4u != n) return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "bad stream body");
    body_handle = (zi_handle_t)(int32_t)zi_zcl1_read_u32(p + off);
    if (body_handle < 3) return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "bad body_handle");
    uint32_t hf = zi_handle_hflags(body_handle);
    if ((hf & ZI_H_READABLE) == 0) return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "body_handle not readable");
    if (!has_cl) return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "stream body requires Content-Length");
    if (has_chunked_te) return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "chunked request bodies not supported");
    if (cl_val > 0x7FFFFFFFULL) return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "content-length too large");
  } else {
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "bad body_kind");
  }

  char host[256];
  char path[2048];
  char authority[512];
  uint32_t port = 0;
  if (!parse_http_url(url, url_len, host, sizeof(host), &port, path, sizeof(path), authority, sizeof(authority))) {
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "unsupported url");
  }

  const char *allow = getenv("ZI_NET_ALLOW");
  if (!allowlist_allows_outbound(allow, host, port)) {
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_denied", "outbound connect denied by policy");
  }

  int fd = -1;
  if (!connect_tcp(host, port, &fd)) {
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_io", "connect failed");
  }

  // Build request bytes.
  const char *m = (const char *)method;
  char reqline[4096];
  if (method_len + strlen(path) + 32u >= sizeof(reqline)) {
    (void)close(fd);
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "request line too long");
  }
  memcpy(reqline, m, method_len);
  reqline[method_len] = 0;
  int rl = snprintf(reqline, sizeof(reqline), "%.*s %s HTTP/1.1\r\n", (int)method_len, m, path);
  if (rl <= 0) {
    (void)close(fd);
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_internal", "snprintf failed");
  }

  // Estimate buffer: req line + headers + body.
  uint64_t out_cap = (uint64_t)rl + 256ull + (uint64_t)c->lim.max_header_bytes + (uint64_t)body_len;
  if (out_cap > 8ull * 1024ull * 1024ull) {
    (void)close(fd);
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "request too large");
  }
  uint8_t *out = (uint8_t *)malloc((size_t)out_cap);
  if (!out) {
    (void)close(fd);
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_oom", "oom building request");
  }
  uint32_t woff = 0;
  memcpy(out + woff, reqline, (size_t)rl);
  woff += (uint32_t)rl;

  if (!has_host) {
    int hn = snprintf((char *)out + woff, (size_t)out_cap - woff, "Host: %s\r\n", authority);
    if (hn <= 0) {
      free(out);
      (void)close(fd);
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_internal", "snprintf failed");
    }
    woff += (uint32_t)hn;
  }
  if (!has_conn) {
    const char conn_close[] = "Connection: close\r\n";
    memcpy(out + woff, conn_close, sizeof(conn_close) - 1u);
    woff += (uint32_t)(sizeof(conn_close) - 1u);
  }

  // Copy caller headers verbatim.
  uint32_t cur = (uint32_t)(hdr_start - p);
  for (uint32_t i = 0; i < hcount; i++) {
    uint32_t name_len = zi_zcl1_read_u32(p + cur);
    cur += 4;
    const uint8_t *name = p + cur;
    cur += name_len;
    uint32_t val_len = zi_zcl1_read_u32(p + cur);
    cur += 4;
    const uint8_t *val = p + cur;
    cur += val_len;

    // Ensure line fits.
    if ((uint64_t)woff + (uint64_t)name_len + 2ull + (uint64_t)val_len + 2ull >= out_cap) {
      free(out);
      (void)close(fd);
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "headers too large");
    }
    memcpy(out + woff, name, name_len);
    woff += name_len;
    out[woff++] = ':';
    out[woff++] = ' ';
    if (val_len) {
      memcpy(out + woff, val, val_len);
      woff += val_len;
    }
    out[woff++] = '\r';
    out[woff++] = '\n';
  }

  if (body_kind == 1) {
    char cl[64];
    int cn = snprintf(cl, sizeof(cl), "Content-Length: %u\r\n", body_len);
    if (cn <= 0 || (uint64_t)woff + (uint64_t)cn >= out_cap) {
      free(out);
      (void)close(fd);
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_internal", "snprintf failed");
    }
    memcpy(out + woff, cl, (size_t)cn);
    woff += (uint32_t)cn;
  }

  out[woff++] = '\r';
  out[woff++] = '\n';
  if (body_kind == 1 && body_len) {
    if ((uint64_t)woff + (uint64_t)body_len > out_cap) {
      free(out);
      (void)close(fd);
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_internal", "request overflow");
    }
    memcpy(out + woff, body, body_len);
    woff += body_len;
  }

  int ok_send = send_all(fd, out, (size_t)woff);
  free(out);
  if (!ok_send) {
    (void)close(fd);
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_io", "send failed");
  }

  if (body_kind == 2) {
    uint64_t remaining = cl_val;
    const zi_mem_v1 *mem = zi_runtime25_mem();
    if (!mem || !mem->map_ro) {
      (void)close(fd);
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_nosys", "no guest mem mapper");
    }
    const zi_size32_t chunk_cap = 64u * 1024u;
    zi_ptr_t tmp_ptr = zi_alloc(chunk_cap);
    if (tmp_ptr == 0) {
      (void)close(fd);
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_oom", "oom temp buffer");
    }
    while (remaining > 0) {
      zi_size32_t want = chunk_cap;
      if ((uint64_t)want > remaining) want = (zi_size32_t)remaining;
      int32_t rn = zi_read(body_handle, tmp_ptr, want);
      if (rn < 0) {
        (void)zi_free(tmp_ptr);
        (void)close(fd);
        return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_io", "read body_handle failed");
      }
      if (rn == 0) {
        (void)zi_free(tmp_ptr);
        (void)close(fd);
        return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_io", "early eof from body_handle");
      }
      const uint8_t *bp = NULL;
      if (!mem->map_ro(mem->ctx, tmp_ptr, (zi_size32_t)rn, &bp) || !bp) {
        (void)zi_free(tmp_ptr);
        (void)close(fd);
        return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_internal", "map_ro failed");
      }
      if (!send_all(fd, bp, (size_t)rn)) {
        (void)zi_free(tmp_ptr);
        (void)close(fd);
        return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_io", "send body failed");
      }
      remaining -= (uint64_t)rn;
    }
    (void)zi_free(tmp_ptr);
  }

  // Read response headers.
  uint8_t *rbuf = NULL;
  size_t rcap = 4096;
  size_t rlen = 0;
  rbuf = (uint8_t *)malloc(rcap);
  if (!rbuf) {
    (void)close(fd);
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_oom", "oom reading response");
  }
  const uint8_t hdr_end_seq[] = {'\r', '\n', '\r', '\n'};
  int hdr_end_off = -1;
  while (1) {
    if (rlen >= (size_t)c->lim.max_header_bytes + 4u) {
      free(rbuf);
      (void)close(fd);
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "response headers too large");
    }
    if (rcap - rlen < 2048) {
      size_t ncap = rcap * 2u;
      if (ncap < rcap) {
        free(rbuf);
        (void)close(fd);
        return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_internal", "overflow");
      }
      if (ncap > (size_t)c->lim.max_header_bytes + 4096u) ncap = (size_t)c->lim.max_header_bytes + 4096u;
      uint8_t *nb = (uint8_t *)realloc(rbuf, ncap);
      if (!nb) {
        free(rbuf);
        (void)close(fd);
        return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_oom", "oom reading response");
      }
      rbuf = nb;
      rcap = ncap;
    }
    ssize_t rn = recv(fd, rbuf + rlen, rcap - rlen, 0);
    if (rn < 0) {
      if (errno == EINTR) continue;
      free(rbuf);
      (void)close(fd);
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_io", "recv failed");
    }
    if (rn == 0) {
      free(rbuf);
      (void)close(fd);
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_io", "unexpected eof");
    }
    rlen += (size_t)rn;
    hdr_end_off = find_seq(rbuf, rlen, hdr_end_seq, sizeof(hdr_end_seq));
    if (hdr_end_off >= 0) break;
  }
  size_t header_bytes = (size_t)hdr_end_off + 4u;

  // Parse status line
  int line_end = find_seq(rbuf, header_bytes, (const uint8_t *)"\r\n", 2);
  if (line_end < 0) {
    free(rbuf);
    (void)close(fd);
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "bad status line");
  }
  uint8_t *line = rbuf;
  uint32_t line_len = (uint32_t)line_end;
  if (line_len < 12u || memcmp(line, "HTTP/1.1 ", 9) != 0) {
    free(rbuf);
    (void)close(fd);
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "unsupported http version");
  }
  uint32_t status = 0;
  if (line[9] < '0' || line[9] > '9' || line[10] < '0' || line[10] > '9' || line[11] < '0' || line[11] > '9') {
    free(rbuf);
    (void)close(fd);
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "bad status code");
  }
  status = (uint32_t)(line[9] - '0') * 100u + (uint32_t)(line[10] - '0') * 10u + (uint32_t)(line[11] - '0');

  // Parse headers
  uint32_t maxh = c->lim.max_header_count;
  if (maxh > 4096u) maxh = 4096u;
  hdr *headers = (hdr *)calloc((size_t)maxh, sizeof(hdr));
  if (!headers) {
    free(rbuf);
    (void)close(fd);
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_oom", "oom headers");
  }
  uint32_t rhcount = 0;
  uint8_t *hp = rbuf + (uint32_t)line_end + 2u;
  // Parse up to (and including) the "\r\n\r\n" terminator; the loop already
  // treats the empty line as a stop marker.
  uint8_t *hend = rbuf + (uint32_t)header_bytes;
  uint64_t content_len = 0;
  int has_content_len = 0;
  int has_chunked = 0;
  while (hp < hend) {
    uint8_t *eol = find_crlf(hp, hend);
    if (!eol) break;
    if (eol == hp) {
      hp += 2;
      continue;
    }
    uint8_t *colon = (uint8_t *)memchr(hp, ':', (size_t)(eol - hp));
    if (!colon) {
      free(headers);
      free(rbuf);
      (void)close(fd);
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "bad header line");
    }
    uint8_t *name = hp;
    uint32_t name_len = (uint32_t)(colon - hp);
    uint8_t *val = colon + 1;
    uint8_t *val_end = eol;
    while (val < val_end && (*val == ' ' || *val == '\t')) val++;
    while (val_end > val && (val_end[-1] == ' ' || val_end[-1] == '\t')) val_end--;
    uint32_t val_len = (uint32_t)(val_end - val);
    if (name_len == 0) {
      free(headers);
      free(rbuf);
      (void)close(fd);
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "bad header name");
    }
    if (contains_ctl_or_lf(name, name_len) || contains_ctl_or_lf(val, val_len)) {
      free(headers);
      free(rbuf);
      (void)close(fd);
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "invalid header chars");
    }
    lower_ascii(name, name_len);
    if (rhcount >= maxh) {
      free(headers);
      free(rbuf);
      (void)close(fd);
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "too many headers");
    }
    headers[rhcount].name = name;
    headers[rhcount].name_len = name_len;
    headers[rhcount].val = val;
    headers[rhcount].val_len = val_len;
    rhcount++;

    if (name_len == 14 && memcmp(name, "content-length", 14) == 0) {
      uint64_t v = 0;
      if (parse_u64_dec(val, val_len, &v)) {
        content_len = v;
        has_content_len = 1;
      }
    }
    if (name_len == 17 && memcmp(name, "transfer-encoding", 17) == 0) {
      if (contains_nocase_token(val, val_len, "chunked")) has_chunked = 1;
    }
    hp = eol + 2;
  }

  uint32_t resp_body_kind = ZI_HTTP_BODY_NONE;
  uint8_t *resp_inline = NULL;
  uint32_t resp_inline_len = 0;
  zi_handle_t resp_body_handle = 0;

  size_t already = rlen - header_bytes;
  if (has_chunked) {
    resp_body_kind = ZI_HTTP_BODY_STREAM;
    uint32_t pre_len = 0;
    if (already > 0) {
      if (already > (size_t)UINT32_MAX) pre_len = UINT32_MAX;
      else pre_len = (uint32_t)already;
    }
    // Header parsing above uses blocking recv; once the response is framed, make
    // the socket nonblocking for streamed body reads.
    set_nonblocking_best_effort(fd);
    zi_http_chunked_stream *cbs = chunked_stream_new(fd, rbuf + header_bytes, pre_len, c->lim.max_header_bytes, 1);
    if (!cbs) {
      free(headers);
      free(rbuf);
      (void)close(fd);
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_oom", "oom stream");
    }
    resp_body_handle = zi_handle25_alloc_with_poll(&CHUNKED_BODY_OPS, &CHUNKED_BODY_POLL_OPS, cbs, ZI_H_READABLE | ZI_H_ENDABLE);
    if (resp_body_handle < 3) {
      chunked_end(cbs);
      free(headers);
      free(rbuf);
      (void)close(fd);
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_internal", "failed alloc body handle");
    }
    fd = -1; // owned by handle
  } else {
    if (!has_content_len) content_len = 0;
    if (content_len == 0) {
      resp_body_kind = ZI_HTTP_BODY_NONE;
      (void)close(fd);
      fd = -1;
    } else if (content_len <= (uint64_t)c->lim.max_inline_body_bytes) {
    resp_body_kind = ZI_HTTP_BODY_INLINE;
    resp_inline_len = (uint32_t)content_len;
    resp_inline = (uint8_t *)malloc((size_t)resp_inline_len);
    if (!resp_inline) {
      free(headers);
      free(rbuf);
      (void)close(fd);
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_oom", "oom body");
    }
    size_t take = already;
    if (take > (size_t)resp_inline_len) take = (size_t)resp_inline_len;
    if (take) memcpy(resp_inline, rbuf + header_bytes, take);
    size_t boff = take;
    while (boff < (size_t)resp_inline_len) {
      ssize_t rn = recv(fd, resp_inline + boff, (size_t)resp_inline_len - boff, 0);
      if (rn < 0) {
        if (errno == EINTR) continue;
        free(resp_inline);
        free(headers);
        free(rbuf);
        (void)close(fd);
        return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_io", "recv failed");
      }
      if (rn == 0) {
        free(resp_inline);
        free(headers);
        free(rbuf);
        (void)close(fd);
        return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_io", "unexpected eof");
      }
      boff += (size_t)rn;
    }
    (void)close(fd);
    fd = -1;
    } else {
    resp_body_kind = ZI_HTTP_BODY_STREAM;
    uint32_t pre_len = 0;
    if (already > 0) {
      if ((uint64_t)already >= content_len) pre_len = (uint32_t)content_len;
      else pre_len = (uint32_t)already;
    }
    uint64_t rem = content_len;
    if ((uint64_t)pre_len > rem) pre_len = (uint32_t)rem;
    rem -= (uint64_t)pre_len;
    // Header parsing above uses blocking recv; once the response is framed, make
    // the socket nonblocking for streamed body reads.
    set_nonblocking_best_effort(fd);
    zi_http_body_stream *bs = body_stream_new(fd, rem, rbuf + header_bytes, pre_len, 1);
    if (!bs) {
      free(headers);
      free(rbuf);
      (void)close(fd);
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_oom", "oom stream");
    }
    resp_body_handle = zi_handle25_alloc_with_poll(&BODY_OPS, &BODY_POLL_OPS, bs, ZI_H_READABLE | ZI_H_ENDABLE);
    if (resp_body_handle < 3) {
      body_end(bs);
      free(headers);
      free(rbuf);
      (void)close(fd);
      return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_internal", "failed alloc body handle");
    }
    fd = -1; // owned by handle
    }
  }

  // Build response payload
  uint64_t payload_len = 0;
  payload_len += 4;
  payload_len += 4;
  for (uint32_t i = 0; i < rhcount; i++) {
    payload_len += 4 + headers[i].name_len;
    payload_len += 4 + headers[i].val_len;
  }
  payload_len += 4;
  if (resp_body_kind == ZI_HTTP_BODY_INLINE) {
    payload_len += 4 + resp_inline_len;
  } else if (resp_body_kind == ZI_HTTP_BODY_STREAM) {
    payload_len += 4;
  }
  if (payload_len > 16u * 1024u * 1024u) {
    if (resp_inline) free(resp_inline);
    if (resp_body_handle >= 3) (void)zi_end(resp_body_handle);
    free(headers);
    free(rbuf);
    if (fd >= 0) (void)close(fd);
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_invalid", "response payload too large");
  }
  uint8_t *pl = (uint8_t *)malloc((size_t)payload_len);
  if (!pl) {
    if (resp_inline) free(resp_inline);
    if (resp_body_handle >= 3) (void)zi_end(resp_body_handle);
    free(headers);
    free(rbuf);
    if (fd >= 0) (void)close(fd);
    return set_out_frame_err(c, ZI_HTTP_OP_FETCH, rid, "t_http_oom", "oom response payload");
  }
  uint32_t poff = 0;
  zi_zcl1_write_u32(pl + poff, status);
  poff += 4;
  zi_zcl1_write_u32(pl + poff, rhcount);
  poff += 4;
  for (uint32_t i = 0; i < rhcount; i++) {
    zi_zcl1_write_u32(pl + poff, headers[i].name_len);
    poff += 4;
    memcpy(pl + poff, headers[i].name, headers[i].name_len);
    poff += headers[i].name_len;
    zi_zcl1_write_u32(pl + poff, headers[i].val_len);
    poff += 4;
    if (headers[i].val_len) memcpy(pl + poff, headers[i].val, headers[i].val_len);
    poff += headers[i].val_len;
  }
  zi_zcl1_write_u32(pl + poff, resp_body_kind);
  poff += 4;
  if (resp_body_kind == ZI_HTTP_BODY_INLINE) {
    zi_zcl1_write_u32(pl + poff, resp_inline_len);
    poff += 4;
    memcpy(pl + poff, resp_inline, resp_inline_len);
    poff += resp_inline_len;
  } else if (resp_body_kind == ZI_HTTP_BODY_STREAM) {
    zi_zcl1_write_u32(pl + poff, (uint32_t)(int32_t)resp_body_handle);
    poff += 4;
  }

  int ok = set_out_frame_ok(c, ZI_HTTP_OP_FETCH, rid, pl, (uint32_t)payload_len);
  free(pl);
  free(resp_inline);
  free(headers);
  free(rbuf);
  if (!ok) {
    if (resp_body_handle >= 3) (void)zi_end(resp_body_handle);
  }
  return ok;
}

static int32_t http_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  zi_http_cap_ctx *c = (zi_http_cap_ctx *)ctx;
  if (!c) return ZI_E_INTERNAL;
  if (c->closed) return ZI_E_CLOSED;
  if (cap == 0) return 0;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_rw) return ZI_E_NOSYS;
  if (dst_ptr == 0) return ZI_E_BOUNDS;

  pthread_mutex_lock(&c->mu);
  if (!c->out || c->out_len == 0 || c->out_off >= c->out_len) {
    pthread_mutex_unlock(&c->mu);
    return ZI_E_AGAIN;
  }

  uint8_t *dst = NULL;
  if (!mem->map_rw(mem->ctx, dst_ptr, cap, &dst) || !dst) {
    pthread_mutex_unlock(&c->mu);
    return ZI_E_BOUNDS;
  }
  uint32_t avail = c->out_len - c->out_off;
  uint32_t n = cap < avail ? cap : avail;
  memcpy(dst, c->out + c->out_off, n);
  c->out_off += n;
  if (c->out_off == c->out_len) {
    free_out_locked(c);
  }
  pthread_mutex_unlock(&c->mu);
  return (int32_t)n;
}

static int32_t http_write(void *ctx, zi_ptr_t src_ptr, zi_size32_t len) {
  zi_http_cap_ctx *c = (zi_http_cap_ctx *)ctx;
  if (!c) return ZI_E_INTERNAL;
  if (c->closed) return ZI_E_CLOSED;
  if (len == 0) return 0;

  pthread_mutex_lock(&c->mu);
  if (c->out && c->out_len != 0 && c->out_off < c->out_len) {
    pthread_mutex_unlock(&c->mu);
    return ZI_E_AGAIN;
  }
  pthread_mutex_unlock(&c->mu);

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_ro) return ZI_E_NOSYS;
  if (src_ptr == 0) return ZI_E_BOUNDS;
  const uint8_t *src = NULL;
  if (!mem->map_ro(mem->ctx, src_ptr, len, &src) || !src) return ZI_E_BOUNDS;

  uint32_t need = c->in_len + len;
  if (!ensure_in_cap(c, need)) {
    free_in(c);
    return ZI_E_BOUNDS;
  }
  memcpy(c->in + c->in_len, src, len);
  c->in_len += len;

  if (c->in_len < 24u) {
    return (int32_t)len;
  }

  if (!(c->in[0] == 'Z' && c->in[1] == 'C' && c->in[2] == 'L' && c->in[3] == '1')) {
    c->in_len = 0;
    return ZI_E_INVALID;
  }

  uint32_t payload_len = zi_zcl1_read_u32(c->in + 20);
  uint64_t frame_len64 = 24ull + (uint64_t)payload_len;
  if (frame_len64 > 64ull * 1024ull * 1024ull) {
    c->in_len = 0;
    return ZI_E_BOUNDS;
  }
  uint32_t frame_len = (uint32_t)frame_len64;
  if (frame_len > c->in_len) {
    return (int32_t)len;
  }
  if (frame_len != c->in_len) {
    c->in_len = 0;
    return ZI_E_INVALID;
  }

  zi_zcl1_frame fr;
  if (!zi_zcl1_parse(c->in, c->in_len, &fr)) {
    c->in_len = 0;
    return ZI_E_INVALID;
  }

  int ok = dispatch_request(c, &fr);
  c->in_len = 0;
  if (!ok || !c->out || c->out_len == 0) {
    (void)set_out_frame_err(c, fr.op, fr.rid, "t_http_internal", "dispatch failed");
  }
  return (int32_t)len;
}

static int32_t http_end(void *ctx) {
  zi_http_cap_ctx *c = (zi_http_cap_ctx *)ctx;
  if (!c) return ZI_E_INTERNAL;

  pthread_mutex_lock(&c->mu);
  c->closed = 1;
  for (uint32_t i = 0; i < ZI_HTTP_MAX_LISTENERS; i++) {
    if (c->listeners[i].in_use && c->listeners[i].fd >= 0) {
      (void)close(c->listeners[i].fd);
      c->listeners[i].fd = -1;
    }
    memset(&c->listeners[i], 0, sizeof(c->listeners[i]));
  }
  pthread_cond_broadcast(&c->cv);
  pthread_mutex_unlock(&c->mu);

  if (c->srv_thr_started) {
    (void)pthread_join(c->srv_thr, NULL);
    c->srv_thr_started = 0;
  }

  if (c->reqs) {
    for (uint32_t i = 0; i < c->reqs_cap; i++) {
      if (c->reqs[i].in_use) {
        if (c->reqs[i].resp_body_handle >= 3) {
          (void)zi_end(c->reqs[i].resp_body_handle);
          c->reqs[i].resp_body_handle = 0;
        }
        close_req(&c->reqs[i]);
      }
    }
  }

  free(c->reqs);
  c->reqs = NULL;
  c->reqs_cap = 0;
  free_out(c);
  free_in(c);

  if (c->notify_r >= 0) (void)close(c->notify_r);
  if (c->notify_w >= 0) (void)close(c->notify_w);
  c->notify_r = -1;
  c->notify_w = -1;

  pthread_cond_destroy(&c->cv);
  pthread_mutex_destroy(&c->mu);
  memset(c, 0, sizeof(*c));
  free(c);
  return 0;
}

static const zi_handle_ops_v1 HTTP_OPS = {
    .read = http_read,
    .write = http_write,
    .end = http_end,
};

static const zi_cap_v1 CAP = {
    .kind = ZI_CAP_KIND_NET,
    .name = ZI_CAP_NAME_HTTP,
    .version = 1,
    .cap_flags = ZI_CAP_CAN_OPEN | ZI_CAP_MAY_BLOCK,
    .meta = NULL,
    .meta_len = 0,
};

const zi_cap_v1 *zi_net_http25_cap(void) { return &CAP; }

int zi_net_http25_register(void) { return zi_cap_register(&CAP); }

zi_handle_t zi_net_http25_open_from_params(zi_ptr_t params_ptr, zi_size32_t params_len) {
  (void)params_ptr;
  if (params_len != 0) return (zi_handle_t)ZI_E_INVALID;
  if (!zi_handles25_init()) return (zi_handle_t)ZI_E_INTERNAL;

  zi_http_cap_ctx *c = (zi_http_cap_ctx *)calloc(1u, sizeof(*c));
  if (!c) return (zi_handle_t)ZI_E_OOM;

  c->notify_r = -1;
  c->notify_w = -1;
  c->notify_pending = 0;
  c->srv_thr_started = 0;

  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&c->mu, &attr);
  pthread_mutexattr_destroy(&attr);
  pthread_cond_init(&c->cv, NULL);

  {
    int fds[2];
    if (pipe(fds) == 0) {
      c->notify_r = fds[0];
      c->notify_w = fds[1];
      set_nonblocking_best_effort(c->notify_r);
      set_nonblocking_best_effort(c->notify_w);
    }
  }

  if (pthread_create(&c->srv_thr, NULL, http_server_thread_main, c) == 0) {
    c->srv_thr_started = 1;
  }

  c->lim = load_limits();
  c->next_listener_id = 0;
  c->next_rid = 1;

  c->reqs_cap = c->lim.max_inflight_requests;
  if (c->reqs_cap < 1u) c->reqs_cap = 1u;
  if (c->reqs_cap > 4096u) c->reqs_cap = 4096u;
  c->reqs = (zi_http_req *)calloc((size_t)c->reqs_cap, sizeof(zi_http_req));
  if (!c->reqs) {
    free(c);
    return (zi_handle_t)ZI_E_OOM;
  }
  for (uint32_t i = 0; i < c->reqs_cap; i++) {
    c->reqs[i].fd = -1;
  }

  zi_handle_t h = zi_handle25_alloc_with_poll(&HTTP_OPS, &HTTP_POLL_OPS, c, ZI_H_READABLE | ZI_H_WRITABLE | ZI_H_ENDABLE);
  if (h < 3) {
    http_end(c);
    return (zi_handle_t)ZI_E_INTERNAL;
  }
  return h;
}
