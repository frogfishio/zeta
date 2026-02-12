#include "zi_telemetry.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  char *out;
  size_t cap;
  size_t len;
} buf;

static int b_putc(buf *b, char ch) {
  if (b->len + 1 >= b->cap) return 0;
  b->out[b->len++] = ch;
  b->out[b->len] = '\0';
  return 1;
}

static int b_puts(buf *b, const char *s) {
  if (!s) s = "";
  size_t n = strlen(s);
  if (b->len + n >= b->cap) return 0;
  memcpy(b->out + b->len, s, n);
  b->len += n;
  b->out[b->len] = '\0';
  return 1;
}

static int b_put_u64(buf *b, uint64_t v) {
  char tmp[32];
  int n = 0;
  if (v == 0) {
    tmp[n++] = '0';
  } else {
    char rev[32];
    int rn = 0;
    while (v > 0 && rn < (int)sizeof(rev)) {
      rev[rn++] = (char)('0' + (v % 10));
      v /= 10;
    }
    while (rn > 0) tmp[n++] = rev[--rn];
  }
  tmp[n] = '\0';
  return b_puts(b, tmp);
}

static int b_put_json_string_bytes(buf *b, const uint8_t *s, uint32_t n) {
  if (!b_putc(b, '"')) return 0;
  for (uint32_t i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)s[i];
    switch (ch) {
      case '"': if (!b_puts(b, "\\\"")) return 0; break;
      case '\\': if (!b_puts(b, "\\\\")) return 0; break;
      case '\b': if (!b_puts(b, "\\b")) return 0; break;
      case '\f': if (!b_puts(b, "\\f")) return 0; break;
      case '\n': if (!b_puts(b, "\\n")) return 0; break;
      case '\r': if (!b_puts(b, "\\r")) return 0; break;
      case '\t': if (!b_puts(b, "\\t")) return 0; break;
      default:
        if (ch < 0x20 || ch == 0x7F) {
          static const char hex[] = "0123456789abcdef";
          char esc[7] = {'\\', 'u', '0', '0', hex[(ch >> 4) & 0xF], hex[ch & 0xF], 0};
          if (!b_puts(b, esc)) return 0;
        } else {
          if (!b_putc(b, (char)ch)) return 0;
        }
        break;
    }
  }
  return b_putc(b, '"');
}

static int b_put_key(buf *b, const char *k) {
  // keys are ASCII literals
  if (!b_putc(b, '"')) return 0;
  if (!b_puts(b, k)) return 0;
  if (!b_puts(b, "\":")) return 0;
  return 1;
}

static int is_ascii_space(unsigned char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
}

static int body_looks_like_json(const uint8_t *body, uint32_t body_len) {
  if (!body || body_len == 0) return 0;
  uint32_t i = 0;
  while (i < body_len && is_ascii_space(body[i])) i++;
  if (i >= body_len) return 0;
  unsigned char ch = body[i];
  if (ch == '{' || ch == '[' || ch == '"') return 1;
  if (ch == '-' || (ch >= '0' && ch <= '9')) return 1;
  if (ch == 't' || ch == 'f' || ch == 'n') return 1; // true/false/null
  return 0;
}

size_t zi_telemetry_format_jsonl(const zi_telemetry_clock *clock, const uint8_t *topic,
                                uint32_t topic_len, const uint8_t *body,
                                uint32_t body_len, char *out, size_t out_cap) {
  if (!out || out_cap == 0) return 0;
  buf b = {.out = out, .cap = out_cap, .len = 0};
  out[0] = '\0';

  uint64_t ts = clock ? clock->ts_ms : 0;

  if (!b_putc(&b, '{')) return 0;

  if (!b_put_key(&b, "ts")) return 0;
  if (!b_put_u64(&b, ts)) return 0;

  if (!b_putc(&b, ',')) return 0;
  if (!b_put_key(&b, "topic")) return 0;
  if (!topic) {
    static const uint8_t empty[] = "";
    if (!b_put_json_string_bytes(&b, empty, 0)) return 0;
  } else {
    if (!b_put_json_string_bytes(&b, topic, topic_len)) return 0;
  }

  if (!b_putc(&b, ',')) return 0;
  if (!b_put_key(&b, "body")) return 0;

  if (body_looks_like_json(body, body_len)) {
    // Best-effort raw embed (caller is responsible for well-formed JSON).
    if (b.len + body_len + 2 >= b.cap) return 0;
    memcpy(b.out + b.len, body, body_len);
    b.len += body_len;
    b.out[b.len] = '\0';
  } else {
    if (!body) {
      static const uint8_t empty[] = "";
      if (!b_put_json_string_bytes(&b, empty, 0)) return 0;
    } else {
      if (!b_put_json_string_bytes(&b, body, body_len)) return 0;
    }
  }

  if (!b_putc(&b, '}')) return 0;
  if (!b_putc(&b, '\n')) return 0;

  return b.len;
}

int zi_telemetry_stderr_jsonl(const zi_telemetry_clock *clock, const uint8_t *topic,
                             uint32_t topic_len, const uint8_t *body,
                             uint32_t body_len) {
  char line[2048];
  size_t n = zi_telemetry_format_jsonl(clock, topic, topic_len, body, body_len, line,
                                       sizeof(line));
  if (!n) return 0;
  size_t w = fwrite(line, 1, n, stderr);
  (void)fflush(stderr);
  return w == n;
}
