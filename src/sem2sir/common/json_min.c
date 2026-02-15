#include "json_min.h"

#include <stdlib.h>
#include <string.h>

static bool json_peek_non_ws(GritJsonCursor *c, char *out) {
    if (!grit_json_skip_ws(c)) return false;
    if (c->p >= c->end) return false;
    if (out) *out = *c->p;
    return true;
}

static bool is_ws(char ch) {
    return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t';
}

bool grit_json_skip_ws(GritJsonCursor *c) {
    while (c->p < c->end && is_ws(*c->p)) {
        c->p++;
    }
    return true;
}

static bool consume(GritJsonCursor *c, char ch) {
    if (c->p >= c->end || *c->p != ch) return false;
    c->p++;
    return true;
}

static bool peek(GritJsonCursor *c, char *out) {
    if (c->p >= c->end) return false;
    *out = *c->p;
    return true;
}

static bool parse_hex4(GritJsonCursor *c, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (c->p >= c->end) return false;
        char ch = *c->p++;
        v <<= 4;
        if (ch >= '0' && ch <= '9') v |= (uint32_t)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= (uint32_t)(10 + (ch - 'a'));
        else if (ch >= 'A' && ch <= 'F') v |= (uint32_t)(10 + (ch - 'A'));
        else return false;
    }
    *out = v;
    return true;
}

static bool utf8_append(uint32_t cp, char **buf, size_t *len, size_t *cap) {
    // Minimal UTF-8 encoding for codepoints <= 0x10FFFF.
    unsigned char tmp[4];
    size_t n = 0;
    if (cp <= 0x7F) {
        tmp[0] = (unsigned char)cp;
        n = 1;
    } else if (cp <= 0x7FF) {
        tmp[0] = 0xC0 | (unsigned char)(cp >> 6);
        tmp[1] = 0x80 | (unsigned char)(cp & 0x3F);
        n = 2;
    } else if (cp <= 0xFFFF) {
        tmp[0] = 0xE0 | (unsigned char)(cp >> 12);
        tmp[1] = 0x80 | (unsigned char)((cp >> 6) & 0x3F);
        tmp[2] = 0x80 | (unsigned char)(cp & 0x3F);
        n = 3;
    } else if (cp <= 0x10FFFF) {
        tmp[0] = 0xF0 | (unsigned char)(cp >> 18);
        tmp[1] = 0x80 | (unsigned char)((cp >> 12) & 0x3F);
        tmp[2] = 0x80 | (unsigned char)((cp >> 6) & 0x3F);
        tmp[3] = 0x80 | (unsigned char)(cp & 0x3F);
        n = 4;
    } else {
        return false;
    }

    if (*len + n + 1 > *cap) {
        size_t new_cap = (*cap == 0) ? 64 : (*cap * 2);
        while (*len + n + 1 > new_cap) new_cap *= 2;
        char *new_buf = (char *)realloc(*buf, new_cap);
        if (!new_buf) return false;
        *buf = new_buf;
        *cap = new_cap;
    }

    memcpy(*buf + *len, tmp, n);
    *len += n;
    (*buf)[*len] = '\0';
    return true;
}

static bool buf_append_byte(unsigned char b, char **buf, size_t *len, size_t *cap) {
    if (*len + 2 > *cap) {
        size_t new_cap = (*cap == 0) ? 64 : (*cap * 2);
        while (*len + 2 > new_cap) new_cap *= 2;
        char *new_buf = (char *)realloc(*buf, new_cap);
        if (!new_buf) return false;
        *buf = new_buf;
        *cap = new_cap;
    }
    (*buf)[*len] = (char)b;
    *len += 1;
    (*buf)[*len] = '\0';
    return true;
}

bool grit_json_parse_string_alloc(GritJsonCursor *c, char **out) {
    *out = NULL;

    grit_json_skip_ws(c);
    if (!consume(c, '"')) return false;

    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;

    while (c->p < c->end) {
        char ch = *c->p++;
        if (ch == '"') {
            if (!buf) {
                buf = (char *)malloc(1);
                if (!buf) return false;
                buf[0] = '\0';
            }
            *out = buf;
            return true;
        }

        if ((unsigned char)ch < 0x20) {
            free(buf);
            return false;
        }

        if (ch == '\\') {
            if (c->p >= c->end) {
                free(buf);
                return false;
            }
            char esc = *c->p++;
            switch (esc) {
                case '"': if (!buf_append_byte('"', &buf, &len, &cap)) { free(buf); return false; } break;
                case '\\': if (!buf_append_byte('\\', &buf, &len, &cap)) { free(buf); return false; } break;
                case '/': if (!buf_append_byte('/', &buf, &len, &cap)) { free(buf); return false; } break;
                case 'b': if (!buf_append_byte('\b', &buf, &len, &cap)) { free(buf); return false; } break;
                case 'f': if (!buf_append_byte('\f', &buf, &len, &cap)) { free(buf); return false; } break;
                case 'n': if (!buf_append_byte('\n', &buf, &len, &cap)) { free(buf); return false; } break;
                case 'r': if (!buf_append_byte('\r', &buf, &len, &cap)) { free(buf); return false; } break;
                case 't': if (!buf_append_byte('\t', &buf, &len, &cap)) { free(buf); return false; } break;
                case 'u': {
                    uint32_t u = 0;
                    if (!parse_hex4(c, &u)) { free(buf); return false; }

                    // Surrogate pairs: \uD800..\uDBFF \uDC00..\uDFFF
                    if (u >= 0xD800 && u <= 0xDBFF) {
                        // Expect another \uXXXX
                        if (c->p + 2 > c->end || c->p[0] != '\\' || c->p[1] != 'u') { free(buf); return false; }
                        c->p += 2;
                        uint32_t v = 0;
                        if (!parse_hex4(c, &v)) { free(buf); return false; }
                        if (v < 0xDC00 || v > 0xDFFF) { free(buf); return false; }
                        uint32_t cp = 0x10000 + (((u - 0xD800) << 10) | (v - 0xDC00));
                        if (!utf8_append(cp, &buf, &len, &cap)) { free(buf); return false; }
                    } else if (u >= 0xDC00 && u <= 0xDFFF) {
                        // Lone low surrogate is invalid.
                        free(buf);
                        return false;
                    } else {
                        if (!utf8_append(u, &buf, &len, &cap)) { free(buf); return false; }
                    }
                    break;
                }
                default:
                    free(buf);
                    return false;
            }
            continue;
        }

        // JSON text is UTF-8. For non-escaped bytes (including bytes >= 0x80),
        // preserve the original byte sequence rather than re-encoding each byte.
        if (!buf_append_byte((unsigned char)ch, &buf, &len, &cap)) {
            free(buf);
            return false;
        }
    }

    free(buf);
    return false;
}

static bool skip_number(GritJsonCursor *c) {
    // JSON number grammar (simplified): -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
    grit_json_skip_ws(c);
    const char *p = c->p;

    if (p < c->end && *p == '-') p++;
    if (p >= c->end) return false;

    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (p < c->end && *p >= '0' && *p <= '9') p++;
    } else {
        return false;
    }

    if (p < c->end && *p == '.') {
        p++;
        if (p >= c->end || *p < '0' || *p > '9') return false;
        while (p < c->end && *p >= '0' && *p <= '9') p++;
    }

    if (p < c->end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < c->end && (*p == '+' || *p == '-')) p++;
        if (p >= c->end || *p < '0' || *p > '9') return false;
        while (p < c->end && *p >= '0' && *p <= '9') p++;
    }

    c->p = p;
    return true;
}

static bool skip_literal(GritJsonCursor *c, const char *lit) {
    grit_json_skip_ws(c);
    size_t n = strlen(lit);
    if ((size_t)(c->end - c->p) < n) return false;
    if (memcmp(c->p, lit, n) != 0) return false;
    c->p += n;
    return true;
}

static bool skip_array(GritJsonCursor *c) {
    grit_json_skip_ws(c);
    if (!consume(c, '[')) return false;
    grit_json_skip_ws(c);

    char ch = 0;
    if (peek(c, &ch) && ch == ']') {
        c->p++;
        return true;
    }

    for (;;) {
        if (!grit_json_skip_value(c)) return false;
        grit_json_skip_ws(c);

        if (!peek(c, &ch)) return false;
        if (ch == ',') {
            c->p++;
            continue;
        }
        if (ch == ']') {
            c->p++;
            return true;
        }
        return false;
    }
}

static bool skip_object(GritJsonCursor *c) {
    grit_json_skip_ws(c);
    if (!consume(c, '{')) return false;
    grit_json_skip_ws(c);

    char ch = 0;
    if (peek(c, &ch) && ch == '}') {
        c->p++;
        return true;
    }

    for (;;) {
        char *k = NULL;
        if (!grit_json_parse_string_alloc(c, &k)) return false;
        free(k);

        grit_json_skip_ws(c);
        if (!consume(c, ':')) return false;

        if (!grit_json_skip_value(c)) return false;

        grit_json_skip_ws(c);
        if (!peek(c, &ch)) return false;
        if (ch == ',') {
            c->p++;
            continue;
        }
        if (ch == '}') {
            c->p++;
            return true;
        }
        return false;
    }
}

bool grit_json_skip_value(GritJsonCursor *c) {
    grit_json_skip_ws(c);
    if (c->p >= c->end) return false;

    char ch = *c->p;
    if (ch == '"') {
        char *s = NULL;
        bool ok = grit_json_parse_string_alloc(c, &s);
        free(s);
        return ok;
    }
    if (ch == '{') return skip_object(c);
    if (ch == '[') return skip_array(c);
    if (ch == 't') return skip_literal(c, "true");
    if (ch == 'f') return skip_literal(c, "false");
    if (ch == 'n') return skip_literal(c, "null");
    if (ch == '-' || (ch >= '0' && ch <= '9')) return skip_number(c);

    return false;
}

bool grit_json_consume_char(GritJsonCursor *c, char expected) {
    char ch = 0;
    if (!json_peek_non_ws(c, &ch)) return false;
    if (ch != expected) return false;
    c->p++;
    return true;
}

bool grit_json_parse_int64(GritJsonCursor *c, int64_t *out) {
    char ch = 0;
    if (!json_peek_non_ws(c, &ch)) return false;

    const char *p = c->p;
    int sign = 1;
    if (p < c->end && *p == '-') {
        sign = -1;
        p++;
    }

    if (p >= c->end || *p < '0' || *p > '9') {
        return false;
    }

    int64_t v = 0;
    while (p < c->end && *p >= '0' && *p <= '9') {
        int64_t d = (int64_t)(*p - '0');
        // simple overflow check
        if (v > (INT64_MAX - d) / 10) {
            return false;
        }
        v = v * 10 + d;
        p++;
    }

    // For now we only support integer tokens; reject decimal/exponent.
    if (p < c->end && (*p == '.' || *p == 'e' || *p == 'E')) {
        return false;
    }

    c->p = p;
    if (out) *out = (sign < 0) ? -v : v;
    return true;
}

bool grit_json_parse_bool(GritJsonCursor *c, bool *out) {
    char ch = 0;
    if (!json_peek_non_ws(c, &ch)) return false;

    size_t remain = (size_t)(c->end - c->p);
    if (remain >= 4 && memcmp(c->p, "true", 4) == 0) {
        c->p += 4;
        if (out) *out = true;
        return true;
    }
    if (remain >= 5 && memcmp(c->p, "false", 5) == 0) {
        c->p += 5;
        if (out) *out = false;
        return true;
    }
    return false;
}

bool grit_json_get_root_string_field_alloc(const char *buf, size_t len, const char *field, char **out) {
    *out = NULL;

    GritJsonCursor c = grit_json_cursor(buf, len);
    grit_json_skip_ws(&c);
    if (!consume(&c, '{')) return false;

    grit_json_skip_ws(&c);
    char ch = 0;
    if (peek(&c, &ch) && ch == '}') {
        return false;
    }

    for (;;) {
        char *k = NULL;
        if (!grit_json_parse_string_alloc(&c, &k)) return false;

        grit_json_skip_ws(&c);
        if (!consume(&c, ':')) {
            free(k);
            return false;
        }

        if (strcmp(k, field) == 0) {
            free(k);
            char *v = NULL;
            if (!grit_json_parse_string_alloc(&c, &v)) {
                return false;
            }
            *out = v;
            return true;
        }

        free(k);
        if (!grit_json_skip_value(&c)) return false;

        grit_json_skip_ws(&c);
        if (!peek(&c, &ch)) return false;
        if (ch == ',') {
            c.p++;
            continue;
        }
        if (ch == '}') {
            return false;
        }
        return false;
    }
}
