// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sirc_emit.h"
#include "version.h"

// Flex/Bison
extern FILE* yyin;
int yyparse(void);
void yyrestart(FILE* input_file);

typedef struct TypeEntry {
  char* key;      // canonical key, e.g. "prim:i32", "ptr:<id>", "fn:(a,b)->r"
  int64_t id;
} TypeEntry;

typedef struct FnEntry {
  char* name;
  int64_t sig_type; // type id of fn signature
  int64_t ret_type; // type id of return
  size_t param_count;
  bool is_extern;
  bool is_defined;
  int64_t fn_node; // node id of "fn" node (when defined)
} FnEntry;

typedef struct NamedTypeEntry {
  char* name;
  int64_t type_id;
} NamedTypeEntry;

typedef struct LocalEntry {
  char* name;
  int64_t ty;
} LocalEntry;

typedef enum sirc_ids_mode {
  SIRC_IDS_NUMERIC = 0,
  SIRC_IDS_STRING = 1,
} sirc_ids_mode_t;

typedef enum sirc_emit_src_mode {
  SIRC_EMIT_SRC_NONE = 0,
  SIRC_EMIT_SRC_LOC = 1,
  SIRC_EMIT_SRC_SRCREF = 2,
  SIRC_EMIT_SRC_BOTH = 3,
} sirc_emit_src_mode_t;

typedef struct SrcMapEntry {
  int line;
  int col;
  int64_t id_num;
  char* id_str;
  bool emitted;
} SrcMapEntry;

typedef struct Emitter {
  FILE* out;
  const char* input_path;

  sirc_ids_mode_t ids_mode;
  sirc_emit_src_mode_t emit_src;

  int64_t next_type_id;
  int64_t next_node_id;

  // For string-id output: internal numeric id -> stable-ish string id.
  char** node_id_by_id;
  size_t node_id_cap;
  char** type_id_by_id;
  size_t type_id_cap;

  // src records (only used when emit_src includes src_ref)
  SrcMapEntry* srcs;
  size_t srcs_len;
  size_t srcs_cap;
  int64_t next_src_id;

  // Source-scoped node-id generation (multiple emitted nodes can originate from one .sir line).
  int last_id_line;
  uint32_t line_id_seq;

  char** node_name_by_id;
  size_t node_name_cap;

  TypeEntry* types;
  size_t types_len;
  size_t types_cap;

  FnEntry* fns;
  size_t fns_len;
  size_t fns_cap;

  NamedTypeEntry* named_types;
  size_t named_types_len;
  size_t named_types_cap;

  char** features;
  size_t features_len;
  size_t features_cap;

  // Per-function locals: name -> type_ref for name nodes.
  LocalEntry* locals;
  size_t locals_len;
  size_t locals_cap;

  // CFG block name -> reserved node id (function-local; reset via sirc_cfg_begin)
  struct {
    char* name;
    int64_t id;
    bool has_params;
  } * blocks;
  size_t blocks_len;
  size_t blocks_cap;

  // unit meta
  char* unit;
  char* target;
} Emitter;

static Emitter g_emit = {0};

static void record_type_out_id(int64_t id, const char* key);
static void emit_node_id_value(int64_t id);
static void emit_type_id_value(int64_t id);
static void emit_node_ref_obj(int64_t id);
static void emit_type_ref_obj(int64_t ty);
static char* xstrdup(const char* s);
static void json_write_escaped(FILE* out, const char* s);
static void locals_clear(void);
static void emit_record_src_loc_trailer(void);

int sirc_last_line = 1;
int sirc_last_col = 1;
char sirc_last_tok[64] = {0};

const char* sirc_input_path(void) { return g_emit.input_path ? g_emit.input_path : "<input>"; }

typedef enum sirc_diag_format {
  SIRC_DIAG_TEXT = 0,
  SIRC_DIAG_JSON = 1,
} sirc_diag_format_t;

typedef struct sirc_diag {
  bool set;
  const char* code;
  const char* path;
  int line;
  int col;
  char* msg;
  char tok[64];
} sirc_diag_t;

static sirc_diag_t g_diag = {0};
static sirc_diag_format_t g_diag_format = SIRC_DIAG_TEXT;
static bool g_diag_all = false;
static bool g_strict = false;

typedef struct sirc_diag_entry {
  const char* code;
  char* path;
  int line;
  int col;
  char* msg;
  char tok[64];
} sirc_diag_entry_t;

static sirc_diag_entry_t* g_diags = NULL;
static size_t g_diags_len = 0;
static size_t g_diags_cap = 0;

static void diag_reset(void) {
  free(g_diag.msg);
  memset(&g_diag, 0, sizeof(g_diag));
}

static void diags_clear(void) {
  for (size_t i = 0; i < g_diags_len; i++) {
    free(g_diags[i].path);
    free(g_diags[i].msg);
  }
  free(g_diags);
  g_diags = NULL;
  g_diags_len = 0;
  g_diags_cap = 0;
  diag_reset();
}

static char* read_source_line(const char* path, int line_no) {
  if (!path || !path[0]) return NULL;
  if (strcmp(path, "<input>") == 0) return NULL;
  if (line_no <= 0) return NULL;
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;

  char* line = NULL;
  size_t cap = 0;
  size_t len = 0;
  int ch = 0;
  int cur = 1;
  while ((ch = fgetc(f)) != EOF) {
    if (cur == line_no) {
      if (ch == '\n') break;
      if (cap < len + 2) {
        size_t ncap = cap ? cap * 2 : 256;
        char* np = (char*)realloc(line, ncap);
        if (!np) break;
        line = np;
        cap = ncap;
      }
      line[len++] = (char)ch;
    }
    if (ch == '\n') {
      if (cur >= line_no) break;
      cur++;
    }
  }
  if (line) line[len] = 0;
  fclose(f);
  return line;
}

static void diag_setf(const char* code, const char* fmt, ...) {
  if (g_diag_all) {
    if (g_diags_len == g_diags_cap) {
      g_diags_cap = g_diags_cap ? g_diags_cap * 2 : 8;
      g_diags = (sirc_diag_entry_t*)realloc(g_diags, g_diags_cap * sizeof(sirc_diag_entry_t));
      if (!g_diags) {
        // If we can't allocate, fall back to single-diag behavior.
        g_diag_all = false;
      }
    }
    if (g_diag_all) {
      sirc_diag_entry_t* e = &g_diags[g_diags_len++];
      memset(e, 0, sizeof(*e));
      e->code = code ? code : "sirc.error";
      e->path = xstrdup(sirc_input_path());
      e->line = sirc_last_line;
      e->col = sirc_last_col;
      memcpy(e->tok, sirc_last_tok, sizeof(e->tok));

      va_list ap;
      va_start(ap, fmt);
      char tmp[2048];
      vsnprintf(tmp, sizeof(tmp), fmt, ap);
      va_end(ap);
      e->msg = xstrdup(tmp);
      return;
    }
  }

  if (g_diag.set) return;
  diag_reset();
  g_diag.set = true;
  g_diag.code = code ? code : "sirc.error";
  g_diag.path = sirc_input_path();
  g_diag.line = sirc_last_line;
  g_diag.col = sirc_last_col;
  memcpy(g_diag.tok, sirc_last_tok, sizeof(g_diag.tok));

  va_list ap;
  va_start(ap, fmt);
  char tmp[2048];
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  g_diag.msg = xstrdup(tmp);
}

static void diag_print_entry(const sirc_diag_entry_t* e) {
  if (!e) return;
  if (g_diag_format == SIRC_DIAG_JSON) {
    FILE* out = stderr;
    fputs("{\"k\":\"diag\",\"tool\":\"sirc\",\"code\":", out);
    json_write_escaped(out, e->code ? e->code : "sirc.error");
    fputs(",\"path\":", out);
    json_write_escaped(out, e->path ? e->path : "<input>");
    fprintf(out, ",\"line\":%d,\"col\":%d", e->line, e->col);
    if (e->msg) {
      fputs(",\"msg\":", out);
      json_write_escaped(out, e->msg);
    }
    if (e->tok[0]) {
      fputs(",\"near\":", out);
      json_write_escaped(out, e->tok);
    }
    fputs("}\n", out);
    return;
  }

  // Best-effort caret diagnostics (single line).
  // We keep this simple: show the source line if it can be read, then a caret at the column.
  // (Multi-line context and tab-aware caret alignment can be added later.)
  char* line = read_source_line(e->path, e->line);
  if (line) {
    fprintf(stderr, "  |\n");
    fprintf(stderr, "%4d | %s\n", e->line, line);
    fprintf(stderr, "  | ");
    int caret_col = e->col;
    if (caret_col < 1) caret_col = 1;
    for (int i = 1; i < caret_col; i++) fputc(' ', stderr);
    fputc('^', stderr);
    fputc('\n', stderr);
    free(line);
  }

  // Text mode (current style).
  fprintf(stderr, "%s:%d:%d: error: %s", e->path ? e->path : "<input>", e->line, e->col, e->msg ? e->msg : "error");
  if (e->tok[0]) fprintf(stderr, " (near '%s')", e->tok);
  fputc('\n', stderr);
  if (e->code) fprintf(stderr, "  code: %s\n", e->code);
}

static void diag_print_one(void) {
  if (!g_diag.set) return;
  sirc_diag_entry_t e = {0};
  e.code = g_diag.code;
  e.path = (char*)g_diag.path;
  e.line = g_diag.line;
  e.col = g_diag.col;
  e.msg = g_diag.msg;
  memcpy(e.tok, g_diag.tok, sizeof(e.tok));
  diag_print_entry(&e);
}

static void diag_print_all(void) {
  if (g_diag_all && g_diags_len) {
    for (size_t i = 0; i < g_diags_len; i++) diag_print_entry(&g_diags[i]);
    return;
  }
  diag_print_one();
}

void yyerror(const char* s) {
  diag_setf("sirc.parse.syntax", "%s", s ? s : "syntax error");
}

static void die_at_last(const char* fmt, ...) {
  char tmp[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  diag_setf("sirc.error", "%s", tmp);
  diag_print_one();
  exit(1);
}

static void strict_failf(const char* code, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char tmp[2048];
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  diag_setf(code ? code : "sirc.strict", "%s", tmp);
  diag_print_one();
  exit(1);
}

static void* xmalloc(size_t n) {
  void* p = malloc(n ? n : 1);
  if (!p) die_at_last("sirc: out of memory");
  return p;
}

static void* xrealloc(void* p, size_t n) {
  void* q = realloc(p, n ? n : 1);
  if (!q) die_at_last("sirc: out of memory");
  return q;
}

static char* xstrdup(const char* s) {
  size_t n = s ? strlen(s) : 0;
  char* out = (char*)xmalloc(n + 1);
  if (n) memcpy(out, s, n);
  out[n] = 0;
  return out;
}

static void sirc_reset_compiler_state(void) {
  // Output + input path are managed by the caller.
  // ids_mode is managed by main().

  // Free per-id string ids.
  for (size_t i = 0; i < g_emit.node_id_cap; i++) free(g_emit.node_id_by_id ? g_emit.node_id_by_id[i] : NULL);
  free(g_emit.node_id_by_id);
  g_emit.node_id_by_id = NULL;
  g_emit.node_id_cap = 0;

  // type_id_by_id entries point into g_emit.types[*].key; only free the array.
  free(g_emit.type_id_by_id);
  g_emit.type_id_by_id = NULL;
  g_emit.type_id_cap = 0;

  // Free node name table (internal helper for alloca(type)).
  for (size_t i = 0; i < g_emit.node_name_cap; i++) free(g_emit.node_name_by_id ? g_emit.node_name_by_id[i] : NULL);
  free(g_emit.node_name_by_id);
  g_emit.node_name_by_id = NULL;
  g_emit.node_name_cap = 0;

  // Types.
  for (size_t i = 0; i < g_emit.types_len; i++) free(g_emit.types[i].key);
  free(g_emit.types);
  g_emit.types = NULL;
  g_emit.types_len = 0;
  g_emit.types_cap = 0;

  // Functions.
  for (size_t i = 0; i < g_emit.fns_len; i++) free(g_emit.fns[i].name);
  free(g_emit.fns);
  g_emit.fns = NULL;
  g_emit.fns_len = 0;
  g_emit.fns_cap = 0;

  // Named types.
  for (size_t i = 0; i < g_emit.named_types_len; i++) free(g_emit.named_types[i].name);
  free(g_emit.named_types);
  g_emit.named_types = NULL;
  g_emit.named_types_len = 0;
  g_emit.named_types_cap = 0;

  // Features.
  for (size_t i = 0; i < g_emit.features_len; i++) free(g_emit.features[i]);
  free(g_emit.features);
  g_emit.features = NULL;
  g_emit.features_len = 0;
  g_emit.features_cap = 0;

  // CFG blocks.
  for (size_t i = 0; i < g_emit.blocks_len; i++) free(g_emit.blocks[i].name);
  free(g_emit.blocks);
  g_emit.blocks = NULL;
  g_emit.blocks_len = 0;
  g_emit.blocks_cap = 0;

  // Locals.
  locals_clear();
  free(g_emit.locals);
  g_emit.locals = NULL;
  g_emit.locals_len = 0;
  g_emit.locals_cap = 0;

  // Unit meta.
  free(g_emit.unit);
  free(g_emit.target);
  g_emit.unit = NULL;
  g_emit.target = NULL;

  // Src map.
  for (size_t i = 0; i < g_emit.srcs_len; i++) free(g_emit.srcs[i].id_str);
  free(g_emit.srcs);
  g_emit.srcs = NULL;
  g_emit.srcs_len = 0;
  g_emit.srcs_cap = 0;
  g_emit.next_src_id = 1;

  g_emit.next_type_id = 1;
  g_emit.next_node_id = 10;
  g_emit.last_id_line = 0;
  g_emit.line_id_seq = 0;

  diags_clear();
}

static void json_write_escaped(FILE* out, const char* s) {
  fputc('"', out);
  for (const unsigned char* p = (const unsigned char*)s; p && *p; p++) {
    unsigned char c = *p;
    switch (c) {
      case '\\': fputs("\\\\", out); break;
      case '"': fputs("\\\"", out); break;
      case '\n': fputs("\\n", out); break;
      case '\r': fputs("\\r", out); break;
      case '\t': fputs("\\t", out); break;
      default:
        if (c < 0x20) {
          fprintf(out, "\\u%04x", (unsigned)c);
        } else {
          fputc((int)c, out);
        }
        break;
    }
  }
  fputc('"', out);
}

static void emitf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_emit.out, fmt, ap);
  va_end(ap);
}

static void emit_meta(void) {
  if (!g_emit.unit) g_emit.unit = xstrdup("unit");
  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"meta\",\"producer\":\"sirc\",\"unit\":");
  json_write_escaped(g_emit.out, g_emit.unit);
  if (g_emit.target || g_emit.features_len) {
    emitf(",\"ext\":{");
    bool any = false;
    if (g_emit.target) {
      emitf("\"target\":{\"triple\":");
      json_write_escaped(g_emit.out, g_emit.target);
      emitf("}");
      any = true;
    }
    if (g_emit.features_len) {
      if (any) emitf(",");
      emitf("\"features\":[");
      for (size_t i = 0; i < g_emit.features_len; i++) {
        if (i) emitf(",");
        json_write_escaped(g_emit.out, g_emit.features[i]);
      }
      emitf("]");
    }
    emitf("}");
  }
  emitf("}\n");
}

static const char* path_basename(const char* p) {
  if (!p) return NULL;
  const char* s = strrchr(p, '/');
  return s ? (s + 1) : p;
}

static void emit_loc_obj(void) {
  int line = sirc_last_line > 0 ? sirc_last_line : 1;
  int col = sirc_last_col > 0 ? sirc_last_col : 1;
  emitf("{\"line\":%d", line);
  if (col > 0) emitf(",\"col\":%d", col);
  const char* bn = path_basename(g_emit.input_path);
  if (bn && bn[0] && strcmp(bn, "<input>") != 0) {
    emitf(",\"unit\":");
    json_write_escaped(g_emit.out, bn);
  }
  emitf("}");
}

static SrcMapEntry* src_lookup(int line, int col) {
  for (size_t i = 0; i < g_emit.srcs_len; i++) {
    if (g_emit.srcs[i].line == line && g_emit.srcs[i].col == col) return &g_emit.srcs[i];
  }
  return NULL;
}

static SrcMapEntry* src_get_or_create(int line, int col) {
  if (line <= 0) line = 1;
  if (col <= 0) col = 1;
  SrcMapEntry* e = src_lookup(line, col);
  if (e) return e;
  if (g_emit.srcs_len == g_emit.srcs_cap) {
    g_emit.srcs_cap = g_emit.srcs_cap ? g_emit.srcs_cap * 2 : 32;
    g_emit.srcs = (SrcMapEntry*)xrealloc(g_emit.srcs, g_emit.srcs_cap * sizeof(SrcMapEntry));
  }
  e = &g_emit.srcs[g_emit.srcs_len++];
  memset(e, 0, sizeof(*e));
  e->line = line;
  e->col = col;
  if (g_emit.ids_mode == SIRC_IDS_NUMERIC) {
    e->id_num = g_emit.next_src_id++;
  } else {
    const char* file = path_basename(g_emit.input_path ? g_emit.input_path : "<input>");
    char buf[2048];
    snprintf(buf, sizeof(buf), "src:%s:%d:%d", file, line, col);
    e->id_str = xstrdup(buf);
  }
  return e;
}

static void emit_src_id_value(const SrcMapEntry* e) {
  if (!e) return;
  if (g_emit.ids_mode == SIRC_IDS_NUMERIC) {
    emitf("%lld", (long long)e->id_num);
  } else {
    json_write_escaped(g_emit.out, e->id_str ? e->id_str : "src");
  }
}

static void emit_src_record_if_needed(void) {
  if (!(g_emit.emit_src == SIRC_EMIT_SRC_SRCREF || g_emit.emit_src == SIRC_EMIT_SRC_BOTH)) return;
  SrcMapEntry* e = src_get_or_create(sirc_last_line, sirc_last_col);
  if (!e || e->emitted) return;
  e->emitted = true;

  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"src\",\"id\":");
  emit_src_id_value(e);
  const char* bn = path_basename(g_emit.input_path);
  if (bn && bn[0] && strcmp(bn, "<input>") != 0) {
    emitf(",\"file\":");
    json_write_escaped(g_emit.out, bn);
  }
  emitf(",\"line\":%d", e->line);
  if (e->col > 0) emitf(",\"col\":%d", e->col);
  char* text = read_source_line(g_emit.input_path, e->line);
  if (text) {
    emitf(",\"text\":");
    json_write_escaped(g_emit.out, text);
    free(text);
  }
  emitf("}\n");
}

static void emit_record_prepare(void) {
  // If we are emitting src_ref, the referenced src record MUST appear earlier in the stream.
  if (g_emit.emit_src == SIRC_EMIT_SRC_SRCREF || g_emit.emit_src == SIRC_EMIT_SRC_BOTH) {
    emit_src_record_if_needed();
  }
}

static void emit_record_src_loc_trailer(void) {
  if (g_emit.emit_src == SIRC_EMIT_SRC_NONE) return;

  if (g_emit.emit_src == SIRC_EMIT_SRC_LOC || g_emit.emit_src == SIRC_EMIT_SRC_BOTH) {
    emitf(",\"loc\":");
    emit_loc_obj();
  }

  if (g_emit.emit_src == SIRC_EMIT_SRC_SRCREF || g_emit.emit_src == SIRC_EMIT_SRC_BOTH) {
    SrcMapEntry* e = src_get_or_create(sirc_last_line, sirc_last_col);
    if (e) {
      emitf(",\"src_ref\":");
      emit_src_id_value(e);
    }
  }
}

static int64_t type_lookup(const char* key) {
  for (size_t i = 0; i < g_emit.types_len; i++) {
    if (strcmp(g_emit.types[i].key, key) == 0) return g_emit.types[i].id;
  }
  return 0;
}

static int64_t type_insert(const char* key) {
  int64_t id = g_emit.next_type_id++;
  if (g_emit.types_len == g_emit.types_cap) {
    g_emit.types_cap = g_emit.types_cap ? g_emit.types_cap * 2 : 32;
    g_emit.types = (TypeEntry*)xrealloc(g_emit.types, g_emit.types_cap * sizeof(TypeEntry));
  }
  g_emit.types[g_emit.types_len++] = (TypeEntry){.key = xstrdup(key), .id = id};
  record_type_out_id(id, g_emit.types[g_emit.types_len - 1].key);
  return id;
}

static int64_t type_prim(const char* prim) {
  char key[64];
  snprintf(key, sizeof(key), "prim:%s", prim);
  int64_t id = type_lookup(key);
  if (id) return id;
  id = type_insert(key);
  emit_record_prepare();
  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":");
  emit_type_id_value(id);
  emitf(",\"kind\":\"prim\",\"prim\":");
  json_write_escaped(g_emit.out, prim);
  emit_record_src_loc_trailer();
  emitf("}\n");
  return id;
}

static int64_t prim_id_or_zero(const char* prim) {
  char key[64];
  snprintf(key, sizeof(key), "prim:%s", prim);
  return type_lookup(key);
}

static const char* type_key_for_id(int64_t id) {
  if (id <= 0) return NULL;
  for (size_t i = 0; i < g_emit.types_len; i++) {
    if (g_emit.types[i].id == id) return g_emit.types[i].key;
  }
  return NULL;
}

static int64_t type_ptr(int64_t of) {
  const char* ofk = type_key_for_id(of);
  if (!ofk) die_at_last("sirc: internal error: ptr(of) references unknown type id %lld", (long long)of);
  const size_t cap = strlen(ofk) + 16;
  char* key = (char*)xmalloc(cap);
  snprintf(key, cap, "ptr(%s)", ofk);

  int64_t id = type_lookup(key);
  if (id) {
    free(key);
    return id;
  }
  id = type_insert(key);
  free(key);
  emit_record_prepare();
  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":");
  emit_type_id_value(id);
  emitf(",\"kind\":\"ptr\",\"of\":");
  emit_type_id_value(of);
  emit_record_src_loc_trailer();
  emitf("}\n");
  return id;
}

static int64_t type_array(int64_t of, int64_t len) {
  const char* ofk = type_key_for_id(of);
  if (!ofk) die_at_last("sirc: internal error: array(of) references unknown type id %lld", (long long)of);
  const size_t cap = strlen(ofk) + 64;
  char* key = (char*)xmalloc(cap);
  snprintf(key, cap, "array(%s,%lld)", ofk, (long long)len);

  int64_t id = type_lookup(key);
  if (id) {
    free(key);
    return id;
  }
  id = type_insert(key);
  free(key);
  emit_record_prepare();
  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":");
  emit_type_id_value(id);
  emitf(",\"kind\":\"array\",\"of\":");
  emit_type_id_value(of);
  emitf(",\"len\":%lld", (long long)len);
  emit_record_src_loc_trailer();
  emitf("}\n");
  return id;
}

static int64_t type_vec(int64_t lane, int64_t lanes) {
  if (lanes <= 0) die_at_last("sirc: vec(lane, lanes) requires lanes > 0");
  const char* lk = type_key_for_id(lane);
  if (!lk) die_at_last("sirc: internal error: vec(lane) references unknown type id %lld", (long long)lane);
  const size_t cap = strlen(lk) + 64;
  char* key = (char*)xmalloc(cap);
  snprintf(key, cap, "vec(%s,%lld)", lk, (long long)lanes);

  int64_t id = type_lookup(key);
  if (id) {
    free(key);
    return id;
  }
  id = type_insert(key);
  free(key);

  emit_record_prepare();
  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":");
  emit_type_id_value(id);
  emitf(",\"kind\":\"vec\",\"lane\":");
  emit_type_id_value(lane);
  emitf(",\"lanes\":%lld", (long long)lanes);
  emit_record_src_loc_trailer();
  emitf("}\n");
  return id;
}

static int64_t named_type_lookup(const char* name) {
  for (size_t i = 0; i < g_emit.named_types_len; i++) {
    if (strcmp(g_emit.named_types[i].name, name) == 0) return g_emit.named_types[i].type_id;
  }
  return 0;
}

static void named_type_set(char* name, int64_t type_id) {
  if (!name) return;
  for (size_t i = 0; i < g_emit.named_types_len; i++) {
    if (strcmp(g_emit.named_types[i].name, name) == 0) {
      g_emit.named_types[i].type_id = type_id;
      free(name);
      return;
    }
  }
  if (g_emit.named_types_len == g_emit.named_types_cap) {
    g_emit.named_types_cap = g_emit.named_types_cap ? g_emit.named_types_cap * 2 : 32;
    g_emit.named_types = (NamedTypeEntry*)xrealloc(g_emit.named_types, g_emit.named_types_cap * sizeof(NamedTypeEntry));
  }
  g_emit.named_types[g_emit.named_types_len++] = (NamedTypeEntry){.name = name, .type_id = type_id};
}

static int64_t type_fn(const int64_t* params, size_t n, int64_t ret) {
  // canonical key: fn(p1,p2)->r (in terms of other type keys)
  const char* retk = type_key_for_id(ret);
  if (!retk) die_at_last("sirc: internal error: fn(ret) references unknown type id %lld", (long long)ret);

  size_t cap = 32 + strlen(retk);
  for (size_t i = 0; i < n; i++) {
    const char* pk = type_key_for_id(params[i]);
    if (!pk) die_at_last("sirc: internal error: fn(params) references unknown type id %lld", (long long)params[i]);
    cap += strlen(pk) + 2;
  }
  char* key = (char*)xmalloc(cap);
  size_t off = 0;
  off += (size_t)snprintf(key + off, cap - off, "fn(");
  for (size_t i = 0; i < n; i++) {
    const char* pk = type_key_for_id(params[i]);
    off += (size_t)snprintf(key + off, cap - off, "%s%s", (i ? "," : ""), pk);
  }
  off += (size_t)snprintf(key + off, cap - off, ")->%s", retk);

  int64_t id = type_lookup(key);
  if (id) {
    free(key);
    return id;
  }
  id = type_insert(key);
  free(key);

  emit_record_prepare();
  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":");
  emit_type_id_value(id);
  emitf(",\"kind\":\"fn\",\"params\":[");
  for (size_t i = 0; i < n; i++) {
    if (i) emitf(",");
    emit_type_id_value(params[i]);
  }
  emitf("],\"ret\":");
  emit_type_id_value(ret);
  emit_record_src_loc_trailer();
  emitf("}\n");
  return id;
}

static int64_t type_fun(int64_t sig) {
  const char* sigk = type_key_for_id(sig);
  if (!sigk) die_at_last("sirc: internal error: fun(sig) references unknown type id %lld", (long long)sig);
  const size_t cap = strlen(sigk) + 16;
  char* key = (char*)xmalloc(cap);
  snprintf(key, cap, "fun(%s)", sigk);

  int64_t id = type_lookup(key);
  if (id) {
    free(key);
    return id;
  }
  id = type_insert(key);
  free(key);

  emit_record_prepare();
  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":");
  emit_type_id_value(id);
  emitf(",\"kind\":\"fun\",\"sig\":");
  emit_type_id_value(sig);
  emit_record_src_loc_trailer();
  emitf("}\n");
  return id;
}

static int64_t type_closure(int64_t call_sig, int64_t env) {
  const char* ck = type_key_for_id(call_sig);
  const char* ek = type_key_for_id(env);
  if (!ck) die_at_last("sirc: internal error: closure(callSig) references unknown type id %lld", (long long)call_sig);
  if (!ek) die_at_last("sirc: internal error: closure(env) references unknown type id %lld", (long long)env);

  const size_t cap = strlen(ck) + strlen(ek) + 32;
  char* key = (char*)xmalloc(cap);
  snprintf(key, cap, "closure(%s,%s)", ck, ek);

  int64_t id = type_lookup(key);
  if (id) {
    free(key);
    return id;
  }
  id = type_insert(key);
  free(key);

  emit_record_prepare();
  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":");
  emit_type_id_value(id);
  emitf(",\"kind\":\"closure\",\"callSig\":");
  emit_type_id_value(call_sig);
  emitf(",\"env\":");
  emit_type_id_value(env);
  emit_record_src_loc_trailer();
  emitf("}\n");
  return id;
}

typedef struct SircTypeList {
  int64_t* tys;
  size_t len;
  size_t cap;
} SircTypeList;

SircTypeList* sirc_types_empty(void) { return (SircTypeList*)calloc(1, sizeof(SircTypeList)); }

SircTypeList* sirc_types_single(int64_t ty) {
  SircTypeList* l = sirc_types_empty();
  l->cap = 4;
  l->tys = (int64_t*)xmalloc(l->cap * sizeof(int64_t));
  l->tys[0] = ty;
  l->len = 1;
  return l;
}

SircTypeList* sirc_types_append(SircTypeList* l, int64_t ty) {
  if (!l) l = sirc_types_empty();
  if (l->len == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 4;
    l->tys = (int64_t*)xrealloc(l->tys, l->cap * sizeof(int64_t));
  }
  l->tys[l->len++] = ty;
  return l;
}

typedef struct SircSumVariantList {
  char** names;
  int64_t* payload_tys; // 0 means nullary
  size_t len;
  size_t cap;
} SircSumVariantList;

SircSumVariantList* sirc_sum_variants_empty(void) { return (SircSumVariantList*)calloc(1, sizeof(SircSumVariantList)); }

SircSumVariantList* sirc_sum_variants_append(SircSumVariantList* l, char* name, int64_t payload_ty) {
  if (!l) l = sirc_sum_variants_empty();
  if (!name || !name[0]) {
    free(name);
    return l;
  }
  if (l->len == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 4;
    l->names = (char**)xrealloc(l->names, l->cap * sizeof(char*));
    l->payload_tys = (int64_t*)xrealloc(l->payload_tys, l->cap * sizeof(int64_t));
  }
  l->names[l->len] = name;
  l->payload_tys[l->len] = payload_ty;
  l->len++;
  return l;
}

SircSumVariantList* sirc_sum_variants_merge(SircSumVariantList* a, SircSumVariantList* b) {
  if (!a) a = sirc_sum_variants_empty();
  if (!b || !b->len) {
    if (b) {
      free(b->names);
      free(b->payload_tys);
      free(b);
    }
    return a;
  }
  for (size_t i = 0; i < b->len; i++) {
    a = sirc_sum_variants_append(a, b->names[i], b->payload_tys[i]);
    b->names[i] = NULL;
  }
  free(b->names);
  free(b->payload_tys);
  free(b);
  return a;
}

static int64_t type_sum(const SircSumVariantList* v) {
  if (!v || !v->len) die_at_last("sirc: sum(...) requires at least one variant");

  // canonical key: sum{A,B:ty,...} (in terms of type keys)
  size_t cap = 32;
  for (size_t i = 0; i < v->len; i++) {
    cap += strlen(v->names[i]) + 2;
    if (v->payload_tys[i]) {
      const char* pk = type_key_for_id(v->payload_tys[i]);
      if (!pk) die_at_last("sirc: internal error: sum variant '%s' references unknown type id %lld", v->names[i], (long long)v->payload_tys[i]);
      cap += strlen(pk) + 1;
    }
  }
  char* key = (char*)xmalloc(cap);
  size_t off = 0;
  off += (size_t)snprintf(key + off, cap - off, "sum{");
  for (size_t i = 0; i < v->len; i++) {
    if (i) off += (size_t)snprintf(key + off, cap - off, ",");
    off += (size_t)snprintf(key + off, cap - off, "%s", v->names[i]);
    if (v->payload_tys[i]) {
      const char* pk = type_key_for_id(v->payload_tys[i]);
      off += (size_t)snprintf(key + off, cap - off, ":%s", pk);
    }
  }
  (void)snprintf(key + off, cap - off, "}");

  int64_t id = type_lookup(key);
  if (id) {
    free(key);
    return id;
  }
  id = type_insert(key);
  free(key);

  emit_record_prepare();
  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":");
  emit_type_id_value(id);
  emitf(",\"kind\":\"sum\",\"variants\":[");
  for (size_t i = 0; i < v->len; i++) {
    if (i) emitf(",");
    emitf("{\"name\":");
    json_write_escaped(g_emit.out, v->names[i]);
    if (v->payload_tys[i]) {
      emitf(",\"ty\":");
      emit_type_id_value(v->payload_tys[i]);
    }
    emitf("}");
  }
  emitf("]");
  emit_record_src_loc_trailer();
  emitf("}\n");
  return id;
}

static int64_t type_from_name(const char* name) {
  if (!name) return 0;
  int64_t alias = named_type_lookup(name);
  if (alias) return alias;
  if (strcmp(name, "i8") == 0) return type_prim("i8");
  if (strcmp(name, "i16") == 0) return type_prim("i16");
  if (strcmp(name, "i32") == 0) return type_prim("i32");
  if (strcmp(name, "i64") == 0) return type_prim("i64");
  if (strcmp(name, "f32") == 0) return type_prim("f32");
  if (strcmp(name, "f64") == 0) return type_prim("f64");
  if (strcmp(name, "bool") == 0) return type_prim("bool");
  if (strcmp(name, "void") == 0) return type_prim("void");
  if (strcmp(name, "ptr") == 0) {
    int64_t i8 = type_prim("i8");
    return type_ptr(i8);
  }
  // Named/extended types must be introduced via `type <Name> = ...` aliases in .sir.
  // (We keep the diagnostic code stable so integrators can match on it.)
  diag_setf("sirc.type.unknown", "unknown type name '%s'", name);
  diag_print_one();
  exit(1);
}

static void fn_add_extern(const char* name, int64_t sig_ty, int64_t ret_ty) {
  if (!name) return;
  // overwrite if exists
  for (size_t i = 0; i < g_emit.fns_len; i++) {
    if (strcmp(g_emit.fns[i].name, name) == 0) {
      g_emit.fns[i].sig_type = sig_ty;
      g_emit.fns[i].ret_type = ret_ty;
      g_emit.fns[i].is_extern = true;
      return;
    }
  }
  if (g_emit.fns_len == g_emit.fns_cap) {
    g_emit.fns_cap = g_emit.fns_cap ? g_emit.fns_cap * 2 : 32;
    g_emit.fns = (FnEntry*)xrealloc(g_emit.fns, g_emit.fns_cap * sizeof(FnEntry));
  }
  g_emit.fns[g_emit.fns_len++] =
      (FnEntry){.name = xstrdup(name), .sig_type = sig_ty, .ret_type = ret_ty, .param_count = 0, .is_extern = true, .is_defined = false, .fn_node = 0};
}

static const FnEntry* fn_find(const char* name) {
  for (size_t i = 0; i < g_emit.fns_len; i++) {
    if (strcmp(g_emit.fns[i].name, name) == 0) return &g_emit.fns[i];
  }
  return NULL;
}

static FnEntry* fn_find_mut(const char* name) {
  for (size_t i = 0; i < g_emit.fns_len; i++) {
    if (strcmp(g_emit.fns[i].name, name) == 0) return &g_emit.fns[i];
  }
  return NULL;
}

static void locals_clear(void) {
  for (size_t i = 0; i < g_emit.locals_len; i++) free(g_emit.locals[i].name);
  g_emit.locals_len = 0;
}

static void locals_set(const char* name, int64_t ty) {
  if (!name || !name[0] || ty == 0) return;
  for (size_t i = 0; i < g_emit.locals_len; i++) {
    if (strcmp(g_emit.locals[i].name, name) == 0) {
      g_emit.locals[i].ty = ty;
      return;
    }
  }
  if (g_emit.locals_len == g_emit.locals_cap) {
    g_emit.locals_cap = g_emit.locals_cap ? g_emit.locals_cap * 2 : 32;
    g_emit.locals = xrealloc(g_emit.locals, g_emit.locals_cap * sizeof(*g_emit.locals));
  }
  g_emit.locals[g_emit.locals_len++] = (LocalEntry){.name = xstrdup(name), .ty = ty};
}

static int64_t locals_get(const char* name) {
  if (!name || !name[0]) return 0;
  for (size_t i = 0; i < g_emit.locals_len; i++) {
    if (strcmp(g_emit.locals[i].name, name) == 0) return g_emit.locals[i].ty;
  }
  return 0;
}

static void ensure_node_id_slot(int64_t id) {
  if (id < 0) return;
  const size_t need = (size_t)id + 1;
  if (need <= g_emit.node_id_cap) return;
  size_t cap = g_emit.node_id_cap ? g_emit.node_id_cap : 256;
  while (cap < need) cap *= 2;
  g_emit.node_id_by_id = (char**)xrealloc(g_emit.node_id_by_id, cap * sizeof(char*));
  for (size_t i = g_emit.node_id_cap; i < cap; i++) g_emit.node_id_by_id[i] = NULL;
  g_emit.node_id_cap = cap;
}

static void ensure_type_id_slot(int64_t id) {
  if (id < 0) return;
  const size_t need = (size_t)id + 1;
  if (need <= g_emit.type_id_cap) return;
  size_t cap = g_emit.type_id_cap ? g_emit.type_id_cap : 128;
  while (cap < need) cap *= 2;
  g_emit.type_id_by_id = (char**)xrealloc(g_emit.type_id_by_id, cap * sizeof(char*));
  for (size_t i = g_emit.type_id_cap; i < cap; i++) g_emit.type_id_by_id[i] = NULL;
  g_emit.type_id_cap = cap;
}

static void record_node_out_id(int64_t id) {
  if (g_emit.ids_mode != SIRC_IDS_STRING) return;
  if (id <= 0) return;
  ensure_node_id_slot(id);
  if ((size_t)id < g_emit.node_id_cap && g_emit.node_id_by_id[id]) return;

  const int line = sirc_last_line;
  if (g_emit.last_id_line != line) {
    g_emit.last_id_line = line;
    g_emit.line_id_seq = 0;
  }
  const uint32_t seq = g_emit.line_id_seq++;

  char buf[64];
  snprintf(buf, sizeof(buf), "n:%d:%u", line, (unsigned)seq);
  g_emit.node_id_by_id[id] = xstrdup(buf);
}

static void record_type_out_id(int64_t id, const char* key) {
  if (g_emit.ids_mode != SIRC_IDS_STRING) return;
  if (id <= 0 || !key) return;
  ensure_type_id_slot(id);
  if ((size_t)id < g_emit.type_id_cap) g_emit.type_id_by_id[id] = (char*)key; // owned by TypeEntry
}

static void emit_node_id_value(int64_t id) {
  if (g_emit.ids_mode == SIRC_IDS_STRING) {
    ensure_node_id_slot(id);
    const char* s = ((size_t)id < g_emit.node_id_cap) ? g_emit.node_id_by_id[id] : NULL;
    if (!s) die_at_last("sirc: internal error: missing string id for node %lld", (long long)id);
    json_write_escaped(g_emit.out, s);
  } else {
    emitf("%lld", (long long)id);
  }
}

static void emit_type_id_value(int64_t id) {
  if (g_emit.ids_mode == SIRC_IDS_STRING) {
    ensure_type_id_slot(id);
    const char* s = ((size_t)id < g_emit.type_id_cap) ? g_emit.type_id_by_id[id] : NULL;
    if (!s) die_at_last("sirc: internal error: missing string id for type %lld", (long long)id);
    json_write_escaped(g_emit.out, s);
  } else {
    emitf("%lld", (long long)id);
  }
}

static void emit_node_ref_obj(int64_t id) {
  emitf("{\"t\":\"ref\",\"id\":");
  emit_node_id_value(id);
  emitf("}");
}

static void emit_type_ref_obj(int64_t ty) {
  emitf("{\"t\":\"ref\",\"k\":\"type\",\"id\":");
  emit_type_id_value(ty);
  emitf("}");
}

static int64_t next_node_id(void) {
  int64_t id = g_emit.next_node_id++;
  record_node_out_id(id);
  return id;
}

static void ensure_node_name_slot(int64_t id) {
  if (id < 0) return;
  size_t need = (size_t)id + 1;
  if (need <= g_emit.node_name_cap) return;
  size_t cap = g_emit.node_name_cap ? g_emit.node_name_cap : 256;
  while (cap < need) cap *= 2;
  g_emit.node_name_by_id = (char**)xrealloc(g_emit.node_name_by_id, cap * sizeof(char*));
  for (size_t i = g_emit.node_name_cap; i < cap; i++) g_emit.node_name_by_id[i] = NULL;
  g_emit.node_name_cap = cap;
}

static void record_node_name(int64_t id, const char* name) {
  if (!name) return;
  ensure_node_name_slot(id);
  if ((size_t)id < g_emit.node_name_cap) {
    free(g_emit.node_name_by_id[id]);
    g_emit.node_name_by_id[id] = xstrdup(name);
  }
}

static const char* lookup_node_name(int64_t id) {
  if (id < 0 || (size_t)id >= g_emit.node_name_cap) return NULL;
  return g_emit.node_name_by_id[id];
}

static int64_t emit_node_with_fields_begin(const char* tag, int64_t type_ref) {
  int64_t id = next_node_id();
  emit_record_prepare();
  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_node_id_value(id);
  emitf(",\"tag\":");
  json_write_escaped(g_emit.out, tag);
  if (type_ref) {
    emitf(",\"type_ref\":");
    emit_type_id_value(type_ref);
  }
  emitf(",\"fields\":{");
  return id;
}

static void emit_fields_end(void) {
  emitf("}");
  emit_record_src_loc_trailer();
  emitf("}\n");
}

static int64_t emit_param_node(const char* name, int64_t type_ref) {
  int64_t id = emit_node_with_fields_begin("param", type_ref);
  emitf("\"name\":");
  json_write_escaped(g_emit.out, name);
  emit_fields_end();
  return id;
}

static int64_t emit_bparam_node(int64_t type_ref) {
  int64_t id = next_node_id();
  emit_record_prepare();
  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_node_id_value(id);
  emitf(",\"tag\":\"bparam\",\"type_ref\":");
  emit_type_id_value(type_ref);
  emit_record_src_loc_trailer();
  emitf("}\n");
  return id;
}

static int64_t emit_name_node(const char* name) {
  const int64_t ty = locals_get(name);
  int64_t id = emit_node_with_fields_begin("name", ty);
  emitf("\"name\":");
  json_write_escaped(g_emit.out, name);
  emit_fields_end();
  record_node_name(id, name);
  return id;
}

static int64_t emit_const_int_node(int64_t value, int64_t type_ref, const char* prim) {
  char tag[32];
  snprintf(tag, sizeof(tag), "const.%s", prim);
  int64_t id = emit_node_with_fields_begin(tag, type_ref);
  emitf("\"value\":%lld", (long long)value);
  emit_fields_end();
  return id;
}

static int64_t emit_cstr_node(const char* s) {
  // `cstr` is a pointer to NUL-terminated bytes; under `--verify-strict` call sites
  // need a concrete argument type_ref, so we always attach `ptr(i8)`.
  const int64_t i8 = type_from_name("i8");
  const int64_t p_i8 = type_ptr(i8);
  int64_t id = emit_node_with_fields_begin("cstr", p_i8);
  emitf("\"value\":");
  json_write_escaped(g_emit.out, s);
  emit_fields_end();
  return id;
}

static int64_t emit_decl_fn_node(const char* name, int64_t sig_type) {
  int64_t id = emit_node_with_fields_begin("decl.fn", sig_type);
  emitf("\"name\":");
  json_write_escaped(g_emit.out, name);
  emit_fields_end();
  return id;
}

static int64_t emit_call_indirect(int64_t ret_type, int64_t sig_type, int64_t callee_node, const int64_t* args, size_t argc) {
  int64_t id = emit_node_with_fields_begin("call.indirect", ret_type);
  emitf("\"sig\":");
  emit_type_ref_obj(sig_type);
  emitf(",\"args\":[");
  emit_node_ref_obj(callee_node);
  for (size_t i = 0; i < argc; i++) {
    emitf(",");
    emit_node_ref_obj(args[i]);
  }
  emitf("]");
  emit_fields_end();
  return id;
}

static int64_t emit_call_direct(int64_t ret_type, int64_t callee_fn_node, const int64_t* args, size_t argc) {
  int64_t id = emit_node_with_fields_begin("call", ret_type);
  emitf("\"callee\":");
  emit_node_ref_obj(callee_fn_node);
  emitf(",\"args\":[");
  for (size_t i = 0; i < argc; i++) {
    if (i) emitf(",");
    emit_node_ref_obj(args[i]);
  }
  emitf("]");
  emit_fields_end();
  return id;
}

static int64_t emit_let_node(const char* name, int64_t value_node) {
  int64_t id = emit_node_with_fields_begin("let", 0);
  emitf("\"name\":");
  json_write_escaped(g_emit.out, name);
  emitf(",\"value\":");
  emit_node_ref_obj(value_node);
  emit_fields_end();
  return id;
}

static int64_t emit_term_ret_node(int64_t value_node) {
  int64_t id = emit_node_with_fields_begin("term.ret", 0);
  emitf("\"value\":");
  emit_node_ref_obj(value_node);
  emit_fields_end();
  return id;
}

static int64_t emit_block_node(const int64_t* stmts, size_t n) {
  int64_t id = emit_node_with_fields_begin("block", 0);
  emitf("\"stmts\":[");
  for (size_t i = 0; i < n; i++) {
    if (i) emitf(",");
    emit_node_ref_obj(stmts[i]);
  }
  emitf("]");
  emit_fields_end();
  return id;
}

static int64_t emit_fn_node(const char* name, bool is_public, int64_t fn_type, const int64_t* params, size_t nparams, int64_t body_block) {
  int64_t id = emit_node_with_fields_begin("fn", fn_type);
  emitf("\"name\":");
  json_write_escaped(g_emit.out, name);
  emitf(",\"linkage\":");
  json_write_escaped(g_emit.out, is_public ? "public" : "local");
  emitf(",\"params\":[");
  for (size_t i = 0; i < nparams; i++) {
    if (i) emitf(",");
    emit_node_ref_obj(params[i]);
  }
  emitf("],\"body\":");
  emit_node_ref_obj(body_block);
  emit_fields_end();
  return id;
}

// ---- API used by the parser (sir.y) ----
void sirc_emit_unit(char* unit, char* target) {
  if (g_emit.unit) free(g_emit.unit);
  if (g_emit.target) free(g_emit.target);
  g_emit.unit = unit;
  if (target && strcmp(target, "host") == 0) {
    free(target);
    g_emit.target = NULL;
  } else {
    g_emit.target = target;
  }
  emit_meta();
}

void sirc_add_feature(char* feature) {
  if (!feature) return;
  if (g_emit.features_len == g_emit.features_cap) {
    g_emit.features_cap = g_emit.features_cap ? g_emit.features_cap * 2 : 16;
    g_emit.features = (char**)xrealloc(g_emit.features, g_emit.features_cap * sizeof(char*));
  }
  g_emit.features[g_emit.features_len++] = feature;
}

int64_t sirc_type_from_name(char* name) {
  int64_t id = type_from_name(name);
  free(name);
  return id;
}

int64_t sirc_type_ptr_of(int64_t of) { return type_ptr(of); }

int64_t sirc_type_array_of(int64_t of, long long len) {
  if (len < 0) die_at_last("sirc: array length must be >= 0");
  return type_array(of, (int64_t)len);
}

int64_t sirc_type_fn_of(SircTypeList* params, int64_t ret) {
  const int64_t* p = params ? params->tys : NULL;
  const size_t n = params ? params->len : 0;
  int64_t id = type_fn(p, n, ret);
  if (params) {
    free(params->tys);
    free(params);
  }
  return id;
}

int64_t sirc_type_fun_of(int64_t sig) { return type_fun(sig); }

int64_t sirc_type_closure_of(int64_t call_sig, int64_t env) { return type_closure(call_sig, env); }

int64_t sirc_type_sum_of(SircSumVariantList* variants) {
  const int64_t id = type_sum(variants);
  if (variants) {
    for (size_t i = 0; i < variants->len; i++) free(variants->names[i]);
    free(variants->names);
    free(variants->payload_tys);
    free(variants);
  }
  return id;
}

int64_t sirc_type_vec_of(int64_t lane, long long lanes) { return type_vec(lane, (int64_t)lanes); }

void sirc_type_alias(char* name, int64_t ty) { named_type_set(name, ty); }

typedef struct SircParamList {
  char** names;
  int64_t* types;
  int64_t* nodes;
  size_t len;
  size_t cap;
  bool is_block;
} SircParamList;

typedef struct SircNodeList {
  int64_t* nodes;
  size_t len;
  size_t cap;
} SircNodeList;

typedef struct SircExprList {
  int64_t* nodes;
  size_t len;
  size_t cap;
} SircExprList;

typedef enum AttrScalarKind {
  ATTR_SCALAR_STR = 1,
  ATTR_SCALAR_INT = 2,
  ATTR_SCALAR_BOOL = 3,
  ATTR_SCALAR_INT_LIST = 4,
} AttrScalarKind;

typedef struct AttrScalar {
  AttrScalarKind kind;
  union {
    char* s;
    long long i;
    int b;
    struct {
      long long* items;
      size_t len;
    } ilist;
  } v;
} AttrScalar;

typedef enum AttrKind {
  ATTR_FLAG_BOOL = 1,    // +flag => fields.flags.flag = true
  ATTR_FIELD_SCALAR = 2, // key v => fields[key] = v
  ATTR_FLAGS_SCALAR = 3, // flags key v => fields.flags[key] = v
  ATTR_SIG = 4,          // sig <fnname> (used by call.indirect)
  ATTR_COUNT = 5,        // count <expr> (used by alloca)
} AttrKind;

typedef struct AttrItem {
  AttrKind kind;
  char* key; // flag name / field key / sig fn name
  AttrScalar scalar;
  int64_t node_ref;
} AttrItem;

typedef struct SircAttrList {
  AttrItem* items;
  size_t len;
  size_t cap;
} SircAttrList;

typedef struct SircIntList {
  long long* items;
  size_t len;
  size_t cap;
} SircIntList;

static SircParamList* params_new(bool is_block) {
  SircParamList* p = (SircParamList*)calloc(1, sizeof(SircParamList));
  if (p) p->is_block = is_block;
  return p;
}
static SircNodeList* nodelist_new(void) { return (SircNodeList*)calloc(1, sizeof(SircNodeList)); }
static SircExprList* exprlist_new(void) { return (SircExprList*)calloc(1, sizeof(SircExprList)); }

static void params_add(SircParamList* p, char* name, int64_t ty) {
  if (!p) return;
  if (p->len == p->cap) {
    p->cap = p->cap ? p->cap * 2 : 8;
    p->names = (char**)xrealloc(p->names, p->cap * sizeof(char*));
    p->types = (int64_t*)xrealloc(p->types, p->cap * sizeof(int64_t));
    p->nodes = (int64_t*)xrealloc(p->nodes, p->cap * sizeof(int64_t));
  }
  int64_t pn = p->is_block ? emit_bparam_node(ty) : emit_param_node(name, ty);
  locals_set(name, ty);
  p->names[p->len] = name;
  p->types[p->len] = ty;
  p->nodes[p->len] = pn;
  p->len++;
}

static void nodelist_add(SircNodeList* l, int64_t n) {
  if (!l) return;
  if (n == 0) return;
  if (l->len == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 16;
    l->nodes = (int64_t*)xrealloc(l->nodes, l->cap * sizeof(int64_t));
  }
  l->nodes[l->len++] = n;
}

static void exprlist_add(SircExprList* l, int64_t n) {
  if (!l) return;
  if (l->len == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 8;
    l->nodes = (int64_t*)xrealloc(l->nodes, l->cap * sizeof(int64_t));
  }
  l->nodes[l->len++] = n;
}

SircParamList* sirc_params_empty(void) { return params_new(false); }
SircParamList* sirc_params_single(char* name, int64_t ty) {
  SircParamList* p = params_new(false);
  params_add(p, name, ty);
  return p;
}
SircParamList* sirc_params_append(SircParamList* p, char* name, int64_t ty) {
  if (!p) p = params_new(false);
  params_add(p, name, ty);
  return p;
}

SircParamList* sirc_bparams_empty(void) { return params_new(true); }
SircParamList* sirc_bparams_single(char* name, int64_t ty) {
  SircParamList* p = params_new(true);
  params_add(p, name, ty);
  return p;
}
SircParamList* sirc_bparams_append(SircParamList* p, char* name, int64_t ty) {
  if (!p) p = params_new(true);
  params_add(p, name, ty);
  return p;
}

SircNodeList* sirc_stmtlist_empty(void) { return nodelist_new(); }
SircNodeList* sirc_stmtlist_single(int64_t n) {
  SircNodeList* l = nodelist_new();
  nodelist_add(l, n);
  return l;
}
SircNodeList* sirc_stmtlist_append(SircNodeList* l, int64_t n) {
  if (!l) l = nodelist_new();
  nodelist_add(l, n);
  return l;
}

int64_t sirc_nodelist_first(const SircNodeList* l) { return (l && l->len) ? l->nodes[0] : 0; }

SircExprList* sirc_args_empty(void) { return exprlist_new(); }
SircExprList* sirc_args_single(int64_t n) {
  SircExprList* l = exprlist_new();
  exprlist_add(l, n);
  return l;
}
SircExprList* sirc_args_append(SircExprList* l, int64_t n) {
  if (!l) l = exprlist_new();
  exprlist_add(l, n);
  return l;
}

static SircAttrList* attrs_new(void) {
  SircAttrList* l = (SircAttrList*)calloc(1, sizeof(SircAttrList));
  if (!l) die_at_last("sirc: out of memory");
  return l;
}

static void attrs_reserve(SircAttrList* l, size_t add) {
  if (!l) return;
  size_t want = l->len + add;
  if (want <= l->cap) return;
  size_t nc = l->cap ? l->cap * 2 : 8;
  while (nc < want) nc *= 2;
  l->items = (AttrItem*)xrealloc(l->items, nc * sizeof(AttrItem));
  l->cap = nc;
}

SircAttrList* sirc_attrs_empty(void) { return attrs_new(); }

static SircAttrList* attrs_add_item(SircAttrList* l, AttrItem it) {
  if (!l) l = attrs_new();
  attrs_reserve(l, 1);
  l->items[l->len++] = it;
  return l;
}

SircAttrList* sirc_attrs_merge(SircAttrList* a, SircAttrList* b) {
  if (!a) return b;
  if (!b) return a;
  if (!b->len) {
    free(b->items);
    free(b);
    return a;
  }
  attrs_reserve(a, b->len);
  memcpy(a->items + a->len, b->items, b->len * sizeof(AttrItem));
  a->len += b->len;
  free(b->items);
  free(b);
  return a;
}

SircAttrList* sirc_attrs_add_flag(SircAttrList* l, char* name) {
  AttrItem it = {.kind = ATTR_FLAG_BOOL, .key = name};
  return attrs_add_item(l, it);
}

SircAttrList* sirc_attrs_add_field_scalar_str(SircAttrList* l, char* key, char* v) {
  AttrItem it = {.kind = ATTR_FIELD_SCALAR, .key = key, .scalar = {.kind = ATTR_SCALAR_STR, .v = {.s = v}}};
  return attrs_add_item(l, it);
}

SircAttrList* sirc_attrs_add_field_scalar_int(SircAttrList* l, char* key, long long v) {
  AttrItem it = {.kind = ATTR_FIELD_SCALAR, .key = key, .scalar = {.kind = ATTR_SCALAR_INT, .v = {.i = v}}};
  return attrs_add_item(l, it);
}

SircAttrList* sirc_attrs_add_field_scalar_bool(SircAttrList* l, char* key, int v) {
  AttrItem it = {.kind = ATTR_FIELD_SCALAR, .key = key, .scalar = {.kind = ATTR_SCALAR_BOOL, .v = {.b = v}}};
  return attrs_add_item(l, it);
}

SircAttrList* sirc_attrs_add_flags_scalar_str(SircAttrList* l, char* key, char* v) {
  AttrItem it = {.kind = ATTR_FLAGS_SCALAR, .key = key, .scalar = {.kind = ATTR_SCALAR_STR, .v = {.s = v}}};
  return attrs_add_item(l, it);
}

SircAttrList* sirc_attrs_add_flags_scalar_int(SircAttrList* l, char* key, long long v) {
  AttrItem it = {.kind = ATTR_FLAGS_SCALAR, .key = key, .scalar = {.kind = ATTR_SCALAR_INT, .v = {.i = v}}};
  return attrs_add_item(l, it);
}

SircAttrList* sirc_attrs_add_flags_scalar_bool(SircAttrList* l, char* key, int v) {
  AttrItem it = {.kind = ATTR_FLAGS_SCALAR, .key = key, .scalar = {.kind = ATTR_SCALAR_BOOL, .v = {.b = v}}};
  return attrs_add_item(l, it);
}

SircAttrList* sirc_attrs_add_flags_scalar_int_list(SircAttrList* l, char* key, SircIntList* v) {
  long long* items = NULL;
  size_t len = 0;
  if (v) {
    items = v->items;
    len = v->len;
    free(v);
  }
  AttrItem it = {.kind = ATTR_FLAGS_SCALAR, .key = key, .scalar = {.kind = ATTR_SCALAR_INT_LIST, .v = {.ilist = {.items = items, .len = len}}}};
  return attrs_add_item(l, it);
}

SircAttrList* sirc_attrs_add_sig(SircAttrList* l, char* fn_name) {
  AttrItem it = {.kind = ATTR_SIG, .key = fn_name};
  return attrs_add_item(l, it);
}

SircAttrList* sirc_attrs_add_count(SircAttrList* l, int64_t node_ref) {
  AttrItem it = {.kind = ATTR_COUNT, .node_ref = node_ref};
  return attrs_add_item(l, it);
}

void sirc_attrs_free(SircAttrList* l) {
  if (!l) return;
  for (size_t i = 0; i < l->len; i++) {
    AttrItem* it = &l->items[i];
    if (it->key) free(it->key);
    if ((it->kind == ATTR_FIELD_SCALAR || it->kind == ATTR_FLAGS_SCALAR) && it->scalar.kind == ATTR_SCALAR_STR && it->scalar.v.s) {
      free(it->scalar.v.s);
    }
    if ((it->kind == ATTR_FIELD_SCALAR || it->kind == ATTR_FLAGS_SCALAR) && it->scalar.kind == ATTR_SCALAR_INT_LIST && it->scalar.v.ilist.items) {
      free(it->scalar.v.ilist.items);
    }
  }
  free(l->items);
  free(l);
}

SircIntList* sirc_ints_empty(void) {
  SircIntList* l = (SircIntList*)calloc(1, sizeof(SircIntList));
  if (!l) die_at_last("sirc: out of memory");
  return l;
}

SircIntList* sirc_ints_append(SircIntList* l, long long v) {
  if (!l) l = sirc_ints_empty();
  if (l->len == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 8;
    l->items = (long long*)xrealloc(l->items, l->cap * sizeof(long long));
  }
  l->items[l->len++] = v;
  return l;
}

void sirc_ints_free(SircIntList* l) {
  if (!l) return;
  free(l->items);
  free(l);
}

int64_t sirc_value_ident(char* name) {
  int64_t id = emit_name_node(name);
  free(name);
  return id;
}

int64_t sirc_value_string(char* s) {
  int64_t id = emit_cstr_node(s);
  free(s);
  return id;
}

int64_t sirc_value_bool(int b) {
  // represent as const.bool 0/1 using bool type
  int64_t ty = type_prim("bool");
  return emit_const_int_node(b ? 1 : 0, ty, "bool");
}

int64_t sirc_value_int(long long v) {
  int64_t ty = type_prim("i32");
  return emit_const_int_node((int64_t)v, ty, "i32");
}

static int64_t emit_const_float_bits(const char* prim, int64_t type_ref, const char* bits_hex) {
  char tag[32];
  snprintf(tag, sizeof(tag), "const.%s", prim);
  int64_t id = emit_node_with_fields_begin(tag, type_ref);
  emitf("\"bits\":");
  json_write_escaped(g_emit.out, bits_hex);
  emit_fields_end();
  return id;
}

static void bits_hex_f32(float v, char out[11]) {
  uint32_t u = 0;
  memcpy(&u, &v, sizeof(u));
  snprintf(out, 11, "0x%08x", (unsigned)u);
}

static void bits_hex_f64(double v, char out[19]) {
  uint64_t u = 0;
  memcpy(&u, &v, sizeof(u));
  snprintf(out, 19, "0x%016llx", (unsigned long long)u);
}

int64_t sirc_value_float(double v) {
  int64_t ty = type_prim("f64");
  char bits[19];
  bits_hex_f64(v, bits);
  return emit_const_float_bits("f64", ty, bits);
}

int64_t sirc_typed_float(double v, int64_t ty) {
  if (ty == prim_id_or_zero("f32")) {
    float f = (float)v;
    int64_t tty = type_prim("f32");
    char bits[11];
    bits_hex_f32(f, bits);
    return emit_const_float_bits("f32", tty, bits);
  }
  if (ty == prim_id_or_zero("f64")) {
    int64_t tty = type_prim("f64");
    char bits[19];
    bits_hex_f64(v, bits);
    return emit_const_float_bits("f64", tty, bits);
  }
  die_at_last("sirc: typed float requires f32 or f64 type");
  return 0;
}

int64_t sirc_typed_int(long long v, int64_t ty) {
  // only prim ints supported
  const char* prim = NULL;
  if (ty == prim_id_or_zero("i8")) prim = "i8";
  else if (ty == prim_id_or_zero("i16")) prim = "i16";
  else if (ty == prim_id_or_zero("i32")) prim = "i32";
  else if (ty == prim_id_or_zero("i64")) prim = "i64";
  if (!prim) die_at_last("sirc: typed int requires integer type");
  return emit_const_int_node((int64_t)v, ty, prim);
}

char* sirc_dotted_join(char* a, char* b) {
  size_t na = strlen(a);
  size_t nb = strlen(b);
  char* out = (char*)xmalloc(na + 1 + nb + 1);
  memcpy(out, a, na);
  out[na] = '.';
  memcpy(out + na + 1, b, nb + 1);
  free(a);
  free(b);
  return out;
}

char* sirc_colon_join(char* a, char* b) {
  size_t na = strlen(a);
  size_t nb = strlen(b);
  char* out = (char*)xmalloc(na + 1 + nb + 1);
  memcpy(out, a, na);
  out[na] = ':';
  memcpy(out + na + 1, b, nb + 1);
  free(a);
  free(b);
  return out;
}

static bool has_dot(const char* s) { return s && strchr(s, '.') != NULL; }

static int strict_mnemonic_arity(const char* name) {
  if (!name || !name[0]) return -1;

  if (strcmp(name, "ptr.sym") == 0) return 1;
  if (strcmp(name, "mem.copy") == 0) return 3;
  if (strcmp(name, "mem.fill") == 0) return 3;

  const char* dot = strchr(name, '.');
  if (!dot) return -1;
  const size_t prefix_len = (size_t)(dot - name);
  const char* op = dot + 1;
  if (!op || !op[0]) return -1;
  if (strchr(op, '.')) return -1; // avoid false positives for multi-dot ops (e.g. i32.zext.i8)

  bool int_prefix = (prefix_len == 2 && strncmp(name, "i8", 2) == 0) || (prefix_len == 3 && strncmp(name, "i16", 3) == 0) ||
                    (prefix_len == 3 && strncmp(name, "i32", 3) == 0) || (prefix_len == 3 && strncmp(name, "i64", 3) == 0);
  if (!int_prefix) return -1;

  if (strcmp(op, "add") == 0) return 2;
  if (strcmp(op, "sub") == 0) return 2;
  if (strcmp(op, "mul") == 0) return 2;
  if (strcmp(op, "and") == 0) return 2;
  if (strcmp(op, "or") == 0) return 2;
  if (strcmp(op, "xor") == 0) return 2;
  if (strcmp(op, "shl") == 0) return 2;
  if (strcmp(op, "shr") == 0) return 2;
  if (strcmp(op, "sar") == 0) return 2;
  if (strcmp(op, "eq") == 0) return 2;
  if (strcmp(op, "ne") == 0) return 2;
  if (strcmp(op, "lt") == 0) return 2;
  if (strcmp(op, "le") == 0) return 2;
  if (strcmp(op, "gt") == 0) return 2;
  if (strcmp(op, "ge") == 0) return 2;
  if (strcmp(op, "abs") == 0) return 1;
  if (strcmp(op, "neg") == 0) return 1;
  if (strcmp(op, "not") == 0) return 1;

  return -1;
}

static void emit_attr_scalar(const AttrScalar* s) {
  if (!s) {
    emitf("null");
    return;
  }
  switch (s->kind) {
    case ATTR_SCALAR_INT: emitf("%lld", (long long)s->v.i); return;
    case ATTR_SCALAR_BOOL: emitf("%s", s->v.b ? "true" : "false"); return;
    case ATTR_SCALAR_STR: json_write_escaped(g_emit.out, s->v.s ? s->v.s : ""); return;
    case ATTR_SCALAR_INT_LIST:
      emitf("[");
      for (size_t i = 0; i < s->v.ilist.len; i++) {
        if (i) emitf(",");
        emitf("%lld", (long long)s->v.ilist.items[i]);
      }
      emitf("]");
      return;
    default: emitf("null"); return;
  }
}

typedef struct AttrAccum {
  AttrItem* root;
  size_t root_len;
  AttrItem* flags;
  size_t flags_len;
  const char* sig_fn;
  int64_t count_ref;
  bool have_sig;
  bool have_count;
} AttrAccum;

static void attrs_accum_init(AttrAccum* a) { memset(a, 0, sizeof(*a)); }

static AttrItem* attrs_find_scalar_item(AttrItem* items, size_t n, const char* key) {
  for (size_t i = 0; i < n; i++) {
    if (items[i].key && strcmp(items[i].key, key) == 0) return &items[i];
  }
  return NULL;
}

static void attrs_accum_from_list(AttrAccum* acc, SircAttrList* l) {
  attrs_accum_init(acc);
  if (!l || !l->len) return;

  for (size_t i = 0; i < l->len; i++) {
    AttrItem* it = &l->items[i];
    if (it->kind == ATTR_SIG) {
      if (acc->have_sig) die_at_last("sirc: duplicate sig attribute");
      acc->sig_fn = it->key;
      acc->have_sig = true;
    } else if (it->kind == ATTR_COUNT) {
      if (acc->have_count) die_at_last("sirc: duplicate count attribute");
      acc->count_ref = it->node_ref;
      acc->have_count = true;
    } else if (it->kind == ATTR_FIELD_SCALAR) {
      acc->root_len++;
    } else if (it->kind == ATTR_FLAG_BOOL || it->kind == ATTR_FLAGS_SCALAR) {
      acc->flags_len++;
    }
  }

  if (acc->root_len) acc->root = (AttrItem*)xmalloc(acc->root_len * sizeof(AttrItem));
  if (acc->flags_len) acc->flags = (AttrItem*)xmalloc(acc->flags_len * sizeof(AttrItem));
  size_t r = 0, f = 0;
  for (size_t i = 0; i < l->len; i++) {
    AttrItem* it = &l->items[i];
    if (it->kind == ATTR_FIELD_SCALAR) acc->root[r++] = *it;
    else if (it->kind == ATTR_FLAG_BOOL || it->kind == ATTR_FLAGS_SCALAR) acc->flags[f++] = *it;
  }
}

static void attrs_accum_free(AttrAccum* acc) {
  free(acc->root);
  free(acc->flags);
  attrs_accum_init(acc);
}

static bool attrs_has_any(const AttrAccum* a) {
  return a && (a->root_len || a->flags_len || a->have_sig || a->have_count);
}

static void emit_flags_object(AttrItem* items, size_t n, bool leading_comma) {
  if (!items || !n) return;
  if (leading_comma) emitf(",");
  emitf("\"flags\":{");
  bool first = true;
  for (size_t i = 0; i < n; i++) {
    AttrItem* it = &items[i];
    if (!it->key) continue;
    if (!first) emitf(",");
    first = false;
    emitf("\"%s\":", it->key);
    if (it->kind == ATTR_FLAG_BOOL) emitf("true");
    else emit_attr_scalar(&it->scalar);
  }
  emitf("}");
}

void sirc_cfg_begin(void) {
  for (size_t i = 0; i < g_emit.blocks_len; i++) free(g_emit.blocks[i].name);
  g_emit.blocks_len = 0;
}

static int64_t block_id_for_name(const char* name) {
  for (size_t i = 0; i < g_emit.blocks_len; i++) {
    if (strcmp(g_emit.blocks[i].name, name) == 0) return g_emit.blocks[i].id;
  }
  if (g_emit.blocks_len == g_emit.blocks_cap) {
    g_emit.blocks_cap = g_emit.blocks_cap ? g_emit.blocks_cap * 2 : 32;
    g_emit.blocks = xrealloc(g_emit.blocks, g_emit.blocks_cap * sizeof(*g_emit.blocks));
  }
  int64_t id = next_node_id();
  g_emit.blocks[g_emit.blocks_len].name = xstrdup(name);
  g_emit.blocks[g_emit.blocks_len].id = id;
  g_emit.blocks[g_emit.blocks_len].has_params = false;
  g_emit.blocks_len++;
  return id;
}

static void block_mark_has_params(int64_t id, bool has_params) {
  for (size_t i = 0; i < g_emit.blocks_len; i++) {
    if (g_emit.blocks[i].id == id) {
      g_emit.blocks[i].has_params = has_params;
      return;
    }
  }
}

static bool block_has_params(int64_t id) {
  for (size_t i = 0; i < g_emit.blocks_len; i++) {
    if (g_emit.blocks[i].id == id) return g_emit.blocks[i].has_params;
  }
  return false;
}

static void emit_block_node_at(int64_t id, const int64_t* params, size_t nparams, const int64_t* stmts, size_t nstmts) {
  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_node_id_value(id);
  emitf(",\"tag\":\"block\",\"fields\":{");
  bool any = false;
  if (params && nparams) {
    emitf("\"params\":[");
    for (size_t i = 0; i < nparams; i++) {
      if (i) emitf(",");
      emit_node_ref_obj(params[i]);
    }
    emitf("]");
    any = true;
  }
  if (stmts) {
    if (any) emitf(",");
    emitf("\"stmts\":[");
    for (size_t i = 0; i < nstmts; i++) {
      if (i) emitf(",");
      emit_node_ref_obj(stmts[i]);
    }
    emitf("]");
  }
  emitf("}}\n");
}

int64_t sirc_select(int64_t ty, int64_t cond, int64_t then_v, int64_t else_v) {
  int64_t id = emit_node_with_fields_begin("select", ty);
  emitf("\"args\":[");
  emit_node_ref_obj(cond);
  emitf(",");
  emit_node_ref_obj(then_v);
  emitf(",");
  emit_node_ref_obj(else_v);
  emitf("]");
  emit_fields_end();
  return id;
}

int64_t sirc_ptr_sizeof(int64_t ty) {
  int64_t i64 = type_prim("i64");
  int64_t id = emit_node_with_fields_begin("ptr.sizeof", i64);
  emitf("\"ty\":");
  emit_type_ref_obj(ty);
  emitf(",\"args\":[]");
  emit_fields_end();
  return id;
}

int64_t sirc_ptr_alignof(int64_t ty) {
  int64_t i32 = type_prim("i32");
  int64_t id = emit_node_with_fields_begin("ptr.alignof", i32);
  emitf("\"ty\":");
  emit_type_ref_obj(ty);
  emitf(",\"args\":[]");
  emit_fields_end();
  return id;
}

int64_t sirc_ptr_offset(int64_t ty, int64_t base, int64_t index) {
  int64_t id = emit_node_with_fields_begin("ptr.offset", 0);
  emitf("\"ty\":");
  emit_type_ref_obj(ty);
  emitf(",\"args\":[");
  emit_node_ref_obj(base);
  emitf(",");
  emit_node_ref_obj(index);
  emitf("]");
  emit_fields_end();
  return id;
}

typedef struct SircSwitchCaseList {
  int64_t* lit_nodes;
  int64_t* to_blocks;
  size_t len;
  size_t cap;
} SircSwitchCaseList;

static SircSwitchCaseList* cases_new(void) { return (SircSwitchCaseList*)calloc(1, sizeof(SircSwitchCaseList)); }

SircSwitchCaseList* sirc_cases_empty(void) { return cases_new(); }

SircSwitchCaseList* sirc_cases_append(SircSwitchCaseList* l, int64_t lit_node, char* to_block_name) {
  if (!l) l = cases_new();
  if (!to_block_name) return l;
  if (l->len == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 8;
    l->lit_nodes = (int64_t*)xrealloc(l->lit_nodes, l->cap * sizeof(int64_t));
    l->to_blocks = (int64_t*)xrealloc(l->to_blocks, l->cap * sizeof(int64_t));
  }
  l->lit_nodes[l->len] = lit_node;
  l->to_blocks[l->len] = block_id_for_name(to_block_name);
  l->len++;
  free(to_block_name);
  return l;
}

int64_t sirc_term_br(char* to_block_name, SircExprList* args) {
  if (!to_block_name) die_at_last("sirc: term.br missing target");
  int64_t bid = block_id_for_name(to_block_name);
  free(to_block_name);

  int64_t id = emit_node_with_fields_begin("term.br", 0);
  emitf("\"to\":");
  emit_node_ref_obj(bid);
  if (args && args->len) {
    emitf(",\"args\":[");
    for (size_t i = 0; i < args->len; i++) {
      if (i) emitf(",");
      emit_node_ref_obj(args->nodes[i]);
    }
    emitf("]");
  }
  emit_fields_end();
  if (args) {
    free(args->nodes);
    free(args);
  }
  return id;
}

int64_t sirc_block_ref(char* name) {
  if (!name || !name[0]) {
    free(name);
    die_at_last("sirc: block ref requires a name");
  }
  int64_t id = block_id_for_name(name);
  free(name);
  return id;
}

typedef struct SircSemSwitchCaseList {
  int64_t* lit_nodes;
  SircBranch* bodies;
  size_t len;
  size_t cap;
} SircSemSwitchCaseList;

typedef struct SircSemMatchCaseList {
  long long* variants;
  SircBranch* bodies;
  size_t len;
  size_t cap;
} SircSemMatchCaseList;

typedef struct SircBranchList {
  SircBranch* items;
  size_t len;
  size_t cap;
} SircBranchList;

static SircSemSwitchCaseList* sem_switch_cases_new(void) { return (SircSemSwitchCaseList*)calloc(1, sizeof(SircSemSwitchCaseList)); }
static SircSemMatchCaseList* sem_match_cases_new(void) { return (SircSemMatchCaseList*)calloc(1, sizeof(SircSemMatchCaseList)); }
static SircBranchList* branch_list_new(void) { return (SircBranchList*)calloc(1, sizeof(SircBranchList)); }

SircSemSwitchCaseList* sirc_sem_switch_cases_empty(void) { return sem_switch_cases_new(); }

SircSemSwitchCaseList* sirc_sem_switch_cases_append(SircSemSwitchCaseList* l, int64_t lit_node, SircBranch body) {
  if (!l) l = sem_switch_cases_new();
  if (l->len == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 8;
    l->lit_nodes = (int64_t*)xrealloc(l->lit_nodes, l->cap * sizeof(int64_t));
    l->bodies = (SircBranch*)xrealloc(l->bodies, l->cap * sizeof(SircBranch));
  }
  l->lit_nodes[l->len] = lit_node;
  l->bodies[l->len] = body;
  l->len++;
  return l;
}

SircSemMatchCaseList* sirc_sem_match_cases_empty(void) { return sem_match_cases_new(); }

SircSemMatchCaseList* sirc_sem_match_cases_append(SircSemMatchCaseList* l, long long variant, SircBranch body) {
  if (!l) l = sem_match_cases_new();
  if (l->len == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 8;
    l->variants = (long long*)xrealloc(l->variants, l->cap * sizeof(long long));
    l->bodies = (SircBranch*)xrealloc(l->bodies, l->cap * sizeof(SircBranch));
  }
  l->variants[l->len] = variant;
  l->bodies[l->len] = body;
  l->len++;
  return l;
}

SircBranchList* sirc_branch_list_empty(void) { return branch_list_new(); }

SircBranchList* sirc_branch_list_append(SircBranchList* l, SircBranch b) {
  if (!l) l = branch_list_new();
  if (l->len == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 8;
    l->items = (SircBranch*)xrealloc(l->items, l->cap * sizeof(SircBranch));
  }
  l->items[l->len++] = b;
  return l;
}

static void emit_branch_operand_obj(SircBranch b) {
  if (b.kind == SIRC_BRANCH_VAL) {
    emitf("{\"kind\":\"val\",\"v\":");
    emit_node_ref_obj(b.node);
    emitf("}");
    return;
  }
  if (b.kind == SIRC_BRANCH_THUNK) {
    emitf("{\"kind\":\"thunk\",\"f\":");
    emit_node_ref_obj(b.node);
    emitf("}");
    return;
  }
  die_at_last("sirc: internal error: bad branch kind");
}

int64_t sirc_sem_if(int64_t cond, SircBranch then_b, SircBranch else_b, int64_t ty) {
  int64_t id = emit_node_with_fields_begin("sem.if", ty);
  emitf("\"args\":[");
  emit_node_ref_obj(cond);
  emitf(",");
  emit_branch_operand_obj(then_b);
  emitf(",");
  emit_branch_operand_obj(else_b);
  emitf("]");
  emit_fields_end();
  return id;
}

int64_t sirc_sem_cond(int64_t cond, SircBranch then_b, SircBranch else_b, int64_t ty) {
  int64_t id = emit_node_with_fields_begin("sem.cond", ty);
  emitf("\"args\":[");
  emit_node_ref_obj(cond);
  emitf(",");
  emit_branch_operand_obj(then_b);
  emitf(",");
  emit_branch_operand_obj(else_b);
  emitf("]");
  emit_fields_end();
  return id;
}

int64_t sirc_sem_and_sc(int64_t lhs, SircBranch rhs_b) {
  int64_t bty = type_prim("bool");
  int64_t id = emit_node_with_fields_begin("sem.and_sc", bty);
  emitf("\"args\":[");
  emit_node_ref_obj(lhs);
  emitf(",");
  emit_branch_operand_obj(rhs_b);
  emitf("]");
  emit_fields_end();
  return id;
}

int64_t sirc_sem_or_sc(int64_t lhs, SircBranch rhs_b) {
  int64_t bty = type_prim("bool");
  int64_t id = emit_node_with_fields_begin("sem.or_sc", bty);
  emitf("\"args\":[");
  emit_node_ref_obj(lhs);
  emitf(",");
  emit_branch_operand_obj(rhs_b);
  emitf("]");
  emit_fields_end();
  return id;
}

int64_t sirc_sem_switch(int64_t scrut, SircSemSwitchCaseList* cases, SircBranch def_b, int64_t ty) {
  int64_t id = emit_node_with_fields_begin("sem.switch", ty);
  emitf("\"args\":[");
  emit_node_ref_obj(scrut);
  emitf("],\"cases\":[");
  if (cases) {
    for (size_t i = 0; i < cases->len; i++) {
      if (i) emitf(",");
      emitf("{\"lit\":");
      emit_node_ref_obj(cases->lit_nodes[i]);
      emitf(",\"body\":");
      emit_branch_operand_obj(cases->bodies[i]);
      emitf("}");
    }
  }
  emitf("],\"default\":");
  emit_branch_operand_obj(def_b);
  emit_fields_end();
  if (cases) {
    free(cases->lit_nodes);
    free(cases->bodies);
    free(cases);
  }
  return id;
}

int64_t sirc_sem_match_sum(int64_t sum_ty, int64_t scrut, SircSemMatchCaseList* cases, SircBranch def_b, int64_t ty) {
  int64_t id = emit_node_with_fields_begin("sem.match_sum", ty);
  emitf("\"sum\":");
  emit_type_ref_obj(sum_ty);
  emitf(",\"args\":[");
  emit_node_ref_obj(scrut);
  emitf("],\"cases\":[");
  if (cases) {
    for (size_t i = 0; i < cases->len; i++) {
      if (i) emitf(",");
      emitf("{\"variant\":%lld,\"body\":", (long long)cases->variants[i]);
      emit_branch_operand_obj(cases->bodies[i]);
      emitf("}");
    }
  }
  emitf("],\"default\":");
  emit_branch_operand_obj(def_b);
  emit_fields_end();
  if (cases) {
    free(cases->variants);
    free(cases->bodies);
    free(cases);
  }
  return id;
}

int64_t sirc_sem_while(SircBranch cond_thunk, SircBranch body_thunk) {
  int64_t id = emit_node_with_fields_begin("sem.while", 0);
  emitf("\"args\":[");
  emit_branch_operand_obj(cond_thunk);
  emitf(",");
  emit_branch_operand_obj(body_thunk);
  emitf("]");
  emit_fields_end();
  return id;
}

int64_t sirc_sem_break(void) {
  int64_t id = emit_node_with_fields_begin("sem.break", 0);
  emit_fields_end();
  return id;
}

int64_t sirc_sem_continue(void) {
  int64_t id = emit_node_with_fields_begin("sem.continue", 0);
  emit_fields_end();
  return id;
}

int64_t sirc_sem_defer(SircBranch thunk) {
  int64_t id = emit_node_with_fields_begin("sem.defer", 0);
  emitf("\"args\":[");
  emit_branch_operand_obj(thunk);
  emitf("]");
  emit_fields_end();
  return id;
}

int64_t sirc_sem_scope(SircBranchList* defers, int64_t body_block) {
  int64_t id = emit_node_with_fields_begin("sem.scope", 0);
  emitf("\"defers\":[");
  if (defers) {
    for (size_t i = 0; i < defers->len; i++) {
      if (i) emitf(",");
      emit_branch_operand_obj(defers->items[i]);
    }
  }
  emitf("],\"body\":");
  emit_node_ref_obj(body_block);
  emit_fields_end();
  if (defers) {
    free(defers->items);
    free(defers);
  }
  return id;
}

int64_t sirc_block_value(SircNodeList* stmts) {
  const int64_t* ns = (stmts && stmts->len) ? stmts->nodes : NULL;
  const size_t n = (stmts && stmts->len) ? stmts->len : 0;
  int64_t id = emit_block_node(ns, n);
  if (stmts) {
    free(stmts->nodes);
    free(stmts);
  }
  return id;
}

static void emit_branch_args_obj(SircExprList* args) {
  if (!args || !args->len) return;
  emitf(",\"args\":[");
  for (size_t i = 0; i < args->len; i++) {
    if (i) emitf(",");
    emit_node_ref_obj(args->nodes[i]);
  }
  emitf("]");
}

int64_t sirc_term_cbr(int64_t cond, char* then_block_name, SircExprList* then_args, char* else_block_name, SircExprList* else_args) {
  if (!then_block_name || !else_block_name) die_at_last("sirc: term.cbr requires then/else targets");
  int64_t then_id = block_id_for_name(then_block_name);
  int64_t else_id = block_id_for_name(else_block_name);
  free(then_block_name);
  free(else_block_name);

  int64_t id = emit_node_with_fields_begin("term.cbr", 0);
  emitf("\"cond\":");
  emit_node_ref_obj(cond);
  emitf(",\"then\":{");
  emitf("\"to\":");
  emit_node_ref_obj(then_id);
  emit_branch_args_obj(then_args);
  emitf("}");
  emitf(",\"else\":{");
  emitf("\"to\":");
  emit_node_ref_obj(else_id);
  emit_branch_args_obj(else_args);
  emitf("}");
  emit_fields_end();
  if (then_args) {
    free(then_args->nodes);
    free(then_args);
  }
  if (else_args) {
    free(else_args->nodes);
    free(else_args);
  }
  return id;
}

int64_t sirc_term_switch(int64_t scrut, SircSwitchCaseList* cases, char* default_block_name) {
  if (!default_block_name) die_at_last("sirc: term.switch requires default target");
  int64_t def_id = block_id_for_name(default_block_name);
  free(default_block_name);

  int64_t id = emit_node_with_fields_begin("term.switch", 0);
  emitf("\"scrut\":");
  emit_node_ref_obj(scrut);
  emitf(",\"cases\":[");
  if (cases) {
    for (size_t i = 0; i < cases->len; i++) {
      if (i) emitf(",");
      emitf("{\"lit\":");
      emit_node_ref_obj(cases->lit_nodes[i]);
      emitf(",\"to\":");
      emit_node_ref_obj(cases->to_blocks[i]);
      emitf("}");
    }
  }
  emitf("]");
  emitf(",\"default\":{\"to\":");
  emit_node_ref_obj(def_id);
  emitf("}");
  emit_fields_end();

  if (cases) {
    free(cases->lit_nodes);
    free(cases->to_blocks);
    free(cases);
  }
  return id;
}

int64_t sirc_term_ret_opt(int has_value, int64_t value_node) {
  int64_t id = emit_node_with_fields_begin("term.ret", 0);
  if (has_value) {
    emitf("\"value\":");
    emit_node_ref_obj(value_node);
  }
  emit_fields_end();
  return id;
}

static int64_t emit_term_simple_with_attrs(const char* tag, SircAttrList* attrs) {
  AttrAccum acc;
  attrs_accum_from_list(&acc, attrs);

  int64_t id = emit_node_with_fields_begin(tag, 0);
  bool first = true;

  for (size_t i = 0; i < acc.root_len; i++) {
    AttrItem* it = &acc.root[i];
    if (!it->key) continue;
    if (!first) emitf(",");
    first = false;
    emitf("\"%s\":", it->key);
    emit_attr_scalar(&it->scalar);
  }

  if (acc.flags_len) {
    emitf("%s\"flags\":{", first ? "" : ",");
    bool ffirst = true;
    for (size_t i = 0; i < acc.flags_len; i++) {
      AttrItem* it = &acc.flags[i];
      if (!it->key) continue;
      if (!ffirst) emitf(",");
      ffirst = false;
      emitf("\"%s\":", it->key);
      if (it->kind == ATTR_FLAG_BOOL) emitf("true");
      else emit_attr_scalar(&it->scalar);
    }
    emitf("}");
  }

  emit_fields_end();
  attrs_accum_free(&acc);
  sirc_attrs_free(attrs);
  return id;
}

int64_t sirc_term_unreachable(SircAttrList* attrs) { return emit_term_simple_with_attrs("term.unreachable", attrs); }

int64_t sirc_term_trap(SircAttrList* attrs) { return emit_term_simple_with_attrs("term.trap", attrs); }

int64_t sirc_block_def(char* name, SircParamList* bparams, SircNodeList* stmts) {
  if (!name) die_at_last("sirc: block requires a name");
  int64_t bid = block_id_for_name(name);

  size_t nparams = (bparams && bparams->len) ? bparams->len : 0;
  block_mark_has_params(bid, nparams != 0);
  int64_t* pnodes = (nparams ? bparams->nodes : NULL);

  // Prepend lets that bind block param names to their bparam values.
  SircNodeList* all = nodelist_new();
  if (nparams) {
    for (size_t i = 0; i < nparams; i++) {
      int64_t letn = emit_let_node(bparams->names[i], bparams->nodes[i]);
      nodelist_add(all, letn);
    }
  }
  if (stmts && stmts->len) {
    for (size_t i = 0; i < stmts->len; i++) nodelist_add(all, stmts->nodes[i]);
  }

  emit_block_node_at(bid, pnodes, nparams, all->nodes, all->len);

  free(name);
  if (bparams) {
    for (size_t i = 0; i < bparams->len; i++) free(bparams->names[i]);
    free(bparams->names);
    free(bparams->types);
    free(bparams->nodes);
    free(bparams);
  }
  if (stmts) {
    free(stmts->nodes);
    free(stmts);
  }
  free(all->nodes);
  free(all);
  return bid;
}

void sirc_fn_def_cfg(char* name, SircParamList* params, int64_t ret, bool is_public, int64_t entry_block, SircNodeList* blocks) {
  int64_t* ptys = NULL;
  int64_t* pnodes = NULL;
  size_t nparams = 0;
  if (params) {
    ptys = params->types;
    pnodes = params->nodes;
    nparams = params->len;
  }

  int64_t fn_ty = type_fn(ptys, nparams, ret);

  // Ensure any blocks with params (bparam PHIs) are lowered before branches that add incoming args.
  if (blocks && blocks->len > 1) {
    int64_t* reordered = (int64_t*)xmalloc(blocks->len * sizeof(int64_t));
    size_t j = 0;
    for (size_t i = 0; i < blocks->len; i++) {
      if (block_has_params(blocks->nodes[i])) reordered[j++] = blocks->nodes[i];
    }
    for (size_t i = 0; i < blocks->len; i++) {
      if (!block_has_params(blocks->nodes[i])) reordered[j++] = blocks->nodes[i];
    }
    memcpy(blocks->nodes, reordered, blocks->len * sizeof(int64_t));
    free(reordered);
  }

  int64_t id = emit_node_with_fields_begin("fn", fn_ty);
  emitf("\"name\":");
  json_write_escaped(g_emit.out, name);
  emitf(",\"linkage\":");
  json_write_escaped(g_emit.out, is_public ? "public" : "local");
  emitf(",\"params\":[");
  for (size_t i = 0; i < nparams; i++) {
    if (i) emitf(",");
    emit_node_ref_obj(pnodes[i]);
  }
  emitf("],\"entry\":");
  emit_node_ref_obj(entry_block);
  emitf(",\"blocks\":[");
  if (blocks) {
    for (size_t i = 0; i < blocks->len; i++) {
      if (i) emitf(",");
      emit_node_ref_obj(blocks->nodes[i]);
    }
  }
  emitf("]");
  emit_fields_end();

  FnEntry* e = fn_find_mut(name);
  if (e) {
    e->sig_type = fn_ty;
    e->ret_type = ret;
    e->param_count = nparams;
    e->is_defined = true;
    e->fn_node = id;
  } else {
    if (g_emit.fns_len == g_emit.fns_cap) {
      g_emit.fns_cap = g_emit.fns_cap ? g_emit.fns_cap * 2 : 32;
      g_emit.fns = (FnEntry*)xrealloc(g_emit.fns, g_emit.fns_cap * sizeof(FnEntry));
    }
    g_emit.fns[g_emit.fns_len++] =
        (FnEntry){.name = xstrdup(name), .sig_type = fn_ty, .ret_type = ret, .param_count = nparams, .is_extern = false, .is_defined = true, .fn_node = id};
  }

  free(name);
  if (params) {
    for (size_t i = 0; i < params->len; i++) free(params->names[i]);
    free(params->names);
    free(params->types);
    free(params->nodes);
    free(params);
  }
  if (blocks) {
    free(blocks->nodes);
    free(blocks);
  }
  locals_clear();
}

static int64_t sirc_call_impl(char* name, SircExprList* args, SircAttrList* attrs, int64_t forced_type_ref) {
  size_t argc = args ? args->len : 0;
  int64_t* argv = args ? args->nodes : NULL;

  AttrAccum acc;
  attrs_accum_from_list(&acc, attrs);

#define SIRC_CALL_CLEANUP()            \
  do {                                 \
    free(name);                        \
    if (args) free(args->nodes);       \
    free(args);                        \
    attrs_accum_free(&acc);            \
    sirc_attrs_free(attrs);            \
  } while (0)

  if (strcmp(name, "alloca") == 0) {
    if (argc < 1 || argc > 2) die_at_last("sirc: alloca(type[, count]) expected");
    const char* tname = lookup_node_name(argv[0]);
    if (!tname) die_at_last("sirc: alloca first arg must be a type name identifier");
    int64_t ty = type_from_name(tname);

    int64_t count_ref = (argc == 2) ? argv[1] : 0;
    if (acc.have_count) {
      if (count_ref) die_at_last("sirc: alloca count specified twice (positional and tail)");
      count_ref = acc.count_ref;
    }

    long long align_v = 0;
    bool align_present = false;
    AttrItem* al = attrs_find_scalar_item(acc.flags, acc.flags_len, "align");
    if (!al) al = attrs_find_scalar_item(acc.root, acc.root_len, "align");
    if (al && al->scalar.kind == ATTR_SCALAR_INT) {
      align_present = true;
      align_v = al->scalar.v.i;
    }

    if (g_strict) {
      if (acc.have_sig) strict_failf("sirc.strict.attr.unsupported", "alloca: 'sig' attribute is not supported");
      for (size_t i = 0; i < acc.root_len; i++) {
        AttrItem* it = &acc.root[i];
        if (!it->key) continue;
        if (strcmp(it->key, "align") == 0) {
          if (it->scalar.kind != ATTR_SCALAR_INT) strict_failf("sirc.strict.attr.bad_type", "alloca: align must be an integer");
          continue;
        }
        strict_failf("sirc.strict.attr.unknown", "alloca: unknown attribute '%s'", it->key);
      }
      for (size_t i = 0; i < acc.flags_len; i++) {
        AttrItem* it = &acc.flags[i];
        if (!it->key) continue;
        if (strcmp(it->key, "align") == 0 && it->kind != ATTR_FLAGS_SCALAR) {
          strict_failf("sirc.strict.attr.bad_type", "alloca: align must be an integer");
        }
      }
    }

    int64_t id = emit_node_with_fields_begin("alloca", 0);
    emitf("\"ty\":");
    emit_type_ref_obj(ty);

    if (count_ref || align_present || acc.flags_len) {
      emitf(",\"flags\":{");
      bool first = true;
      if (count_ref) {
        emitf("\"count\":");
        emit_node_ref_obj(count_ref);
        first = false;
      }
      if (align_present) {
        if (!first) emitf(",");
        emitf("\"align\":%lld", (long long)align_v);
        first = false;
      }
      for (size_t i = 0; i < acc.flags_len; i++) {
        AttrItem* it = &acc.flags[i];
        if (!it->key) continue;
        if (strcmp(it->key, "align") == 0) continue;
        if (!first) emitf(",");
        emitf("\"%s\":", it->key);
        if (it->kind == ATTR_FLAG_BOOL) emitf("true");
        else emit_attr_scalar(&it->scalar);
        first = false;
      }
      emitf("}");
    }

    emit_fields_end();
    SIRC_CALL_CLEANUP();
    return id;
  }

  if (strcmp(name, "call.indirect") == 0) {
    if (argc < 1) die_at_last("sirc: call.indirect(fp, ...) requires at least 1 arg");
    if (!acc.have_sig || !acc.sig_fn) die_at_last("sirc: call.indirect requires 'sig <fnname>' tail");
    const FnEntry* fn = fn_find(acc.sig_fn);
    if (!fn) die_at_last("sirc: call.indirect unknown sig function '%s'", acc.sig_fn);

    if (g_strict) {
      if (acc.have_count) strict_failf("sirc.strict.attr.unsupported", "call.indirect: 'count' attribute is not supported");
      if (acc.root_len) strict_failf("sirc.strict.attr.unsupported", "call.indirect: extra attributes are not supported in --strict");
      if (acc.flags_len) strict_failf("sirc.strict.attr.unsupported", "call.indirect: flags are not supported in --strict");
      const size_t want = 1 + fn->param_count;
      if (fn->param_count && argc != want) {
        strict_failf("sirc.strict.arity", "call.indirect: expected %zu args (fp + %zu params), got %zu", want, fn->param_count, argc);
      }
    }

    int64_t id = emit_node_with_fields_begin("call.indirect", fn->ret_type);
    emitf("\"sig\":");
    emit_type_ref_obj(fn->sig_type);
    emitf(",\"args\":[");
    emit_node_ref_obj(argv[0]);
    for (size_t i = 1; i < argc; i++) {
      emitf(",");
      emit_node_ref_obj(argv[i]);
    }
    emitf("]");

    // allow optional extra fields/flags (rare, but consistent)
    for (size_t i = 0; i < acc.root_len; i++) {
      AttrItem* it = &acc.root[i];
      if (!it->key) continue;
      emitf(",\"%s\":", it->key);
      emit_attr_scalar(&it->scalar);
    }
    if (acc.flags_len) emit_flags_object(acc.flags, acc.flags_len, true);
    emit_fields_end();

    SIRC_CALL_CLEANUP();
    return id;
  }

  if (strcmp(name, "load.vec") == 0) {
    if (argc != 1) die_at_last("sirc: load.vec requires 1 arg (addr)");
    if (!forced_type_ref) die_at_last("sirc: load.vec requires an explicit vec type via '... as <VecType>'");

    if (g_strict) {
      if (acc.have_sig) strict_failf("sirc.strict.attr.unsupported", "load.vec: 'sig' attribute is not supported");
      if (acc.have_count) strict_failf("sirc.strict.attr.unsupported", "load.vec: 'count' attribute is not supported");
      for (size_t i = 0; i < acc.root_len; i++) {
        AttrItem* it = &acc.root[i];
        if (!it->key) continue;
        if (strcmp(it->key, "align") == 0) {
          if (it->scalar.kind != ATTR_SCALAR_INT) strict_failf("sirc.strict.attr.bad_type", "load.vec: align must be an integer");
          continue;
        }
        if (strcmp(it->key, "vol") == 0) {
          if (it->scalar.kind != ATTR_SCALAR_BOOL) strict_failf("sirc.strict.attr.bad_type", "load.vec: vol must be a boolean");
          continue;
        }
        strict_failf("sirc.strict.attr.unknown", "load.vec: unknown attribute '%s'", it->key);
      }
      for (size_t i = 0; i < acc.flags_len; i++) {
        AttrItem* it = &acc.flags[i];
        if (!it->key) continue;
        if (strcmp(it->key, "align") == 0) {
          if (!(it->kind == ATTR_FLAGS_SCALAR && it->scalar.kind == ATTR_SCALAR_INT)) {
            strict_failf("sirc.strict.attr.bad_type", "load.vec: align must be an integer");
          }
          continue;
        }
        if (strcmp(it->key, "vol") == 0) {
          if (!(it->kind == ATTR_FLAGS_SCALAR && it->scalar.kind == ATTR_SCALAR_BOOL)) {
            strict_failf("sirc.strict.attr.bad_type", "load.vec: vol must be a boolean");
          }
          continue;
        }
        strict_failf("sirc.strict.attr.unsupported", "load.vec: flags are not supported");
      }
    }

    int64_t id = emit_node_with_fields_begin("load.vec", forced_type_ref);
    emitf("\"addr\":");
    emit_node_ref_obj(argv[0]);
    AttrItem* a = attrs_find_scalar_item(acc.flags, acc.flags_len, "align");
    if (!a) a = attrs_find_scalar_item(acc.root, acc.root_len, "align");
    if (a && a->scalar.kind == ATTR_SCALAR_INT) emitf(",\"align\":%lld", (long long)a->scalar.v.i);
    AttrItem* v = attrs_find_scalar_item(acc.flags, acc.flags_len, "vol");
    if (!v) v = attrs_find_scalar_item(acc.root, acc.root_len, "vol");
    if (v && v->scalar.kind == ATTR_SCALAR_BOOL) emitf(",\"vol\":%s", v->scalar.v.b ? "true" : "false");
    emit_fields_end();
    SIRC_CALL_CLEANUP();
    return id;
  }

  if (strncmp(name, "load.", 5) == 0) {
    if (argc != 1) die_at_last("sirc: %s requires 1 arg (addr)", name);
    const char* tname = name + 5;
    int64_t ty = type_from_name(tname);

    if (g_strict) {
      if (acc.have_sig) strict_failf("sirc.strict.attr.unsupported", "%s: 'sig' attribute is not supported", name);
      if (acc.have_count) strict_failf("sirc.strict.attr.unsupported", "%s: 'count' attribute is not supported", name);
      for (size_t i = 0; i < acc.root_len; i++) {
        AttrItem* it = &acc.root[i];
        if (!it->key) continue;
        if (strcmp(it->key, "align") == 0) {
          if (it->scalar.kind != ATTR_SCALAR_INT) strict_failf("sirc.strict.attr.bad_type", "%s: align must be an integer", name);
          continue;
        }
        if (strcmp(it->key, "vol") == 0) {
          if (it->scalar.kind != ATTR_SCALAR_BOOL) strict_failf("sirc.strict.attr.bad_type", "%s: vol must be a boolean", name);
          continue;
        }
        strict_failf("sirc.strict.attr.unknown", "%s: unknown attribute '%s'", name, it->key);
      }
      for (size_t i = 0; i < acc.flags_len; i++) {
        AttrItem* it = &acc.flags[i];
        if (!it->key) continue;
        if (strcmp(it->key, "align") == 0) {
          if (!(it->kind == ATTR_FLAGS_SCALAR && it->scalar.kind == ATTR_SCALAR_INT)) {
            strict_failf("sirc.strict.attr.bad_type", "%s: align must be an integer", name);
          }
          continue;
        }
        if (strcmp(it->key, "vol") == 0) {
          if (!(it->kind == ATTR_FLAGS_SCALAR && it->scalar.kind == ATTR_SCALAR_BOOL)) {
            strict_failf("sirc.strict.attr.bad_type", "%s: vol must be a boolean", name);
          }
          continue;
        }
        strict_failf("sirc.strict.attr.unsupported", "%s: flags are not supported", name);
      }
    }

    int64_t id = emit_node_with_fields_begin(name, ty);
    emitf("\"addr\":");
    emit_node_ref_obj(argv[0]);
    AttrItem* a = attrs_find_scalar_item(acc.flags, acc.flags_len, "align");
    if (!a) a = attrs_find_scalar_item(acc.root, acc.root_len, "align");
    if (a && a->scalar.kind == ATTR_SCALAR_INT) emitf(",\"align\":%lld", (long long)a->scalar.v.i);
    AttrItem* v = attrs_find_scalar_item(acc.flags, acc.flags_len, "vol");
    if (!v) v = attrs_find_scalar_item(acc.root, acc.root_len, "vol");
    if (v && v->scalar.kind == ATTR_SCALAR_BOOL) emitf(",\"vol\":%s", v->scalar.v.b ? "true" : "false");
    emit_fields_end();
    SIRC_CALL_CLEANUP();
    return id;
  }

  if (strcmp(name, "store.vec") == 0) {
    if (argc != 2) die_at_last("sirc: store.vec requires 2 args (addr, value)");

    if (g_strict) {
      if (acc.have_sig) strict_failf("sirc.strict.attr.unsupported", "store.vec: 'sig' attribute is not supported");
      if (acc.have_count) strict_failf("sirc.strict.attr.unsupported", "store.vec: 'count' attribute is not supported");
      for (size_t i = 0; i < acc.root_len; i++) {
        AttrItem* it = &acc.root[i];
        if (!it->key) continue;
        if (strcmp(it->key, "align") == 0) {
          if (it->scalar.kind != ATTR_SCALAR_INT) strict_failf("sirc.strict.attr.bad_type", "store.vec: align must be an integer");
          continue;
        }
        if (strcmp(it->key, "vol") == 0) {
          if (it->scalar.kind != ATTR_SCALAR_BOOL) strict_failf("sirc.strict.attr.bad_type", "store.vec: vol must be a boolean");
          continue;
        }
        strict_failf("sirc.strict.attr.unknown", "store.vec: unknown attribute '%s'", it->key);
      }
      for (size_t i = 0; i < acc.flags_len; i++) {
        AttrItem* it = &acc.flags[i];
        if (!it->key) continue;
        if (strcmp(it->key, "align") == 0) {
          if (!(it->kind == ATTR_FLAGS_SCALAR && it->scalar.kind == ATTR_SCALAR_INT)) {
            strict_failf("sirc.strict.attr.bad_type", "store.vec: align must be an integer");
          }
          continue;
        }
        if (strcmp(it->key, "vol") == 0) {
          if (!(it->kind == ATTR_FLAGS_SCALAR && it->scalar.kind == ATTR_SCALAR_BOOL)) {
            strict_failf("sirc.strict.attr.bad_type", "store.vec: vol must be a boolean");
          }
          continue;
        }
        strict_failf("sirc.strict.attr.unsupported", "store.vec: flags are not supported");
      }
    }

    int64_t id = emit_node_with_fields_begin("store.vec", 0);
    emitf("\"addr\":");
    emit_node_ref_obj(argv[0]);
    emitf(",\"value\":");
    emit_node_ref_obj(argv[1]);
    AttrItem* a = attrs_find_scalar_item(acc.flags, acc.flags_len, "align");
    if (!a) a = attrs_find_scalar_item(acc.root, acc.root_len, "align");
    if (a && a->scalar.kind == ATTR_SCALAR_INT) emitf(",\"align\":%lld", (long long)a->scalar.v.i);
    AttrItem* v = attrs_find_scalar_item(acc.flags, acc.flags_len, "vol");
    if (!v) v = attrs_find_scalar_item(acc.root, acc.root_len, "vol");
    if (v && v->scalar.kind == ATTR_SCALAR_BOOL) emitf(",\"vol\":%s", v->scalar.v.b ? "true" : "false");
    emit_fields_end();
    SIRC_CALL_CLEANUP();
    return id;
  }

  if (strncmp(name, "store.", 6) == 0) {
    if (argc != 2) die_at_last("sirc: %s requires 2 args (addr, value)", name);

    if (g_strict) {
      if (acc.have_sig) strict_failf("sirc.strict.attr.unsupported", "%s: 'sig' attribute is not supported", name);
      if (acc.have_count) strict_failf("sirc.strict.attr.unsupported", "%s: 'count' attribute is not supported", name);
      for (size_t i = 0; i < acc.root_len; i++) {
        AttrItem* it = &acc.root[i];
        if (!it->key) continue;
        if (strcmp(it->key, "align") == 0) {
          if (it->scalar.kind != ATTR_SCALAR_INT) strict_failf("sirc.strict.attr.bad_type", "%s: align must be an integer", name);
          continue;
        }
        if (strcmp(it->key, "vol") == 0) {
          if (it->scalar.kind != ATTR_SCALAR_BOOL) strict_failf("sirc.strict.attr.bad_type", "%s: vol must be a boolean", name);
          continue;
        }
        strict_failf("sirc.strict.attr.unknown", "%s: unknown attribute '%s'", name, it->key);
      }
      for (size_t i = 0; i < acc.flags_len; i++) {
        AttrItem* it = &acc.flags[i];
        if (!it->key) continue;
        if (strcmp(it->key, "align") == 0) {
          if (!(it->kind == ATTR_FLAGS_SCALAR && it->scalar.kind == ATTR_SCALAR_INT)) {
            strict_failf("sirc.strict.attr.bad_type", "%s: align must be an integer", name);
          }
          continue;
        }
        if (strcmp(it->key, "vol") == 0) {
          if (!(it->kind == ATTR_FLAGS_SCALAR && it->scalar.kind == ATTR_SCALAR_BOOL)) {
            strict_failf("sirc.strict.attr.bad_type", "%s: vol must be a boolean", name);
          }
          continue;
        }
        strict_failf("sirc.strict.attr.unsupported", "%s: flags are not supported", name);
      }
    }

    int64_t id = emit_node_with_fields_begin(name, 0);
    emitf("\"addr\":");
    emit_node_ref_obj(argv[0]);
    emitf(",\"value\":");
    emit_node_ref_obj(argv[1]);
    AttrItem* a = attrs_find_scalar_item(acc.flags, acc.flags_len, "align");
    if (!a) a = attrs_find_scalar_item(acc.root, acc.root_len, "align");
    if (a && a->scalar.kind == ATTR_SCALAR_INT) emitf(",\"align\":%lld", (long long)a->scalar.v.i);
    AttrItem* v = attrs_find_scalar_item(acc.flags, acc.flags_len, "vol");
    if (!v) v = attrs_find_scalar_item(acc.root, acc.root_len, "vol");
    if (v && v->scalar.kind == ATTR_SCALAR_BOOL) emitf(",\"vol\":%s", v->scalar.v.b ? "true" : "false");
    emit_fields_end();
    SIRC_CALL_CLEANUP();
    return id;
  }

  if (!has_dot(name)) {
    const FnEntry* fn = fn_find(name);
    if (!fn) die_at_last("sirc: unknown function '%s'", name);
    if (attrs_has_any(&acc)) die_at_last("sirc: attribute tail not supported on direct function calls");
    if (g_strict && fn->param_count && argc != fn->param_count) {
      strict_failf("sirc.strict.arity", "%s: expected %zu args, got %zu", name, fn->param_count, argc);
    }
    int64_t call = 0;
    if (fn->is_extern) {
      int64_t callee = emit_decl_fn_node(name, fn->sig_type);
      call = emit_call_indirect(fn->ret_type, fn->sig_type, callee, argv, argc);
    } else if (fn->is_defined && fn->fn_node) {
      call = emit_call_direct(fn->ret_type, fn->fn_node, argv, argc);
    } else {
      die_at_last("sirc: function '%s' is not callable (missing definition?)", name);
    }
    SIRC_CALL_CLEANUP();
    return call;
  }

  // mnemonic-style call: tag is the dotted name.
  if (g_strict) {
    if (acc.have_sig) strict_failf("sirc.strict.attr.unsupported", "%s: 'sig' attribute is not supported", name);
    if (acc.have_count) strict_failf("sirc.strict.attr.unsupported", "%s: 'count' attribute is not supported", name);
    const int want = strict_mnemonic_arity(name);
    if (want >= 0 && argc != (size_t)want) {
      strict_failf("sirc.strict.arity", "%s: expected %d args, got %zu", name, want, argc);
    }
    for (size_t i = 0; i < acc.root_len; i++) {
      AttrItem* it = &acc.root[i];
      if (!it->key) continue;
      if (strcmp(it->key, "ty") == 0) {
        if (!(it->scalar.kind == ATTR_SCALAR_STR && it->scalar.v.s && it->scalar.v.s[0])) {
          strict_failf("sirc.strict.attr.bad_type", "%s: ty must be a type name string", name);
        }
      }
    }
  }

  int64_t id = emit_node_with_fields_begin(name, forced_type_ref);
  bool first = true;
  if (argc) {
    emitf("\"args\":[");
    for (size_t i = 0; i < argc; i++) {
      if (i) emitf(",");
      emit_node_ref_obj(argv[i]);
    }
    emitf("]");
    first = false;
  }
  for (size_t i = 0; i < acc.root_len; i++) {
    AttrItem* it = &acc.root[i];
    if (!it->key) continue;
    emitf("%s\"%s\":", first ? "" : ",", it->key);
    if (strcmp(it->key, "ty") == 0 && it->scalar.kind == ATTR_SCALAR_STR && it->scalar.v.s && it->scalar.v.s[0]) {
      const int64_t ty = type_from_name(it->scalar.v.s);
      emit_type_ref_obj(ty);
    } else {
      emit_attr_scalar(&it->scalar);
    }
    first = false;
  }
  if (acc.flags_len) {
    emit_flags_object(acc.flags, acc.flags_len, !first);
    first = false;
  }
  emit_fields_end();

  SIRC_CALL_CLEANUP();
  return id;
}

int64_t sirc_call(char* name, SircExprList* args, SircAttrList* attrs) { return sirc_call_impl(name, args, attrs, 0); }

int64_t sirc_call_typed(char* name, SircExprList* args, SircAttrList* attrs, int64_t type_ref) {
  return sirc_call_impl(name, args, attrs, type_ref);
}

int64_t sirc_stmt_let(char* name, int64_t ty, int64_t value) {
  locals_set(name, ty);
  int64_t id = emit_let_node(name, value);
  free(name);
  return id;
}

int64_t sirc_stmt_return(int64_t value) { return emit_term_ret_node(value); }

void sirc_extern_fn(char* name, SircParamList* params, int64_t ret) {
  // build signature type
  int64_t* tys = NULL;
  size_t n = 0;
  if (params) {
    tys = params->types;
    n = params->len;
  }
  int64_t sig = type_fn(tys, n, ret);
  fn_add_extern(name, sig, ret);
  FnEntry* e = fn_find_mut(name);
  if (e) e->param_count = n;
  free(name);
  if (params) {
    for (size_t i = 0; i < params->len; i++) free(params->names[i]);
    free(params->names);
    free(params->types);
    free(params->nodes);
    free(params);
  }
  locals_clear();
}

void sirc_fn_def(char* name, SircParamList* params, int64_t ret, bool is_public, SircNodeList* stmts) {
  int64_t* ptys = NULL;
  int64_t* pnodes = NULL;
  size_t nparams = 0;
  if (params) {
    ptys = params->types;
    pnodes = params->nodes;
    nparams = params->len;
  }

  int64_t fn_ty = type_fn(ptys, nparams, ret);
  int64_t block = emit_block_node(stmts ? stmts->nodes : NULL, stmts ? stmts->len : 0);
  int64_t fn_node = emit_fn_node(name, is_public, fn_ty, pnodes, nparams, block);
  FnEntry* e = fn_find_mut(name);
  if (e) {
    e->sig_type = fn_ty;
    e->ret_type = ret;
    e->param_count = nparams;
    e->is_defined = true;
    e->is_extern = e->is_extern; // keep if previously declared extern (unusual but harmless)
    e->fn_node = fn_node;
  } else {
    if (g_emit.fns_len == g_emit.fns_cap) {
      g_emit.fns_cap = g_emit.fns_cap ? g_emit.fns_cap * 2 : 32;
      g_emit.fns = (FnEntry*)xrealloc(g_emit.fns, g_emit.fns_cap * sizeof(FnEntry));
    }
    g_emit.fns[g_emit.fns_len++] =
        (FnEntry){.name = xstrdup(name), .sig_type = fn_ty, .ret_type = ret, .param_count = nparams, .is_extern = false, .is_defined = true, .fn_node = fn_node};
  }

  if (params) {
    for (size_t i = 0; i < params->len; i++) free(params->names[i]);
    free(params->names);
    free(params->types);
    free(params->nodes);
    free(params);
  }
  if (stmts) {
    free(stmts->nodes);
    free(stmts);
  }
  free(name);
  locals_clear();
}

static char* default_out_path(const char* in_path) {
  size_t n = strlen(in_path);
  const char* suf = ".jsonl";
  char* out = (char*)xmalloc(n + strlen(suf) + 1);
  memcpy(out, in_path, n);
  memcpy(out + n, suf, strlen(suf) + 1);
  return out;
}

static void usage(FILE* out) {
  fprintf(out,
          "sirc — SIR sugar translator (.sir → .sir.jsonl)\n"
          "\n"
          "Usage:\n"
          "  sirc <input.sir> [-o <output.sir.jsonl>]\n"
          "  sirc --tool -o <output.jsonl> <input.sir>...\n"
          "  sirc --print-support [--format text|json]\n"
          "\n"
          "Options:\n"
          "  --help, -h    Show this help message\n"
          "  --version     Show version information\n"
          "  --ids <mode>  Id mode: string (default) or numeric\n"
          "  --emit-src <none|loc|src_ref|both>  Attach source mapping (default: loc)\n"
          "  --diagnostics <text|json>  Diagnostic output format (default: text)\n"
          "  --all         Report all errors\n"
          "  --strict      Enable frontend hygiene checks (reject unknown attrs for known constructs)\n"
          "  --lint        Parse/validate only (no output)\n"
          "  --tool        Enable filelist + -o output mode\n"
          "  --print-support  Print supported syntax/features and exit\n"
          "  --format <text|json>  Output format for --print-support (default: text)\n"
          "  -o <path>     Write output JSONL to a file\n"
          "\n"
          "License: GPLv3+\n"
          "© 2026 Frogfish — Author: Alexander Croft\n");
}

static void print_support(FILE* out, bool as_json) {
  if (!out) return;
  if (as_json) {
    fputs("{\"tool\":\"sirc\",\"version\":", out);
    json_write_escaped(out, SIRC_VERSION);
    fputs(",\"ids_default\":\"string\",\"ids_modes\":[\"string\",\"numeric\"],\"features\":[", out);
    fputs("\"fun:v1\",\"closure:v1\",\"adt:v1\",\"sem:v1\"", out);
    fputs("],\"emit_src_default\":\"loc\",\"emit_src_modes\":[\"none\",\"loc\",\"src_ref\",\"both\"]", out);
    fputs(",\"strict\":true", out);
    fputs(",\"types\":[", out);
    fputs("\"prim(i8,i16,i32,i64,f32,f64,bool,void,ptr)\",\"^T\",\"array(T,N)\",\"fn(T,...)->R\",\"fun(Sig)\",\"closure(CallSig,EnvTy)\",\"sum{V, V:Ty,...}\"",
          out);
    fputs("],\"expr\":[", out);
    fputs("\"ident\",\"bool\",\"string\",\"int[:type]\",\"float[:type]\",\"call\",\"call as Type\",\"sem.*\"", out);
    fputs("],\"cfg\":[", out);
    fputs("\"block\",\"term.br\",\"term.cbr\",\"term.switch\",\"term.ret\",\"term.trap\",\"term.unreachable\"", out);
    fputs("]}\n", out);
    return;
  }

  fprintf(out, "sirc %s\n", SIRC_VERSION);
  fprintf(out, "\n");
  fprintf(out, "IDs:\n");
  fprintf(out, "  default: string\n");
  fprintf(out, "  modes: string, numeric\n");
  fprintf(out, "\n");
  fprintf(out, "Strict mode:\n");
  fprintf(out, "  --strict: reject unknown/ignored attrs on known constructs; enforce arity on a conservative subset\n");
  fprintf(out, "\n");
  fprintf(out, "Features/packs (emittable):\n");
  fprintf(out, "  - fun:v1\n");
  fprintf(out, "  - closure:v1\n");
  fprintf(out, "  - adt:v1\n");
  fprintf(out, "  - sem:v1\n");
  fprintf(out, "\n");
  fprintf(out, "Type constructors:\n");
  fprintf(out, "  - prims: i8 i16 i32 i64 f32 f64 bool void ptr\n");
  fprintf(out, "  - pointer: ^T\n");
  fprintf(out, "  - array: array(T, N)\n");
  fprintf(out, "  - signature: fn(T, ...) -> R\n");
  fprintf(out, "  - fun type: fun(Sig)\n");
  fprintf(out, "  - closure type: closure(CallSig, EnvTy)\n");
  fprintf(out, "  - sum type: sum{None, Some:T, ...}\n");
  fprintf(out, "\n");
  fprintf(out, "Typed mnemonic calls:\n");
  fprintf(out, "  - tag(args...) ... as Type\n");
  fprintf(out, "\n");
  fprintf(out, "sem:v1 sugar:\n");
  fprintf(out, "  - sem.if(cond, val <expr>, thunk <fun>) as T\n");
  fprintf(out, "  - sem.cond(cond, ...) as T\n");
  fprintf(out, "  - sem.and_sc(lhs, rhsBranch)\n");
  fprintf(out, "  - sem.or_sc(lhs, rhsBranch)\n");
  fprintf(out, "  - sem.switch(scrut, cases:[case <lit> -> <branch>, ...], default:<branch>) as T\n");
  fprintf(out, "  - sem.match_sum(sumTy, scrut, cases:[case <variant> -> <branch>, ...], default:<branch>) as T\n");
  fprintf(out, "  - sem.while(thunk <cond>, thunk <body>)\n");
  fprintf(out, "  - sem.break / sem.continue\n");
  fprintf(out, "  - sem.defer(thunk <cleanup>)\n");
  fprintf(out, "  - sem.scope(defers:[thunk <f>, ...], body: do <stmts...> end)\n");
}

static int compile_one(const char* in_path, FILE* out) {
  if (!in_path || !out) return 2;

  diags_clear();

  FILE* in = fopen(in_path, "rb");
  if (!in) {
    g_emit.input_path = in_path;
    diag_setf("sirc.io.open", "%s: %s", in_path, strerror(errno));
    diag_print_all();
    return 2;
  }

  g_emit.input_path = in_path;
  g_emit.out = out;
  g_emit.next_type_id = 1;
  g_emit.next_node_id = 10;
  g_emit.last_id_line = 0;
  g_emit.line_id_seq = 0;
  g_emit.next_src_id = 1;
  for (size_t i = 0; i < g_emit.srcs_len; i++) free(g_emit.srcs[i].id_str);
  free(g_emit.srcs);
  g_emit.srcs = NULL;
  g_emit.srcs_len = 0;
  g_emit.srcs_cap = 0;

  yyin = in;
  yyrestart(in);
  const int rc = yyparse();
  fclose(in);

  if (rc != 0) {
    if (!g_diag.set && !(g_diag_all && g_diags_len)) diag_setf("sirc.parse", "parse failed");
    diag_print_all();
    return 1;
  }
  if (g_diag.set || (g_diag_all && g_diags_len)) {
    diag_print_all();
    return 1;
  }
  return 0;
}

int main(int argc, char** argv) {
  const char* out_path = NULL;
  char* out_path_owned = NULL;
  bool lint = false;
  bool tool_mode = false;
  bool do_print_support = false;
  bool print_support_json = false;

  const char** inputs = NULL;
  size_t inputs_len = 0;
  size_t inputs_cap = 0;

  g_emit.ids_mode = SIRC_IDS_STRING;
  g_emit.emit_src = SIRC_EMIT_SRC_LOC;

  for (int i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage(stdout);
      return 0;
    }
    if (strcmp(a, "--version") == 0) {
      printf("sirc %s\n", SIRC_VERSION);
      printf("License: GPLv3+\n");
      printf("© 2026 Frogfish — Author: Alexander Croft\n");
      return 0;
    }
    if (strcmp(a, "--print-support") == 0) {
      do_print_support = true;
      continue;
    }
    if (strcmp(a, "--format") == 0) {
      if (i + 1 >= argc) die_at_last("sirc: --format requires text|json");
      const char* f = argv[++i];
      if (strcmp(f, "text") == 0) print_support_json = false;
      else if (strcmp(f, "json") == 0) print_support_json = true;
      else die_at_last("sirc: unknown --format: %s", f);
      continue;
    }
    if (strcmp(a, "--diagnostics") == 0) {
      if (i + 1 >= argc) die_at_last("sirc: --diagnostics requires text|json");
      const char* f = argv[++i];
      if (strcmp(f, "text") == 0) g_diag_format = SIRC_DIAG_TEXT;
      else if (strcmp(f, "json") == 0) g_diag_format = SIRC_DIAG_JSON;
      else die_at_last("sirc: unknown --diagnostics format: %s", f);
      continue;
    }
    if (strcmp(a, "--emit-src") == 0) {
      if (i + 1 >= argc) die_at_last("sirc: --emit-src requires none|loc|src_ref|both");
      const char* m = argv[++i];
      if (strcmp(m, "none") == 0) g_emit.emit_src = SIRC_EMIT_SRC_NONE;
      else if (strcmp(m, "loc") == 0) g_emit.emit_src = SIRC_EMIT_SRC_LOC;
      else if (strcmp(m, "src_ref") == 0) g_emit.emit_src = SIRC_EMIT_SRC_SRCREF;
      else if (strcmp(m, "both") == 0) g_emit.emit_src = SIRC_EMIT_SRC_BOTH;
      else die_at_last("sirc: unknown --emit-src mode: %s", m);
      continue;
    }
    if (strcmp(a, "--all") == 0) {
      g_diag_all = true;
      continue;
    }
    if (strcmp(a, "--strict") == 0) {
      g_strict = true;
      continue;
    }
    if (strcmp(a, "--lint") == 0) {
      lint = true;
      continue;
    }
    if (strcmp(a, "--tool") == 0) {
      tool_mode = true;
      continue;
    }
    if (strcmp(a, "-o") == 0) {
      if (i + 1 >= argc) die_at_last("sirc: -o requires a path");
      out_path = argv[++i];
      continue;
    }
    if (strcmp(a, "--ids") == 0) {
      if (i + 1 >= argc) die_at_last("sirc: --ids requires a mode (string|numeric)");
      const char* m = argv[++i];
      if (strcmp(m, "string") == 0) g_emit.ids_mode = SIRC_IDS_STRING;
      else if (strcmp(m, "numeric") == 0) g_emit.ids_mode = SIRC_IDS_NUMERIC;
      else die_at_last("sirc: unknown --ids mode: %s", m);
      continue;
    }
    if (a[0] == '-') die_at_last("sirc: unknown flag: %s", a);
    if (inputs_len == inputs_cap) {
      inputs_cap = inputs_cap ? inputs_cap * 2 : 8;
      inputs = (const char**)realloc((void*)inputs, inputs_cap * sizeof(const char*));
      if (!inputs) die_at_last("sirc: out of memory");
    }
    inputs[inputs_len++] = a;
  }

  if (do_print_support) {
    print_support(stdout, print_support_json);
    free(out_path_owned);
    free((void*)inputs);
    return 0;
  }

  if (!inputs_len) die_at_last("sirc: missing input .sir path");
  if (!tool_mode && inputs_len != 1) die_at_last("sirc: multiple inputs require --tool");

  FILE* out = NULL;
  if (lint) {
    out = fopen("/dev/null", "wb");
    if (!out) die_at_last("sirc: failed to open /dev/null");
  } else {
    if (!out_path) {
      if (tool_mode) die_at_last("sirc: --tool requires -o <output.jsonl>");
      out_path_owned = default_out_path(inputs[0]);
      out_path = out_path_owned;
    }
    out = fopen(out_path, "wb");
    if (!out) die_at_last("%s: %s", out_path, strerror(errno));
  }

  int tool_rc = 0;
  for (size_t fi = 0; fi < inputs_len; fi++) {
    sirc_reset_compiler_state();

    // In --tool mode, avoid producing partial output for a failing file:
    // compile into a temp file and only append on success.
    FILE* dst = out;
    FILE* tmp = NULL;
    if (tool_mode && !lint) {
      tmp = tmpfile();
      if (!tmp) {
        g_emit.input_path = inputs[fi];
        diag_setf("sirc.io.tmpfile", "failed to create temp file");
        diag_print_one();
        tool_rc = 2;
        break;
      }
      dst = tmp;
    }

    const int rc = compile_one(inputs[fi], dst);
    if (tmp) {
      if (rc == 0) {
        rewind(tmp);
        char buf[8192];
        size_t n = 0;
        while ((n = fread(buf, 1, sizeof(buf), tmp)) != 0) {
          if (fwrite(buf, 1, n, out) != n) {
            fclose(tmp);
            g_emit.input_path = inputs[fi];
            diag_setf("sirc.io.write", "failed to write output");
            diag_print_one();
            tool_rc = 2;
            goto done;
          }
        }
      } else {
      }
      fclose(tmp);
    }

    if (rc != 0) {
      if (!tool_rc) tool_rc = rc;
      if (!g_diag_all) break;
    }
  }

done:
  fclose(out);

  free(out_path_owned);
  free((void*)inputs);

  return tool_rc;
}
