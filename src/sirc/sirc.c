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
  bool is_extern;
  bool is_defined;
  int64_t fn_node; // node id of "fn" node (when defined)
} FnEntry;

typedef struct Emitter {
  FILE* out;
  const char* input_path;

  int64_t next_type_id;
  int64_t next_node_id;

  TypeEntry* types;
  size_t types_len;
  size_t types_cap;

  FnEntry* fns;
  size_t fns_len;
  size_t fns_cap;

  // unit meta
  char* unit;
  char* target;
} Emitter;

static Emitter g_emit = {0};

int sirc_last_line = 1;
int sirc_last_col = 1;
char sirc_last_tok[64] = {0};

const char* sirc_input_path(void) { return g_emit.input_path ? g_emit.input_path : "<input>"; }

static void die_at_last(const char* fmt, ...) {
  fprintf(stderr, "%s:%d:%d: error: ", sirc_input_path(), sirc_last_line, sirc_last_col);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  if (sirc_last_tok[0]) fprintf(stderr, " (near '%s')", sirc_last_tok);
  fputc('\n', stderr);
  exit(2);
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
  if (g_emit.target) {
    emitf(",\"ext\":{\"target\":{\"triple\":");
    json_write_escaped(g_emit.out, g_emit.target);
    emitf("}}");
  }
  emitf("}\n");
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
  return id;
}

static int64_t type_prim(const char* prim) {
  char key[64];
  snprintf(key, sizeof(key), "prim:%s", prim);
  int64_t id = type_lookup(key);
  if (id) return id;
  id = type_insert(key);
  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":%lld,\"kind\":\"prim\",\"prim\":", (long long)id);
  json_write_escaped(g_emit.out, prim);
  emitf("}\n");
  return id;
}

static int64_t prim_id_or_zero(const char* prim) {
  char key[64];
  snprintf(key, sizeof(key), "prim:%s", prim);
  return type_lookup(key);
}

static int64_t type_ptr(int64_t of) {
  char key[64];
  snprintf(key, sizeof(key), "ptr:%lld", (long long)of);
  int64_t id = type_lookup(key);
  if (id) return id;
  id = type_insert(key);
  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":%lld,\"kind\":\"ptr\",\"of\":%lld}\n", (long long)id, (long long)of);
  return id;
}

static int64_t type_fn(const int64_t* params, size_t n, int64_t ret) {
  // canonical key: fn:p1,p2->r
  size_t cap = 128 + n * 24;
  char* key = (char*)xmalloc(cap);
  size_t off = 0;
  off += (size_t)snprintf(key + off, cap - off, "fn:");
  for (size_t i = 0; i < n; i++) {
    off += (size_t)snprintf(key + off, cap - off, "%s%lld", (i ? "," : ""), (long long)params[i]);
  }
  off += (size_t)snprintf(key + off, cap - off, "->%lld", (long long)ret);

  int64_t id = type_lookup(key);
  if (id) {
    free(key);
    return id;
  }
  id = type_insert(key);
  free(key);

  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":%lld,\"kind\":\"fn\",\"params\":[", (long long)id);
  for (size_t i = 0; i < n; i++) {
    if (i) emitf(",");
    emitf("%lld", (long long)params[i]);
  }
  emitf("],\"ret\":%lld}\n", (long long)ret);
  return id;
}

static int64_t type_from_name(const char* name) {
  if (!name) return 0;
  if (strcmp(name, "i8") == 0) return type_prim("i8");
  if (strcmp(name, "i16") == 0) return type_prim("i16");
  if (strcmp(name, "i32") == 0) return type_prim("i32");
  if (strcmp(name, "i64") == 0) return type_prim("i64");
  if (strcmp(name, "f32") == 0) return type_prim("f32");
  if (strcmp(name, "f64") == 0) return type_prim("f64");
  if (strcmp(name, "bool") == 0) return type_prim("bool");
  if (strcmp(name, "ptr") == 0) {
    int64_t i8 = type_prim("i8");
    return type_ptr(i8);
  }
  // named types not implemented yet
  die_at_last("sirc: unknown type name '%s' (only prim/bool/ptr supported for now)", name);
  return 0;
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
      (FnEntry){.name = xstrdup(name), .sig_type = sig_ty, .ret_type = ret_ty, .is_extern = true, .is_defined = false, .fn_node = 0};
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

static int64_t next_node_id(void) { return g_emit.next_node_id++; }

static int64_t emit_node_with_fields_begin(const char* tag, int64_t type_ref) {
  int64_t id = next_node_id();
  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":%lld,\"tag\":", (long long)id);
  json_write_escaped(g_emit.out, tag);
  if (type_ref) emitf(",\"type_ref\":%lld", (long long)type_ref);
  emitf(",\"fields\":{");
  return id;
}

static void emit_fields_end(void) { emitf("}}\n"); }

static int64_t emit_param_node(const char* name, int64_t type_ref) {
  int64_t id = emit_node_with_fields_begin("param", type_ref);
  emitf("\"name\":");
  json_write_escaped(g_emit.out, name);
  emit_fields_end();
  return id;
}

static int64_t emit_name_node(const char* name) {
  int64_t id = emit_node_with_fields_begin("name", 0);
  emitf("\"name\":");
  json_write_escaped(g_emit.out, name);
  emit_fields_end();
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
  int64_t id = emit_node_with_fields_begin("cstr", 0);
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
  emitf("\"sig\":{\"t\":\"ref\",\"id\":%lld},\"args\":[{\"t\":\"ref\",\"id\":%lld}", (long long)sig_type, (long long)callee_node);
  for (size_t i = 0; i < argc; i++) {
    emitf(",{\"t\":\"ref\",\"id\":%lld}", (long long)args[i]);
  }
  emitf("]");
  emit_fields_end();
  return id;
}

static int64_t emit_call_mnemonic(const char* tag, int64_t type_ref, const int64_t* args, size_t argc) {
  int64_t id = emit_node_with_fields_begin(tag, type_ref);
  emitf("\"args\":[");
  for (size_t i = 0; i < argc; i++) {
    if (i) emitf(",");
    emitf("{\"t\":\"ref\",\"id\":%lld}", (long long)args[i]);
  }
  emitf("]");
  emit_fields_end();
  return id;
}

static int64_t emit_call_direct(int64_t ret_type, int64_t callee_fn_node, const int64_t* args, size_t argc) {
  int64_t id = emit_node_with_fields_begin("call", ret_type);
  emitf("\"callee\":{\"t\":\"ref\",\"id\":%lld},\"args\":[", (long long)callee_fn_node);
  for (size_t i = 0; i < argc; i++) {
    if (i) emitf(",");
    emitf("{\"t\":\"ref\",\"id\":%lld}", (long long)args[i]);
  }
  emitf("]");
  emit_fields_end();
  return id;
}

static int64_t emit_let_node(const char* name, int64_t value_node) {
  int64_t id = emit_node_with_fields_begin("let", 0);
  emitf("\"name\":");
  json_write_escaped(g_emit.out, name);
  emitf(",\"value\":{\"t\":\"ref\",\"id\":%lld}", (long long)value_node);
  emit_fields_end();
  return id;
}

static int64_t emit_term_ret_node(int64_t value_node) {
  int64_t id = emit_node_with_fields_begin("term.ret", 0);
  emitf("\"value\":{\"t\":\"ref\",\"id\":%lld}", (long long)value_node);
  emit_fields_end();
  return id;
}

static int64_t emit_block_node(const int64_t* stmts, size_t n) {
  int64_t id = emit_node_with_fields_begin("block", 0);
  emitf("\"stmts\":[");
  for (size_t i = 0; i < n; i++) {
    if (i) emitf(",");
    emitf("{\"t\":\"ref\",\"id\":%lld}", (long long)stmts[i]);
  }
  emitf("]");
  emit_fields_end();
  return id;
}

static int64_t emit_fn_node(const char* name, int64_t fn_type, const int64_t* params, size_t nparams, int64_t body_block) {
  int64_t id = emit_node_with_fields_begin("fn", fn_type);
  emitf("\"name\":");
  json_write_escaped(g_emit.out, name);
  emitf(",\"params\":[");
  for (size_t i = 0; i < nparams; i++) {
    if (i) emitf(",");
    emitf("{\"t\":\"ref\",\"id\":%lld}", (long long)params[i]);
  }
  emitf("],\"body\":{\"t\":\"ref\",\"id\":%lld}", (long long)body_block);
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

int64_t sirc_type_from_name(char* name) {
  int64_t id = type_from_name(name);
  free(name);
  return id;
}

int64_t sirc_type_ptr_of(int64_t of) { return type_ptr(of); }

typedef struct SircParamList {
  char** names;
  int64_t* types;
  int64_t* nodes;
  size_t len;
  size_t cap;
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

static SircParamList* params_new(void) { return (SircParamList*)calloc(1, sizeof(SircParamList)); }
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
  int64_t pn = emit_param_node(name, ty);
  p->names[p->len] = name;
  p->types[p->len] = ty;
  p->nodes[p->len] = pn;
  p->len++;
}

static void nodelist_add(SircNodeList* l, int64_t n) {
  if (!l) return;
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

SircParamList* sirc_params_empty(void) { return params_new(); }
SircParamList* sirc_params_single(char* name, int64_t ty) {
  SircParamList* p = params_new();
  params_add(p, name, ty);
  return p;
}
SircParamList* sirc_params_append(SircParamList* p, char* name, int64_t ty) {
  if (!p) p = params_new();
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

static bool has_dot(const char* s) { return s && strchr(s, '.') != NULL; }

static unsigned natural_align_for_type_name(const char* tname) {
  if (!tname) return 1;
  if (strcmp(tname, "i8") == 0) return 1;
  if (strcmp(tname, "i16") == 0) return 2;
  if (strcmp(tname, "i32") == 0) return 4;
  if (strcmp(tname, "i64") == 0) return 8;
  if (strcmp(tname, "f32") == 0) return 4;
  if (strcmp(tname, "f64") == 0) return 8;
  if (strcmp(tname, "bool") == 0) return 1;
  if (strcmp(tname, "ptr") == 0) return 8; // assumes 64-bit host for now
  return 1;
}

static int64_t emit_load_node(const char* tname, int64_t type_ref, int64_t addr_node) {
  char tag[32];
  snprintf(tag, sizeof(tag), "load.%s", tname);
  int64_t id = emit_node_with_fields_begin(tag, type_ref);
  emitf("\"addr\":{\"t\":\"ref\",\"id\":%lld}", (long long)addr_node);
  emitf(",\"align\":%u", natural_align_for_type_name(tname));
  emit_fields_end();
  return id;
}

static int64_t emit_store_node(const char* tname, int64_t addr_node, int64_t value_node) {
  char tag[32];
  snprintf(tag, sizeof(tag), "store.%s", tname);
  int64_t id = emit_node_with_fields_begin(tag, 0);
  emitf("\"addr\":{\"t\":\"ref\",\"id\":%lld},\"value\":{\"t\":\"ref\",\"id\":%lld}", (long long)addr_node, (long long)value_node);
  emitf(",\"align\":%u", natural_align_for_type_name(tname));
  emit_fields_end();
  return id;
}

int64_t sirc_select(int64_t ty, int64_t cond, int64_t then_v, int64_t else_v) {
  int64_t id = emit_node_with_fields_begin("select", ty);
  emitf("\"args\":[{\"t\":\"ref\",\"id\":%lld},{\"t\":\"ref\",\"id\":%lld},{\"t\":\"ref\",\"id\":%lld}]",
        (long long)cond, (long long)then_v, (long long)else_v);
  emit_fields_end();
  return id;
}

int64_t sirc_call(char* name, SircExprList* args) {
  size_t argc = args ? args->len : 0;
  int64_t* argv = args ? args->nodes : NULL;

  if (strncmp(name, "load.", 5) == 0) {
    const char* tname = name + 5;
    if (argc != 1) die_at_last("sirc: %s requires 1 arg (addr)", name);
    int64_t ty = type_from_name(tname);
    int64_t out = emit_load_node(tname, ty, argv[0]);
    free(name);
    free(args ? args->nodes : NULL);
    free(args);
    return out;
  }
  if (strncmp(name, "store.", 6) == 0) {
    const char* tname = name + 6;
    if (argc != 2) die_at_last("sirc: %s requires 2 args (addr, value)", name);
    int64_t out = emit_store_node(tname, argv[0], argv[1]);
    free(name);
    free(args ? args->nodes : NULL);
    free(args);
    return out;
  }

  if (!has_dot(name)) {
    const FnEntry* fn = fn_find(name);
    if (!fn) die_at_last("sirc: unknown function '%s'", name);
    int64_t call = 0;
    if (fn->is_extern) {
      int64_t callee = emit_decl_fn_node(name, fn->sig_type);
      call = emit_call_indirect(fn->ret_type, fn->sig_type, callee, argv, argc);
    } else if (fn->is_defined && fn->fn_node) {
      call = emit_call_direct(fn->ret_type, fn->fn_node, argv, argc);
    } else {
      die_at_last("sirc: function '%s' is not callable (missing definition?)", name);
    }
    free(name);
    free(args ? args->nodes : NULL);
    free(args);
    return call;
  }

  // mnemonic-style call: tag is the dotted name.
  int64_t call = emit_call_mnemonic(name, 0, argv, argc);
  free(name);
  free(args ? args->nodes : NULL);
  free(args);
  return call;
}

int64_t sirc_stmt_let(char* name, int64_t ty, int64_t value) {
  (void)ty;
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
  free(name);
  if (params) {
    for (size_t i = 0; i < params->len; i++) free(params->names[i]);
    free(params->names);
    free(params->types);
    free(params->nodes);
    free(params);
  }
}

void sirc_fn_def(char* name, SircParamList* params, int64_t ret, SircNodeList* stmts) {
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
  int64_t fn_node = emit_fn_node(name, fn_ty, pnodes, nparams, block);
  FnEntry* e = fn_find_mut(name);
  if (e) {
    e->sig_type = fn_ty;
    e->ret_type = ret;
    e->is_defined = true;
    e->is_extern = e->is_extern; // keep if previously declared extern (unusual but harmless)
    e->fn_node = fn_node;
  } else {
    if (g_emit.fns_len == g_emit.fns_cap) {
      g_emit.fns_cap = g_emit.fns_cap ? g_emit.fns_cap * 2 : 32;
      g_emit.fns = (FnEntry*)xrealloc(g_emit.fns, g_emit.fns_cap * sizeof(FnEntry));
    }
    g_emit.fns[g_emit.fns_len++] =
        (FnEntry){.name = xstrdup(name), .sig_type = fn_ty, .ret_type = ret, .is_extern = false, .is_defined = true, .fn_node = fn_node};
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
}

static char* default_out_path(const char* in_path) {
  size_t n = strlen(in_path);
  const char* suf = ".jsonl";
  char* out = (char*)xmalloc(n + strlen(suf) + 1);
  memcpy(out, in_path, n);
  memcpy(out + n, suf, strlen(suf) + 1);
  return out;
}

int main(int argc, char** argv) {
  const char* in_path = NULL;
  const char* out_path = NULL;
  char* out_path_owned = NULL;

  for (int i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      fprintf(stderr, "Usage: sirc <input.sir> [-o <output.sir.jsonl>]\n");
      return 0;
    }
    if (strcmp(a, "-o") == 0) {
      if (i + 1 >= argc) die_at_last("sirc: -o requires a path");
      out_path = argv[++i];
      continue;
    }
    if (a[0] == '-') die_at_last("sirc: unknown flag: %s", a);
    if (!in_path) in_path = a;
    else die_at_last("sirc: unexpected arg: %s", a);
  }
  if (!in_path) die_at_last("sirc: missing input .sir path");
  if (!out_path) {
    out_path_owned = default_out_path(in_path);
    out_path = out_path_owned;
  }

  FILE* in = fopen(in_path, "rb");
  if (!in) die_at_last("%s: %s", in_path, strerror(errno));
  FILE* out = fopen(out_path, "wb");
  if (!out) die_at_last("%s: %s", out_path, strerror(errno));

  g_emit.out = out;
  g_emit.input_path = in_path;
  g_emit.next_type_id = 1;
  g_emit.next_node_id = 10;

  yyin = in;
  yyrestart(in);
  int rc = yyparse();

  fclose(in);
  fclose(out);

  free(out_path_owned);

  return rc == 0 ? 0 : 1;
}
