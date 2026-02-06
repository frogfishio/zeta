#include "zi_problem.h"

#include <string.h>

static const char *error_id(zi_problem_error e) {
  switch (e) {
    case ZI_ERR_VALIDATION_ERROR: return "validation_error";
    case ZI_ERR_INVALID_REQUEST: return "invalid_request";
    case ZI_ERR_ALREADY_EXISTS: return "already_exists";
    case ZI_ERR_INVALID_TOKEN: return "invalid_token";
    case ZI_ERR_TOKEN_EXPIRED: return "token_expired";
    case ZI_ERR_AUTH_ERROR: return "auth_error";
    case ZI_ERR_INSUFFICIENT_SCOPE: return "insufficient_scope";
    case ZI_ERR_NOT_FOUND: return "not_found";
    case ZI_ERR_UNSUPPORTED_METHOD: return "unsupported_method";
    case ZI_ERR_SYSTEM_ERROR: return "system_error";
    case ZI_ERR_CONFIGURATION_ERROR: return "configuration_error";
    case ZI_ERR_SERVICE_ERROR: return "service_error";
    case ZI_ERR_BAD_REQUEST: return "bad_request";
    case ZI_ERR_PAYMENT_REQUIRED: return "payment_required";
    case ZI_ERR_CONFLICT: return "conflict";
    case ZI_ERR_UNAUTHORIZED: return "unauthorized";
    case ZI_ERR_FORBIDDEN: return "forbidden";
    case ZI_ERR_TOO_MANY_REQUESTS: return "too_many_requests";
    case ZI_ERR_NOT_IMPLEMENTED: return "not_implemented";
    case ZI_ERR_BAD_GATEWAY: return "bad_gateway";
    case ZI_ERR_SERVICE_UNAVAILABLE: return "service_unavailable";
    case ZI_ERR_GATEWAY_TIMEOUT: return "gateway_timeout";
    default: return "service_error";
  }
}

const char *zi_problem_error_id(zi_problem_error e) {
  return error_id(e);
}

uint32_t zi_problem_status(zi_problem_error e) {
  switch (e) {
    case ZI_ERR_BAD_REQUEST: return 400;
    case ZI_ERR_VALIDATION_ERROR: return 400;
    case ZI_ERR_INVALID_REQUEST: return 400;
    case ZI_ERR_ALREADY_EXISTS: return 409;
    case ZI_ERR_INVALID_TOKEN: return 401;
    case ZI_ERR_TOKEN_EXPIRED: return 401;
    case ZI_ERR_UNAUTHORIZED: return 401;
    case ZI_ERR_AUTH_ERROR: return 401;
    case ZI_ERR_PAYMENT_REQUIRED: return 402;
    case ZI_ERR_FORBIDDEN: return 403;
    case ZI_ERR_INSUFFICIENT_SCOPE: return 403;
    case ZI_ERR_NOT_FOUND: return 404;
    case ZI_ERR_UNSUPPORTED_METHOD: return 405;
    case ZI_ERR_CONFLICT: return 409;
    case ZI_ERR_TOO_MANY_REQUESTS: return 429;
    case ZI_ERR_SYSTEM_ERROR: return 500;
    case ZI_ERR_CONFIGURATION_ERROR: return 500;
    case ZI_ERR_SERVICE_ERROR: return 500;
    case ZI_ERR_NOT_IMPLEMENTED: return 501;
    case ZI_ERR_BAD_GATEWAY: return 502;
    case ZI_ERR_SERVICE_UNAVAILABLE: return 503;
    case ZI_ERR_GATEWAY_TIMEOUT: return 504;
    default: return 500;
  }
}

// Keep a stable title table to avoid runtime string building.
static const char *error_title(zi_problem_error e) {
  switch (e) {
    case ZI_ERR_VALIDATION_ERROR: return "Validation Error";
    case ZI_ERR_INVALID_REQUEST: return "Invalid Request";
    case ZI_ERR_ALREADY_EXISTS: return "Already Exists";
    case ZI_ERR_INVALID_TOKEN: return "Invalid Token";
    case ZI_ERR_TOKEN_EXPIRED: return "Token Expired";
    case ZI_ERR_AUTH_ERROR: return "Auth Error";
    case ZI_ERR_INSUFFICIENT_SCOPE: return "Insufficient Scope";
    case ZI_ERR_NOT_FOUND: return "Not Found";
    case ZI_ERR_UNSUPPORTED_METHOD: return "Unsupported Method";
    case ZI_ERR_SYSTEM_ERROR: return "System Error";
    case ZI_ERR_CONFIGURATION_ERROR: return "Configuration Error";
    case ZI_ERR_SERVICE_ERROR: return "Service Error";
    case ZI_ERR_BAD_REQUEST: return "Bad Request";
    case ZI_ERR_PAYMENT_REQUIRED: return "Payment Required";
    case ZI_ERR_CONFLICT: return "Conflict";
    case ZI_ERR_UNAUTHORIZED: return "Unauthorized";
    case ZI_ERR_FORBIDDEN: return "Forbidden";
    case ZI_ERR_TOO_MANY_REQUESTS: return "Too Many Requests";
    case ZI_ERR_NOT_IMPLEMENTED: return "Not Implemented";
    case ZI_ERR_BAD_GATEWAY: return "Bad Gateway";
    case ZI_ERR_SERVICE_UNAVAILABLE: return "Service Unavailable";
    case ZI_ERR_GATEWAY_TIMEOUT: return "Gateway Timeout";
    default: return "Service Error";
  }
}

const char *zi_problem_title(zi_problem_error e) {
  return error_title(e);
}

void zi_problem_init(zi_problem_details *p, zi_problem_error e, const char *detail,
                     const char *trace) {
  if (!p) return;
  p->error = e;
  p->status = zi_problem_status(e);
  p->detail = detail ? detail : "An error occurred";
  p->trace = trace;
  p->chain_count = 0;
}

int zi_problem_chain_push(zi_problem_details *p, zi_problem_error e,
                          const char *error_description, const char *stage,
                          uint64_t at_ms) {
  if (!p) return 0;
  if (p->chain_count >= (uint32_t)ZI_PROBLEM_CHAIN_MAX) return 0;
  if (!error_description) error_description = "";
  zi_problem_chain_item *it = &p->chain[p->chain_count++];
  it->error = e;
  it->error_description = error_description;
  it->stage = stage;
  it->at_ms = at_ms;
  return 1;
}

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

static int b_put_u32(buf *b, uint32_t v) {
  char tmp[16];
  int n = 0;
  // minimal u32 formatting
  if (v == 0) {
    tmp[n++] = '0';
  } else {
    char rev[16];
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

static int b_put_json_string(buf *b, const char *s) {
  if (!b_putc(b, '"')) return 0;
  if (!s) s = "";
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    unsigned char ch = *p;
    switch (ch) {
      case '"': if (!b_puts(b, "\\\"")) return 0; break;
      case '\\': if (!b_puts(b, "\\\\")) return 0; break;
      case '\b': if (!b_puts(b, "\\b")) return 0; break;
      case '\f': if (!b_puts(b, "\\f")) return 0; break;
      case '\n': if (!b_puts(b, "\\n")) return 0; break;
      case '\r': if (!b_puts(b, "\\r")) return 0; break;
      case '\t': if (!b_puts(b, "\\t")) return 0; break;
      default:
        if (ch < 0x20) {
          // Emit as \u00XX
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
  if (!b_put_json_string(b, k)) return 0;
  return b_putc(b, ':');
}

size_t zi_problem_to_json(const zi_problem_details *p, char *out, size_t out_cap) {
  if (!p || !out || out_cap == 0) return 0;
  buf b = {.out = out, .cap = out_cap, .len = 0};
  out[0] = '\0';

  const char *id = zi_problem_error_id(p->error);
  const char *title = zi_problem_title(p->error);

  if (!b_putc(&b, '{')) return 0;

  // type
  if (!b_put_key(&b, "type")) return 0;
  if (!b_puts(&b, "\"urn:zi-error:")) return 0;
  if (!b_puts(&b, id)) return 0;
  if (!b_putc(&b, '"')) return 0;

  // title
  if (!b_putc(&b, ',')) return 0;
  if (!b_put_key(&b, "title")) return 0;
  if (!b_put_json_string(&b, title)) return 0;

  // status
  if (!b_putc(&b, ',')) return 0;
  if (!b_put_key(&b, "status")) return 0;
  if (!b_put_u32(&b, p->status)) return 0;

  // detail
  if (!b_putc(&b, ',')) return 0;
  if (!b_put_key(&b, "detail")) return 0;
  if (!b_put_json_string(&b, p->detail)) return 0;

  // trace (optional)
  if (p->trace && p->trace[0]) {
    if (!b_putc(&b, ',')) return 0;
    if (!b_put_key(&b, "trace")) return 0;
    if (!b_put_json_string(&b, p->trace)) return 0;
  }

  // chain
  if (!b_putc(&b, ',')) return 0;
  if (!b_put_key(&b, "chain")) return 0;
  if (!b_putc(&b, '[')) return 0;
  for (uint32_t i = 0; i < p->chain_count; i++) {
    const zi_problem_chain_item *it = &p->chain[i];
    if (i) {
      if (!b_putc(&b, ',')) return 0;
    }
    if (!b_putc(&b, '{')) return 0;

    if (!b_put_key(&b, "error")) return 0;
    if (!b_put_json_string(&b, zi_problem_error_id(it->error))) return 0;

    if (!b_putc(&b, ',')) return 0;
    if (!b_put_key(&b, "error_description")) return 0;
    if (!b_put_json_string(&b, it->error_description)) return 0;

    if (it->stage && it->stage[0]) {
      if (!b_putc(&b, ',')) return 0;
      if (!b_put_key(&b, "stage")) return 0;
      if (!b_put_json_string(&b, it->stage)) return 0;
    }

    if (!b_putc(&b, ',')) return 0;
    if (!b_put_key(&b, "at")) return 0;
    if (!b_put_u64(&b, it->at_ms)) return 0;

    if (!b_putc(&b, '}')) return 0;
  }
  if (!b_putc(&b, ']')) return 0;

  if (!b_putc(&b, '}')) return 0;

  return b.len;
}
