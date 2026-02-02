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

typedef struct NamedTypeEntry {
  char* name;
  int64_t type_id;
} NamedTypeEntry;

typedef struct Emitter {
  FILE* out;
  const char* input_path;

  int64_t next_type_id;
  int64_t next_node_id;

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

static int64_t type_array(int64_t of, int64_t len) {
  char key[96];
  snprintf(key, sizeof(key), "array:%lld,%lld", (long long)of, (long long)len);
  int64_t id = type_lookup(key);
  if (id) return id;
  id = type_insert(key);
  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":%lld,\"kind\":\"array\",\"of\":%lld,\"len\":%lld}\n", (long long)id, (long long)of,
        (long long)len);
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
  int64_t alias = named_type_lookup(name);
  if (alias) return alias;
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

static int64_t emit_bparam_node(int64_t type_ref) {
  int64_t id = next_node_id();
  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":%lld,\"tag\":\"bparam\",\"type_ref\":%lld}\n", (long long)id, (long long)type_ref);
  return id;
}

static int64_t emit_name_node(const char* name) {
  int64_t id = emit_node_with_fields_begin("name", 0);
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
  for (size_t i = 0; i < g_emit.features_len; i++) free(g_emit.features[i]);
  g_emit.features_len = 0;
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

static void emit_type_ref_obj(int64_t ty) { emitf("{\"t\":\"ref\",\"k\":\"type\",\"id\":%lld}", (long long)ty); }

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
  emitf("{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":%lld,\"tag\":\"block\",\"fields\":{", (long long)id);
  bool any = false;
  if (params && nparams) {
    emitf("\"params\":[");
    for (size_t i = 0; i < nparams; i++) {
      if (i) emitf(",");
      emitf("{\"t\":\"ref\",\"id\":%lld}", (long long)params[i]);
    }
    emitf("]");
    any = true;
  }
  if (stmts) {
    if (any) emitf(",");
    emitf("\"stmts\":[");
    for (size_t i = 0; i < nstmts; i++) {
      if (i) emitf(",");
      emitf("{\"t\":\"ref\",\"id\":%lld}", (long long)stmts[i]);
    }
    emitf("]");
  }
  emitf("}}\n");
}

int64_t sirc_select(int64_t ty, int64_t cond, int64_t then_v, int64_t else_v) {
  int64_t id = emit_node_with_fields_begin("select", ty);
  emitf("\"args\":[{\"t\":\"ref\",\"id\":%lld},{\"t\":\"ref\",\"id\":%lld},{\"t\":\"ref\",\"id\":%lld}]",
        (long long)cond, (long long)then_v, (long long)else_v);
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
  emitf(",\"args\":[{\"t\":\"ref\",\"id\":%lld},{\"t\":\"ref\",\"id\":%lld}]", (long long)base, (long long)index);
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
  emitf("\"to\":{\"t\":\"ref\",\"id\":%lld}", (long long)bid);
  if (args && args->len) {
    emitf(",\"args\":[");
    for (size_t i = 0; i < args->len; i++) {
      if (i) emitf(",");
      emitf("{\"t\":\"ref\",\"id\":%lld}", (long long)args->nodes[i]);
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

int64_t sirc_term_cbr(int64_t cond, char* then_block_name, char* else_block_name) {
  if (!then_block_name || !else_block_name) die_at_last("sirc: term.cbr requires then/else targets");
  int64_t then_id = block_id_for_name(then_block_name);
  int64_t else_id = block_id_for_name(else_block_name);
  free(then_block_name);
  free(else_block_name);

  int64_t id = emit_node_with_fields_begin("term.cbr", 0);
  emitf("\"cond\":{\"t\":\"ref\",\"id\":%lld}", (long long)cond);
  emitf(",\"then\":{\"to\":{\"t\":\"ref\",\"id\":%lld}}", (long long)then_id);
  emitf(",\"else\":{\"to\":{\"t\":\"ref\",\"id\":%lld}}", (long long)else_id);
  emit_fields_end();
  return id;
}

int64_t sirc_term_switch(int64_t scrut, SircSwitchCaseList* cases, char* default_block_name) {
  if (!default_block_name) die_at_last("sirc: term.switch requires default target");
  int64_t def_id = block_id_for_name(default_block_name);
  free(default_block_name);

  int64_t id = emit_node_with_fields_begin("term.switch", 0);
  emitf("\"scrut\":{\"t\":\"ref\",\"id\":%lld}", (long long)scrut);
  emitf(",\"cases\":[");
  if (cases) {
    for (size_t i = 0; i < cases->len; i++) {
      if (i) emitf(",");
      emitf("{\"lit\":{\"t\":\"ref\",\"id\":%lld},\"to\":{\"t\":\"ref\",\"id\":%lld}}", (long long)cases->lit_nodes[i],
            (long long)cases->to_blocks[i]);
    }
  }
  emitf("]");
  emitf(",\"default\":{\"to\":{\"t\":\"ref\",\"id\":%lld}}", (long long)def_id);
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
  if (has_value) emitf("\"value\":{\"t\":\"ref\",\"id\":%lld}", (long long)value_node);
  emit_fields_end();
  return id;
}

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

void sirc_fn_def_cfg(char* name, SircParamList* params, int64_t ret, int64_t entry_block, SircNodeList* blocks) {
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
  emitf(",\"params\":[");
  for (size_t i = 0; i < nparams; i++) {
    if (i) emitf(",");
    emitf("{\"t\":\"ref\",\"id\":%lld}", (long long)pnodes[i]);
  }
  emitf("],\"entry\":{\"t\":\"ref\",\"id\":%lld}", (long long)entry_block);
  emitf(",\"blocks\":[");
  if (blocks) {
    for (size_t i = 0; i < blocks->len; i++) {
      if (i) emitf(",");
      emitf("{\"t\":\"ref\",\"id\":%lld}", (long long)blocks->nodes[i]);
    }
  }
  emitf("]");
  emit_fields_end();

  FnEntry* e = fn_find_mut(name);
  if (e) {
    e->sig_type = fn_ty;
    e->ret_type = ret;
    e->is_defined = true;
    e->fn_node = id;
  } else {
    if (g_emit.fns_len == g_emit.fns_cap) {
      g_emit.fns_cap = g_emit.fns_cap ? g_emit.fns_cap * 2 : 32;
      g_emit.fns = (FnEntry*)xrealloc(g_emit.fns, g_emit.fns_cap * sizeof(FnEntry));
    }
    g_emit.fns[g_emit.fns_len++] =
        (FnEntry){.name = xstrdup(name), .sig_type = fn_ty, .ret_type = ret, .is_extern = false, .is_defined = true, .fn_node = id};
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
}

int64_t sirc_call(char* name, SircExprList* args) {
  size_t argc = args ? args->len : 0;
  int64_t* argv = args ? args->nodes : NULL;

  if (strcmp(name, "alloca") == 0) {
    if (argc < 1 || argc > 2) die_at_last("sirc: alloca(type[, count]) expected");
    const char* tname = lookup_node_name(argv[0]);
    if (!tname) die_at_last("sirc: alloca first arg must be a type name identifier");
    int64_t ty = type_from_name(tname);

    int64_t id = emit_node_with_fields_begin("alloca", 0);
    emitf("\"ty\":");
    emit_type_ref_obj(ty);
    if (argc == 2) {
      emitf(",\"flags\":{");
      emitf("\"count\":{\"t\":\"ref\",\"id\":%lld}", (long long)argv[1]);
      emitf("}");
    }
    emit_fields_end();

    free(name);
    free(args ? args->nodes : NULL);
    free(args);
    return id;
  }

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
