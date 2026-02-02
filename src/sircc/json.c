// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "json.h"
#include "sircc.h"

#include <ctype.h>
#include <string.h>

typedef struct Parser {
  Arena* arena;
  const char* s;
  size_t i;
  JsonError* err;
} Parser;

static void set_err(Parser* p, const char* msg) {
  if (p->err && !p->err->msg) {
    p->err->offset = p->i;
    p->err->msg = msg;
  }
}

static void skip_ws(Parser* p) {
  while (p->s[p->i] && (p->s[p->i] == ' ' || p->s[p->i] == '\n' || p->s[p->i] == '\r' ||
                        p->s[p->i] == '\t')) {
    p->i++;
  }
}

static bool consume(Parser* p, char c) {
  if (p->s[p->i] == c) {
    p->i++;
    return true;
  }
  return false;
}

static JsonValue* make(Parser* p, JsonType t) {
  JsonValue* v = (JsonValue*)arena_alloc(p->arena, sizeof(JsonValue));
  if (!v) {
    set_err(p, "out of memory");
    return NULL;
  }
  v->type = t;
  return v;
}

static bool parse_value(Parser* p, JsonValue** out);

static bool parse_literal(Parser* p, const char* lit) {
  size_t n = strlen(lit);
  if (strncmp(p->s + p->i, lit, n) == 0) {
    p->i += n;
    return true;
  }
  return false;
}

static bool parse_number(Parser* p, JsonValue** out) {
  size_t start = p->i;
  if (p->s[p->i] == '-') p->i++;
  if (!isdigit((unsigned char)p->s[p->i])) {
    set_err(p, "expected digit");
    return false;
  }
  while (isdigit((unsigned char)p->s[p->i])) p->i++;

  // We only support integer numbers for now.
  size_t len = p->i - start;
  if (len == 0 || len > 32) {
    set_err(p, "invalid number");
    return false;
  }
  char buf[64];
  memcpy(buf, p->s + start, len);
  buf[len] = 0;

  char* end = NULL;
  long long v = strtoll(buf, &end, 10);
  if (!end || *end != 0) {
    set_err(p, "invalid integer");
    return false;
  }

  JsonValue* num = make(p, JSON_NUMBER);
  if (!num) return false;
  num->v.i = (int64_t)v;
  *out = num;
  return true;
}

static bool parse_string(Parser* p, JsonValue** out) {
  if (!consume(p, '"')) {
    set_err(p, "expected '\"'");
    return false;
  }

  // Decode into a temporary buffer and then arena-dup.
  size_t cap = 64;
  size_t len = 0;
  char* tmp = (char*)arena_alloc(p->arena, cap);
  if (!tmp) return false;

  while (p->s[p->i]) {
    char c = p->s[p->i++];
    if (c == '"') break;
    if (c == '\\') {
      char e = p->s[p->i++];
      switch (e) {
        case '"': c = '"'; break;
        case '\\': c = '\\'; break;
        case '/': c = '/'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        case 'u': {
          // Minimal \uXXXX support: accept it but only preserve ASCII codepoints.
          unsigned v = 0;
          for (int k = 0; k < 4; k++) {
            char h = p->s[p->i++];
            if (!isxdigit((unsigned char)h)) {
              set_err(p, "invalid \\u escape");
              return false;
            }
            v <<= 4;
            if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f') v |= (unsigned)(10 + (h - 'a'));
            else v |= (unsigned)(10 + (h - 'A'));
          }
          c = (v <= 0x7F) ? (char)v : '?';
          break;
        }
        default:
          set_err(p, "invalid escape");
          return false;
      }
    }

    if (len + 2 > cap) {
      size_t new_cap = cap * 2;
      char* bigger = (char*)arena_alloc(p->arena, new_cap);
      if (!bigger) return false;
      memcpy(bigger, tmp, len);
      tmp = bigger;
      cap = new_cap;
    }
    tmp[len++] = c;
  }

  tmp[len] = 0;
  char* s = arena_strdup(p->arena, tmp);
  if (!s) return false;

  JsonValue* str = make(p, JSON_STRING);
  if (!str) return false;
  str->v.s = s;
  *out = str;
  return true;
}

static bool parse_array(Parser* p, JsonValue** out) {
  if (!consume(p, '[')) {
    set_err(p, "expected '['");
    return false;
  }
  skip_ws(p);

  JsonValue* arrv = make(p, JSON_ARRAY);
  if (!arrv) return false;

  size_t cap = 4;
  JsonValue** items = (JsonValue**)arena_alloc(p->arena, cap * sizeof(JsonValue*));
  if (!items) return false;
  size_t len = 0;

  if (consume(p, ']')) {
    arrv->v.arr.items = items;
    arrv->v.arr.len = 0;
    *out = arrv;
    return true;
  }

  while (true) {
    skip_ws(p);
    JsonValue* item = NULL;
    if (!parse_value(p, &item)) return false;
    if (len == cap) {
      size_t new_cap = cap * 2;
      JsonValue** bigger = (JsonValue**)arena_alloc(p->arena, new_cap * sizeof(JsonValue*));
      if (!bigger) return false;
      memcpy(bigger, items, len * sizeof(JsonValue*));
      items = bigger;
      cap = new_cap;
    }
    items[len++] = item;
    skip_ws(p);
    if (consume(p, ']')) break;
    if (!consume(p, ',')) {
      set_err(p, "expected ',' or ']'");
      return false;
    }
  }

  arrv->v.arr.items = items;
  arrv->v.arr.len = len;
  *out = arrv;
  return true;
}

static bool parse_object(Parser* p, JsonValue** out) {
  if (!consume(p, '{')) {
    set_err(p, "expected '{'");
    return false;
  }
  skip_ws(p);

  JsonValue* objv = make(p, JSON_OBJECT);
  if (!objv) return false;

  size_t cap = 4;
  JsonObjectItem* items = (JsonObjectItem*)arena_alloc(p->arena, cap * sizeof(JsonObjectItem));
  if (!items) return false;
  size_t len = 0;

  if (consume(p, '}')) {
    objv->v.obj.items = items;
    objv->v.obj.len = 0;
    *out = objv;
    return true;
  }

  while (true) {
    skip_ws(p);
    JsonValue* keyv = NULL;
    if (!parse_string(p, &keyv)) return false;
    const char* key = keyv->v.s;
    skip_ws(p);
    if (!consume(p, ':')) {
      set_err(p, "expected ':'");
      return false;
    }
    skip_ws(p);
    JsonValue* val = NULL;
    if (!parse_value(p, &val)) return false;

    if (len == cap) {
      size_t new_cap = cap * 2;
      JsonObjectItem* bigger = (JsonObjectItem*)arena_alloc(p->arena, new_cap * sizeof(JsonObjectItem));
      if (!bigger) return false;
      memcpy(bigger, items, len * sizeof(JsonObjectItem));
      items = bigger;
      cap = new_cap;
    }
    items[len++] = (JsonObjectItem){.key = key, .value = val};

    skip_ws(p);
    if (consume(p, '}')) break;
    if (!consume(p, ',')) {
      set_err(p, "expected ',' or '}'");
      return false;
    }
  }

  objv->v.obj.items = items;
  objv->v.obj.len = len;
  *out = objv;
  return true;
}

static bool parse_value(Parser* p, JsonValue** out) {
  skip_ws(p);
  char c = p->s[p->i];
  if (!c) {
    set_err(p, "unexpected end of input");
    return false;
  }

  if (c == '{') return parse_object(p, out);
  if (c == '[') return parse_array(p, out);
  if (c == '"') return parse_string(p, out);
  if (c == '-' || isdigit((unsigned char)c)) return parse_number(p, out);
  if (parse_literal(p, "null")) {
    *out = make(p, JSON_NULL);
    return *out != NULL;
  }
  if (parse_literal(p, "true")) {
    JsonValue* b = make(p, JSON_BOOL);
    if (!b) return false;
    b->v.b = true;
    *out = b;
    return true;
  }
  if (parse_literal(p, "false")) {
    JsonValue* b = make(p, JSON_BOOL);
    if (!b) return false;
    b->v.b = false;
    *out = b;
    return true;
  }

  set_err(p, "unexpected token");
  return false;
}

bool json_parse(Arena* arena, const char* input, JsonValue** out, JsonError* err) {
  if (err) {
    err->msg = NULL;
    err->offset = 0;
  }
  Parser p = {.arena = arena, .s = input, .i = 0, .err = err};
  if (!parse_value(&p, out)) return false;
  skip_ws(&p);
  if (p.s[p.i] != 0) {
    set_err(&p, "trailing characters");
    return false;
  }
  return true;
}

static JsonValue* obj_get_impl(const JsonValue* obj, const char* key) {
  if (!obj || obj->type != JSON_OBJECT) return NULL;
  for (size_t i = 0; i < obj->v.obj.len; i++) {
    if (strcmp(obj->v.obj.items[i].key, key) == 0) return obj->v.obj.items[i].value;
  }
  return NULL;
}

JsonValue* json_obj_get(const JsonValue* obj, const char* key) {
  return obj_get_impl(obj, key);
}

const char* json_get_string(const JsonValue* v) {
  if (!v || v->type != JSON_STRING) return NULL;
  return v->v.s;
}

bool json_get_i64(const JsonValue* v, int64_t* out) {
  if (!v || v->type != JSON_NUMBER) return false;
  if (out) *out = v->v.i;
  return true;
}

bool json_is_object(const JsonValue* v) {
  return v && v->type == JSON_OBJECT;
}

bool json_is_array(const JsonValue* v) {
  return v && v->type == JSON_ARRAY;
}

