#include "sir_jsonl.h"

#include "sem_hosted.h"
#include "sir_module.h"

#include "json.h"
#include "sircc.h"

#include <stdbool.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

typedef struct type_info {
  bool present;
  bool is_fn;
  bool is_array;
  bool is_ptr;
  bool is_struct;
  bool is_fun;
  bool layout_visiting;
  sir_prim_type_t prim; // for prim
  uint32_t* params;     // for fn, arena-owned
  uint32_t param_count;
  uint32_t ret; // SIR type id
  uint32_t array_of;
  uint32_t array_len;
  uint32_t ptr_of;
  uint32_t fun_sig; // for fun: SIR type id of underlying fn signature
  uint32_t* struct_fields;         // arena-owned; SIR type ids
  uint32_t* struct_field_align;    // arena-owned; 0 means default
  uint32_t struct_field_count;
  bool struct_packed;
  uint32_t struct_align_override;  // 0 means computed
  uint32_t loc_line;
} type_info_t;

typedef enum sym_init_kind {
  SYM_INIT_NONE = 0,
  SYM_INIT_NUM,
  SYM_INIT_NODE,
} sym_init_kind_t;

typedef struct sym_info {
  bool present;
  const char* name; // arena-owned
  const char* kind; // arena-owned (var/const/...)
  uint32_t type_ref;
  sym_init_kind_t init_kind;
  int64_t init_num;
  uint32_t init_node;
  sir_global_id_t gid; // sircore global id (1-based) once created
  uint32_t loc_line;
} sym_info_t;

typedef struct node_info {
  bool present;
  const char* tag;       // arena-owned
  uint32_t type_ref;     // 0 if missing
  JsonValue* fields_obj; // object or NULL
  uint32_t loc_line;
} node_info_t;

typedef enum val_kind {
  VK_INVALID = 0,
  VK_I1,
  VK_I8,
  VK_I16,
  VK_I32,
  VK_I64,
  VK_PTR,
  VK_BOOL,
  VK_F32,
  VK_F64,
} val_kind_t;

typedef struct sirj_ctx {
  Arena arena;

  type_info_t* types; // indexed by type id (0..cap-1)
  uint32_t type_cap;

  node_info_t* nodes; // indexed by node id
  uint32_t node_cap;

  sym_info_t* syms; // indexed by sym id
  uint32_t symrec_cap;

  // Lowering maps
  sir_sym_id_t* sym_by_node; // indexed by node id; 0 means unset
  uint32_t sym_cap;

  sir_val_id_t* val_by_node; // indexed by node id; stores slot+1 (0 means unset)
  val_kind_t* kind_by_node;  // indexed by node id
  uint32_t val_cap;
  uint32_t kind_cap;

  sir_val_id_t next_slot;

  // sem:v1 scoped defers (stack of fun.sym node ids).
  uint32_t defers[64];
  uint32_t defer_count;

  // Small per-function constants (slots are function-local).
  sir_val_id_t cached_true_slot;
  sir_val_id_t cached_false_slot;

  // Lowering context
  bool in_cfg; // true while lowering a CFG-form fn.blocks block

  sir_module_builder_t* mb;
  sir_func_id_t fn;
  sir_func_id_t* func_by_node; // node_id -> func_id (0 if none)
  uint32_t func_by_node_cap;

  // Primitive module type ids
  sir_type_id_t ty_i1;
  sir_type_id_t ty_i8;
  sir_type_id_t ty_i16;
  sir_type_id_t ty_i32;
  sir_type_id_t ty_i64;
  sir_type_id_t ty_ptr;
  sir_type_id_t ty_bool;
  sir_type_id_t ty_f32;
  sir_type_id_t ty_f64;

  // Current-function param bindings (name -> slot).
  struct {
    const char* name;
    sir_val_id_t slot;
    val_kind_t kind;
  } params[32];
  uint32_t param_count;

  // Diagnostics
  sem_diag_format_t diag_format;
  const char* cur_path;
  bool diag_all;
  struct {
    const char* code;
    char msg[256];
    const char* path;
    uint32_t line;
    uint32_t node_id;
    const char* tag;
    uint32_t fid;
    uint32_t ip;
    const char* op;
  } diags[16];
  uint32_t diag_count;
  struct {
    bool set;
    const char* code;
    char msg[256];
    const char* path;
    uint32_t line;
    uint32_t node_id;
    const char* tag;
    uint32_t fid;
    uint32_t ip;
    const char* op;
  } diag;
} sirj_ctx_t;

static bool eval_node(sirj_ctx_t* c, uint32_t node_id, sir_val_id_t* out_slot, val_kind_t* out_kind);
static bool resolve_internal_func_by_name(const sirj_ctx_t* c, const char* nm, sir_func_id_t* out);
static bool parse_const_i32_value(const sirj_ctx_t* c, uint32_t node_id, int32_t* out);

static void sirj_diag_setf(sirj_ctx_t* c, const char* code, const char* path, uint32_t line, uint32_t node_id, const char* tag, const char* fmt,
                           ...) {
  if (!c) return;
  va_list ap;
  va_start(ap, fmt);
  char tmp[256];
  (void)vsnprintf(tmp, sizeof(tmp), fmt ? fmt : "error", ap);
  va_end(ap);

  if (!c->diag.set) {
    c->diag.set = true;
    c->diag.code = code ? code : "sem.error";
    c->diag.path = path;
    c->diag.line = line;
    c->diag.node_id = node_id;
    c->diag.tag = tag;
    c->diag.fid = 0;
    c->diag.ip = 0;
    c->diag.op = NULL;
    memcpy(c->diag.msg, tmp, sizeof(tmp));
  }

  if (c->diag_all && c->diag_count < (uint32_t)(sizeof(c->diags) / sizeof(c->diags[0]))) {
    const uint32_t i = c->diag_count++;
    c->diags[i].code = code ? code : "sem.error";
    c->diags[i].path = path;
    c->diags[i].line = line;
    c->diags[i].node_id = node_id;
    c->diags[i].tag = tag;
    c->diags[i].fid = 0;
    c->diags[i].ip = 0;
    c->diags[i].op = NULL;
    memcpy(c->diags[i].msg, tmp, sizeof(tmp));
  }
}

static void sirj_diag_setf_ex(sirj_ctx_t* c, const char* code, const char* path, uint32_t line, uint32_t node_id, const char* tag, uint32_t fid,
                              uint32_t ip, const char* op, const char* fmt, ...) {
  if (!c) return;
  va_list ap;
  va_start(ap, fmt);
  char tmp[256];
  (void)vsnprintf(tmp, sizeof(tmp), fmt ? fmt : "error", ap);
  va_end(ap);

  if (!c->diag.set) {
    c->diag.set = true;
    c->diag.code = code ? code : "sem.error";
    c->diag.path = path;
    c->diag.line = line;
    c->diag.node_id = node_id;
    c->diag.tag = tag;
    c->diag.fid = fid;
    c->diag.ip = ip;
    c->diag.op = op;
    memcpy(c->diag.msg, tmp, sizeof(tmp));
  }

  if (c->diag_all && c->diag_count < (uint32_t)(sizeof(c->diags) / sizeof(c->diags[0]))) {
    const uint32_t i = c->diag_count++;
    c->diags[i].code = code ? code : "sem.error";
    c->diags[i].path = path;
    c->diags[i].line = line;
    c->diags[i].node_id = node_id;
    c->diags[i].tag = tag;
    c->diags[i].fid = fid;
    c->diags[i].ip = ip;
    c->diags[i].op = op;
    memcpy(c->diags[i].msg, tmp, sizeof(tmp));
  }
}

static void sem_json_write_escaped(FILE* out, const char* s) {
  if (!out) return;
  if (!s) s = "";
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    const unsigned char ch = *p;
    if (ch == '\\' || ch == '"') {
      fputc('\\', out);
      fputc((int)ch, out);
    } else if (ch == '\n') {
      fputs("\\n", out);
    } else if (ch == '\r') {
      fputs("\\r", out);
    } else if (ch == '\t') {
      fputs("\\t", out);
    } else if (ch < 0x20) {
      fprintf(out, "\\u%04x", (unsigned)ch);
    } else {
      fputc((int)ch, out);
    }
  }
}

static void sem_print_one_diag(sem_diag_format_t fmt, const char* code, const char* msg, const char* path, uint32_t line, uint32_t node,
                               const char* tag, uint32_t fid, uint32_t ip, const char* op) {
  if (!code) code = "sem.error";
  if (!msg || !msg[0]) msg = "error";
  if (!path) path = "";
  if (!tag) tag = "";
  if (!op) op = "";

  if (fmt == SEM_DIAG_JSON) {
    fprintf(stderr, "{\"tool\":\"sem\",\"code\":\"");
    sem_json_write_escaped(stderr, code);
    fprintf(stderr, "\",\"message\":\"");
    sem_json_write_escaped(stderr, msg);
    fprintf(stderr, "\"");
    if (path[0]) {
      fprintf(stderr, ",\"path\":\"");
      sem_json_write_escaped(stderr, path);
      fprintf(stderr, "\"");
    }
    if (line) fprintf(stderr, ",\"line\":%u", (unsigned)line);
    if (node) fprintf(stderr, ",\"node\":%u", (unsigned)node);
    if (fid) fprintf(stderr, ",\"fid\":%u", (unsigned)fid);
    if (fid) fprintf(stderr, ",\"ip\":%u", (unsigned)ip);
    if (op[0]) {
      fprintf(stderr, ",\"op\":\"");
      sem_json_write_escaped(stderr, op);
      fprintf(stderr, "\"");
    }
    if (tag[0]) {
      fprintf(stderr, ",\"tag\":\"");
      sem_json_write_escaped(stderr, tag);
      fprintf(stderr, "\"");
    }
    fprintf(stderr, "}\n");
    return;
  }

  if (path[0] && line) {
    fprintf(stderr, "sem: %s: %s (%s:%u)\n", code, msg, path, (unsigned)line);
  } else if (path[0]) {
    fprintf(stderr, "sem: %s: %s (%s)\n", code, msg, path);
  } else {
    fprintf(stderr, "sem: %s: %s\n", code, msg);
  }
  if (node || tag[0]) {
    fprintf(stderr, "sem:   at node=%u tag=%s\n", (unsigned)node, tag);
  }
  if (fid) {
    fprintf(stderr, "sem:   at fid=%u ip=%u op=%s\n", (unsigned)fid, (unsigned)ip, op[0] ? op : "?");
  }
}

static void sem_print_diag(const sirj_ctx_t* c) {
  if (!c || !c->diag.set) return;
  if (c->diag_all && c->diag_count) {
    for (uint32_t i = 0; i < c->diag_count; i++) {
      sem_print_one_diag(c->diag_format, c->diags[i].code, c->diags[i].msg, c->diags[i].path, c->diags[i].line, c->diags[i].node_id,
                         c->diags[i].tag, c->diags[i].fid, c->diags[i].ip, c->diags[i].op);
    }
    return;
  }
  sem_print_one_diag(c->diag_format, c->diag.code, c->diag.msg, c->diag.path, c->diag.line, c->diag.node_id, c->diag.tag, c->diag.fid, c->diag.ip,
                     c->diag.op);
}

typedef struct sem_last_step {
  sir_func_id_t fid;
  uint32_t ip;
  sir_inst_kind_t op;
  uint32_t node_id;
  uint32_t line;
} sem_last_step_t;

typedef struct sem_wrap_sink {
  const sir_exec_event_sink_t* inner;
  sem_last_step_t last;
} sem_wrap_sink_t;

static void sem_wrap_on_step(void* user, const sir_module_t* m, sir_func_id_t fid, uint32_t ip, sir_inst_kind_t k) {
  sem_wrap_sink_t* w = (sem_wrap_sink_t*)user;
  if (!w) return;
  if (w->inner && w->inner->on_step) w->inner->on_step(w->inner->user, m, fid, ip, k);

  w->last.fid = fid;
  w->last.ip = ip;
  w->last.op = k;
  w->last.node_id = 0;
  w->last.line = 0;
  if (m && fid && fid <= m->func_count) {
    const sir_func_t* f = &m->funcs[fid - 1];
    if (f && ip < f->inst_count) {
      w->last.node_id = f->insts[ip].src_node_id;
      w->last.line = f->insts[ip].src_line;
    }
  }
}

static void sem_wrap_on_mem(void* user, const sir_module_t* m, sir_func_id_t fid, uint32_t ip, sir_mem_event_kind_t mk, zi_ptr_t addr,
                            uint32_t size) {
  sem_wrap_sink_t* w = (sem_wrap_sink_t*)user;
  if (!w) return;
  if (w->inner && w->inner->on_mem) w->inner->on_mem(w->inner->user, m, fid, ip, mk, addr, size);
}

static void sem_wrap_on_hostcall(void* user, const sir_module_t* m, sir_func_id_t fid, uint32_t ip, const char* callee, int32_t rc) {
  sem_wrap_sink_t* w = (sem_wrap_sink_t*)user;
  if (!w) return;
  if (w->inner && w->inner->on_hostcall) w->inner->on_hostcall(w->inner->user, m, fid, ip, callee, rc);
}

static void ctx_dispose(sirj_ctx_t* c) {
  if (!c) return;
  if (c->mb) {
    sir_mb_free(c->mb);
    c->mb = NULL;
  }
  free(c->types);
  free(c->nodes);
  free(c->syms);
  free(c->sym_by_node);
  free(c->val_by_node);
  free(c->kind_by_node);
  free(c->func_by_node);
  arena_free(&c->arena);
  memset(c, 0, sizeof(*c));
}

static bool grow_u32(void** p, uint32_t* cap, uint32_t need, size_t elem_size) {
  if (!p || !cap) return false;
  if (need <= *cap) return true;
  uint32_t ncap = *cap ? *cap : 64;
  while (ncap < need) ncap *= 2;
  void* np = realloc(*p, (size_t)ncap * elem_size);
  if (!np) return false;
  memset((uint8_t*)np + (size_t)(*cap) * elem_size, 0, (size_t)(ncap - *cap) * elem_size);
  *p = np;
  *cap = ncap;
  return true;
}

static bool ensure_type_cap(sirj_ctx_t* c, uint32_t type_id) {
  return grow_u32((void**)&c->types, &c->type_cap, type_id + 1u, sizeof(type_info_t));
}

static bool ensure_node_cap(sirj_ctx_t* c, uint32_t node_id) {
  if (!grow_u32((void**)&c->nodes, &c->node_cap, node_id + 1u, sizeof(node_info_t))) return false;
  if (!grow_u32((void**)&c->sym_by_node, &c->sym_cap, node_id + 1u, sizeof(sir_sym_id_t))) return false;
  if (!grow_u32((void**)&c->val_by_node, &c->val_cap, node_id + 1u, sizeof(sir_val_id_t))) return false;
  if (!grow_u32((void**)&c->kind_by_node, &c->kind_cap, node_id + 1u, sizeof(val_kind_t))) return false;
  if (!grow_u32((void**)&c->func_by_node, &c->func_by_node_cap, node_id + 1u, sizeof(sir_func_id_t))) return false;
  return true;
}

static bool ensure_symrec_cap(sirj_ctx_t* c, uint32_t sym_id) {
  return grow_u32((void**)&c->syms, &c->symrec_cap, sym_id + 1u, sizeof(sym_info_t));
}

static bool json_get_u32(const JsonValue* v, uint32_t* out) {
  if (!out) return false;
  int64_t i = 0;
  if (!json_get_i64(v, &i)) return false;
  if (i < 0 || i > 0x7FFFFFFFll) return false;
  *out = (uint32_t)i;
  return true;
}

static bool json_get_bool(const JsonValue* v, bool* out) {
  if (!out) return false;
  if (!v || v->type != JSON_BOOL) return false;
  *out = v->v.b;
  return true;
}

static const JsonValue* obj_req(const JsonValue* obj, const char* key) {
  const JsonValue* v = json_obj_get(obj, key);
  return v;
}

static bool parse_ref_id(const JsonValue* v, uint32_t* out_id) {
  if (!out_id) return false;
  if (!json_is_object(v)) return false;
  const JsonValue* tv = json_obj_get(v, "t");
  const JsonValue* idv = json_obj_get(v, "id");
  const char* ts = json_get_string(tv);
  if (!ts || strcmp(ts, "ref") != 0) return false;
  return json_get_u32(idv, out_id);
}

static bool parse_u32_array(const JsonValue* v, uint32_t** out, uint32_t* out_n, Arena* arena) {
  if (!out || !out_n || !arena) return false;
  if (!json_is_array(v)) return false;
  const JsonArray* a = &v->v.arr;
  uint32_t n = (uint32_t)a->len;
  if (n != a->len) return false;
  uint32_t* ids = (uint32_t*)arena_alloc(arena, (size_t)n * sizeof(uint32_t));
  if (!ids && n) return false;
  for (uint32_t i = 0; i < n; i++) {
    if (!json_get_u32(a->items[i], &ids[i])) return false;
  }
  *out = ids;
  *out_n = n;
  return true;
}

static bool parse_hex_u64(const char* s, uint64_t* out) {
  if (!s || !out) return false;
  if (s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
  uint64_t v = 0;
  bool any = false;
  for (const char* p = s + 2; *p; p++) {
    unsigned d = 0;
    const char ch = *p;
    if (ch >= '0' && ch <= '9') d = (unsigned)(ch - '0');
    else if (ch >= 'a' && ch <= 'f') d = 10u + (unsigned)(ch - 'a');
    else if (ch >= 'A' && ch <= 'F') d = 10u + (unsigned)(ch - 'A');
    else return false;
    any = true;
    if (v > (UINT64_MAX - d) / 16u) return false;
    v = (v * 16u) + d;
  }
  if (!any) return false;
  *out = v;
  return true;
}

static bool parse_hex_u32(const char* s, uint32_t* out) {
  if (!out) return false;
  uint64_t v = 0;
  if (!parse_hex_u64(s, &v)) return false;
  if (v > UINT32_MAX) return false;
  *out = (uint32_t)v;
  return true;
}

static bool is_pow2_u32(uint32_t x) {
  return x != 0u && (x & (x - 1u)) == 0u;
}

static bool sem_f32_is_nan_bits(uint32_t bits) {
  const uint32_t exp = bits & 0x7F800000u;
  const uint32_t frac = bits & 0x007FFFFFu;
  return exp == 0x7F800000u && frac != 0;
}

static uint32_t sem_f32_canon_bits(uint32_t bits) {
  return sem_f32_is_nan_bits(bits) ? 0x7FC00000u : bits;
}

static bool sem_f64_is_nan_bits(uint64_t bits) {
  const uint64_t exp = bits & 0x7FF0000000000000ull;
  const uint64_t frac = bits & 0x000FFFFFFFFFFFFFull;
  return exp == 0x7FF0000000000000ull && frac != 0;
}

static uint64_t sem_f64_canon_bits(uint64_t bits) {
  return sem_f64_is_nan_bits(bits) ? 0x7FF8000000000000ull : bits;
}

static sir_prim_type_t prim_from_string(const char* s) {
  if (!s) return SIR_PRIM_INVALID;
  if (strcmp(s, "void") == 0) return SIR_PRIM_VOID;
  if (strcmp(s, "i1") == 0) return SIR_PRIM_I1;
  if (strcmp(s, "i8") == 0) return SIR_PRIM_I8;
  if (strcmp(s, "i16") == 0) return SIR_PRIM_I16;
  if (strcmp(s, "i32") == 0) return SIR_PRIM_I32;
  if (strcmp(s, "i64") == 0) return SIR_PRIM_I64;
  if (strcmp(s, "ptr") == 0) return SIR_PRIM_PTR;
  if (strcmp(s, "bool") == 0) return SIR_PRIM_BOOL;
  if (strcmp(s, "f32") == 0) return SIR_PRIM_F32;
  if (strcmp(s, "f64") == 0) return SIR_PRIM_F64;
  return SIR_PRIM_INVALID;
}

static sir_type_id_t mod_ty_for_prim(sirj_ctx_t* c, sir_prim_type_t prim) {
  if (!c) return 0;
  switch (prim) {
    case SIR_PRIM_VOID:
      return 0;
    case SIR_PRIM_I1:
      return c->ty_i1;
    case SIR_PRIM_I8:
      return c->ty_i8;
    case SIR_PRIM_I16:
      return c->ty_i16;
    case SIR_PRIM_I32:
      return c->ty_i32;
    case SIR_PRIM_I64:
      return c->ty_i64;
    case SIR_PRIM_PTR:
      return c->ty_ptr;
    case SIR_PRIM_BOOL:
      return c->ty_bool;
    case SIR_PRIM_F32:
      return c->ty_f32;
    case SIR_PRIM_F64:
      return c->ty_f64;
    default:
      return 0;
  }
}

static bool ensure_prim_types(sirj_ctx_t* c) {
  if (!c || !c->mb) return false;
  if (!c->ty_i1) c->ty_i1 = sir_mb_type_prim(c->mb, SIR_PRIM_I1);
  if (!c->ty_i8) c->ty_i8 = sir_mb_type_prim(c->mb, SIR_PRIM_I8);
  if (!c->ty_i16) c->ty_i16 = sir_mb_type_prim(c->mb, SIR_PRIM_I16);
  if (!c->ty_i32) c->ty_i32 = sir_mb_type_prim(c->mb, SIR_PRIM_I32);
  if (!c->ty_i64) c->ty_i64 = sir_mb_type_prim(c->mb, SIR_PRIM_I64);
  if (!c->ty_ptr) c->ty_ptr = sir_mb_type_prim(c->mb, SIR_PRIM_PTR);
  if (!c->ty_bool) c->ty_bool = sir_mb_type_prim(c->mb, SIR_PRIM_BOOL);
  if (!c->ty_f32) c->ty_f32 = sir_mb_type_prim(c->mb, SIR_PRIM_F32);
  if (!c->ty_f64) c->ty_f64 = sir_mb_type_prim(c->mb, SIR_PRIM_F64);
  return c->ty_i1 && c->ty_i8 && c->ty_i16 && c->ty_i32 && c->ty_i64 && c->ty_ptr && c->ty_bool && c->ty_f32 && c->ty_f64;
}

static sir_val_id_t alloc_slot(sirj_ctx_t* c, val_kind_t k) {
  if (!c || k == VK_INVALID) return 0;
  const sir_val_id_t slot = c->next_slot++;
  // slots are 0-based; return slot id
  (void)k;
  return slot;
}

static bool set_node_val(sirj_ctx_t* c, uint32_t node_id, sir_val_id_t slot, val_kind_t k) {
  if (!c) return false;
  if (!ensure_node_cap(c, node_id)) return false;
  c->val_by_node[node_id] = slot + 1u;
  c->kind_by_node[node_id] = k;
  return true;
}

static bool get_node_val(const sirj_ctx_t* c, uint32_t node_id, sir_val_id_t* out_slot, val_kind_t* out_k) {
  if (!c || !out_slot || !out_k) return false;
  if (node_id >= c->val_cap) return false;
  const sir_val_id_t v = c->val_by_node[node_id];
  if (!v) return false;
  *out_slot = v - 1u;
  *out_k = c->kind_by_node[node_id];
  return true;
}

static void reset_value_cache(sirj_ctx_t* c) {
  if (!c) return;
  if (c->val_by_node && c->val_cap) memset(c->val_by_node, 0, (size_t)c->val_cap * sizeof(sir_val_id_t));
  if (c->kind_by_node && c->kind_cap) memset(c->kind_by_node, 0, (size_t)c->kind_cap * sizeof(val_kind_t));
}

static bool type_to_val_kind(const sirj_ctx_t* c, uint32_t type_id, val_kind_t* out) {
  if (!c || !out) return false;
  if (type_id == 0 || type_id >= c->type_cap) return false;
  const type_info_t* t = &c->types[type_id];
  if (!t->present || t->is_fn) return false;
  // SEM only executes prim-like value kinds.
  if (t->is_ptr || t->prim == SIR_PRIM_PTR) {
    *out = VK_PTR;
    return true;
  }
  switch (t->prim) {
    case SIR_PRIM_I1:
      *out = VK_I1;
      return true;
    case SIR_PRIM_I8:
      *out = VK_I8;
      return true;
    case SIR_PRIM_I16:
      *out = VK_I16;
      return true;
    case SIR_PRIM_I32:
      *out = VK_I32;
      return true;
    case SIR_PRIM_I64:
      *out = VK_I64;
      return true;
    case SIR_PRIM_BOOL:
      *out = VK_BOOL;
      return true;
    case SIR_PRIM_F32:
      *out = VK_F32;
      return true;
    case SIR_PRIM_F64:
      *out = VK_F64;
      return true;
    default:
      return false;
  }
}

static bool get_const_bool_cached(sirj_ctx_t* c, bool v, sir_val_id_t* out_slot) {
  if (!c || !out_slot) return false;
  if (v && c->cached_true_slot) {
    *out_slot = c->cached_true_slot;
    return true;
  }
  if (!v && c->cached_false_slot) {
    *out_slot = c->cached_false_slot;
    return true;
  }
  const sir_val_id_t s = alloc_slot(c, VK_BOOL);
  if (!sir_mb_emit_const_bool(c->mb, c->fn, s, v)) return false;
  if (v)
    c->cached_true_slot = s;
  else
    c->cached_false_slot = s;
  *out_slot = s;
  return true;
}

static bool emit_copy_slot(sirj_ctx_t* c, sir_val_id_t dst, sir_val_id_t src) {
  if (!c) return false;
  // Generic copy using SELECT with a constant-true condition:
  //   dst = (true ? src : src)
  sir_val_id_t t = 0;
  if (!get_const_bool_cached(c, true, &t)) return false;
  if (!sir_mb_emit_select(c->mb, c->fn, dst, t, src, src)) return false;
  return true;
}

static bool emit_call_fun_sym(sirj_ctx_t* c, uint32_t callsite_node_id, uint32_t fun_sym_node_id, const uint32_t* arg_nodes, uint32_t argc,
                              bool discard_result, bool has_dst_override, sir_val_id_t dst_override, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c) return false;
  if (fun_sym_node_id >= c->node_cap || !c->nodes[fun_sym_node_id].present) return false;
  const node_info_t* callee_n = &c->nodes[fun_sym_node_id];
  if (!callee_n->tag || strcmp(callee_n->tag, "fun.sym") != 0) return false;
  if (!callee_n->fields_obj || callee_n->fields_obj->type != JSON_OBJECT) return false;
  const char* fn_name = json_get_string(json_obj_get(callee_n->fields_obj, "name"));
  if (!fn_name) return false;

  sir_func_id_t callee_fid = 0;
  if (!resolve_internal_func_by_name(c, fn_name, &callee_fid)) return false;

  const uint32_t fun_ty = callee_n->type_ref;
  if (fun_ty == 0 || fun_ty >= c->type_cap || !c->types[fun_ty].present || !c->types[fun_ty].is_fun) return false;
  const uint32_t sig_tid = c->types[fun_ty].fun_sig;
  if (sig_tid == 0 || sig_tid >= c->type_cap || !c->types[sig_tid].present || !c->types[sig_tid].is_fn) return false;
  const type_info_t* sti = &c->types[sig_tid];

  if (argc != (uint32_t)sti->param_count) return false;
  if (argc > 16) return false;

  sir_val_id_t args_slots[16];
  for (uint32_t i = 0; i < argc; i++) {
    sir_val_id_t s = 0;
    val_kind_t k = VK_INVALID;
    if (!eval_node(c, arg_nodes[i], &s, &k)) return false;
    val_kind_t expected = VK_INVALID;
    if (!type_to_val_kind(c, sti->params[i], &expected)) return false;
    if (k != expected) return false;
    args_slots[i] = s;
  }

  // Compute result contract from fn type.
  uint8_t result_count = 0;
  val_kind_t rk = VK_INVALID;
  if (sti->ret) {
    if (sti->ret >= c->type_cap || !c->types[sti->ret].present || c->types[sti->ret].is_fn) return false;
    if (c->types[sti->ret].prim == SIR_PRIM_VOID) {
      result_count = 0;
      rk = VK_INVALID;
    } else {
      if (!type_to_val_kind(c, sti->ret, &rk)) return false;
      if (rk == VK_INVALID) return false;
      result_count = 1;
    }
  }
  sir_val_id_t res_slot = 0;
  if (result_count) {
    res_slot = has_dst_override ? dst_override : alloc_slot(c, rk);
  }

  sir_mb_set_src(c->mb, callsite_node_id, (callsite_node_id < c->node_cap && c->nodes[callsite_node_id].present) ? c->nodes[callsite_node_id].loc_line : 0);
  const bool ok = result_count ? sir_mb_emit_call_func_res(c->mb, c->fn, callee_fid, args_slots, argc, &res_slot, result_count)
                               : sir_mb_emit_call_func_res(c->mb, c->fn, callee_fid, args_slots, argc, NULL, 0);
  sir_mb_clear_src(c->mb);
  if (!ok) return false;

  if (!discard_result && result_count) {
    if (out_slot) *out_slot = res_slot;
    if (out_kind) *out_kind = rk;
  } else {
    if (out_slot) *out_slot = 0;
    if (out_kind) *out_kind = VK_INVALID;
  }
  return true;
}

static bool emit_run_defers(sirj_ctx_t* c, uint32_t base_depth, uint32_t callsite_node_id) {
  if (!c) return false;
  if (base_depth > c->defer_count) return false;
  for (uint32_t i = c->defer_count; i > base_depth; i--) {
    const uint32_t fun_node = c->defers[i - 1];
    if (!emit_call_fun_sym(c, callsite_node_id, fun_node, NULL, 0, true, false, 0, NULL, NULL)) return false;
  }
  c->defer_count = base_depth;
  return true;
}

static bool coerce_exit_i32(sirj_ctx_t* c, uint32_t callsite_node_id, sir_val_id_t slot, val_kind_t kind, sir_val_id_t* out_i32) {
  if (!c || !out_i32) return false;
  if (kind == VK_I32) {
    *out_i32 = slot;
    return true;
  }

  // Only coerce a small set for tool exit codes.
  sir_mb_set_src(c->mb, callsite_node_id, (callsite_node_id < c->node_cap && c->nodes[callsite_node_id].present) ? c->nodes[callsite_node_id].loc_line : 0);
  if (kind == VK_BOOL) {
    const sir_val_id_t one = alloc_slot(c, VK_I32);
    const sir_val_id_t zero = alloc_slot(c, VK_I32);
    const sir_val_id_t dst = alloc_slot(c, VK_I32);
    if (!sir_mb_emit_const_i32(c->mb, c->fn, one, 1)) return false;
    if (!sir_mb_emit_const_i32(c->mb, c->fn, zero, 0)) return false;
    if (!sir_mb_emit_select(c->mb, c->fn, dst, slot, one, zero)) return false;
    sir_mb_clear_src(c->mb);
    *out_i32 = dst;
    return true;
  }
  if (kind == VK_I8) {
    const sir_val_id_t dst = alloc_slot(c, VK_I32);
    if (!sir_mb_emit_i32_zext_i8(c->mb, c->fn, dst, slot)) return false;
    sir_mb_clear_src(c->mb);
    *out_i32 = dst;
    return true;
  }
  if (kind == VK_I16) {
    const sir_val_id_t dst = alloc_slot(c, VK_I32);
    if (!sir_mb_emit_i32_zext_i16(c->mb, c->fn, dst, slot)) return false;
    sir_mb_clear_src(c->mb);
    *out_i32 = dst;
    return true;
  }
  if (kind == VK_I64) {
    const sir_val_id_t dst = alloc_slot(c, VK_I32);
    if (!sir_mb_emit_i32_trunc_i64(c->mb, c->fn, dst, slot)) return false;
    sir_mb_clear_src(c->mb);
    *out_i32 = dst;
    return true;
  }

  sir_mb_clear_src(c->mb);
  sirj_diag_setf(c, "sem.entry.exit_type", c->cur_path, 0, callsite_node_id, NULL, "entry exit value must be i32 (got %d)", (int)kind);
  return false;
}

typedef enum sem_branch_kind {
  SEM_BRANCH_VAL = 1,
  SEM_BRANCH_THUNK = 2,
} sem_branch_kind_t;

typedef struct sem_branch {
  sem_branch_kind_t kind;
  uint32_t node_id; // value node id when VAL; fun.sym node id when THUNK
} sem_branch_t;

static bool parse_sem_branch(const JsonValue* v, sem_branch_t* out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  if (!json_is_object(v)) return false;
  const char* k = json_get_string(json_obj_get((JsonValue*)v, "kind"));
  if (!k) return false;
  if (strcmp(k, "val") == 0) {
    uint32_t rid = 0;
    if (!parse_ref_id(json_obj_get((JsonValue*)v, "v"), &rid)) return false;
    out->kind = SEM_BRANCH_VAL;
    out->node_id = rid;
    return true;
  }
  if (strcmp(k, "thunk") == 0) {
    uint32_t rid = 0;
    if (!parse_ref_id(json_obj_get((JsonValue*)v, "f"), &rid)) return false;
    out->kind = SEM_BRANCH_THUNK;
    out->node_id = rid;
    return true;
  }
  return false;
}

static bool resolve_decl_fn_sym(sirj_ctx_t* c, uint32_t node_id, sir_sym_id_t* out) {
  if (!c || !out) return false;
  if (node_id >= c->sym_cap) return false;
  if (c->sym_by_node[node_id]) {
    *out = c->sym_by_node[node_id];
    return true;
  }

  if (node_id >= c->node_cap || !c->nodes[node_id].present) return false;
  const node_info_t* n = &c->nodes[node_id];
  if (!n->tag || strcmp(n->tag, "decl.fn") != 0) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;

  const JsonValue* namev = json_obj_get(n->fields_obj, "name");
  const char* nm = json_get_string(namev);
  if (!nm || nm[0] == '\0') return false;

  if (!ensure_prim_types(c)) return false;

  // Build signature from referenced SIR type (must be fn).
  const uint32_t tr = n->type_ref;
  if (tr == 0 || tr >= c->type_cap || !c->types[tr].present || !c->types[tr].is_fn) return false;
  const type_info_t* ti = &c->types[tr];

  sir_type_id_t params[16];
  if (ti->param_count > (uint32_t)(sizeof(params) / sizeof(params[0]))) return false;
  for (uint32_t i = 0; i < ti->param_count; i++) {
    const uint32_t sir_tid = ti->params[i];
    if (sir_tid == 0 || sir_tid >= c->type_cap || !c->types[sir_tid].present || c->types[sir_tid].is_fn) return false;
    const sir_type_id_t mt = mod_ty_for_prim(c, c->types[sir_tid].prim);
    if (!mt) return false;
    params[i] = mt;
  }

  sir_type_id_t results[1];
  uint32_t result_count = 0;
  if (ti->ret != 0) {
    const uint32_t sir_rid = ti->ret;
    if (sir_rid == 0 || sir_rid >= c->type_cap || !c->types[sir_rid].present || c->types[sir_rid].is_fn) return false;
    const sir_prim_type_t rp = c->types[sir_rid].prim;
    if (rp != SIR_PRIM_VOID) {
      const sir_type_id_t mt = mod_ty_for_prim(c, rp);
      if (!mt) return false;
      results[0] = mt;
      result_count = 1;
    }
  }

  sir_sig_t sig = {
      .params = params,
      .param_count = ti->param_count,
      .results = result_count ? results : NULL,
      .result_count = result_count,
  };

  const sir_sym_id_t sid = sir_mb_sym_extern_fn(c->mb, nm, sig);
  if (!sid) return false;
  c->sym_by_node[node_id] = sid;
  *out = sid;
  return true;
}

static bool val_kind_for_type_ref(const sirj_ctx_t* c, uint32_t type_ref, val_kind_t* out) {
  if (!c || !out) return false;
  if (type_ref == 0 || type_ref >= c->type_cap) return false;
  if (!c->types[type_ref].present) return false;
  if (c->types[type_ref].is_fn) return false;
  switch (c->types[type_ref].prim) {
    case SIR_PRIM_VOID:
      return false;
    case SIR_PRIM_I1:
      *out = VK_I1;
      return true;
    case SIR_PRIM_I8:
      *out = VK_I8;
      return true;
    case SIR_PRIM_I16:
      *out = VK_I16;
      return true;
    case SIR_PRIM_I32:
      *out = VK_I32;
      return true;
    case SIR_PRIM_I64:
      *out = VK_I64;
      return true;
    case SIR_PRIM_PTR:
      *out = VK_PTR;
      return true;
    case SIR_PRIM_BOOL:
      *out = VK_BOOL;
      return true;
    case SIR_PRIM_F32:
      *out = VK_F32;
      return true;
    case SIR_PRIM_F64:
      *out = VK_F64;
      return true;
    default:
      return false;
  }
}

static bool round_up_u32(uint32_t x, uint32_t a, uint32_t* out) {
  if (!out) return false;
  if (a == 0 || !is_pow2_u32(a)) return false;
  const uint64_t y = ((uint64_t)x + (uint64_t)a - 1ull) & ~(uint64_t)(a - 1u);
  if (y > 0x7FFFFFFFull) return false;
  *out = (uint32_t)y;
  return true;
}

static bool type_layout(sirj_ctx_t* c, uint32_t type_ref, uint32_t* out_size, uint32_t* out_align) {
  if (!c || !out_size || !out_align) return false;
  if (type_ref == 0 || type_ref >= c->type_cap) return false;
  if (!c->types[type_ref].present) return false;
  type_info_t* t = &c->types[type_ref];
  if (t->is_fn) return false;

  if (t->is_array) {
    if (t->array_of == 0) return false;
    uint32_t es = 0, ea = 0;
    if (!type_layout(c, t->array_of, &es, &ea)) return false;
    if (es == 0) return false;
    if (t->array_len == 0) return false;
    const uint64_t size64 = (uint64_t)es * (uint64_t)t->array_len;
    if (size64 > 0x7FFFFFFFull) return false;
    *out_size = (uint32_t)size64;
    *out_align = ea ? ea : 1;
    return true;
  }

  if (t->is_struct) {
    if (t->layout_visiting) return false;
    t->layout_visiting = true;

    uint32_t off = 0;
    uint32_t max_align = 1;

    if (t->struct_field_count && (!t->struct_fields || !t->struct_field_align)) {
      t->layout_visiting = false;
      return false;
    }

    for (uint32_t i = 0; i < t->struct_field_count; i++) {
      const uint32_t field_ty = t->struct_fields[i];
      uint32_t fs = 0, fa = 0;
      if (!type_layout(c, field_ty, &fs, &fa)) {
        t->layout_visiting = false;
        return false;
      }
      if (fs == 0) {
        t->layout_visiting = false;
        return false;
      }

      uint32_t field_align = 0;
      if (t->struct_field_align[i]) {
        field_align = t->struct_field_align[i];
      } else {
        field_align = t->struct_packed ? 1u : (fa ? fa : 1u);
      }
      if (field_align == 0 || !is_pow2_u32(field_align)) {
        t->layout_visiting = false;
        return false;
      }

      uint32_t new_off = 0;
      if (!round_up_u32(off, field_align, &new_off)) {
        t->layout_visiting = false;
        return false;
      }
      off = new_off;

      const uint64_t next = (uint64_t)off + (uint64_t)fs;
      if (next > 0x7FFFFFFFull) {
        t->layout_visiting = false;
        return false;
      }
      off = (uint32_t)next;

      if (field_align > max_align) max_align = field_align;
    }

    uint32_t align = max_align ? max_align : 1;
    if (t->struct_align_override) {
      if (!is_pow2_u32(t->struct_align_override)) {
        t->layout_visiting = false;
        return false;
      }
      if (t->struct_align_override < align) {
        t->layout_visiting = false;
        return false;
      }
      align = t->struct_align_override;
    }
    if (align == 0) align = 1;

    uint32_t size = 0;
    if (!round_up_u32(off, align, &size)) {
      t->layout_visiting = false;
      return false;
    }

    t->layout_visiting = false;
    *out_size = size;
    *out_align = align;
    return true;
  }

  uint32_t size = 0, align = 1;
  switch (t->prim) {
    case SIR_PRIM_VOID:
      size = 0;
      align = 1;
      break;
    case SIR_PRIM_I1:
      size = 1;
      align = 1;
      break;
    case SIR_PRIM_I8:
      size = 1;
      align = 1;
      break;
    case SIR_PRIM_I16:
      size = 2;
      align = 2;
      break;
    case SIR_PRIM_I32:
      size = 4;
      align = 4;
      break;
    case SIR_PRIM_I64:
      size = 8;
      align = 8;
      break;
    case SIR_PRIM_PTR:
      size = 8;
      align = 8;
      break;
    case SIR_PRIM_BOOL:
      size = 1;
      align = 1;
      break;
    case SIR_PRIM_F32:
      size = 4;
      align = 4;
      break;
    case SIR_PRIM_F64:
      size = 8;
      align = 8;
      break;
    default:
      return false;
  }
  *out_size = size;
  *out_align = align;
  return true;
}

static bool build_const_bytes(sirj_ctx_t* c, uint32_t node_id, uint32_t type_ref, uint8_t** out_bytes, uint32_t* out_len) {
  if (!c || !out_bytes || !out_len) return false;
  *out_bytes = NULL;
  *out_len = 0;
  if (node_id >= c->node_cap || !c->nodes[node_id].present) return false;
  const node_info_t* n = &c->nodes[node_id];
  if (!n->tag) return false;

  uint32_t size = 0, align = 1;
  if (!type_layout(c, type_ref, &size, &align)) return false;
  (void)align;

  if (strcmp(n->tag, "const.zero") == 0) {
    if (n->type_ref != type_ref) return false;
    uint8_t* b = (uint8_t*)arena_alloc(&c->arena, size);
    if (!b && size) return false;
    if (b && size) memset(b, 0, size);
    *out_bytes = b;
    *out_len = size;
    return true;
  }

  if (strcmp(n->tag, "const.i8") == 0) {
    if (size != 1) return false;
    if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
    uint8_t* b = (uint8_t*)arena_alloc(&c->arena, 1);
    if (!b) return false;
    int64_t v = 0;
    if (!json_get_i64(json_obj_get(n->fields_obj, "value"), &v)) return false;
    if (v < 0 || v > 255) return false;
    b[0] = (uint8_t)v;
    *out_bytes = b;
    *out_len = 1;
    return true;
  }

  if (strcmp(n->tag, "const.i16") == 0) {
    if (size != 2) return false;
    if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
    int64_t v = 0;
    if (!json_get_i64(json_obj_get(n->fields_obj, "value"), &v)) return false;
    if (v < 0 || v > 65535) return false;
    uint8_t* b = (uint8_t*)arena_alloc(&c->arena, 2);
    if (!b) return false;
    const uint16_t x = (uint16_t)v;
    memcpy(b, &x, 2);
    *out_bytes = b;
    *out_len = 2;
    return true;
  }

  if (strcmp(n->tag, "const.i32") == 0) {
    if (size != 4) return false;
    int64_t v = 0;
    if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
    if (!json_get_i64(json_obj_get(n->fields_obj, "value"), &v)) return false;
    if (v < INT32_MIN || v > INT32_MAX) return false;
    uint8_t* b = (uint8_t*)arena_alloc(&c->arena, 4);
    if (!b) return false;
    const int32_t x = (int32_t)v;
    memcpy(b, &x, 4);
    *out_bytes = b;
    *out_len = 4;
    return true;
  }

  if (strcmp(n->tag, "const.i64") == 0) {
    if (size != 8) return false;
    int64_t v = 0;
    if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
    if (!json_get_i64(json_obj_get(n->fields_obj, "value"), &v)) return false;
    uint8_t* b = (uint8_t*)arena_alloc(&c->arena, 8);
    if (!b) return false;
    memcpy(b, &v, 8);
    *out_bytes = b;
    *out_len = 8;
    return true;
  }

  if (strcmp(n->tag, "const.f32") == 0) {
    if (size != 4) return false;
    if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
    const char* bits_s = json_get_string(json_obj_get(n->fields_obj, "bits"));
    uint32_t bits = 0;
    if (!parse_hex_u32(bits_s, &bits)) return false;
    bits = sem_f32_canon_bits(bits);
    uint8_t* b = (uint8_t*)arena_alloc(&c->arena, 4);
    if (!b) return false;
    memcpy(b, &bits, 4);
    *out_bytes = b;
    *out_len = 4;
    return true;
  }

  if (strcmp(n->tag, "const.f64") == 0) {
    if (size != 8) return false;
    if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
    const char* bits_s = json_get_string(json_obj_get(n->fields_obj, "bits"));
    uint64_t bits = 0;
    if (!parse_hex_u64(bits_s, &bits)) return false;
    bits = sem_f64_canon_bits(bits);
    uint8_t* b = (uint8_t*)arena_alloc(&c->arena, 8);
    if (!b) return false;
    memcpy(b, &bits, 8);
    *out_bytes = b;
    *out_len = 8;
    return true;
  }

  if (strcmp(n->tag, "const.struct") == 0) {
    if (n->type_ref != type_ref) return false;
    if (type_ref == 0 || type_ref >= c->type_cap) return false;
    const type_info_t* t = &c->types[type_ref];
    if (!t->present || !t->is_struct) return false;
    if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;

    uint8_t* b = (uint8_t*)arena_alloc(&c->arena, size);
    if (!b && size) return false;
    if (b && size) memset(b, 0, size);

    const uint32_t nfield = t->struct_field_count;
    uint32_t* field_off = NULL;
    uint32_t* field_size = NULL;
    if (nfield) {
      field_off = (uint32_t*)arena_alloc(&c->arena, (size_t)nfield * sizeof(uint32_t));
      field_size = (uint32_t*)arena_alloc(&c->arena, (size_t)nfield * sizeof(uint32_t));
      if (!field_off || !field_size) return false;

      uint32_t off = 0;
      for (uint32_t i = 0; i < nfield; i++) {
        const uint32_t fty = t->struct_fields[i];
        uint32_t fs = 0, fa = 0;
        if (!type_layout(c, fty, &fs, &fa)) return false;
        if (fs == 0) return false;
        uint32_t falign = 0;
        if (t->struct_field_align && t->struct_field_align[i]) falign = t->struct_field_align[i];
        else falign = t->struct_packed ? 1u : (fa ? fa : 1u);
        if (falign == 0 || !is_pow2_u32(falign)) return false;
        if (!round_up_u32(off, falign, &off)) return false;
        field_off[i] = off;
        field_size[i] = fs;
        const uint64_t next = (uint64_t)off + (uint64_t)fs;
        if (next > 0x7FFFFFFFull) return false;
        off = (uint32_t)next;
      }
    }

    const JsonValue* fv = json_obj_get(n->fields_obj, "fields");
    if (!fv) {
      *out_bytes = b;
      *out_len = size;
      return true;
    }
    if (!json_is_array(fv)) return false;
    const JsonArray* a = &fv->v.arr;

    uint32_t prev_i = 0;
    bool prev_set = false;
    for (size_t ai = 0; ai < a->len; ai++) {
      const JsonValue* asn = a->items[ai];
      if (!json_is_object(asn)) return false;
      uint32_t fi = 0;
      if (!json_get_u32(json_obj_get(asn, "i"), &fi)) return false;
      if (fi >= nfield) return false;
      if (prev_set && fi <= prev_i) return false;
      prev_i = fi;
      prev_set = true;

      const JsonValue* vv = json_obj_get(asn, "v");
      uint32_t rid = 0;
      if (!parse_ref_id(vv, &rid)) return false;
      uint8_t* eb = NULL;
      uint32_t elen = 0;
      if (!build_const_bytes(c, rid, t->struct_fields[fi], &eb, &elen)) return false;
      if (elen != field_size[fi]) return false;
      if (elen && !eb) return false;
      if (elen) memcpy(b + field_off[fi], eb, elen);
    }

    *out_bytes = b;
    *out_len = size;
    return true;
  }

  if (strcmp(n->tag, "const.array") == 0) {
    if (n->type_ref != type_ref) return false;
    const type_info_t* t = &c->types[type_ref];
    if (!t->present || !t->is_array) return false;
    if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
    const JsonValue* ev = json_obj_get(n->fields_obj, "elems");
    if (!json_is_array(ev)) return false;
    if (ev->v.arr.len != t->array_len) return false;

    uint32_t es = 0, ea = 0;
    if (!type_layout(c, t->array_of, &es, &ea)) return false;
    (void)ea;

    uint8_t* b = (uint8_t*)arena_alloc(&c->arena, size);
    if (!b) return false;
    uint32_t off = 0;
    for (uint32_t i = 0; i < t->array_len; i++) {
      uint32_t rid = 0;
      if (!parse_ref_id(ev->v.arr.items[i], &rid)) return false;
      uint8_t* eb = NULL;
      uint32_t elen = 0;
      if (!build_const_bytes(c, rid, t->array_of, &eb, &elen)) return false;
      if (elen != es) return false;
      memcpy(b + off, eb, es);
      off += es;
    }
    if (off != size) return false;
    *out_bytes = b;
    *out_len = size;
    return true;
  }

  if (strcmp(n->tag, "const.repeat") == 0) {
    if (n->type_ref != type_ref) return false;
    const type_info_t* t = &c->types[type_ref];
    if (!t->present || !t->is_array) return false;
    if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
    uint32_t count = 0;
    if (!json_get_u32(json_obj_get(n->fields_obj, "count"), &count)) return false;
    if (count != t->array_len) return false;
    uint32_t elem_id = 0;
    if (!parse_ref_id(json_obj_get(n->fields_obj, "elem"), &elem_id)) return false;

    uint32_t es = 0, ea = 0;
    if (!type_layout(c, t->array_of, &es, &ea)) return false;
    (void)ea;

    uint8_t* eb = NULL;
    uint32_t elen = 0;
    if (!build_const_bytes(c, elem_id, t->array_of, &eb, &elen)) return false;
    if (elen != es) return false;

    uint8_t* b = (uint8_t*)arena_alloc(&c->arena, size);
    if (!b) return false;
    for (uint32_t i = 0; i < count; i++) {
      memcpy(b + (size_t)i * es, eb, es);
    }
    *out_bytes = b;
    *out_len = size;
    return true;
  }

  return false;
}

static bool eval_node(sirj_ctx_t* c, uint32_t node_id, sir_val_id_t* out_slot, val_kind_t* out_kind);
static bool resolve_internal_func_by_name(const sirj_ctx_t* c, const char* nm, sir_func_id_t* out);

static bool eval_bparam(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;

  // bparam values are block parameters; they live in a dedicated value slot and
  // are assigned by `term.br` args at runtime.
  sir_val_id_t cached = 0;
  val_kind_t ck = VK_INVALID;
  if (get_node_val(c, node_id, &cached, &ck)) {
    *out_slot = cached;
    *out_kind = ck;
    return true;
  }

  val_kind_t k = VK_INVALID;
  if (!val_kind_for_type_ref(c, n->type_ref, &k)) return false;
  const sir_val_id_t slot = alloc_slot(c, k);
  if (!set_node_val(c, node_id, slot, k)) return false;
  *out_slot = slot;
  *out_kind = k;
  return true;
}

static bool eval_const_i1(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  int64_t i = 0;
  if (!json_get_i64(json_obj_get(n->fields_obj, "value"), &i)) return false;
  if (i != 0 && i != 1) return false;
  const sir_val_id_t slot = alloc_slot(c, VK_I1);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_const_i1(c->mb, c->fn, slot, i == 1)) return false;
  if (!set_node_val(c, node_id, slot, VK_I1)) return false;
  *out_slot = slot;
  *out_kind = VK_I1;
  return true;
}

static bool eval_const_i32(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* vv = json_obj_get(n->fields_obj, "value");
  int64_t i = 0;
  if (!json_get_i64(vv, &i)) return false;
  if (i < INT32_MIN || i > INT32_MAX) return false;
  const sir_val_id_t slot = alloc_slot(c, VK_I32);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_const_i32(c->mb, c->fn, slot, (int32_t)i)) return false;
  if (!set_node_val(c, node_id, slot, VK_I32)) return false;
  *out_slot = slot;
  *out_kind = VK_I32;
  return true;
}

static bool eval_const_i8(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* vv = json_obj_get(n->fields_obj, "value");
  int64_t i = 0;
  if (!json_get_i64(vv, &i)) return false;
  if (i < 0 || i > 255) return false;
  const sir_val_id_t slot = alloc_slot(c, VK_I8);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_const_i8(c->mb, c->fn, slot, (uint8_t)i)) return false;
  if (!set_node_val(c, node_id, slot, VK_I8)) return false;
  *out_slot = slot;
  *out_kind = VK_I8;
  return true;
}

static bool eval_const_i16(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* vv = json_obj_get(n->fields_obj, "value");
  int64_t i = 0;
  if (!json_get_i64(vv, &i)) return false;
  if (i < 0 || i > 65535) return false;
  const sir_val_id_t slot = alloc_slot(c, VK_I16);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_const_i16(c->mb, c->fn, slot, (uint16_t)i)) return false;
  if (!set_node_val(c, node_id, slot, VK_I16)) return false;
  *out_slot = slot;
  *out_kind = VK_I16;
  return true;
}

static bool eval_const_i64(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* vv = json_obj_get(n->fields_obj, "value");
  int64_t i = 0;
  if (!json_get_i64(vv, &i)) return false;
  const sir_val_id_t slot = alloc_slot(c, VK_I64);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_const_i64(c->mb, c->fn, slot, i)) return false;
  if (!set_node_val(c, node_id, slot, VK_I64)) return false;
  *out_slot = slot;
  *out_kind = VK_I64;
  return true;
}

static bool eval_const_f32(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const char* bits_s = json_get_string(json_obj_get(n->fields_obj, "bits"));
  uint32_t bits = 0;
  if (!parse_hex_u32(bits_s, &bits)) return false;
  bits = sem_f32_canon_bits(bits);
  const sir_val_id_t slot = alloc_slot(c, VK_F32);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_const_f32_bits(c->mb, c->fn, slot, bits)) return false;
  if (!set_node_val(c, node_id, slot, VK_F32)) return false;
  *out_slot = slot;
  *out_kind = VK_F32;
  return true;
}

static bool eval_const_f64(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const char* bits_s = json_get_string(json_obj_get(n->fields_obj, "bits"));
  uint64_t bits = 0;
  if (!parse_hex_u64(bits_s, &bits)) return false;
  bits = sem_f64_canon_bits(bits);
  const sir_val_id_t slot = alloc_slot(c, VK_F64);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_const_f64_bits(c->mb, c->fn, slot, bits)) return false;
  if (!set_node_val(c, node_id, slot, VK_F64)) return false;
  *out_slot = slot;
  *out_kind = VK_F64;
  return true;
}

static bool eval_const_bool(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* vv = json_obj_get(n->fields_obj, "value");
  int64_t i = 0;
  if (!json_get_i64(vv, &i)) return false;
  if (i != 0 && i != 1) return false;
  const sir_val_id_t slot = alloc_slot(c, VK_BOOL);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_const_bool(c->mb, c->fn, slot, i == 1)) return false;
  if (!set_node_val(c, node_id, slot, VK_BOOL)) return false;
  *out_slot = slot;
  *out_kind = VK_BOOL;
  return true;
}

static bool eval_alloca_mnemonic(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, uint32_t size, uint32_t align, sir_val_id_t* out_slot,
                                 val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  const sir_val_id_t slot = alloc_slot(c, VK_PTR);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_alloca(c->mb, c->fn, slot, size, align)) return false;
  if (!set_node_val(c, node_id, slot, VK_PTR)) return false;
  *out_slot = slot;
  *out_kind = VK_PTR;
  return true;
}

static bool eval_store_mnemonic(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_inst_kind_t k) {
  if (!c || !n) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) {
    sirj_diag_setf(c, "sem.parse.store.fields", c->cur_path, n->loc_line, node_id, n->tag, "%s missing/invalid fields object", n->tag);
    return false;
  }
  uint32_t addr_id = 0, val_id = 0;
  if (!parse_ref_id(json_obj_get(n->fields_obj, "addr"), &addr_id)) {
    sirj_diag_setf(c, "sem.parse.store.addr", c->cur_path, n->loc_line, node_id, n->tag, "%s addr must be a ref", n->tag);
    return false;
  }
  if (!parse_ref_id(json_obj_get(n->fields_obj, "value"), &val_id)) {
    sirj_diag_setf(c, "sem.parse.store.value", c->cur_path, n->loc_line, node_id, n->tag, "%s value must be a ref", n->tag);
    return false;
  }
  uint32_t align = 0;
  const JsonValue* alignv = json_obj_get(n->fields_obj, "align");
  if (alignv) {
    if (!json_get_u32(alignv, &align) || align == 0) {
      sirj_diag_setf(c, "sem.parse.store.align", c->cur_path, n->loc_line, node_id, n->tag, "%s align must be a positive integer", n->tag);
      return false;
    }
  } else {
    // Default alignment is the natural alignment of the stored width.
    if (k == SIR_INST_STORE_I8) align = 1;
    else if (k == SIR_INST_STORE_I16) align = 2;
    else if (k == SIR_INST_STORE_I32) align = 4;
    else if (k == SIR_INST_STORE_I64) align = 8;
    else if (k == SIR_INST_STORE_PTR) align = 8;
    else if (k == SIR_INST_STORE_F32) align = 4;
    else if (k == SIR_INST_STORE_F64) align = 8;
    else align = 1;
  }
  if (!is_pow2_u32(align)) {
    sirj_diag_setf(c, "sem.parse.store.align", c->cur_path, n->loc_line, node_id, n->tag, "%s align must be a power of two", n->tag);
    return false;
  }
  sir_val_id_t addr_slot = 0, val_slot = 0;
  val_kind_t ak = VK_INVALID, vk = VK_INVALID;
  if (!eval_node(c, addr_id, &addr_slot, &ak)) return false;
  if (!eval_node(c, val_id, &val_slot, &vk)) return false;
  if (ak != VK_PTR) {
    sirj_diag_setf(c, "sem.store.addr_type", c->cur_path, n->loc_line, node_id, n->tag, "%s addr must be ptr", n->tag);
    return false;
  }

  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (k == SIR_INST_STORE_I8) return sir_mb_emit_store_i8(c->mb, c->fn, addr_slot, val_slot, align);
  if (k == SIR_INST_STORE_I16) return sir_mb_emit_store_i16(c->mb, c->fn, addr_slot, val_slot, align);
  if (k == SIR_INST_STORE_I32) return sir_mb_emit_store_i32(c->mb, c->fn, addr_slot, val_slot, align);
  if (k == SIR_INST_STORE_I64) return sir_mb_emit_store_i64(c->mb, c->fn, addr_slot, val_slot, align);
  if (k == SIR_INST_STORE_PTR) return sir_mb_emit_store_ptr(c->mb, c->fn, addr_slot, val_slot, align);
  if (k == SIR_INST_STORE_F32) return sir_mb_emit_store_f32(c->mb, c->fn, addr_slot, val_slot, align);
  if (k == SIR_INST_STORE_F64) return sir_mb_emit_store_f64(c->mb, c->fn, addr_slot, val_slot, align);
  return false;
}

static bool eval_mem_copy_stmt(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n) {
  if (!c || !n) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 3) {
    sirj_diag_setf(c, "sem.parse.mem.copy.args", c->cur_path, n->loc_line, node_id, n->tag, "mem.copy args must be [dst, src, len]");
    return false;
  }

  uint32_t dst_id = 0, src_id = 0, len_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &dst_id)) return false;
  if (!parse_ref_id(av->v.arr.items[1], &src_id)) return false;
  if (!parse_ref_id(av->v.arr.items[2], &len_id)) return false;

  sir_val_id_t dst_slot = 0, src_slot = 0, len_slot = 0;
  val_kind_t dk = VK_INVALID, sk = VK_INVALID, lk = VK_INVALID;
  if (!eval_node(c, dst_id, &dst_slot, &dk)) return false;
  if (!eval_node(c, src_id, &src_slot, &sk)) return false;
  if (!eval_node(c, len_id, &len_slot, &lk)) return false;
  if (dk != VK_PTR || sk != VK_PTR) return false;
  if (lk != VK_I64 && lk != VK_I32) return false;

  bool overlap_allow = false;
  const JsonValue* fv = json_obj_get(n->fields_obj, "flags");
  if (fv) {
    if (!json_is_object(fv)) return false;
    const char* ov = json_get_string(json_obj_get(fv, "overlap"));
    if (ov) {
      if (strcmp(ov, "allow") == 0) overlap_allow = true;
      else if (strcmp(ov, "disallow") == 0) overlap_allow = false;
      else {
        sirj_diag_setf(c, "sem.parse.mem.copy.overlap", c->cur_path, n->loc_line, node_id, n->tag,
                       "mem.copy flags.overlap must be \"allow\" or \"disallow\"");
        return false;
      }
    }
  }

  sir_mb_set_src(c->mb, node_id, n->loc_line);
  return sir_mb_emit_mem_copy(c->mb, c->fn, dst_slot, src_slot, len_slot, overlap_allow);
}

static bool eval_mem_fill_stmt(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n) {
  if (!c || !n) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 3) {
    sirj_diag_setf(c, "sem.parse.mem.fill.args", c->cur_path, n->loc_line, node_id, n->tag, "mem.fill args must be [dst, byte, len]");
    return false;
  }

  uint32_t dst_id = 0, byte_id = 0, len_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &dst_id)) return false;
  if (!parse_ref_id(av->v.arr.items[1], &byte_id)) return false;
  if (!parse_ref_id(av->v.arr.items[2], &len_id)) return false;

  sir_val_id_t dst_slot = 0, byte_slot = 0, len_slot = 0;
  val_kind_t dk = VK_INVALID, bk = VK_INVALID, lk = VK_INVALID;
  if (!eval_node(c, dst_id, &dst_slot, &dk)) return false;
  if (!eval_node(c, byte_id, &byte_slot, &bk)) return false;
  if (!eval_node(c, len_id, &len_slot, &lk)) return false;
  if (dk != VK_PTR) return false;
  if (bk != VK_I8 && bk != VK_I32) return false;
  if (lk != VK_I64 && lk != VK_I32) return false;

  // ignore flags for now (alignDst/vol)
  const JsonValue* fv = json_obj_get(n->fields_obj, "flags");
  if (fv && !json_is_object(fv)) return false;

  sir_mb_set_src(c->mb, node_id, n->loc_line);
  return sir_mb_emit_mem_fill(c->mb, c->fn, dst_slot, byte_slot, len_slot);
}

static bool eval_load_mnemonic(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_inst_kind_t k, val_kind_t outk, sir_val_id_t* out_slot,
                               val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) {
    sirj_diag_setf(c, "sem.parse.load.fields", c->cur_path, n->loc_line, node_id, n->tag, "%s missing/invalid fields object", n->tag);
    return false;
  }
  uint32_t addr_id = 0;
  if (!parse_ref_id(json_obj_get(n->fields_obj, "addr"), &addr_id)) {
    sirj_diag_setf(c, "sem.parse.load.addr", c->cur_path, n->loc_line, node_id, n->tag, "%s addr must be a ref", n->tag);
    return false;
  }
  uint32_t align = 0;
  const JsonValue* alignv = json_obj_get(n->fields_obj, "align");
  if (alignv) {
    if (!json_get_u32(alignv, &align) || align == 0) {
      sirj_diag_setf(c, "sem.parse.load.align", c->cur_path, n->loc_line, node_id, n->tag, "%s align must be a positive integer", n->tag);
      return false;
    }
  } else {
    // Default alignment is the natural alignment of the loaded width.
    if (k == SIR_INST_LOAD_I8) align = 1;
    else if (k == SIR_INST_LOAD_I16) align = 2;
    else if (k == SIR_INST_LOAD_I32) align = 4;
    else if (k == SIR_INST_LOAD_I64) align = 8;
    else if (k == SIR_INST_LOAD_PTR) align = 8;
    else if (k == SIR_INST_LOAD_F32) align = 4;
    else if (k == SIR_INST_LOAD_F64) align = 8;
    else align = 1;
  }
  if (!is_pow2_u32(align)) {
    sirj_diag_setf(c, "sem.parse.load.align", c->cur_path, n->loc_line, node_id, n->tag, "%s align must be a power of two", n->tag);
    return false;
  }
  sir_val_id_t addr_slot = 0;
  val_kind_t ak = VK_INVALID;
  if (!eval_node(c, addr_id, &addr_slot, &ak)) return false;
  if (ak != VK_PTR) {
    sirj_diag_setf(c, "sem.load.addr_type", c->cur_path, n->loc_line, node_id, n->tag, "%s addr must be ptr", n->tag);
    return false;
  }
  const sir_val_id_t dst = alloc_slot(c, outk);
  bool ok = false;
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (k == SIR_INST_LOAD_I8) ok = sir_mb_emit_load_i8(c->mb, c->fn, dst, addr_slot, align);
  else if (k == SIR_INST_LOAD_I16) ok = sir_mb_emit_load_i16(c->mb, c->fn, dst, addr_slot, align);
  else if (k == SIR_INST_LOAD_I32) ok = sir_mb_emit_load_i32(c->mb, c->fn, dst, addr_slot, align);
  else if (k == SIR_INST_LOAD_I64) ok = sir_mb_emit_load_i64(c->mb, c->fn, dst, addr_slot, align);
  else if (k == SIR_INST_LOAD_PTR) ok = sir_mb_emit_load_ptr(c->mb, c->fn, dst, addr_slot, align);
  else if (k == SIR_INST_LOAD_F32) ok = sir_mb_emit_load_f32(c->mb, c->fn, dst, addr_slot, align);
  else if (k == SIR_INST_LOAD_F64) ok = sir_mb_emit_load_f64(c->mb, c->fn, dst, addr_slot, align);
  if (!ok) return false;
  if (!set_node_val(c, node_id, dst, outk)) return false;
  *out_slot = dst;
  *out_kind = outk;
  return true;
}

static bool eval_cstr(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* vv = json_obj_get(n->fields_obj, "value");
  const char* s = json_get_string(vv);
  if (!s) return false;
  const uint32_t len = (uint32_t)strlen(s);
  if (len != strlen(s)) return false;
  const sir_val_id_t ptr_slot = alloc_slot(c, VK_PTR);
  const sir_val_id_t len_slot = alloc_slot(c, VK_I64);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_const_bytes(c->mb, c->fn, ptr_slot, len_slot, (const uint8_t*)s, len)) return false;
  if (!set_node_val(c, node_id, ptr_slot, VK_PTR)) return false;
  *out_slot = ptr_slot;
  *out_kind = VK_PTR;
  return true;
}

static bool eval_name(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const char* nm = json_get_string(json_obj_get(n->fields_obj, "name"));
  if (!nm) return false;

  for (uint32_t i = 0; i < c->param_count; i++) {
    if (strcmp(c->params[i].name, nm) == 0) {
      const sir_val_id_t slot = c->params[i].slot;
      const val_kind_t k = c->params[i].kind;
      if (!set_node_val(c, node_id, slot, k)) return false;
      *out_slot = slot;
      *out_kind = k;
      return true;
    }
  }
  return false;
}

static bool eval_i32_add_mnemonic(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) {
    sirj_diag_setf(c, "sem.parse.i32.add.fields", c->cur_path, n->loc_line, node_id, n->tag, "i32.add missing/invalid fields object");
    return false;
  }
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 2) {
    sirj_diag_setf(c, "sem.parse.i32.add.args", c->cur_path, n->loc_line, node_id, n->tag, "i32.add args must be [a, b]");
    return false;
  }
  uint32_t a_id = 0, b_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &a_id)) {
    sirj_diag_setf(c, "sem.parse.i32.add.arg", c->cur_path, n->loc_line, node_id, n->tag, "i32.add arg 0 must be a ref");
    return false;
  }
  if (!parse_ref_id(av->v.arr.items[1], &b_id)) {
    sirj_diag_setf(c, "sem.parse.i32.add.arg", c->cur_path, n->loc_line, node_id, n->tag, "i32.add arg 1 must be a ref");
    return false;
  }
  sir_val_id_t a_slot = 0, b_slot = 0;
  val_kind_t ak = VK_INVALID, bk = VK_INVALID;
  if (!eval_node(c, a_id, &a_slot, &ak)) return false;
  if (!eval_node(c, b_id, &b_slot, &bk)) return false;
  if (ak != VK_I32 || bk != VK_I32) {
    sirj_diag_setf(c, "sem.i32.add.arg_type", c->cur_path, n->loc_line, node_id, n->tag, "i32.add args must be i32");
    return false;
  }
  const sir_val_id_t dst = alloc_slot(c, VK_I32);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_i32_add(c->mb, c->fn, dst, a_slot, b_slot)) return false;
  if (!set_node_val(c, node_id, dst, VK_I32)) return false;
  *out_slot = dst;
  *out_kind = VK_I32;
  return true;
}

static bool eval_i32_bin_mnemonic(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_inst_kind_t k, sir_val_id_t* out_slot,
                                  val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) {
    sirj_diag_setf(c, "sem.parse.i32.bin.fields", c->cur_path, n->loc_line, node_id, n->tag, "%s missing/invalid fields object", n->tag);
    return false;
  }
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 2) {
    sirj_diag_setf(c, "sem.parse.i32.bin.args", c->cur_path, n->loc_line, node_id, n->tag, "%s args must be [a, b]", n->tag);
    return false;
  }
  uint32_t a_id = 0, b_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &a_id)) {
    sirj_diag_setf(c, "sem.parse.i32.bin.arg", c->cur_path, n->loc_line, node_id, n->tag, "%s arg 0 must be a ref", n->tag);
    return false;
  }
  if (!parse_ref_id(av->v.arr.items[1], &b_id)) {
    sirj_diag_setf(c, "sem.parse.i32.bin.arg", c->cur_path, n->loc_line, node_id, n->tag, "%s arg 1 must be a ref", n->tag);
    return false;
  }
  sir_val_id_t a_slot = 0, b_slot = 0;
  val_kind_t ak = VK_INVALID, bk = VK_INVALID;
  if (!eval_node(c, a_id, &a_slot, &ak)) return false;
  if (!eval_node(c, b_id, &b_slot, &bk)) return false;
  if (ak != VK_I32 || bk != VK_I32) {
    sirj_diag_setf(c, "sem.i32.bin.arg_type", c->cur_path, n->loc_line, node_id, n->tag, "%s args must be i32", n->tag);
    return false;
  }
  const sir_val_id_t dst = alloc_slot(c, VK_I32);
  bool ok = false;
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  switch (k) {
    case SIR_INST_I32_SUB:
      ok = sir_mb_emit_i32_sub(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_MUL:
      ok = sir_mb_emit_i32_mul(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_AND:
      ok = sir_mb_emit_i32_and(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_OR:
      ok = sir_mb_emit_i32_or(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_XOR:
      ok = sir_mb_emit_i32_xor(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_SHL:
      ok = sir_mb_emit_i32_shl(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_SHR_S:
      ok = sir_mb_emit_i32_shr_s(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_SHR_U:
      ok = sir_mb_emit_i32_shr_u(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_DIV_S_SAT:
      ok = sir_mb_emit_i32_div_s_sat(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_DIV_S_TRAP:
      ok = sir_mb_emit_i32_div_s_trap(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_DIV_U_SAT:
      ok = sir_mb_emit_i32_div_u_sat(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_REM_S_SAT:
      ok = sir_mb_emit_i32_rem_s_sat(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_REM_U_SAT:
      ok = sir_mb_emit_i32_rem_u_sat(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    default:
      return false;
  }
  if (!ok) return false;
  if (!set_node_val(c, node_id, dst, VK_I32)) return false;
  *out_slot = dst;
  *out_kind = VK_I32;
  return true;
}

static bool eval_i32_zext_i8(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) {
    sirj_diag_setf(c, "sem.parse.i32.zext.i8.fields", c->cur_path, n->loc_line, node_id, n->tag, "i32.zext.i8 missing/invalid fields object");
    return false;
  }
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 1) {
    sirj_diag_setf(c, "sem.parse.i32.zext.i8.args", c->cur_path, n->loc_line, node_id, n->tag, "i32.zext.i8 args must be [x]");
    return false;
  }
  uint32_t x_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &x_id)) {
    sirj_diag_setf(c, "sem.parse.i32.zext.i8.arg", c->cur_path, n->loc_line, node_id, n->tag, "i32.zext.i8 arg 0 must be a ref");
    return false;
  }
  sir_val_id_t x_slot = 0;
  val_kind_t xk = VK_INVALID;
  if (!eval_node(c, x_id, &x_slot, &xk)) return false;
  if (xk != VK_I8) {
    sirj_diag_setf(c, "sem.i32.zext.i8.arg_type", c->cur_path, n->loc_line, node_id, n->tag, "i32.zext.i8 arg must be i8");
    return false;
  }
  const sir_val_id_t dst = alloc_slot(c, VK_I32);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_i32_zext_i8(c->mb, c->fn, dst, x_slot)) return false;
  if (!set_node_val(c, node_id, dst, VK_I32)) return false;
  *out_slot = dst;
  *out_kind = VK_I32;
  return true;
}

static bool eval_i32_zext_i16(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) {
    sirj_diag_setf(c, "sem.parse.i32.zext.i16.fields", c->cur_path, n->loc_line, node_id, n->tag, "i32.zext.i16 missing/invalid fields object");
    return false;
  }
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 1) {
    sirj_diag_setf(c, "sem.parse.i32.zext.i16.args", c->cur_path, n->loc_line, node_id, n->tag, "i32.zext.i16 args must be [x]");
    return false;
  }
  uint32_t x_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &x_id)) {
    sirj_diag_setf(c, "sem.parse.i32.zext.i16.arg", c->cur_path, n->loc_line, node_id, n->tag, "i32.zext.i16 arg 0 must be a ref");
    return false;
  }
  sir_val_id_t x_slot = 0;
  val_kind_t xk = VK_INVALID;
  if (!eval_node(c, x_id, &x_slot, &xk)) return false;
  if (xk != VK_I16) {
    sirj_diag_setf(c, "sem.i32.zext.i16.arg_type", c->cur_path, n->loc_line, node_id, n->tag, "i32.zext.i16 arg must be i16");
    return false;
  }
  const sir_val_id_t dst = alloc_slot(c, VK_I32);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_i32_zext_i16(c->mb, c->fn, dst, x_slot)) return false;
  if (!set_node_val(c, node_id, dst, VK_I32)) return false;
  *out_slot = dst;
  *out_kind = VK_I32;
  return true;
}

static bool eval_i64_zext_i32(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) {
    sirj_diag_setf(c, "sem.parse.i64.zext.i32.fields", c->cur_path, n->loc_line, node_id, n->tag, "i64.zext.i32 missing/invalid fields object");
    return false;
  }
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 1) {
    sirj_diag_setf(c, "sem.parse.i64.zext.i32.args", c->cur_path, n->loc_line, node_id, n->tag, "i64.zext.i32 args must be [x]");
    return false;
  }
  uint32_t x_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &x_id)) {
    sirj_diag_setf(c, "sem.parse.i64.zext.i32.arg", c->cur_path, n->loc_line, node_id, n->tag, "i64.zext.i32 arg 0 must be a ref");
    return false;
  }
  sir_val_id_t x_slot = 0;
  val_kind_t xk = VK_INVALID;
  if (!eval_node(c, x_id, &x_slot, &xk)) return false;
  if (xk != VK_I32) {
    sirj_diag_setf(c, "sem.i64.zext.i32.arg_type", c->cur_path, n->loc_line, node_id, n->tag, "i64.zext.i32 arg must be i32");
    return false;
  }
  const sir_val_id_t dst = alloc_slot(c, VK_I64);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_i64_zext_i32(c->mb, c->fn, dst, x_slot)) return false;
  if (!set_node_val(c, node_id, dst, VK_I64)) return false;
  *out_slot = dst;
  *out_kind = VK_I64;
  return true;
}

static bool eval_i32_un_mnemonic(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_inst_kind_t k, sir_val_id_t* out_slot,
                                 val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) {
    sirj_diag_setf(c, "sem.parse.i32.un.fields", c->cur_path, n->loc_line, node_id, n->tag, "%s missing/invalid fields object", n->tag);
    return false;
  }
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 1) {
    sirj_diag_setf(c, "sem.parse.i32.un.args", c->cur_path, n->loc_line, node_id, n->tag, "%s args must be [x]", n->tag);
    return false;
  }
  uint32_t x_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &x_id)) {
    sirj_diag_setf(c, "sem.parse.i32.un.arg", c->cur_path, n->loc_line, node_id, n->tag, "%s arg 0 must be a ref", n->tag);
    return false;
  }
  sir_val_id_t x_slot = 0;
  val_kind_t xk = VK_INVALID;
  if (!eval_node(c, x_id, &x_slot, &xk)) return false;
  if (xk != VK_I32) {
    sirj_diag_setf(c, "sem.i32.un.arg_type", c->cur_path, n->loc_line, node_id, n->tag, "%s arg must be i32", n->tag);
    return false;
  }
  const sir_val_id_t dst = alloc_slot(c, VK_I32);
  bool ok = false;
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (k == SIR_INST_I32_NOT) ok = sir_mb_emit_i32_not(c->mb, c->fn, dst, x_slot);
  else if (k == SIR_INST_I32_NEG) ok = sir_mb_emit_i32_neg(c->mb, c->fn, dst, x_slot);
  else return false;
  if (!ok) return false;
  if (!set_node_val(c, node_id, dst, VK_I32)) return false;
  *out_slot = dst;
  *out_kind = VK_I32;
  return true;
}

static bool eval_i32_trunc_i64(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) {
    sirj_diag_setf(c, "sem.parse.i32.trunc.i64.fields", c->cur_path, n->loc_line, node_id, n->tag, "i32.trunc.i64 missing/invalid fields object");
    return false;
  }
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 1) {
    sirj_diag_setf(c, "sem.parse.i32.trunc.i64.args", c->cur_path, n->loc_line, node_id, n->tag, "i32.trunc.i64 args must be [x]");
    return false;
  }
  uint32_t x_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &x_id)) {
    sirj_diag_setf(c, "sem.parse.i32.trunc.i64.arg", c->cur_path, n->loc_line, node_id, n->tag, "i32.trunc.i64 arg 0 must be a ref");
    return false;
  }
  sir_val_id_t x_slot = 0;
  val_kind_t xk = VK_INVALID;
  if (!eval_node(c, x_id, &x_slot, &xk)) return false;
  if (xk != VK_I64) {
    sirj_diag_setf(c, "sem.i32.trunc.i64.arg_type", c->cur_path, n->loc_line, node_id, n->tag, "i32.trunc.i64 arg must be i64");
    return false;
  }
  const sir_val_id_t dst = alloc_slot(c, VK_I32);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_i32_trunc_i64(c->mb, c->fn, dst, x_slot)) return false;
  if (!set_node_val(c, node_id, dst, VK_I32)) return false;
  *out_slot = dst;
  *out_kind = VK_I32;
  return true;
}

static bool eval_i32_cmp_eq(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 2) return false;
  uint32_t a_id = 0, b_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &a_id)) return false;
  if (!parse_ref_id(av->v.arr.items[1], &b_id)) return false;
  sir_val_id_t a_slot = 0, b_slot = 0;
  val_kind_t ak = VK_INVALID, bk = VK_INVALID;
  if (!eval_node(c, a_id, &a_slot, &ak)) return false;
  if (!eval_node(c, b_id, &b_slot, &bk)) return false;
  if (ak != VK_I32 || bk != VK_I32) return false;
  const sir_val_id_t dst = alloc_slot(c, VK_BOOL);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_i32_cmp_eq(c->mb, c->fn, dst, a_slot, b_slot)) return false;
  if (!set_node_val(c, node_id, dst, VK_BOOL)) return false;
  *out_slot = dst;
  *out_kind = VK_BOOL;
  return true;
}

static bool eval_binop_add(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  uint32_t a_id = 0, b_id = 0;
  if (!parse_ref_id(json_obj_get(n->fields_obj, "lhs"), &a_id)) return false;
  if (!parse_ref_id(json_obj_get(n->fields_obj, "rhs"), &b_id)) return false;
  sir_val_id_t a_slot = 0, b_slot = 0;
  val_kind_t ak = VK_INVALID, bk = VK_INVALID;
  if (!eval_node(c, a_id, &a_slot, &ak)) return false;
  if (!eval_node(c, b_id, &b_slot, &bk)) return false;
  if (ak != VK_I32 || bk != VK_I32) return false;
  const sir_val_id_t dst = alloc_slot(c, VK_I32);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_i32_add(c->mb, c->fn, dst, a_slot, b_slot)) return false;
  if (!set_node_val(c, node_id, dst, VK_I32)) return false;
  *out_slot = dst;
  *out_kind = VK_I32;
  return true;
}

static bool eval_ptr_to_i64_passthrough(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 1) return false;
  uint32_t arg_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &arg_id)) return false;
  sir_val_id_t arg_slot = 0;
  val_kind_t ak = VK_INVALID;
  if (!eval_node(c, arg_id, &arg_slot, &ak)) return false;
  if (ak != VK_PTR) return false;
  const sir_val_id_t dst = alloc_slot(c, VK_I64);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_ptr_to_i64(c->mb, c->fn, dst, arg_slot)) return false;
  if (!set_node_val(c, node_id, dst, VK_I64)) return false;
  *out_slot = dst;
  *out_kind = VK_I64;
  return true;
}

static bool eval_ptr_from_i64(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 1) return false;
  uint32_t arg_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &arg_id)) return false;
  sir_val_id_t arg_slot = 0;
  val_kind_t ak = VK_INVALID;
  if (!eval_node(c, arg_id, &arg_slot, &ak)) return false;
  if (ak != VK_I64 && ak != VK_I32) return false;
  const sir_val_id_t dst = alloc_slot(c, VK_PTR);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_ptr_from_i64(c->mb, c->fn, dst, arg_slot)) return false;
  if (!set_node_val(c, node_id, dst, VK_PTR)) return false;
  *out_slot = dst;
  *out_kind = VK_PTR;
  return true;
}

static bool eval_bool_not(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 1) return false;
  uint32_t x_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &x_id)) return false;
  sir_val_id_t x_slot = 0;
  val_kind_t xk = VK_INVALID;
  if (!eval_node(c, x_id, &x_slot, &xk)) return false;
  if (xk != VK_BOOL) return false;
  const sir_val_id_t dst = alloc_slot(c, VK_BOOL);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_bool_not(c->mb, c->fn, dst, x_slot)) return false;
  if (!set_node_val(c, node_id, dst, VK_BOOL)) return false;
  *out_slot = dst;
  *out_kind = VK_BOOL;
  return true;
}

static bool eval_bool_bin(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_inst_kind_t k, sir_val_id_t* out_slot,
                          val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 2) return false;
  uint32_t a_id = 0, b_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &a_id)) return false;
  if (!parse_ref_id(av->v.arr.items[1], &b_id)) return false;
  sir_val_id_t a_slot = 0, b_slot = 0;
  val_kind_t ak = VK_INVALID, bk = VK_INVALID;
  if (!eval_node(c, a_id, &a_slot, &ak)) return false;
  if (!eval_node(c, b_id, &b_slot, &bk)) return false;
  if (ak != VK_BOOL || bk != VK_BOOL) return false;
  const sir_val_id_t dst = alloc_slot(c, VK_BOOL);
  bool ok = false;
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (k == SIR_INST_BOOL_AND) ok = sir_mb_emit_bool_and(c->mb, c->fn, dst, a_slot, b_slot);
  else if (k == SIR_INST_BOOL_OR) ok = sir_mb_emit_bool_or(c->mb, c->fn, dst, a_slot, b_slot);
  else ok = sir_mb_emit_bool_xor(c->mb, c->fn, dst, a_slot, b_slot);
  if (!ok) return false;
  if (!set_node_val(c, node_id, dst, VK_BOOL)) return false;
  *out_slot = dst;
  *out_kind = VK_BOOL;
  return true;
}

static bool eval_i32_cmp(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_inst_kind_t k, sir_val_id_t* out_slot,
                         val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 2) return false;
  uint32_t a_id = 0, b_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &a_id)) return false;
  if (!parse_ref_id(av->v.arr.items[1], &b_id)) return false;
  sir_val_id_t a_slot = 0, b_slot = 0;
  val_kind_t ak = VK_INVALID, bk = VK_INVALID;
  if (!eval_node(c, a_id, &a_slot, &ak)) return false;
  if (!eval_node(c, b_id, &b_slot, &bk)) return false;
  if (ak != VK_I32 || bk != VK_I32) return false;
  const sir_val_id_t dst = alloc_slot(c, VK_BOOL);
  bool ok = false;
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  switch (k) {
    case SIR_INST_I32_CMP_NE:
      ok = sir_mb_emit_i32_cmp_ne(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_CMP_SLT:
      ok = sir_mb_emit_i32_cmp_slt(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_CMP_SLE:
      ok = sir_mb_emit_i32_cmp_sle(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_CMP_SGT:
      ok = sir_mb_emit_i32_cmp_sgt(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_CMP_SGE:
      ok = sir_mb_emit_i32_cmp_sge(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_CMP_ULT:
      ok = sir_mb_emit_i32_cmp_ult(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_CMP_ULE:
      ok = sir_mb_emit_i32_cmp_ule(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_CMP_UGT:
      ok = sir_mb_emit_i32_cmp_ugt(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    case SIR_INST_I32_CMP_UGE:
      ok = sir_mb_emit_i32_cmp_uge(c->mb, c->fn, dst, a_slot, b_slot);
      break;
    default:
      return false;
  }
  if (!ok) return false;
  if (!set_node_val(c, node_id, dst, VK_BOOL)) return false;
  *out_slot = dst;
  *out_kind = VK_BOOL;
  return true;
}

static bool eval_f32_cmp_ueq(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 2) return false;
  uint32_t a_id = 0, b_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &a_id)) return false;
  if (!parse_ref_id(av->v.arr.items[1], &b_id)) return false;
  sir_val_id_t a_slot = 0, b_slot = 0;
  val_kind_t ak = VK_INVALID, bk = VK_INVALID;
  if (!eval_node(c, a_id, &a_slot, &ak)) return false;
  if (!eval_node(c, b_id, &b_slot, &bk)) return false;
  if (ak != VK_F32 || bk != VK_F32) return false;
  const sir_val_id_t dst = alloc_slot(c, VK_BOOL);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_f32_cmp_ueq(c->mb, c->fn, dst, a_slot, b_slot)) return false;
  if (!set_node_val(c, node_id, dst, VK_BOOL)) return false;
  *out_slot = dst;
  *out_kind = VK_BOOL;
  return true;
}

static bool eval_f64_cmp_olt(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 2) return false;
  uint32_t a_id = 0, b_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &a_id)) return false;
  if (!parse_ref_id(av->v.arr.items[1], &b_id)) return false;
  sir_val_id_t a_slot = 0, b_slot = 0;
  val_kind_t ak = VK_INVALID, bk = VK_INVALID;
  if (!eval_node(c, a_id, &a_slot, &ak)) return false;
  if (!eval_node(c, b_id, &b_slot, &bk)) return false;
  if (ak != VK_F64 || bk != VK_F64) return false;
  const sir_val_id_t dst = alloc_slot(c, VK_BOOL);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_f64_cmp_olt(c->mb, c->fn, dst, a_slot, b_slot)) return false;
  if (!set_node_val(c, node_id, dst, VK_BOOL)) return false;
  *out_slot = dst;
  *out_kind = VK_BOOL;
  return true;
}

static bool eval_ptr_size_alignof(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, bool want_sizeof, sir_val_id_t* out_slot,
                                  val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  uint32_t ty_id = 0;
  if (!parse_ref_id(json_obj_get(n->fields_obj, "ty"), &ty_id)) {
    sirj_diag_setf(c, "sem.ptr.layout.bad_ty", c->cur_path, n->loc_line, node_id, n->tag, "missing/invalid ty ref");
    return false;
  }
  uint32_t size = 0, align = 0;
  if (!type_layout(c, ty_id, &size, &align)) {
    sirj_diag_setf(c, "sem.ptr.layout.bad_ty", c->cur_path, n->loc_line, node_id, n->tag, "unsupported ty id: %u", (unsigned)ty_id);
    return false;
  }
  if (want_sizeof) {
    const sir_val_id_t dst = alloc_slot(c, VK_I64);
    sir_mb_set_src(c->mb, node_id, n->loc_line);
    if (!sir_mb_emit_const_i64(c->mb, c->fn, dst, (int64_t)size)) return false;
    if (!set_node_val(c, node_id, dst, VK_I64)) return false;
    *out_slot = dst;
    *out_kind = VK_I64;
    return true;
  }
  const sir_val_id_t dst = alloc_slot(c, VK_I32);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_const_i32(c->mb, c->fn, dst, (int32_t)align)) return false;
  if (!set_node_val(c, node_id, dst, VK_I32)) return false;
  *out_slot = dst;
  *out_kind = VK_I32;
  return true;
}

static bool find_global_gid_by_name(const sirj_ctx_t* c, const char* name, sir_global_id_t* out_gid) {
  if (!c || !name || !out_gid) return false;
  for (uint32_t i = 0; i < c->symrec_cap; i++) {
    if (!c->syms || !c->syms[i].present) continue;
    if (!c->syms[i].name) continue;
    if (strcmp(c->syms[i].name, name) == 0) {
      if (c->syms[i].gid) {
        *out_gid = c->syms[i].gid;
        return true;
      }
      return false;
    }
  }
  return false;
}

static bool eval_ptr_sym(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const char* nm = json_get_string(json_obj_get(n->fields_obj, "name"));
  if (!nm) return false;
  sir_global_id_t gid = 0;
  if (find_global_gid_by_name(c, nm, &gid)) {
    const sir_val_id_t dst = alloc_slot(c, VK_PTR);
    sir_mb_set_src(c->mb, node_id, n->loc_line);
    if (!sir_mb_emit_global_addr(c->mb, c->fn, dst, gid)) return false;
    if (!set_node_val(c, node_id, dst, VK_PTR)) return false;
    *out_slot = dst;
    *out_kind = VK_PTR;
    return true;
  }

  sir_func_id_t fid = 0;
  if (resolve_internal_func_by_name(c, nm, &fid)) {
    const uint64_t tag = UINT64_C(0xF000000000000000);
    const zi_ptr_t p = (zi_ptr_t)(tag | (uint64_t)fid);
    const sir_val_id_t dst = alloc_slot(c, VK_PTR);
    sir_mb_set_src(c->mb, node_id, n->loc_line);
    if (!sir_mb_emit_const_ptr(c->mb, c->fn, dst, p)) return false;
    if (!set_node_val(c, node_id, dst, VK_PTR)) return false;
    *out_slot = dst;
    *out_kind = VK_PTR;
    return true;
  }

  sirj_diag_setf(c, "sem.sym.unknown", c->cur_path, n->loc_line, node_id, n->tag,
                 "unknown symbol: %s (extern calls: use decl.fn + call.indirect; globals: emit sym; in-module: emit fn)", nm);
  return false;
}

static bool eval_ptr_offset(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  uint32_t ty_id = 0;
  if (!parse_ref_id(json_obj_get(n->fields_obj, "ty"), &ty_id)) {
    sirj_diag_setf(c, "sem.ptr.offset.bad_ty", c->cur_path, n->loc_line, node_id, n->tag, "missing/invalid ty ref");
    return false;
  }
  uint32_t scale = 0, align = 0;
  if (!type_layout(c, ty_id, &scale, &align)) {
    sirj_diag_setf(c, "sem.ptr.offset.bad_ty", c->cur_path, n->loc_line, node_id, n->tag, "unsupported ty id: %u", (unsigned)ty_id);
    return false;
  }
  if (scale == 0) {
    sirj_diag_setf(c, "sem.ptr.offset.void", c->cur_path, n->loc_line, node_id, n->tag, "ptr.offset element type has size 0");
    return false;
  }
  (void)align;
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 2) return false;
  uint32_t base_id = 0, idx_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &base_id)) return false;
  if (!parse_ref_id(av->v.arr.items[1], &idx_id)) return false;
  sir_val_id_t base_slot = 0, idx_slot = 0;
  val_kind_t bk = VK_INVALID, ik = VK_INVALID;
  if (!eval_node(c, base_id, &base_slot, &bk)) return false;
  if (!eval_node(c, idx_id, &idx_slot, &ik)) return false;
  if (bk != VK_PTR) {
    sirj_diag_setf(c, "sem.ptr.offset.base_type", c->cur_path, n->loc_line, node_id, n->tag, "ptr.offset base must be ptr");
    return false;
  }
  if (ik != VK_I64 && ik != VK_I32) {
    sirj_diag_setf(c, "sem.ptr.offset.index_type", c->cur_path, n->loc_line, node_id, n->tag, "ptr.offset index must be i32 or i64");
    return false;
  }
  const sir_val_id_t dst = alloc_slot(c, VK_PTR);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_ptr_offset(c->mb, c->fn, dst, base_slot, idx_slot, scale)) return false;
  if (!set_node_val(c, node_id, dst, VK_PTR)) return false;
  *out_slot = dst;
  *out_kind = VK_PTR;
  return true;
}

static bool eval_select(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  if (n->type_ref == 0) {
    sirj_diag_setf(c, "sem.select.missing_type", c->cur_path, n->loc_line, node_id, n->tag, "select missing type_ref");
    return false;
  }
  if (n->type_ref >= c->type_cap || !c->types[n->type_ref].present || c->types[n->type_ref].is_fn) {
    sirj_diag_setf(c, "sem.select.bad_type", c->cur_path, n->loc_line, node_id, n->tag, "select has invalid type_ref=%u",
                   (unsigned)n->type_ref);
    return false;
  }

  val_kind_t tk = VK_INVALID;
  switch (c->types[n->type_ref].prim) {
    case SIR_PRIM_VOID:
      sirj_diag_setf(c, "sem.select.void", c->cur_path, n->loc_line, node_id, n->tag, "select cannot produce void");
      return false;
    case SIR_PRIM_I1:
      tk = VK_I1;
      break;
    case SIR_PRIM_I8:
      tk = VK_I8;
      break;
    case SIR_PRIM_I16:
      tk = VK_I16;
      break;
    case SIR_PRIM_I32:
      tk = VK_I32;
      break;
    case SIR_PRIM_I64:
      tk = VK_I64;
      break;
    case SIR_PRIM_PTR:
      tk = VK_PTR;
      break;
    case SIR_PRIM_BOOL:
      tk = VK_BOOL;
      break;
    case SIR_PRIM_F32:
      tk = VK_F32;
      break;
    case SIR_PRIM_F64:
      tk = VK_F64;
      break;
    default:
      sirj_diag_setf(c, "sem.select.bad_type", c->cur_path, n->loc_line, node_id, n->tag, "select has unsupported type");
      return false;
  }

  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 3) return false;
  uint32_t cond_id = 0, a_id = 0, b_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &cond_id)) return false;
  if (!parse_ref_id(av->v.arr.items[1], &a_id)) return false;
  if (!parse_ref_id(av->v.arr.items[2], &b_id)) return false;
  sir_val_id_t cond_slot = 0, a_slot = 0, b_slot = 0;
  val_kind_t ck = VK_INVALID, ak = VK_INVALID, bk = VK_INVALID;
  if (!eval_node(c, cond_id, &cond_slot, &ck)) return false;
  if (!eval_node(c, a_id, &a_slot, &ak)) return false;
  if (!eval_node(c, b_id, &b_slot, &bk)) return false;
  if (ck != VK_BOOL) return false;
  if (ak != tk || bk != tk) return false;

  const sir_val_id_t dst = alloc_slot(c, tk);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_select(c->mb, c->fn, dst, cond_slot, a_slot, b_slot)) return false;
  if (!set_node_val(c, node_id, dst, tk)) return false;
  *out_slot = dst;
  *out_kind = tk;
  return true;
}

static bool emit_branch_into_slot(sirj_ctx_t* c, uint32_t callsite_node_id, const sem_branch_t* br, sir_val_id_t dst, val_kind_t dstk) {
  if (!c || !br) return false;
  if (br->kind == SEM_BRANCH_VAL) {
    sir_val_id_t s = 0;
    val_kind_t k = VK_INVALID;
    if (!eval_node(c, br->node_id, &s, &k)) return false;
    if (k != dstk) return false;
    if (s == dst) return true;
    sir_mb_set_src(c->mb, callsite_node_id, (callsite_node_id < c->node_cap && c->nodes[callsite_node_id].present) ? c->nodes[callsite_node_id].loc_line : 0);
    const bool ok = emit_copy_slot(c, dst, s);
    sir_mb_clear_src(c->mb);
    return ok;
  }
  if (br->kind == SEM_BRANCH_THUNK) {
    sir_val_id_t s = 0;
    val_kind_t k = VK_INVALID;
    if (!emit_call_fun_sym(c, callsite_node_id, br->node_id, NULL, 0, false, true, dst, &s, &k)) return false;
    if (k != dstk) return false;
    // When dst_override is used, s should already equal dst.
    return true;
  }
  return false;
}

static bool eval_sem_if(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;

  // Result kind from type_ref.
  if (n->type_ref == 0) {
    sirj_diag_setf(c, "sem.sem.if.missing_type", c->cur_path, n->loc_line, node_id, n->tag, "sem.if missing type_ref");
    return false;
  }
  val_kind_t rk = VK_INVALID;
  if (!type_to_val_kind(c, n->type_ref, &rk) || rk == VK_INVALID || rk == VK_I1) {
    sirj_diag_setf(c, "sem.sem.if.bad_type", c->cur_path, n->loc_line, node_id, n->tag, "sem.if has unsupported type_ref=%u",
                   (unsigned)n->type_ref);
    return false;
  }

  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 3) {
    sirj_diag_setf(c, "sem.sem.if.bad_args", c->cur_path, n->loc_line, node_id, n->tag, "sem.if expects args:[cond, then, else]");
    return false;
  }

  uint32_t cond_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &cond_id)) return false;
  sem_branch_t th = {0}, el = {0};
  if (!parse_sem_branch(av->v.arr.items[1], &th) || !parse_sem_branch(av->v.arr.items[2], &el)) {
    sirj_diag_setf(c, "sem.sem.if.branch_kind", c->cur_path, n->loc_line, node_id, n->tag, "sem.if branch must be {kind:val|thunk,...}");
    return false;
  }

  sir_val_id_t cond_slot = 0;
  val_kind_t ck = VK_INVALID;
  if (!eval_node(c, cond_id, &cond_slot, &ck)) return false;
  if (ck != VK_BOOL) {
    sirj_diag_setf(c, "sem.sem.if.cond_type", c->cur_path, n->loc_line, node_id, n->tag, "sem.if cond must be bool");
    return false;
  }

  const sir_val_id_t res = alloc_slot(c, rk);

  // Fast path: value-only if becomes a single SELECT.
  if (th.kind == SEM_BRANCH_VAL && el.kind == SEM_BRANCH_VAL) {
    sir_val_id_t a = 0, b = 0;
    val_kind_t ak = VK_INVALID, bk = VK_INVALID;
    if (!eval_node(c, th.node_id, &a, &ak)) return false;
    if (!eval_node(c, el.node_id, &b, &bk)) return false;
    if (ak != rk || bk != rk) return false;
    sir_mb_set_src(c->mb, node_id, n->loc_line);
    if (!sir_mb_emit_select(c->mb, c->fn, res, cond_slot, a, b)) return false;
    sir_mb_clear_src(c->mb);
    if (!set_node_val(c, node_id, res, rk)) return false;
    *out_slot = res;
    *out_kind = rk;
    return true;
  }

  // General path: inline control flow.
  uint32_t ip_cbr = 0;
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_cbr(c->mb, c->fn, cond_slot, 0, 0, &ip_cbr)) return false;

  const uint32_t then_ip = sir_mb_func_ip(c->mb, c->fn);
  if (!emit_branch_into_slot(c, node_id, &th, res, rk)) return false;
  uint32_t ip_br_join = 0;
  if (!sir_mb_emit_br(c->mb, c->fn, 0, &ip_br_join)) return false;

  const uint32_t else_ip = sir_mb_func_ip(c->mb, c->fn);
  if (!emit_branch_into_slot(c, node_id, &el, res, rk)) return false;

  const uint32_t join_ip = sir_mb_func_ip(c->mb, c->fn);
  if (!sir_mb_patch_cbr(c->mb, c->fn, ip_cbr, then_ip, else_ip)) return false;
  if (!sir_mb_patch_br(c->mb, c->fn, ip_br_join, join_ip)) return false;
  sir_mb_clear_src(c->mb);

  if (!set_node_val(c, node_id, res, rk)) return false;
  *out_slot = res;
  *out_kind = rk;
  return true;
}

static bool eval_sem_and_or_sc(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, bool is_or, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;

  // Requires bool result.
  if (n->type_ref) {
    val_kind_t rk = VK_INVALID;
    if (!type_to_val_kind(c, n->type_ref, &rk) || rk != VK_BOOL) return false;
  }

  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 2) {
    sirj_diag_setf(c, "sem.sem.sc.bad_args", c->cur_path, n->loc_line, node_id, n->tag, "%s expects args:[lhs, rhs_thunk|val]",
                   is_or ? "sem.or_sc" : "sem.and_sc");
    return false;
  }

  uint32_t lhs_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &lhs_id)) return false;
  sem_branch_t rhs = {0};
  if (!parse_sem_branch(av->v.arr.items[1], &rhs)) return false;

  sir_val_id_t lhs_slot = 0;
  val_kind_t lk = VK_INVALID;
  if (!eval_node(c, lhs_id, &lhs_slot, &lk)) return false;
  if (lk != VK_BOOL) return false;

  const sir_val_id_t res = alloc_slot(c, VK_BOOL);
  uint32_t ip_cbr = 0;

  // For AND: if lhs false -> false, else -> rhs.
  // For OR:  if lhs true  -> true,  else -> rhs.
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_cbr(c->mb, c->fn, lhs_slot, 0, 0, &ip_cbr)) return false;

  uint32_t then_ip = sir_mb_func_ip(c->mb, c->fn);
  if (is_or) {
    if (!sir_mb_emit_const_bool(c->mb, c->fn, res, true)) return false;
  } else {
    // AND true-branch computes rhs.
    if (!emit_branch_into_slot(c, node_id, &rhs, res, VK_BOOL)) return false;
  }
  uint32_t ip_br_join = 0;
  if (!sir_mb_emit_br(c->mb, c->fn, 0, &ip_br_join)) return false;

  uint32_t else_ip = sir_mb_func_ip(c->mb, c->fn);
  if (is_or) {
    // OR false-branch computes rhs.
    if (!emit_branch_into_slot(c, node_id, &rhs, res, VK_BOOL)) return false;
  } else {
    if (!sir_mb_emit_const_bool(c->mb, c->fn, res, false)) return false;
  }

  const uint32_t join_ip = sir_mb_func_ip(c->mb, c->fn);
  if (!sir_mb_patch_cbr(c->mb, c->fn, ip_cbr, then_ip, else_ip)) return false;
  if (!sir_mb_patch_br(c->mb, c->fn, ip_br_join, join_ip)) return false;
  sir_mb_clear_src(c->mb);

  if (!set_node_val(c, node_id, res, VK_BOOL)) return false;
  *out_slot = res;
  *out_kind = VK_BOOL;
  return true;
}

static bool eval_sem_switch(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;

  if (n->type_ref == 0) return false;
  val_kind_t rk = VK_INVALID;
  if (!type_to_val_kind(c, n->type_ref, &rk) || rk == VK_INVALID || rk == VK_I1) return false;

  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len < 1) return false;
  uint32_t scrut_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &scrut_id)) return false;
  sir_val_id_t scrut_slot = 0;
  val_kind_t sk = VK_INVALID;
  if (!eval_node(c, scrut_id, &scrut_slot, &sk)) return false;
  if (sk != VK_I32) return false; // MVP

  const JsonValue* casesv = json_obj_get(n->fields_obj, "cases");
  if (!json_is_array(casesv)) return false;
  const JsonArray* ca = &casesv->v.arr;
  if (ca->len > 64) return false;
  const uint32_t ncase = (uint32_t)ca->len;

  int32_t* case_lits = NULL;
  uint32_t* case_target = NULL;
  sem_branch_t* case_body = NULL;
  if (ncase) {
    case_lits = (int32_t*)arena_alloc(&c->arena, (size_t)ncase * sizeof(int32_t));
    case_target = (uint32_t*)arena_alloc(&c->arena, (size_t)ncase * sizeof(uint32_t));
    case_body = (sem_branch_t*)arena_alloc(&c->arena, (size_t)ncase * sizeof(sem_branch_t));
    if (!case_lits || !case_target || !case_body) return false;
  }
  for (uint32_t i = 0; i < ncase; i++) {
    if (!json_is_object(ca->items[i])) return false;
    uint32_t lit_id = 0;
    if (!parse_ref_id(json_obj_get(ca->items[i], "lit"), &lit_id)) return false;
    if (!parse_const_i32_value(c, lit_id, &case_lits[i])) return false;
    if (!parse_sem_branch(json_obj_get(ca->items[i], "body"), &case_body[i])) return false;
    case_target[i] = 0;
  }
  sem_branch_t defb = {0};
  if (!parse_sem_branch(json_obj_get(n->fields_obj, "default"), &defb)) return false;

  const sir_val_id_t res = alloc_slot(c, rk);
  uint32_t ip_sw = 0;
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_switch(c->mb, c->fn, scrut_slot, case_lits, case_target, ncase, 0, &ip_sw)) return false;

  uint32_t* patch_br = NULL;
  if (ncase) {
    patch_br = (uint32_t*)arena_alloc(&c->arena, (size_t)ncase * sizeof(uint32_t));
    if (!patch_br) return false;
  }

  for (uint32_t i = 0; i < ncase; i++) {
    case_target[i] = sir_mb_func_ip(c->mb, c->fn);
    if (!emit_branch_into_slot(c, node_id, &case_body[i], res, rk)) return false;
    patch_br[i] = 0;
    if (!sir_mb_emit_br(c->mb, c->fn, 0, &patch_br[i])) return false;
  }

  const uint32_t def_ip = sir_mb_func_ip(c->mb, c->fn);
  if (!emit_branch_into_slot(c, node_id, &defb, res, rk)) return false;
  const uint32_t join_ip = sir_mb_func_ip(c->mb, c->fn);

  if (!sir_mb_patch_switch(c->mb, c->fn, ip_sw, case_target, ncase, def_ip)) return false;
  for (uint32_t i = 0; i < ncase; i++) {
    if (!sir_mb_patch_br(c->mb, c->fn, patch_br[i], join_ip)) return false;
  }
  sir_mb_clear_src(c->mb);

  if (!set_node_val(c, node_id, res, rk)) return false;
  *out_slot = res;
  *out_kind = rk;
  return true;
}

static bool eval_ptr_addsub(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, bool is_sub, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 2) return false;
  uint32_t base_id = 0, off_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &base_id)) return false;
  if (!parse_ref_id(av->v.arr.items[1], &off_id)) return false;
  sir_val_id_t base_slot = 0, off_slot = 0;
  val_kind_t bk = VK_INVALID, ok = VK_INVALID;
  if (!eval_node(c, base_id, &base_slot, &bk)) return false;
  if (!eval_node(c, off_id, &off_slot, &ok)) return false;
  if (bk != VK_PTR) return false;
  if (ok != VK_I64 && ok != VK_I32) return false;
  const sir_val_id_t dst = alloc_slot(c, VK_PTR);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  const bool ok_emit =
      is_sub ? sir_mb_emit_ptr_sub(c->mb, c->fn, dst, base_slot, off_slot) : sir_mb_emit_ptr_add(c->mb, c->fn, dst, base_slot, off_slot);
  if (!ok_emit) return false;
  if (!set_node_val(c, node_id, dst, VK_PTR)) return false;
  *out_slot = dst;
  *out_kind = VK_PTR;
  return true;
}

static bool eval_ptr_cmp(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, bool is_ne, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 2) return false;
  uint32_t a_id = 0, b_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &a_id)) return false;
  if (!parse_ref_id(av->v.arr.items[1], &b_id)) return false;
  sir_val_id_t a_slot = 0, b_slot = 0;
  val_kind_t ak = VK_INVALID, bk = VK_INVALID;
  if (!eval_node(c, a_id, &a_slot, &ak)) return false;
  if (!eval_node(c, b_id, &b_slot, &bk)) return false;
  if (ak != VK_PTR || bk != VK_PTR) return false;
  const sir_val_id_t dst = alloc_slot(c, VK_BOOL);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  const bool ok_emit =
      is_ne ? sir_mb_emit_ptr_cmp_ne(c->mb, c->fn, dst, a_slot, b_slot) : sir_mb_emit_ptr_cmp_eq(c->mb, c->fn, dst, a_slot, b_slot);
  if (!ok_emit) return false;
  if (!set_node_val(c, node_id, dst, VK_BOOL)) return false;
  *out_slot = dst;
  *out_kind = VK_BOOL;
  return true;
}

static bool resolve_internal_func_by_name(const sirj_ctx_t* c, const char* nm, sir_func_id_t* out) {
  if (!c || !nm || !out) return false;
  for (uint32_t i = 0; i < c->node_cap; i++) {
    const sir_func_id_t fid = (i < c->func_by_node_cap) ? c->func_by_node[i] : 0;
    if (!fid) continue;
    if (!c->nodes[i].present) continue;
    if (!c->nodes[i].fields_obj || c->nodes[i].fields_obj->type != JSON_OBJECT) continue;
    const char* fnm = json_get_string(json_obj_get(c->nodes[i].fields_obj, "name"));
    if (!fnm) continue;
    if (strcmp(fnm, nm) == 0) {
      *out = fid;
      return true;
    }
  }
  return false;
}

static bool eval_fun_sym(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const char* nm = json_get_string(json_obj_get(n->fields_obj, "name"));
  if (!nm) return false;

  // Resolve to an in-module function id, then encode as a tagged pointer constant.
  sir_func_id_t fid = 0;
  if (!resolve_internal_func_by_name(c, nm, &fid)) {
    sirj_diag_setf(c, "sem.fun.sym.unknown", c->cur_path, n->loc_line, node_id, n->tag, "unknown function for fun.sym: %s", nm);
    return false;
  }

  const uint64_t tag = UINT64_C(0xF000000000000000);
  const zi_ptr_t p = (zi_ptr_t)(tag | (uint64_t)fid);
  const sir_val_id_t dst = alloc_slot(c, VK_PTR);
  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (!sir_mb_emit_const_ptr(c->mb, c->fn, dst, p)) return false;
  if (!set_node_val(c, node_id, dst, VK_PTR)) return false;
  *out_slot = dst;
  *out_kind = VK_PTR;
  return true;
}

static bool eval_call_fun(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) {
    sirj_diag_setf(c, "sem.call.bad_fields", c->cur_path, n->loc_line, node_id, n->tag, "call.fun missing fields");
    return false;
  }
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len < 1) {
    sirj_diag_setf(c, "sem.call.bad_args", c->cur_path, n->loc_line, node_id, n->tag, "call.fun requires args:[callee, ...]");
    return false;
  }

  uint32_t callee_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &callee_id)) return false;
  if (callee_id >= c->node_cap || !c->nodes[callee_id].present) return false;
  const node_info_t* callee_n = &c->nodes[callee_id];

  // MVP: require callee be `fun.sym` so we can resolve it at compile time.
  if (!callee_n->tag || strcmp(callee_n->tag, "fun.sym") != 0) {
    sirj_diag_setf(c, "sem.call.fun.bad_callee", c->cur_path, n->loc_line, node_id, n->tag, "call.fun callee must be fun.sym (MVP)");
    return false;
  }
  if (!callee_n->fields_obj || callee_n->fields_obj->type != JSON_OBJECT) return false;
  const char* fn_name = json_get_string(json_obj_get(callee_n->fields_obj, "name"));
  if (!fn_name) return false;
  sir_func_id_t callee_fid = 0;
  if (!resolve_internal_func_by_name(c, fn_name, &callee_fid)) return false;

  // Signature: fun type -> sig (fn type).
  const uint32_t fun_ty = callee_n->type_ref;
  if (fun_ty == 0 || fun_ty >= c->type_cap || !c->types[fun_ty].present || !c->types[fun_ty].is_fun) {
    sirj_diag_setf(c, "sem.call.fun.bad_sig", c->cur_path, n->loc_line, node_id, n->tag, "call.fun callee missing/invalid fun type_ref");
    return false;
  }
  const uint32_t sig_tid = c->types[fun_ty].fun_sig;
  if (sig_tid == 0 || sig_tid >= c->type_cap || !c->types[sig_tid].present || !c->types[sig_tid].is_fn) {
    sirj_diag_setf(c, "sem.call.fun.bad_sig", c->cur_path, n->loc_line, node_id, n->tag, "call.fun callee fun.sig invalid");
    return false;
  }
  const type_info_t* sti = &c->types[sig_tid];

  const uint32_t argc = (uint32_t)(av->v.arr.len - 1);
  if (argc != (uint32_t)sti->param_count) {
    sirj_diag_setf(c, "sem.call.argc_mismatch", c->cur_path, n->loc_line, node_id, n->tag, "call.fun argc mismatch (got %u expected %u)",
                   (unsigned)argc, (unsigned)sti->param_count);
    return false;
  }
  if (argc > 16) return false;

  sir_val_id_t args_slots[16];
  val_kind_t args_kinds[16];
  for (uint32_t i = 0; i < argc; i++) {
    uint32_t arg_id = 0;
    if (!parse_ref_id(av->v.arr.items[i + 1], &arg_id)) return false;
    sir_val_id_t arg_slot = 0;
    val_kind_t ak = VK_INVALID;
    if (!eval_node(c, arg_id, &arg_slot, &ak)) return false;
    args_slots[i] = arg_slot;
    args_kinds[i] = ak;
  }

  for (uint32_t i = 0; i < argc; i++) {
    const uint32_t pid = sti->params[i];
    if (pid == 0 || pid >= c->type_cap || !c->types[pid].present || c->types[pid].is_fn) return false;
    val_kind_t expect = VK_INVALID;
    switch (c->types[pid].prim) {
      case SIR_PRIM_I1:
        expect = VK_I1;
        break;
      case SIR_PRIM_I8:
        expect = VK_I8;
        break;
      case SIR_PRIM_I16:
        expect = VK_I16;
        break;
      case SIR_PRIM_I32:
        expect = VK_I32;
        break;
      case SIR_PRIM_I64:
        expect = VK_I64;
        break;
      case SIR_PRIM_PTR:
        expect = VK_PTR;
        break;
      case SIR_PRIM_BOOL:
        expect = VK_BOOL;
        break;
      case SIR_PRIM_F32:
        expect = VK_F32;
        break;
      case SIR_PRIM_F64:
        expect = VK_F64;
        break;
      default:
        return false;
    }
    if (args_kinds[i] != expect) {
      sirj_diag_setf(c, "sem.call.arg_type_mismatch", c->cur_path, n->loc_line, node_id, n->tag, "call.fun arg %u type mismatch", (unsigned)i);
      return false;
    }
  }

  const uint32_t ret_tid = sti->ret;
  uint8_t result_count = 0;
  sir_val_id_t res_slots[1];
  val_kind_t rk = VK_INVALID;
  if (ret_tid != 0) {
    if (ret_tid >= c->type_cap || !c->types[ret_tid].present || c->types[ret_tid].is_fn) return false;
    const sir_prim_type_t rp = c->types[ret_tid].prim;
    if (rp == SIR_PRIM_VOID) {
      result_count = 0;
    } else if (rp == SIR_PRIM_I1) rk = VK_I1;
    else if (rp == SIR_PRIM_I8) rk = VK_I8;
    else if (rp == SIR_PRIM_I16) rk = VK_I16;
    else if (rp == SIR_PRIM_I32) rk = VK_I32;
    else if (rp == SIR_PRIM_I64) rk = VK_I64;
    else if (rp == SIR_PRIM_PTR) rk = VK_PTR;
    else if (rp == SIR_PRIM_BOOL) rk = VK_BOOL;
    else if (rp == SIR_PRIM_F32) rk = VK_F32;
    else if (rp == SIR_PRIM_F64) rk = VK_F64;
    else return false;
    if (rk != VK_INVALID) {
      res_slots[0] = alloc_slot(c, rk);
      result_count = 1;
    }
  }

  if (result_count && n->type_ref && n->type_ref != ret_tid) {
    sirj_diag_setf(c, "sem.call.ret_type_mismatch", c->cur_path, n->loc_line, node_id, n->tag, "call.fun return type_ref mismatch");
    return false;
  }

  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (result_count) {
    if (!sir_mb_emit_call_func_res(c->mb, c->fn, callee_fid, args_slots, argc, res_slots, result_count)) return false;
    if (!set_node_val(c, node_id, res_slots[0], rk)) return false;
    *out_slot = res_slots[0];
    *out_kind = rk;
    return true;
  }

  if (!sir_mb_emit_call_func_res(c->mb, c->fn, callee_fid, args_slots, argc, NULL, 0)) return false;
  *out_slot = 0;
  *out_kind = VK_INVALID;
  return true;
}

static bool eval_call_indirect(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) {
    sirj_diag_setf(c, "sem.call.bad_fields", c->cur_path, n->loc_line, node_id, n->tag, "call.indirect missing fields");
    return false;
  }
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len < 1) {
    sirj_diag_setf(c, "sem.call.bad_args", c->cur_path, n->loc_line, node_id, n->tag, "call.indirect missing args");
    return false;
  }

  uint32_t callee_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &callee_id)) {
    sirj_diag_setf(c, "sem.call.bad_callee", c->cur_path, n->loc_line, node_id, n->tag, "call.indirect callee is not a ref");
    return false;
  }

  sir_sym_id_t callee_sym = 0;
  sir_func_id_t callee_fn = 0;
  if (callee_id >= c->node_cap || !c->nodes[callee_id].present) {
    sirj_diag_setf(c, "sem.call.bad_callee", c->cur_path, n->loc_line, node_id, n->tag, "call.indirect callee ref missing");
    return false;
  }
  const node_info_t* cn = &c->nodes[callee_id];
  if (cn->tag && strcmp(cn->tag, "decl.fn") == 0) {
    if (!resolve_decl_fn_sym(c, callee_id, &callee_sym)) {
      sirj_diag_setf(c, "sem.call.bad_decl_fn", c->cur_path, n->loc_line, node_id, n->tag, "call.indirect callee decl.fn invalid");
      return false;
    }
  } else if (cn->tag && strcmp(cn->tag, "ptr.sym") == 0) {
    if (!cn->fields_obj || cn->fields_obj->type != JSON_OBJECT) {
      sirj_diag_setf(c, "sem.call.bad_ptrsym", c->cur_path, n->loc_line, node_id, n->tag, "call.indirect callee ptr.sym invalid");
      return false;
    }
    const char* nm = json_get_string(json_obj_get(cn->fields_obj, "name"));
    if (!nm) {
      sirj_diag_setf(c, "sem.call.bad_ptrsym", c->cur_path, n->loc_line, node_id, n->tag, "call.indirect callee ptr.sym missing name");
      return false;
    }
    if (!resolve_internal_func_by_name(c, nm, &callee_fn)) {
      sirj_diag_setf(c, "sem.call.ptrsym_not_fn", c->cur_path, n->loc_line, node_id, n->tag,
                     "ptr.sym does not resolve to an in-module fn: %s (extern calls: use decl.fn + call.indirect)", nm);
      return false;
    }
  } else {
    sirj_diag_setf(c, "sem.call.bad_callee_tag", c->cur_path, n->loc_line, node_id, n->tag, "call.indirect callee must be decl.fn or ptr.sym");
    return false;
  }

  sir_val_id_t args_slots[16];
  val_kind_t args_kinds[16];
  const uint32_t argc = (uint32_t)(av->v.arr.len - 1);
  if (argc > (uint32_t)(sizeof(args_slots) / sizeof(args_slots[0]))) {
    sirj_diag_setf(c, "sem.call.too_many_args", c->cur_path, n->loc_line, node_id, n->tag, "call.indirect too many args");
    return false;
  }

  for (uint32_t i = 0; i < argc; i++) {
    uint32_t arg_node_id = 0;
    if (!parse_ref_id(av->v.arr.items[i + 1], &arg_node_id)) {
      sirj_diag_setf(c, "sem.call.bad_arg", c->cur_path, n->loc_line, node_id, n->tag, "call.indirect arg %u is not a ref", (unsigned)i);
      return false;
    }
    val_kind_t ak = VK_INVALID;
    if (!eval_node(c, arg_node_id, &args_slots[i], &ak)) {
      if (!c->diag.set) {
        sirj_diag_setf(c, "sem.call.bad_arg", c->cur_path, n->loc_line, node_id, n->tag, "call.indirect failed to evaluate arg %u",
                       (unsigned)i);
      }
      return false;
    }
    args_kinds[i] = ak;
  }

  // Determine return arity from the callee signature.
  // (We only support 0 or 1 return in the sir_module MVP.)
  // Use the SIR `sig` field when present (points to a type id).
  uint32_t sig_tid = 0;
  const JsonValue* sigv = json_obj_get(n->fields_obj, "sig");
  if (sigv) {
    if (!parse_ref_id(sigv, &sig_tid)) {
      sirj_diag_setf(c, "sem.call.bad_sig", c->cur_path, n->loc_line, node_id, n->tag, "call.indirect bad sig ref");
      return false;
    }
  }
  uint32_t ret_tid = 0;
  if (sig_tid && sig_tid < c->type_cap && c->types[sig_tid].present && c->types[sig_tid].is_fn) {
    // If we have a signature, validate argument count and primitive-by-primitive types.
    const type_info_t* sti = &c->types[sig_tid];
    if (sti->param_count != argc) {
      sirj_diag_setf(c, "sem.call.argc_mismatch", c->cur_path, n->loc_line, node_id, n->tag, "call.indirect argc mismatch (got %u expected %u)",
                     (unsigned)argc, (unsigned)sti->param_count);
      return false;
    }
    for (uint32_t i = 0; i < argc; i++) {
      const uint32_t pid = sti->params[i];
      if (pid == 0 || pid >= c->type_cap || !c->types[pid].present || c->types[pid].is_fn) {
        sirj_diag_setf(c, "sem.call.bad_sig", c->cur_path, n->loc_line, node_id, n->tag, "call.indirect sig has invalid param type");
        return false;
      }
      val_kind_t expect = VK_INVALID;
      switch (c->types[pid].prim) {
        case SIR_PRIM_I1:
          expect = VK_I1;
          break;
        case SIR_PRIM_I8:
          expect = VK_I8;
          break;
        case SIR_PRIM_I16:
          expect = VK_I16;
          break;
        case SIR_PRIM_I32:
          expect = VK_I32;
          break;
        case SIR_PRIM_I64:
          expect = VK_I64;
          break;
        case SIR_PRIM_PTR:
          expect = VK_PTR;
          break;
        case SIR_PRIM_BOOL:
          expect = VK_BOOL;
          break;
        case SIR_PRIM_F32:
          expect = VK_F32;
          break;
        case SIR_PRIM_F64:
          expect = VK_F64;
          break;
        default:
          sirj_diag_setf(c, "sem.call.bad_sig", c->cur_path, n->loc_line, node_id, n->tag, "call.indirect sig has unsupported param type");
          return false;
      }
      if (args_kinds[i] != expect) {
        sirj_diag_setf(c, "sem.call.arg_type_mismatch", c->cur_path, n->loc_line, node_id, n->tag,
                       "call.indirect arg %u type mismatch", (unsigned)i);
        return false;
      }
    }
    ret_tid = c->types[sig_tid].ret;
  }

  uint8_t result_count = 0;
  sir_val_id_t res_slots[1];
  val_kind_t rk = VK_INVALID;
  if (ret_tid != 0) {
    if (ret_tid >= c->type_cap || !c->types[ret_tid].present || c->types[ret_tid].is_fn) return false;
    const sir_prim_type_t rp = c->types[ret_tid].prim;
    if (rp == SIR_PRIM_VOID) {
      // No return value.
      result_count = 0;
    } else if (rp == SIR_PRIM_I1) rk = VK_I1;
    else if (rp == SIR_PRIM_I8) rk = VK_I8;
    else if (rp == SIR_PRIM_I16) rk = VK_I16;
    else if (rp == SIR_PRIM_I32) rk = VK_I32;
    else if (rp == SIR_PRIM_I64) rk = VK_I64;
    else if (rp == SIR_PRIM_PTR) rk = VK_PTR;
    else if (rp == SIR_PRIM_BOOL) rk = VK_BOOL;
    else if (rp == SIR_PRIM_F32) rk = VK_F32;
    else if (rp == SIR_PRIM_F64) rk = VK_F64;
    else return false;
    if (rk != VK_INVALID) {
      res_slots[0] = alloc_slot(c, rk);
      result_count = 1;
    }
  }

  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (result_count) {
    if (callee_sym) {
      if (!sir_mb_emit_call_extern_res(c->mb, c->fn, callee_sym, args_slots, argc, res_slots, result_count)) return false;
    } else {
      if (!sir_mb_emit_call_func_res(c->mb, c->fn, callee_fn, args_slots, argc, res_slots, result_count)) return false;
    }
    if (!set_node_val(c, node_id, res_slots[0], rk)) return false;
    *out_slot = res_slots[0];
    *out_kind = rk;
    return true;
  }

  if (callee_sym) {
    if (!sir_mb_emit_call_extern(c->mb, c->fn, callee_sym, args_slots, argc)) return false;
  } else {
    if (!sir_mb_emit_call_func_res(c->mb, c->fn, callee_fn, args_slots, argc, NULL, 0)) return false;
  }
  *out_slot = 0;
  *out_kind = VK_INVALID;
  return true;
}

static bool eval_call_direct(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) {
    sirj_diag_setf(c, "sem.call.bad_fields", c->cur_path, n->loc_line, node_id, n->tag, "call missing fields");
    return false;
  }

  // Preferred shape:
  //   fields: { callee: {t:"ref",id:...}, args: [ {t:"ref",id:...}, ... ] }
  // Legacy tolerated shape:
  //   fields: { args: [ callee, ... ] }.
  uint32_t callee_id = 0;
  const JsonValue* callee_v = json_obj_get(n->fields_obj, "callee");
  const JsonValue* args_v = json_obj_get(n->fields_obj, "args");

  if (callee_v) {
    if (!parse_ref_id(callee_v, &callee_id)) {
      sirj_diag_setf(c, "sem.call.bad_callee", c->cur_path, n->loc_line, node_id, n->tag, "call callee is not a ref");
      return false;
    }
    if (args_v && !json_is_array(args_v)) {
      sirj_diag_setf(c, "sem.call.bad_args", c->cur_path, n->loc_line, node_id, n->tag, "call args must be an array");
      return false;
    }
  } else {
    if (!json_is_array(args_v) || args_v->v.arr.len < 1) {
      sirj_diag_setf(c, "sem.call.bad_args", c->cur_path, n->loc_line, node_id, n->tag, "call requires callee and args");
      return false;
    }
    if (!parse_ref_id(args_v->v.arr.items[0], &callee_id)) {
      sirj_diag_setf(c, "sem.call.bad_callee", c->cur_path, n->loc_line, node_id, n->tag, "call callee is not a ref");
      return false;
    }
  }

  if (callee_id >= c->node_cap || !c->nodes[callee_id].present) {
    sirj_diag_setf(c, "sem.call.bad_callee", c->cur_path, n->loc_line, node_id, n->tag, "call callee ref missing");
    return false;
  }

  // Collect arg ids.
  uint32_t arg_node_ids[16];
  uint32_t argc = 0;
  if (callee_v) {
    if (args_v) {
      const JsonArray* a = &args_v->v.arr;
      argc = (uint32_t)a->len;
      if (argc != a->len) return false;
      if (argc > (uint32_t)(sizeof(arg_node_ids) / sizeof(arg_node_ids[0]))) {
        sirj_diag_setf(c, "sem.call.too_many_args", c->cur_path, n->loc_line, node_id, n->tag, "call too many args");
        return false;
      }
      for (uint32_t i = 0; i < argc; i++) {
        uint32_t rid = 0;
        if (!parse_ref_id(a->items[i], &rid)) {
          sirj_diag_setf(c, "sem.call.bad_arg", c->cur_path, n->loc_line, node_id, n->tag, "call arg %u is not a ref", (unsigned)i);
          return false;
        }
        arg_node_ids[i] = rid;
      }
    } else {
      argc = 0;
    }
  } else {
    const JsonArray* a = &args_v->v.arr;
    if (a->len - 1 > (size_t)(sizeof(arg_node_ids) / sizeof(arg_node_ids[0]))) {
      sirj_diag_setf(c, "sem.call.too_many_args", c->cur_path, n->loc_line, node_id, n->tag, "call too many args");
      return false;
    }
    for (size_t i = 1; i < a->len; i++) {
      uint32_t rid = 0;
      if (!parse_ref_id(a->items[i], &rid)) {
        sirj_diag_setf(c, "sem.call.bad_arg", c->cur_path, n->loc_line, node_id, n->tag, "call arg %u is not a ref", (unsigned)(i - 1));
        return false;
      }
      arg_node_ids[i - 1] = rid;
    }
    argc = (uint32_t)(a->len - 1);
  }

  // Resolve callee.
  sir_sym_id_t callee_sym = 0;
  sir_func_id_t callee_fn = 0;
  const node_info_t* cn = &c->nodes[callee_id];
  if (cn->tag && strcmp(cn->tag, "decl.fn") == 0) {
    if (!resolve_decl_fn_sym(c, callee_id, &callee_sym)) {
      sirj_diag_setf(c, "sem.call.bad_decl_fn", c->cur_path, n->loc_line, node_id, n->tag, "call callee decl.fn invalid");
      return false;
    }
  } else if (cn->tag && strcmp(cn->tag, "fn") == 0) {
    if (callee_id >= c->func_by_node_cap || c->func_by_node[callee_id] == 0) {
      sirj_diag_setf(c, "sem.call.bad_fn", c->cur_path, n->loc_line, node_id, n->tag, "call callee fn is not lowered");
      return false;
    }
    callee_fn = c->func_by_node[callee_id];
  } else if (cn->tag && strcmp(cn->tag, "ptr.sym") == 0) {
    if (!cn->fields_obj || cn->fields_obj->type != JSON_OBJECT) return false;
    const char* nm = json_get_string(json_obj_get(cn->fields_obj, "name"));
    if (!nm) return false;
    if (!resolve_internal_func_by_name(c, nm, &callee_fn)) {
      sirj_diag_setf(c, "sem.call.ptrsym_not_fn", c->cur_path, n->loc_line, node_id, n->tag, "ptr.sym does not resolve to an in-module fn: %s",
                     nm);
      return false;
    }
  } else {
    sirj_diag_setf(c, "sem.call.bad_callee_tag", c->cur_path, n->loc_line, node_id, n->tag, "call callee must be fn or decl.fn");
    return false;
  }

  // Signature from callee's type_ref.
  const uint32_t sig_tid = cn->type_ref;
  if (sig_tid == 0 || sig_tid >= c->type_cap || !c->types[sig_tid].present || !c->types[sig_tid].is_fn) {
    sirj_diag_setf(c, "sem.call.bad_sig", c->cur_path, n->loc_line, node_id, n->tag, "call callee missing/invalid fn type_ref");
    return false;
  }
  const type_info_t* sti = &c->types[sig_tid];
  if (sti->param_count != argc) {
    sirj_diag_setf(c, "sem.call.argc_mismatch", c->cur_path, n->loc_line, node_id, n->tag, "call argc mismatch (got %u expected %u)",
                   (unsigned)argc, (unsigned)sti->param_count);
    return false;
  }

  // Evaluate args.
  sir_val_id_t args_slots[16];
  val_kind_t args_kinds[16];
  if (argc > (uint32_t)(sizeof(args_slots) / sizeof(args_slots[0]))) return false;

  for (uint32_t i = 0; i < argc; i++) {
    const uint32_t arg_id = arg_node_ids[i];
    sir_val_id_t arg_slot = 0;
    val_kind_t ak = VK_INVALID;
    if (!eval_node(c, arg_id, &arg_slot, &ak)) {
      sirj_diag_setf(c, "sem.call.bad_arg", c->cur_path, n->loc_line, node_id, n->tag, "call failed to evaluate arg %u", (unsigned)i);
      return false;
    }
    args_slots[i] = arg_slot;
    args_kinds[i] = ak;
  }

  // Type-check args (primitive-only).
  for (uint32_t i = 0; i < argc; i++) {
    const uint32_t pid = sti->params[i];
    if (pid == 0 || pid >= c->type_cap || !c->types[pid].present || c->types[pid].is_fn) {
      sirj_diag_setf(c, "sem.call.bad_sig", c->cur_path, n->loc_line, node_id, n->tag, "call sig has invalid param type");
      return false;
    }
    val_kind_t expect = VK_INVALID;
    switch (c->types[pid].prim) {
      case SIR_PRIM_I1:
        expect = VK_I1;
        break;
      case SIR_PRIM_I8:
        expect = VK_I8;
        break;
      case SIR_PRIM_I16:
        expect = VK_I16;
        break;
      case SIR_PRIM_I32:
        expect = VK_I32;
        break;
      case SIR_PRIM_I64:
        expect = VK_I64;
        break;
      case SIR_PRIM_PTR:
        expect = VK_PTR;
        break;
      case SIR_PRIM_BOOL:
        expect = VK_BOOL;
        break;
      case SIR_PRIM_F32:
        expect = VK_F32;
        break;
      case SIR_PRIM_F64:
        expect = VK_F64;
        break;
      default:
        sirj_diag_setf(c, "sem.call.bad_sig", c->cur_path, n->loc_line, node_id, n->tag, "call sig has unsupported param type");
        return false;
    }
    if (args_kinds[i] != expect) {
      sirj_diag_setf(c, "sem.call.arg_type_mismatch", c->cur_path, n->loc_line, node_id, n->tag, "call arg %u type mismatch", (unsigned)i);
      return false;
    }
  }

  // Return.
  const uint32_t ret_tid = sti->ret;
  uint8_t result_count = 0;
  sir_val_id_t res_slots[1];
  val_kind_t rk = VK_INVALID;
  if (ret_tid != 0) {
    if (ret_tid >= c->type_cap || !c->types[ret_tid].present || c->types[ret_tid].is_fn) return false;
    const sir_prim_type_t rp = c->types[ret_tid].prim;
    if (rp == SIR_PRIM_VOID) {
      result_count = 0;
    } else if (rp == SIR_PRIM_I1) rk = VK_I1;
    else if (rp == SIR_PRIM_I8) rk = VK_I8;
    else if (rp == SIR_PRIM_I16) rk = VK_I16;
    else if (rp == SIR_PRIM_I32) rk = VK_I32;
    else if (rp == SIR_PRIM_I64) rk = VK_I64;
    else if (rp == SIR_PRIM_PTR) rk = VK_PTR;
    else if (rp == SIR_PRIM_BOOL) rk = VK_BOOL;
    else if (rp == SIR_PRIM_F32) rk = VK_F32;
    else if (rp == SIR_PRIM_F64) rk = VK_F64;
    else return false;
    if (rk != VK_INVALID) {
      res_slots[0] = alloc_slot(c, rk);
      result_count = 1;
    }
  }

  if (result_count && n->type_ref && n->type_ref != ret_tid) {
    sirj_diag_setf(c, "sem.call.ret_type_mismatch", c->cur_path, n->loc_line, node_id, n->tag, "call return type_ref mismatch");
    return false;
  }

  sir_mb_set_src(c->mb, node_id, n->loc_line);
  if (result_count) {
    if (callee_sym) {
      if (!sir_mb_emit_call_extern_res(c->mb, c->fn, callee_sym, args_slots, argc, res_slots, result_count)) return false;
    } else {
      if (!sir_mb_emit_call_func_res(c->mb, c->fn, callee_fn, args_slots, argc, res_slots, result_count)) return false;
    }
    if (!set_node_val(c, node_id, res_slots[0], rk)) return false;
    *out_slot = res_slots[0];
    *out_kind = rk;
    return true;
  }

  if (callee_sym) {
    if (!sir_mb_emit_call_extern(c->mb, c->fn, callee_sym, args_slots, argc)) return false;
  } else {
    if (!sir_mb_emit_call_func_res(c->mb, c->fn, callee_fn, args_slots, argc, NULL, 0)) return false;
  }
  *out_slot = 0;
  *out_kind = VK_INVALID;
  return true;
}

static bool eval_node(sirj_ctx_t* c, uint32_t node_id, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !out_slot || !out_kind) return false;
  sir_val_id_t cached = 0;
  val_kind_t ck = VK_INVALID;
  if (get_node_val(c, node_id, &cached, &ck)) {
    *out_slot = cached;
    *out_kind = ck;
    return true;
  }

  if (node_id >= c->node_cap || !c->nodes[node_id].present) return false;
  const node_info_t* n = &c->nodes[node_id];
  if (!n->tag) return false;

  if (strcmp(n->tag, "bparam") == 0) return eval_bparam(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "const.i1") == 0) return eval_const_i1(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "const.i8") == 0) return eval_const_i8(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "const.i16") == 0) return eval_const_i16(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "const.i32") == 0) return eval_const_i32(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "const.i64") == 0) return eval_const_i64(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "const.f32") == 0) return eval_const_f32(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "const.f64") == 0) return eval_const_f64(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "const.bool") == 0) return eval_const_bool(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "cstr") == 0) return eval_cstr(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "name") == 0) return eval_name(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "ptr.sym") == 0) return eval_ptr_sym(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "fun.sym") == 0) return eval_fun_sym(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "sem.if") == 0) return eval_sem_if(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "sem.and_sc") == 0) return eval_sem_and_or_sc(c, node_id, n, false, out_slot, out_kind);
  if (strcmp(n->tag, "sem.or_sc") == 0) return eval_sem_and_or_sc(c, node_id, n, true, out_slot, out_kind);
  if (strcmp(n->tag, "sem.switch") == 0) return eval_sem_switch(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "ptr.sizeof") == 0) return eval_ptr_size_alignof(c, node_id, n, true, out_slot, out_kind);
  if (strcmp(n->tag, "ptr.alignof") == 0) return eval_ptr_size_alignof(c, node_id, n, false, out_slot, out_kind);
  if (strcmp(n->tag, "ptr.offset") == 0) return eval_ptr_offset(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "ptr.add") == 0) return eval_ptr_addsub(c, node_id, n, false, out_slot, out_kind);
  if (strcmp(n->tag, "ptr.sub") == 0) return eval_ptr_addsub(c, node_id, n, true, out_slot, out_kind);
  if (strcmp(n->tag, "ptr.cmp.eq") == 0) return eval_ptr_cmp(c, node_id, n, false, out_slot, out_kind);
  if (strcmp(n->tag, "ptr.cmp.ne") == 0) return eval_ptr_cmp(c, node_id, n, true, out_slot, out_kind);
  if (strcmp(n->tag, "ptr.to_i64") == 0) return eval_ptr_to_i64_passthrough(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "ptr.from_i64") == 0) return eval_ptr_from_i64(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "bool.not") == 0) return eval_bool_not(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "bool.and") == 0) return eval_bool_bin(c, node_id, n, SIR_INST_BOOL_AND, out_slot, out_kind);
  if (strcmp(n->tag, "bool.or") == 0) return eval_bool_bin(c, node_id, n, SIR_INST_BOOL_OR, out_slot, out_kind);
  if (strcmp(n->tag, "bool.xor") == 0) return eval_bool_bin(c, node_id, n, SIR_INST_BOOL_XOR, out_slot, out_kind);
  if (strcmp(n->tag, "select") == 0) return eval_select(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "i32.add") == 0) return eval_i32_add_mnemonic(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "i32.sub") == 0) return eval_i32_bin_mnemonic(c, node_id, n, SIR_INST_I32_SUB, out_slot, out_kind);
  if (strcmp(n->tag, "i32.mul") == 0) return eval_i32_bin_mnemonic(c, node_id, n, SIR_INST_I32_MUL, out_slot, out_kind);
  if (strcmp(n->tag, "i32.and") == 0) return eval_i32_bin_mnemonic(c, node_id, n, SIR_INST_I32_AND, out_slot, out_kind);
  if (strcmp(n->tag, "i32.or") == 0) return eval_i32_bin_mnemonic(c, node_id, n, SIR_INST_I32_OR, out_slot, out_kind);
  if (strcmp(n->tag, "i32.xor") == 0) return eval_i32_bin_mnemonic(c, node_id, n, SIR_INST_I32_XOR, out_slot, out_kind);
  if (strcmp(n->tag, "i32.not") == 0) return eval_i32_un_mnemonic(c, node_id, n, SIR_INST_I32_NOT, out_slot, out_kind);
  if (strcmp(n->tag, "i32.neg") == 0) return eval_i32_un_mnemonic(c, node_id, n, SIR_INST_I32_NEG, out_slot, out_kind);
  if (strcmp(n->tag, "i32.shl") == 0) return eval_i32_bin_mnemonic(c, node_id, n, SIR_INST_I32_SHL, out_slot, out_kind);
  if (strcmp(n->tag, "i32.shr.s") == 0) return eval_i32_bin_mnemonic(c, node_id, n, SIR_INST_I32_SHR_S, out_slot, out_kind);
  if (strcmp(n->tag, "i32.shr.u") == 0) return eval_i32_bin_mnemonic(c, node_id, n, SIR_INST_I32_SHR_U, out_slot, out_kind);
  if (strcmp(n->tag, "i32.div.s.sat") == 0) return eval_i32_bin_mnemonic(c, node_id, n, SIR_INST_I32_DIV_S_SAT, out_slot, out_kind);
  if (strcmp(n->tag, "i32.div.s.trap") == 0) return eval_i32_bin_mnemonic(c, node_id, n, SIR_INST_I32_DIV_S_TRAP, out_slot, out_kind);
  if (strcmp(n->tag, "i32.div.u.sat") == 0) return eval_i32_bin_mnemonic(c, node_id, n, SIR_INST_I32_DIV_U_SAT, out_slot, out_kind);
  if (strcmp(n->tag, "i32.rem.s.sat") == 0) return eval_i32_bin_mnemonic(c, node_id, n, SIR_INST_I32_REM_S_SAT, out_slot, out_kind);
  if (strcmp(n->tag, "i32.rem.u.sat") == 0) return eval_i32_bin_mnemonic(c, node_id, n, SIR_INST_I32_REM_U_SAT, out_slot, out_kind);
  if (strcmp(n->tag, "i32.zext.i8") == 0) return eval_i32_zext_i8(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "i32.zext.i16") == 0) return eval_i32_zext_i16(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "i64.zext.i32") == 0) return eval_i64_zext_i32(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "i32.trunc.i64") == 0) return eval_i32_trunc_i64(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "i32.cmp.eq") == 0) return eval_i32_cmp_eq(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "i32.cmp.ne") == 0) return eval_i32_cmp(c, node_id, n, SIR_INST_I32_CMP_NE, out_slot, out_kind);
  if (strcmp(n->tag, "i32.cmp.slt") == 0) return eval_i32_cmp(c, node_id, n, SIR_INST_I32_CMP_SLT, out_slot, out_kind);
  if (strcmp(n->tag, "i32.cmp.sle") == 0) return eval_i32_cmp(c, node_id, n, SIR_INST_I32_CMP_SLE, out_slot, out_kind);
  if (strcmp(n->tag, "i32.cmp.sgt") == 0) return eval_i32_cmp(c, node_id, n, SIR_INST_I32_CMP_SGT, out_slot, out_kind);
  if (strcmp(n->tag, "i32.cmp.sge") == 0) return eval_i32_cmp(c, node_id, n, SIR_INST_I32_CMP_SGE, out_slot, out_kind);
  if (strcmp(n->tag, "i32.cmp.ult") == 0) return eval_i32_cmp(c, node_id, n, SIR_INST_I32_CMP_ULT, out_slot, out_kind);
  if (strcmp(n->tag, "i32.cmp.ule") == 0) return eval_i32_cmp(c, node_id, n, SIR_INST_I32_CMP_ULE, out_slot, out_kind);
  if (strcmp(n->tag, "i32.cmp.ugt") == 0) return eval_i32_cmp(c, node_id, n, SIR_INST_I32_CMP_UGT, out_slot, out_kind);
  if (strcmp(n->tag, "i32.cmp.uge") == 0) return eval_i32_cmp(c, node_id, n, SIR_INST_I32_CMP_UGE, out_slot, out_kind);
  if (strcmp(n->tag, "f32.cmp.ueq") == 0) return eval_f32_cmp_ueq(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "f64.cmp.olt") == 0) return eval_f64_cmp_olt(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "binop.add") == 0) return eval_binop_add(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "alloca.i8") == 0) return eval_alloca_mnemonic(c, node_id, n, 1, 1, out_slot, out_kind);
  if (strcmp(n->tag, "alloca.i16") == 0) return eval_alloca_mnemonic(c, node_id, n, 2, 2, out_slot, out_kind);
  if (strcmp(n->tag, "alloca.i32") == 0) return eval_alloca_mnemonic(c, node_id, n, 4, 4, out_slot, out_kind);
  if (strcmp(n->tag, "alloca.i64") == 0) return eval_alloca_mnemonic(c, node_id, n, 8, 8, out_slot, out_kind);
  if (strcmp(n->tag, "alloca.f32") == 0) return eval_alloca_mnemonic(c, node_id, n, 4, 4, out_slot, out_kind);
  if (strcmp(n->tag, "alloca.f64") == 0) return eval_alloca_mnemonic(c, node_id, n, 8, 8, out_slot, out_kind);
  if (strcmp(n->tag, "load.i8") == 0) return eval_load_mnemonic(c, node_id, n, SIR_INST_LOAD_I8, VK_I8, out_slot, out_kind);
  if (strcmp(n->tag, "load.i16") == 0) return eval_load_mnemonic(c, node_id, n, SIR_INST_LOAD_I16, VK_I16, out_slot, out_kind);
  if (strcmp(n->tag, "load.i32") == 0) return eval_load_mnemonic(c, node_id, n, SIR_INST_LOAD_I32, VK_I32, out_slot, out_kind);
  if (strcmp(n->tag, "load.i64") == 0) return eval_load_mnemonic(c, node_id, n, SIR_INST_LOAD_I64, VK_I64, out_slot, out_kind);
  if (strcmp(n->tag, "load.ptr") == 0) return eval_load_mnemonic(c, node_id, n, SIR_INST_LOAD_PTR, VK_PTR, out_slot, out_kind);
  if (strcmp(n->tag, "load.f32") == 0) return eval_load_mnemonic(c, node_id, n, SIR_INST_LOAD_F32, VK_F32, out_slot, out_kind);
  if (strcmp(n->tag, "load.f64") == 0) return eval_load_mnemonic(c, node_id, n, SIR_INST_LOAD_F64, VK_F64, out_slot, out_kind);
  if (strcmp(n->tag, "call.fun") == 0) return eval_call_fun(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "call") == 0) return eval_call_direct(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "call.indirect") == 0) return eval_call_indirect(c, node_id, n, out_slot, out_kind);

  sirj_diag_setf(c, "sem.unsupported.node", c->cur_path, n->loc_line, node_id, n->tag, "unsupported node tag: %s", n->tag);
  return false;
}

static bool exec_stmt(sirj_ctx_t* c, uint32_t stmt_id, bool* out_did_return, sir_val_id_t* out_exit_slot, val_kind_t* out_exit_kind);

static bool exec_inline_block(sirj_ctx_t* c, uint32_t block_id, bool* out_did_return, sir_val_id_t* out_exit_slot, val_kind_t* out_exit_kind) {
  if (!c || !out_did_return || !out_exit_slot) return false;
  *out_did_return = false;
  *out_exit_slot = 0;
  if (out_exit_kind) *out_exit_kind = VK_INVALID;
  if (block_id >= c->node_cap || !c->nodes[block_id].present) return false;
  const node_info_t* bn = &c->nodes[block_id];
  if (!bn->tag || strcmp(bn->tag, "block") != 0) return false;
  if (!bn->fields_obj || bn->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* sv = json_obj_get(bn->fields_obj, "stmts");
  if (!json_is_array(sv)) return false;
  const JsonArray* a = &sv->v.arr;
  for (size_t i = 0; i < a->len; i++) {
    uint32_t sid = 0;
    if (!parse_ref_id(a->items[i], &sid)) return false;
    bool did_ret = false;
    sir_val_id_t exit_slot = 0;
    val_kind_t exit_kind = VK_INVALID;
    if (!exec_stmt(c, sid, &did_ret, &exit_slot, &exit_kind)) return false;
    if (did_ret) {
      *out_did_return = true;
      *out_exit_slot = exit_slot;
      if (out_exit_kind) *out_exit_kind = exit_kind;
      return true;
    }
  }
  return true;
}

static bool exec_stmt(sirj_ctx_t* c, uint32_t stmt_id, bool* out_did_return, sir_val_id_t* out_exit_slot, val_kind_t* out_exit_kind) {
  if (!c || !out_did_return || !out_exit_slot) return false;
  *out_did_return = false;
  *out_exit_slot = 0;
  if (out_exit_kind) *out_exit_kind = VK_INVALID;
  if (stmt_id >= c->node_cap || !c->nodes[stmt_id].present) return false;
  const node_info_t* n = &c->nodes[stmt_id];
  if (!n->tag) return false;

  if (strcmp(n->tag, "let") == 0) {
    if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
    const JsonValue* vv = json_obj_get(n->fields_obj, "value");
    uint32_t vid = 0;
    if (!parse_ref_id(vv, &vid)) return false;
    sir_val_id_t tmp = 0;
    val_kind_t tk = VK_INVALID;
    if (!eval_node(c, vid, &tmp, &tk)) return false;
    (void)tmp;
    (void)tk;
    return true;
  }

  if (strcmp(n->tag, "store.i8") == 0) return eval_store_mnemonic(c, stmt_id, n, SIR_INST_STORE_I8);
  if (strcmp(n->tag, "store.i16") == 0) return eval_store_mnemonic(c, stmt_id, n, SIR_INST_STORE_I16);
  if (strcmp(n->tag, "store.i32") == 0) return eval_store_mnemonic(c, stmt_id, n, SIR_INST_STORE_I32);
  if (strcmp(n->tag, "store.i64") == 0) return eval_store_mnemonic(c, stmt_id, n, SIR_INST_STORE_I64);
  if (strcmp(n->tag, "store.ptr") == 0) return eval_store_mnemonic(c, stmt_id, n, SIR_INST_STORE_PTR);
  if (strcmp(n->tag, "store.f32") == 0) return eval_store_mnemonic(c, stmt_id, n, SIR_INST_STORE_F32);
  if (strcmp(n->tag, "store.f64") == 0) return eval_store_mnemonic(c, stmt_id, n, SIR_INST_STORE_F64);
  if (strcmp(n->tag, "mem.copy") == 0) return eval_mem_copy_stmt(c, stmt_id, n);
  if (strcmp(n->tag, "mem.fill") == 0) return eval_mem_fill_stmt(c, stmt_id, n);
  if (strcmp(n->tag, "call") == 0) {
    // Calls are expression nodes in SIR, but they often appear in block.stmts for side effects.
    sir_val_id_t tmp = 0;
    val_kind_t tk = VK_INVALID;
    if (!eval_node(c, stmt_id, &tmp, &tk)) return false;
    (void)tmp;
    (void)tk;
    return true;
  }
  if (strcmp(n->tag, "call.fun") == 0) {
    // Calls are expression nodes in SIR, but they often appear in block.stmts for side effects.
    sir_val_id_t tmp = 0;
    val_kind_t tk = VK_INVALID;
    if (!eval_node(c, stmt_id, &tmp, &tk)) return false;
    (void)tmp;
    (void)tk;
    return true;
  }
  if (strcmp(n->tag, "call.indirect") == 0) {
    // Calls are expression nodes in SIR, but they often appear in block.stmts for side effects.
    sir_val_id_t tmp = 0;
    val_kind_t tk = VK_INVALID;
    if (!eval_node(c, stmt_id, &tmp, &tk)) return false;
    (void)tmp;
    (void)tk;
    return true;
  }

  if (strcmp(n->tag, "sem.defer") == 0) {
    if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
    const JsonValue* av = json_obj_get(n->fields_obj, "args");
    if (!json_is_array(av) || av->v.arr.len != 1) return false;
    sem_branch_t br = {0};
    if (!parse_sem_branch(av->v.arr.items[0], &br) || br.kind != SEM_BRANCH_THUNK) return false;
    if (c->defer_count >= (uint32_t)(sizeof(c->defers) / sizeof(c->defers[0]))) {
      sirj_diag_setf(c, "sem.defer.too_many", c->cur_path, n->loc_line, stmt_id, n->tag, "too many active defers");
      return false;
    }
    c->defers[c->defer_count++] = br.node_id;
    return true;
  }

  if (strcmp(n->tag, "sem.scope") == 0) {
    if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
    const JsonValue* dv = json_obj_get(n->fields_obj, "defers");
    const JsonValue* bodyv = json_obj_get(n->fields_obj, "body");
    if (!json_is_array(dv) || !json_is_object(bodyv)) return false;
    uint32_t body_id = 0;
    if (!parse_ref_id(bodyv, &body_id)) return false;

    const uint32_t base = c->defer_count;
    const JsonArray* da = &dv->v.arr;
    for (size_t i = 0; i < da->len; i++) {
      sem_branch_t br = {0};
      if (!parse_sem_branch(da->items[i], &br) || br.kind != SEM_BRANCH_THUNK) return false;
      if (c->defer_count >= (uint32_t)(sizeof(c->defers) / sizeof(c->defers[0]))) return false;
      c->defers[c->defer_count++] = br.node_id;
    }

    bool did_ret = false;
    sir_val_id_t exit_slot = 0;
    val_kind_t exit_kind = VK_INVALID;
    if (!exec_inline_block(c, body_id, &did_ret, &exit_slot, &exit_kind)) return false;
    if (did_ret) {
      *out_did_return = true;
      *out_exit_slot = exit_slot;
      if (out_exit_kind) *out_exit_kind = exit_kind;
      return true;
    }

    // Fallthrough: run all defers registered within this scope (including nested sem.defer).
    if (!emit_run_defers(c, base, stmt_id)) return false;
    return true;
  }

  if (strcmp(n->tag, "sem.continue") == 0) {
    // MVP: used inside thunk bodies for sem.while; treat as "return 0" from the thunk.
    if (c->in_cfg) {
      sirj_diag_setf(c, "sem.sem.continue.cfg", c->cur_path, n->loc_line, stmt_id, n->tag, "sem.continue not supported in CFG-form blocks (MVP)");
      return false;
    }
    const sir_val_id_t slot = alloc_slot(c, VK_I32);
    sir_mb_set_src(c->mb, stmt_id, n->loc_line);
    if (!sir_mb_emit_const_i32(c->mb, c->fn, slot, 0)) return false;
    sir_mb_clear_src(c->mb);
    *out_did_return = true;
    *out_exit_slot = slot;
    if (out_exit_kind) *out_exit_kind = VK_I32;
    return true;
  }

  if (strcmp(n->tag, "sem.while") == 0) {
    if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
    const JsonValue* av = json_obj_get(n->fields_obj, "args");
    if (!json_is_array(av) || av->v.arr.len != 2) return false;
    sem_branch_t cond = {0}, body = {0};
    if (!parse_sem_branch(av->v.arr.items[0], &cond) || cond.kind != SEM_BRANCH_THUNK) return false;
    if (!parse_sem_branch(av->v.arr.items[1], &body) || body.kind != SEM_BRANCH_THUNK) return false;

    // Inline loop in sircore bytecode:
    // header:
    //   cond = call cond_thunk()
    //   cbr cond, body_ip, exit_ip
    // body:
    //   call body_thunk()
    //   br header
    // exit:
    const uint32_t header_ip = sir_mb_func_ip(c->mb, c->fn);

    const sir_val_id_t cond_slot = alloc_slot(c, VK_BOOL);
    sir_val_id_t tmp = 0;
    val_kind_t tk = VK_INVALID;
    if (!emit_call_fun_sym(c, stmt_id, cond.node_id, NULL, 0, false, true, cond_slot, &tmp, &tk)) return false;
    if (tk != VK_BOOL) return false;

    uint32_t ip_cbr = 0;
    sir_mb_set_src(c->mb, stmt_id, n->loc_line);
    if (!sir_mb_emit_cbr(c->mb, c->fn, cond_slot, 0, 0, &ip_cbr)) return false;
    const uint32_t body_ip = sir_mb_func_ip(c->mb, c->fn);

    if (!emit_call_fun_sym(c, stmt_id, body.node_id, NULL, 0, true, false, 0, NULL, NULL)) return false;
    if (!sir_mb_emit_br(c->mb, c->fn, header_ip, NULL)) return false;
    const uint32_t exit_ip = sir_mb_func_ip(c->mb, c->fn);
    if (!sir_mb_patch_cbr(c->mb, c->fn, ip_cbr, body_ip, exit_ip)) return false;
    sir_mb_clear_src(c->mb);
    return true;
  }

  if (strcmp(n->tag, "term.trap") == 0) {
    // Deterministic trap: terminate process.
    sir_mb_set_src(c->mb, stmt_id, n->loc_line);
    if (!sir_mb_emit_exit(c->mb, c->fn, 255)) return false;
    sir_mb_clear_src(c->mb);
    *out_did_return = true;
    *out_exit_slot = 0;
    if (out_exit_kind) *out_exit_kind = VK_INVALID;
    return true;
  }

  if (strcmp(n->tag, "term.unreachable") == 0) {
    // Deterministic trap: terminate process.
    sir_mb_set_src(c->mb, stmt_id, n->loc_line);
    if (!sir_mb_emit_exit(c->mb, c->fn, 254)) return false;
    sir_mb_clear_src(c->mb);
    *out_did_return = true;
    *out_exit_slot = 0;
    if (out_exit_kind) *out_exit_kind = VK_INVALID;
    return true;
  }

  if (strcmp(n->tag, "term.ret") == 0 || strcmp(n->tag, "return") == 0) {
    // MVP: return a previously computed value (or default 0).
    uint32_t rid = 0;
    if (n->fields_obj && n->fields_obj->type == JSON_OBJECT) {
      const JsonValue* vv = json_obj_get(n->fields_obj, "value");
      if (vv && !parse_ref_id(vv, &rid)) return false;
    }

    sir_val_id_t slot = 0;
    val_kind_t k = VK_INVALID;
    if (rid) {
      if (!eval_node(c, rid, &slot, &k)) return false;
    } else {
      slot = alloc_slot(c, VK_I32);
      if (!sir_mb_emit_const_i32(c->mb, c->fn, slot, 0)) return false;
      k = VK_I32;
    }
    // Run any pending defers (function-scope + active sem.scope stacks).
    if (!emit_run_defers(c, 0, stmt_id)) return false;
    *out_did_return = true;
    *out_exit_slot = slot;
    if (out_exit_kind) *out_exit_kind = k;
    return true;
  }

  return false;
}

typedef enum term_kind {
  TERM_NONE = 0,
  TERM_RETURN_SLOT,
  TERM_BR,
  TERM_CBR,
  TERM_SWITCH,
  TERM_TRAP,
  TERM_UNREACHABLE,
} term_kind_t;

typedef struct term_info {
  term_kind_t k;
  sir_val_id_t value_slot; // for return
  val_kind_t value_kind;   // for return (when available)
  uint32_t to_block;       // for br
  uint32_t* br_arg_nodes;  // arena-owned; len=br_arg_count
  uint32_t br_arg_count;
  sir_val_id_t cond_slot;  // for cbr
  uint32_t then_block;     // for cbr
  uint32_t else_block;     // for cbr
  uint32_t switch_scrut;   // node id for scrut
  uint32_t* switch_lits;   // arena-owned; node ids; len=switch_case_count
  uint32_t* switch_tos;    // arena-owned; block ids; len=switch_case_count
  uint32_t switch_case_count;
  uint32_t switch_default_to;
  uint32_t trap_code; // optional stable tag (ignored by MVP)
} term_info_t;

static bool lower_term_node(sirj_ctx_t* c, uint32_t term_id, term_info_t* out) {
  if (!c || !out) return false;
  memset(out, 0, sizeof(*out));
  if (term_id >= c->node_cap || !c->nodes[term_id].present) return false;
  const node_info_t* n = &c->nodes[term_id];
  if (!n->tag) return false;

  if (strcmp(n->tag, "term.ret") == 0 || strcmp(n->tag, "return") == 0) {
    uint32_t rid = 0;
    if (n->fields_obj && n->fields_obj->type == JSON_OBJECT) {
      const JsonValue* vv = json_obj_get(n->fields_obj, "value");
      if (vv && !parse_ref_id(vv, &rid)) return false;
    }
    sir_val_id_t slot = 0;
    val_kind_t k = VK_INVALID;
    if (rid) {
      if (!eval_node(c, rid, &slot, &k)) return false;
    } else {
      slot = alloc_slot(c, VK_I32);
      if (!sir_mb_emit_const_i32(c->mb, c->fn, slot, 0)) return false;
      k = VK_I32;
    }
    out->k = TERM_RETURN_SLOT;
    out->value_slot = slot;
    out->value_kind = k;
    return true;
  }

  if (strcmp(n->tag, "term.br") == 0) {
    if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
    const JsonValue* tov = json_obj_get(n->fields_obj, "to");
    uint32_t bid = 0;
    if (!parse_ref_id(tov, &bid)) return false;
    out->k = TERM_BR;
    out->to_block = bid;

    const JsonValue* av = json_obj_get(n->fields_obj, "args");
    if (!av) return true;
    if (!json_is_array(av)) return false;
    const JsonArray* a = &av->v.arr;
    if (a->len == 0) return true;

    uint32_t* arg_nodes = (uint32_t*)arena_alloc(&c->arena, (size_t)a->len * sizeof(uint32_t));
    if (!arg_nodes) return false;
    for (size_t i = 0; i < a->len; i++) {
      uint32_t rid = 0;
      if (!parse_ref_id(a->items[i], &rid)) return false;
      arg_nodes[i] = rid;
    }
    out->br_arg_nodes = arg_nodes;
    out->br_arg_count = (uint32_t)a->len;
    return true;
  }

  if (strcmp(n->tag, "term.cbr") == 0 || strcmp(n->tag, "term.condbr") == 0) {
    if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
    uint32_t cond_id = 0;
    if (!parse_ref_id(json_obj_get(n->fields_obj, "cond"), &cond_id)) return false;
    sir_val_id_t cond_slot = 0;
    val_kind_t ck = VK_INVALID;
    if (!eval_node(c, cond_id, &cond_slot, &ck)) return false;
    if (ck != VK_BOOL) return false;

    const JsonValue* thenv = json_obj_get(n->fields_obj, "then");
    const JsonValue* elsev = json_obj_get(n->fields_obj, "else");
    if (!json_is_object(thenv) || !json_is_object(elsev)) return false;
    const JsonValue* thto = json_obj_get(thenv, "to");
    const JsonValue* elto = json_obj_get(elsev, "to");
    if (!json_is_object(thto) || !json_is_object(elto)) return false;
    uint32_t then_bid = 0, else_bid = 0;
    if (!parse_ref_id(thto, &then_bid)) return false;
    if (!parse_ref_id(elto, &else_bid)) return false;

    out->k = TERM_CBR;
    out->cond_slot = cond_slot;
    out->then_block = then_bid;
    out->else_block = else_bid;
    return true;
  }

  if (strcmp(n->tag, "term.switch") == 0) {
    if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
    uint32_t scrut_id = 0;
    if (!parse_ref_id(json_obj_get(n->fields_obj, "scrut"), &scrut_id)) return false;

    const JsonValue* casesv = json_obj_get(n->fields_obj, "cases");
    if (!json_is_array(casesv)) {
      sirj_diag_setf(c, "sem.parse.term.switch.cases", c->cur_path, n->loc_line, term_id, n->tag, "term.switch.cases must be an array");
      return false;
    }
    const JsonArray* ca = &casesv->v.arr;
    if (ca->len > 64) return false;

    uint32_t* lit_ids = NULL;
    uint32_t* to_ids = NULL;
    if (ca->len) {
      lit_ids = (uint32_t*)arena_alloc(&c->arena, ca->len * sizeof(uint32_t));
      to_ids = (uint32_t*)arena_alloc(&c->arena, ca->len * sizeof(uint32_t));
      if (!lit_ids || !to_ids) return false;
    }

    for (size_t i = 0; i < ca->len; i++) {
      if (!json_is_object(ca->items[i])) {
        sirj_diag_setf(c, "sem.parse.term.switch.case", c->cur_path, n->loc_line, term_id, n->tag, "term.switch.cases[%u] must be an object",
                       (unsigned)i);
        return false;
      }
      const JsonValue* litv = json_obj_get(ca->items[i], "lit");
      const JsonValue* tov = json_obj_get(ca->items[i], "to");
      uint32_t lid = 0, bid = 0;
      if (!parse_ref_id(litv, &lid)) return false;
      if (!parse_ref_id(tov, &bid)) return false;
      lit_ids[i] = lid;
      to_ids[i] = bid;
    }

    const JsonValue* defv = json_obj_get(n->fields_obj, "default");
    if (!json_is_object(defv)) {
      sirj_diag_setf(c, "sem.parse.term.switch.default", c->cur_path, n->loc_line, term_id, n->tag, "term.switch.default must be an object");
      return false;
    }
    const JsonValue* defto = json_obj_get(defv, "to");
    uint32_t def_bid = 0;
    if (!parse_ref_id(defto, &def_bid)) return false;

    out->k = TERM_SWITCH;
    out->switch_scrut = scrut_id;
    out->switch_lits = lit_ids;
    out->switch_tos = to_ids;
    out->switch_case_count = (uint32_t)ca->len;
    out->switch_default_to = def_bid;
    return true;
  }

  if (strcmp(n->tag, "term.trap") == 0) {
    // MVP: ignore msg/code payload; treat as deterministic trap.
    out->k = TERM_TRAP;
    out->trap_code = 0;
    return true;
  }

  if (strcmp(n->tag, "term.unreachable") == 0) {
    out->k = TERM_UNREACHABLE;
    return true;
  }

  sirj_diag_setf(c, "sem.unsupported.term", c->cur_path, n->loc_line, term_id, n->tag, "unsupported terminator tag: %s", n->tag);
  return false;
}

typedef struct patch_rec {
  uint8_t k; // 1=br,2=cbr
  uint32_t ip;
  uint32_t a;
  uint32_t b;
  uint32_t* v; // for switch: case targets blocks (len=n)
  uint32_t n;  // for switch: case count
  uint32_t def;
} patch_rec_t;

static bool parse_const_i32_value(const sirj_ctx_t* c, uint32_t node_id, int32_t* out) {
  if (!c || !out) return false;
  if (node_id >= c->node_cap || !c->nodes[node_id].present) return false;
  const node_info_t* n = &c->nodes[node_id];
  if (!n->tag || strcmp(n->tag, "const.i32") != 0) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  int64_t v = 0;
  if (!json_get_i64(json_obj_get(n->fields_obj, "value"), &v)) return false;
  if (v < INT32_MIN || v > INT32_MAX) return false;
  *out = (int32_t)v;
  return true;
}

static bool lower_fn_body(sirj_ctx_t* c, uint32_t fn_node_id, bool is_entry) {
  if (!c) return false;
  if (fn_node_id >= c->node_cap || !c->nodes[fn_node_id].present) return false;
  const node_info_t* fnn = &c->nodes[fn_node_id];
  if (!fnn->fields_obj || fnn->fields_obj->type != JSON_OBJECT) return false;

  const uint32_t fty = fnn->type_ref;
  bool fn_returns_void = false;
  if (!is_entry && fty && fty < c->type_cap && c->types[fty].present && c->types[fty].is_fn) {
    const uint32_t rt = c->types[fty].ret;
    if (rt == 0) {
      fn_returns_void = true;
    } else if (rt < c->type_cap && c->types[rt].present && !c->types[rt].is_fn && c->types[rt].prim == SIR_PRIM_VOID) {
      fn_returns_void = true;
    }
  }

  // CFG form: entry + blocks.
  const JsonValue* entryv = json_obj_get(fnn->fields_obj, "entry");
  const JsonValue* blocksv = json_obj_get(fnn->fields_obj, "blocks");
  if (entryv && blocksv) {
    c->in_cfg = true;
    uint32_t entry_block = 0;
    if (!parse_ref_id(entryv, &entry_block)) return false;
    if (!json_is_array(blocksv)) return false;
    const JsonArray* blks = &blocksv->v.arr;
    if (blks->len == 0) return false;

    // Build block->ip map (node_id indexed).
    uint32_t* block_ip = (uint32_t*)arena_alloc(&c->arena, (size_t)c->node_cap * sizeof(uint32_t));
    if (!block_ip) return false;
    for (uint32_t i = 0; i < c->node_cap; i++) block_ip[i] = 0xFFFFFFFFu;

    patch_rec_t patches[128];
    uint32_t patch_n = 0;

    // Ensure control enters entry block.
    uint32_t first_bid = 0;
    if (!parse_ref_id(blks->items[0], &first_bid)) return false;
    if (first_bid != entry_block) {
      uint32_t jip = 0;
      if (!sir_mb_emit_br(c->mb, c->fn, 0, &jip)) return false;
      if (patch_n >= (uint32_t)(sizeof(patches) / sizeof(patches[0]))) return false;
      patches[patch_n++] = (patch_rec_t){.k = 1, .ip = jip, .a = entry_block, .b = 0};
    }

    // Emit blocks in declared order.
    for (size_t bi = 0; bi < blks->len; bi++) {
      uint32_t bid = 0;
      if (!parse_ref_id(blks->items[bi], &bid)) return false;
      if (bid >= c->node_cap || !c->nodes[bid].present) return false;
      const node_info_t* bn = &c->nodes[bid];
      if (!bn->tag || strcmp(bn->tag, "block") != 0) return false;
      if (!bn->fields_obj || bn->fields_obj->type != JSON_OBJECT) return false;

      block_ip[bid] = sir_mb_func_ip(c->mb, c->fn);

      const JsonValue* sv = json_obj_get(bn->fields_obj, "stmts");
      if (!json_is_array(sv)) return false;
      const JsonArray* a = &sv->v.arr;

      bool saw_term = false;
      for (size_t si = 0; si < a->len; si++) {
        uint32_t sid = 0;
        if (!parse_ref_id(a->items[si], &sid)) return false;

        term_info_t term = {0};
        if (lower_term_node(c, sid, &term)) {
          saw_term = true;
          if (si + 1 != a->len) return false; // no stmts after terminator (MVP)

          if (sid < c->node_cap && c->nodes[sid].present) {
            sir_mb_set_src(c->mb, sid, c->nodes[sid].loc_line);
          }

          if (term.k == TERM_RETURN_SLOT) {
            if (!emit_run_defers(c, 0, sid)) return false;
            if (is_entry) {
              sir_val_id_t i32_slot = 0;
              if (!coerce_exit_i32(c, sid, term.value_slot, term.value_kind, &i32_slot)) return false;
              if (!sir_mb_emit_exit_val(c->mb, c->fn, i32_slot)) return false;
            } else {
              if (fn_returns_void) {
                if (!sir_mb_emit_ret(c->mb, c->fn)) return false;
              } else {
                if (!sir_mb_emit_ret_val(c->mb, c->fn, term.value_slot)) return false;
              }
            }
          } else if (term.k == TERM_BR) {
            // Resolve block params (bparams) and wire branch args.
            const node_info_t* tobn =
                (term.to_block < c->node_cap && c->nodes[term.to_block].present) ? &c->nodes[term.to_block] : NULL;
            if (!tobn || !tobn->tag || strcmp(tobn->tag, "block") != 0) return false;
            if (!tobn->fields_obj || tobn->fields_obj->type != JSON_OBJECT) return false;

            const JsonValue* pv = json_obj_get(tobn->fields_obj, "params");
            uint32_t dst_count = 0;
            sir_val_id_t* dst_slots = NULL;
            if (pv) {
              if (!json_is_array(pv)) return false;
              const JsonArray* pa = &pv->v.arr;
              dst_count = (uint32_t)pa->len;
              if (dst_count) {
                dst_slots = (sir_val_id_t*)arena_alloc(&c->arena, (size_t)dst_count * sizeof(sir_val_id_t));
                if (!dst_slots) return false;
                for (uint32_t pi = 0; pi < dst_count; pi++) {
                  uint32_t bpid = 0;
                  if (!parse_ref_id(pa->items[pi], &bpid)) return false;
                  if (bpid >= c->node_cap || !c->nodes[bpid].present) return false;
                  if (!c->nodes[bpid].tag || strcmp(c->nodes[bpid].tag, "bparam") != 0) return false;
                  sir_val_id_t s = 0;
                  val_kind_t k = VK_INVALID;
                  if (!eval_bparam(c, bpid, &c->nodes[bpid], &s, &k)) return false;
                  (void)k;
                  dst_slots[pi] = s;
                }
              }
            }

            if (dst_count != term.br_arg_count) {
              sirj_diag_setf(c, "sem.cfg.br.args_mismatch", c->cur_path, bn->loc_line, sid, "term.br",
                             "term.br args count mismatch: expected %u (target block params) got %u", (unsigned)dst_count,
                             (unsigned)term.br_arg_count);
              return false;
            }
            sir_val_id_t* src_slots = NULL;
            if (dst_count) {
              if (!term.br_arg_nodes) return false;
              src_slots = (sir_val_id_t*)arena_alloc(&c->arena, (size_t)dst_count * sizeof(sir_val_id_t));
              if (!src_slots) return false;
              for (uint32_t ai = 0; ai < dst_count; ai++) {
                sir_val_id_t s = 0;
                val_kind_t k = VK_INVALID;
                if (!eval_node(c, term.br_arg_nodes[ai], &s, &k)) return false;
                (void)k;
                src_slots[ai] = s;
              }
            }

            uint32_t ip = 0;
            if (sid < c->node_cap && c->nodes[sid].present) {
              sir_mb_set_src(c->mb, sid, c->nodes[sid].loc_line);
            }
            if (!sir_mb_emit_br_args(c->mb, c->fn, 0, src_slots, dst_slots, dst_count, &ip)) return false;
            if (patch_n >= (uint32_t)(sizeof(patches) / sizeof(patches[0]))) return false;
            patches[patch_n++] = (patch_rec_t){.k = 1, .ip = ip, .a = term.to_block, .b = 0};
          } else if (term.k == TERM_CBR) {
            uint32_t ip = 0;
            if (!sir_mb_emit_cbr(c->mb, c->fn, term.cond_slot, 0, 0, &ip)) return false;
            if (patch_n >= (uint32_t)(sizeof(patches) / sizeof(patches[0]))) return false;
            patches[patch_n++] = (patch_rec_t){.k = 2, .ip = ip, .a = term.then_block, .b = term.else_block};
          } else if (term.k == TERM_SWITCH) {
            sir_val_id_t scrut_slot = 0;
            val_kind_t sk = VK_INVALID;
            if (!eval_node(c, term.switch_scrut, &scrut_slot, &sk)) return false;
            if (sk != VK_I32) return false; // MVP

            const uint32_t ncase = term.switch_case_count;
            int32_t* case_lits = NULL;
            uint32_t* case_ip0 = NULL;
            if (ncase) {
              case_lits = (int32_t*)arena_alloc(&c->arena, (size_t)ncase * sizeof(int32_t));
              case_ip0 = (uint32_t*)arena_alloc(&c->arena, (size_t)ncase * sizeof(uint32_t));
              if (!case_lits || !case_ip0) return false;
              for (uint32_t ci = 0; ci < ncase; ci++) {
                if (!parse_const_i32_value(c, term.switch_lits[ci], &case_lits[ci])) {
                  const uint32_t lit_node = term.switch_lits[ci];
                  const node_info_t* ln =
                      (lit_node < c->node_cap && c->nodes[lit_node].present) ? &c->nodes[lit_node] : NULL;
                  sirj_diag_setf(c, "sem.cfg.switch.case_lit", c->cur_path, ln ? ln->loc_line : bn->loc_line, lit_node,
                                 ln ? ln->tag : "?", "term.switch case literal must be const.i32");
                  return false;
                }
                case_ip0[ci] = 0;
              }
            }

            uint32_t ip = 0;
            if (sid < c->node_cap && c->nodes[sid].present) {
              sir_mb_set_src(c->mb, sid, c->nodes[sid].loc_line);
            }
            if (!sir_mb_emit_switch(c->mb, c->fn, scrut_slot, case_lits, case_ip0, ncase, 0, &ip)) return false;
            if (patch_n >= (uint32_t)(sizeof(patches) / sizeof(patches[0]))) return false;
            patches[patch_n++] =
                (patch_rec_t){.k = 3, .ip = ip, .v = term.switch_tos, .n = ncase, .def = term.switch_default_to};
          } else if (term.k == TERM_TRAP) {
            // Deterministic trap: SEM returns a stable non-zero exit code.
            if (!sir_mb_emit_exit(c->mb, c->fn, 255)) return false;
          } else if (term.k == TERM_UNREACHABLE) {
            // Unreachable is also a deterministic trap.
            if (!sir_mb_emit_exit(c->mb, c->fn, 254)) return false;
          } else {
            return false;
          }
        } else {
          if (saw_term) return false;
          bool did_ret = false;
          sir_val_id_t exit_slot = 0;
          val_kind_t exit_kind = VK_INVALID;
          if (!exec_stmt(c, sid, &did_ret, &exit_slot, &exit_kind)) return false;
          if (did_ret) return false;
        }
      }

      if (!saw_term) return false;
    }

    // Patch branch targets to block start IPs.
    for (uint32_t i = 0; i < patch_n; i++) {
      if (patches[i].k == 1) {
        const uint32_t to = patches[i].a;
        if (to >= c->node_cap || block_ip[to] == 0xFFFFFFFFu) return false;
        if (!sir_mb_patch_br(c->mb, c->fn, patches[i].ip, block_ip[to])) return false;
      } else if (patches[i].k == 2) {
        const uint32_t th = patches[i].a;
        const uint32_t el = patches[i].b;
        if (th >= c->node_cap || el >= c->node_cap) return false;
        if (block_ip[th] == 0xFFFFFFFFu || block_ip[el] == 0xFFFFFFFFu) return false;
        if (!sir_mb_patch_cbr(c->mb, c->fn, patches[i].ip, block_ip[th], block_ip[el])) return false;
      } else if (patches[i].k == 3) {
        if (!patches[i].v && patches[i].n) return false;
        if (patches[i].def >= c->node_cap || block_ip[patches[i].def] == 0xFFFFFFFFu) return false;

        uint32_t tmp_small[16];
        uint32_t* tmp = tmp_small;
        if (patches[i].n > (uint32_t)(sizeof(tmp_small) / sizeof(tmp_small[0]))) {
          tmp = (uint32_t*)malloc((size_t)patches[i].n * sizeof(uint32_t));
          if (!tmp) return false;
        }

        for (uint32_t ci = 0; ci < patches[i].n; ci++) {
          const uint32_t bid = patches[i].v[ci];
          if (bid >= c->node_cap || block_ip[bid] == 0xFFFFFFFFu) {
            if (tmp != tmp_small) free(tmp);
            return false;
          }
          tmp[ci] = block_ip[bid];
        }

        const bool ok = sir_mb_patch_switch(c->mb, c->fn, patches[i].ip, tmp, patches[i].n, block_ip[patches[i].def]);
        if (tmp != tmp_small) free(tmp);
        if (!ok) return false;
      }
    }

    c->in_cfg = false;
    return true;
  }

  // Legacy single-block form: body.
  const JsonValue* bodyv = json_obj_get(fnn->fields_obj, "body");
  if (!bodyv) return false;
  uint32_t body_id = 0;
  if (!parse_ref_id(bodyv, &body_id)) return false;
  if (body_id >= c->node_cap || !c->nodes[body_id].present) return false;
  const node_info_t* bn = &c->nodes[body_id];
  if (!bn->tag || strcmp(bn->tag, "block") != 0) return false;
  if (!bn->fields_obj || bn->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* sv = json_obj_get(bn->fields_obj, "stmts");
  if (!json_is_array(sv)) return false;

  const JsonArray* a = &sv->v.arr;
  for (size_t i = 0; i < a->len; i++) {
    uint32_t sid = 0;
    if (!parse_ref_id(a->items[i], &sid)) return false;
    bool did_ret = false;
    sir_val_id_t exit_slot = 0;
    val_kind_t exit_kind = VK_INVALID;
    if (!exec_stmt(c, sid, &did_ret, &exit_slot, &exit_kind)) return false;
    if (did_ret) {
      if (exit_slot == 0 && exit_kind == VK_INVALID) {
        // exec_stmt already emitted a terminator (e.g. term.trap) for this function.
        return true;
      }
      if (is_entry) {
        sir_val_id_t i32_slot = 0;
        if (!coerce_exit_i32(c, sid, exit_slot, exit_kind, &i32_slot)) return false;
        if (!sir_mb_emit_exit_val(c->mb, c->fn, i32_slot)) return false;
      } else {
        if (fn_returns_void) {
          if (!sir_mb_emit_ret(c->mb, c->fn)) return false;
        } else {
          if (!sir_mb_emit_ret_val(c->mb, c->fn, exit_slot)) return false;
        }
      }
      return true;
    }
  }

  // Implicit return 0.
  const sir_val_id_t slot = alloc_slot(c, VK_I32);
  if (!sir_mb_emit_const_i32(c->mb, c->fn, slot, 0)) return false;
  if (!emit_run_defers(c, 0, fn_node_id)) return false;
  if (is_entry) {
    if (!sir_mb_emit_exit_val(c->mb, c->fn, slot)) return false;
  } else {
    if (fn_returns_void) {
      if (!sir_mb_emit_ret(c->mb, c->fn)) return false;
    } else {
      if (!sir_mb_emit_ret_val(c->mb, c->fn, slot)) return false;
    }
  }
  return true;
}

static uint32_t loc_line_from_root(const JsonValue* root, uint32_t fallback) {
  if (!root || root->type != JSON_OBJECT) return fallback;
  const JsonValue* locv = json_obj_get((JsonValue*)root, "loc");
  if (!locv || locv->type != JSON_OBJECT) return fallback;
  uint32_t ln = 0;
  if (!json_get_u32(json_obj_get((JsonValue*)locv, "line"), &ln)) return fallback;
  return ln ? ln : fallback;
}

static bool parse_file(sirj_ctx_t* c, const char* path) {
  if (!c || !path) return false;
  FILE* f = fopen(path, "rb");
  if (!f) return false;

  char* line = NULL;
  size_t cap = 0;
  size_t len = 0;
  int ch = 0;
  uint32_t rec_no = 0;

  const char* diag_path = path;

  // Minimal line reader (no reliance on POSIX getline).
  for (;;) {
    ch = fgetc(f);
    if (ch == EOF) {
      if (len == 0) break;
      ch = '\n';
    }
    if (cap < len + 2) {
      const size_t ncap = cap ? cap * 2 : 4096;
      char* np = (char*)realloc(line, ncap);
      if (!np) {
        free(line);
        fclose(f);
        return false;
      }
      line = np;
      cap = ncap;
    }
    line[len++] = (char)ch;
    if (ch == '\n') {
      line[len] = '\0';
      // skip empty/whitespace lines
      const char* p = line;
      while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
      if (*p == '\0') {
        len = 0;
        continue;
      }

      rec_no++;
      JsonValue* root = NULL;
      JsonError err = {0};
      if (!json_parse(&c->arena, line, &root, &err) || !root) {
        sirj_diag_setf(c, "sem.parse.json", diag_path, rec_no, 0, NULL, "json parse error at offset %u: %s", (unsigned)err.offset,
                       err.msg ? err.msg : "error");
        free(line);
        fclose(f);
        return false;
      }
      if (!json_is_object(root)) {
        sirj_diag_setf(c, "sem.parse.record", diag_path, rec_no, 0, NULL, "record is not an object");
        free(line);
        fclose(f);
        return false;
      }

      const char* k = json_get_string(json_obj_get(root, "k"));
      if (!k) {
        len = 0;
        if (ch == EOF) break;
        continue;
      }

      if (strcmp(k, "type") == 0) {
        const uint32_t loc_line = loc_line_from_root(root, rec_no);
        uint32_t id = 0;
        if (!json_get_u32(json_obj_get(root, "id"), &id)) {
          sirj_diag_setf(c, "sem.parse.type.id", diag_path, loc_line, 0, NULL, "type.id missing/invalid");
          free(line);
          fclose(f);
          return false;
        }
        if (!ensure_type_cap(c, id)) {
          sirj_diag_setf(c, "sem.oom", diag_path, loc_line, 0, NULL, "out of memory");
          free(line);
          fclose(f);
          return false;
        }
        const char* kind = json_get_string(json_obj_get(root, "kind"));
        if (!kind) {
          sirj_diag_setf(c, "sem.parse.type.kind", diag_path, loc_line, 0, NULL, "type.kind missing");
          free(line);
          fclose(f);
          return false;
        }

        type_info_t ti = {0};
        ti.present = true;
        ti.loc_line = loc_line;
        if (strcmp(kind, "prim") == 0) {
          const char* prim = json_get_string(json_obj_get(root, "prim"));
          ti.prim = prim_from_string(prim);
          if (ti.prim == SIR_PRIM_INVALID) {
            sirj_diag_setf(c, "sem.unsupported.prim", diag_path, loc_line, 0, NULL, "unsupported prim: %s", prim ? prim : "(null)");
            free(line);
            fclose(f);
            return false;
          }
        } else if (strcmp(kind, "fn") == 0) {
          ti.is_fn = true;
          const JsonValue* pv = obj_req(root, "params");
          if (!parse_u32_array(pv, &ti.params, &ti.param_count, &c->arena)) {
            sirj_diag_setf(c, "sem.parse.type.fn.params", diag_path, loc_line, 0, NULL, "bad fn params array");
            free(line);
            fclose(f);
            return false;
          }
          if (!json_get_u32(json_obj_get(root, "ret"), &ti.ret)) {
            sirj_diag_setf(c, "sem.parse.type.fn.ret", diag_path, loc_line, 0, NULL, "bad fn ret");
            free(line);
            fclose(f);
            return false;
          }
        } else if (strcmp(kind, "fun") == 0) {
          ti.is_fun = true;
          uint32_t sig = 0;
          if (!json_get_u32(json_obj_get(root, "sig"), &sig)) {
            sirj_diag_setf(c, "sem.parse.type.fun.sig", diag_path, loc_line, 0, NULL, "bad fun.sig");
            free(line);
            fclose(f);
            return false;
          }
          ti.fun_sig = sig;
        } else if (strcmp(kind, "array") == 0) {
          ti.is_array = true;
          if (!json_get_u32(json_obj_get(root, "of"), &ti.array_of)) {
            sirj_diag_setf(c, "sem.parse.type.array.of", diag_path, loc_line, 0, NULL, "bad array.of");
            free(line);
            fclose(f);
            return false;
          }
          if (!json_get_u32(json_obj_get(root, "len"), &ti.array_len)) {
            sirj_diag_setf(c, "sem.parse.type.array.len", diag_path, loc_line, 0, NULL, "bad array.len");
            free(line);
            fclose(f);
            return false;
          }
        } else if (strcmp(kind, "ptr") == 0) {
          ti.is_ptr = true;
          ti.prim = SIR_PRIM_PTR;
          (void)json_get_u32(json_obj_get(root, "of"), &ti.ptr_of);
        } else if (strcmp(kind, "struct") == 0) {
          ti.is_struct = true;
          const JsonValue* fv = json_obj_get(root, "fields");
          if (!json_is_array(fv)) {
            sirj_diag_setf(c, "sem.parse.type.struct.fields", diag_path, loc_line, 0, NULL, "bad struct.fields array");
            free(line);
            fclose(f);
            return false;
          }
          const JsonArray* fa = &fv->v.arr;
          const uint32_t nfield = (uint32_t)fa->len;
          if (nfield != fa->len) {
            sirj_diag_setf(c, "sem.parse.type.struct.fields", diag_path, loc_line, 0, NULL, "struct.fields too large");
            free(line);
            fclose(f);
            return false;
          }
          ti.struct_field_count = nfield;
          if (nfield) {
            ti.struct_fields = (uint32_t*)arena_alloc(&c->arena, (size_t)nfield * sizeof(uint32_t));
            ti.struct_field_align = (uint32_t*)arena_alloc(&c->arena, (size_t)nfield * sizeof(uint32_t));
            if (!ti.struct_fields || !ti.struct_field_align) {
              sirj_diag_setf(c, "sem.oom", diag_path, loc_line, 0, NULL, "out of memory");
              free(line);
              fclose(f);
              return false;
            }
          }
          for (uint32_t fi = 0; fi < nfield; fi++) {
            const JsonValue* fobj = fa->items[fi];
            if (!json_is_object(fobj)) {
              sirj_diag_setf(c, "sem.parse.type.struct.field", diag_path, loc_line, 0, NULL, "struct field must be an object");
              free(line);
              fclose(f);
              return false;
            }
            uint32_t ty = 0;
            if (!json_get_u32(json_obj_get(fobj, "type_ref"), &ty)) {
              const JsonValue* tyv = json_obj_get(fobj, "ty");
              if (!parse_ref_id(tyv, &ty)) {
                sirj_diag_setf(c, "sem.parse.type.struct.field", diag_path, loc_line, 0, NULL, "struct field missing/invalid type_ref");
                free(line);
                fclose(f);
                return false;
              }
            }
            if (ty == 0) {
              sirj_diag_setf(c, "sem.parse.type.struct.field", diag_path, loc_line, 0, NULL, "struct field type_ref must be non-zero");
              free(line);
              fclose(f);
              return false;
            }
            ti.struct_fields[fi] = ty;

            uint32_t falign = 0;
            const JsonValue* av = json_obj_get(fobj, "align");
            if (av) {
              if (!json_get_u32(av, &falign) || falign == 0 || !is_pow2_u32(falign)) {
                sirj_diag_setf(c, "sem.parse.type.struct.field.align", diag_path, loc_line, 0, NULL,
                               "struct field align must be a positive power of two");
                free(line);
                fclose(f);
                return false;
              }
            }
            ti.struct_field_align[fi] = falign;
          }

          bool packed = false;
          const JsonValue* pv = json_obj_get(root, "packed");
          if (pv) {
            if (!json_get_bool(pv, &packed)) {
              sirj_diag_setf(c, "sem.parse.type.struct.packed", diag_path, loc_line, 0, NULL, "struct.packed must be boolean");
              free(line);
              fclose(f);
              return false;
            }
          }
          ti.struct_packed = packed;

          uint32_t salign = 0;
          const JsonValue* av = json_obj_get(root, "align");
          if (av) {
            if (!json_get_u32(av, &salign) || salign == 0 || !is_pow2_u32(salign)) {
              sirj_diag_setf(c, "sem.parse.type.struct.align", diag_path, loc_line, 0, NULL, "struct.align must be a positive power of two");
              free(line);
              fclose(f);
              return false;
            }
          }
          ti.struct_align_override = salign;
        } else {
          // ignore other kinds for now
          memset(&ti, 0, sizeof(ti));
          ti.present = true;
          ti.loc_line = loc_line;
        }
        c->types[id] = ti;
      } else if (strcmp(k, "sym") == 0) {
        const uint32_t loc_line = loc_line_from_root(root, rec_no);
        uint32_t id = 0;
        if (!json_get_u32(json_obj_get(root, "id"), &id)) {
          sirj_diag_setf(c, "sem.parse.sym.id", diag_path, loc_line, 0, NULL, "sym.id missing/invalid");
          free(line);
          fclose(f);
          return false;
        }
        if (!ensure_symrec_cap(c, id)) {
          sirj_diag_setf(c, "sem.oom", diag_path, loc_line, id, NULL, "out of memory");
          free(line);
          fclose(f);
          return false;
        }

        sym_info_t si = {0};
        si.present = true;
        si.loc_line = loc_line;
        si.name = json_get_string(json_obj_get(root, "name"));
        si.kind = json_get_string(json_obj_get(root, "kind"));
        (void)json_get_u32(json_obj_get(root, "type_ref"), &si.type_ref);
        si.init_kind = SYM_INIT_NONE;

        const JsonValue* vv = json_obj_get(root, "value");
        if (vv && vv->type == JSON_OBJECT) {
          const char* t = json_get_string(json_obj_get(vv, "t"));
          if (t && strcmp(t, "num") == 0) {
            int64_t v = 0;
            if (!json_get_i64(json_obj_get(vv, "v"), &v)) {
              sirj_diag_setf(c, "sem.parse.sym.value", diag_path, loc_line, id, "sym", "sym.value num missing/invalid");
              free(line);
              fclose(f);
              return false;
            }
            si.init_kind = SYM_INIT_NUM;
            si.init_num = v;
          } else if (t && strcmp(t, "ref") == 0) {
            uint32_t rid = 0;
            if (!parse_ref_id(vv, &rid)) {
              sirj_diag_setf(c, "sem.parse.sym.value", diag_path, loc_line, id, "sym", "sym.value ref missing/invalid");
              free(line);
              fclose(f);
              return false;
            }
            si.init_kind = SYM_INIT_NODE;
            si.init_node = rid;
          }
        }

        c->syms[id] = si;
      } else if (strcmp(k, "node") == 0) {
        const uint32_t loc_line = loc_line_from_root(root, rec_no);
        uint32_t id = 0;
        if (!json_get_u32(json_obj_get(root, "id"), &id)) {
          sirj_diag_setf(c, "sem.parse.node.id", diag_path, loc_line, 0, NULL, "node.id missing/invalid");
          free(line);
          fclose(f);
          return false;
        }
        if (!ensure_node_cap(c, id)) {
          sirj_diag_setf(c, "sem.oom", diag_path, loc_line, id, NULL, "out of memory");
          free(line);
          fclose(f);
          return false;
        }
        node_info_t ni = {0};
        ni.present = true;
        ni.tag = json_get_string(json_obj_get(root, "tag"));
        (void)json_get_u32(json_obj_get(root, "type_ref"), &ni.type_ref);
        const JsonValue* fv = json_obj_get(root, "fields");
        if (fv && json_is_object(fv)) ni.fields_obj = (JsonValue*)fv;
        ni.loc_line = loc_line;
        c->nodes[id] = ni;
      }

      len = 0;
      if (ch == EOF) break;
    }
  }

  free(line);
  fclose(f);
  return true;
}

static bool find_entry_fn(const sirj_ctx_t* c, uint32_t* out_fn_node_id) {
  if (!c || !out_fn_node_id) return false;
  uint32_t best = 0;
  for (uint32_t i = 0; i < c->node_cap; i++) {
    if (!c->nodes[i].present) continue;
    if (!c->nodes[i].tag || strcmp(c->nodes[i].tag, "fn") != 0) continue;
    const JsonValue* fo = c->nodes[i].fields_obj;
    if (!fo || fo->type != JSON_OBJECT) continue;
    const char* nm = json_get_string(json_obj_get(fo, "name"));
    if (!nm) continue;
    if (strcmp(nm, "zir_main") == 0) {
      *out_fn_node_id = i;
      return true;
    }
    if (!best && strcmp(nm, "main") == 0) best = i;
  }
  if (best) {
    *out_fn_node_id = best;
    return true;
  }
  return false;
}

static bool build_fn_sig(sirj_ctx_t* c, uint32_t fn_type_id, sir_sig_t* out_sig) {
  if (!c || !out_sig) return false;
  if (fn_type_id == 0 || fn_type_id >= c->type_cap) return false;
  const type_info_t* ti = &c->types[fn_type_id];
  if (!ti->present || !ti->is_fn) return false;
  if (!ensure_prim_types(c)) return false;

  if (ti->param_count > 16) return false;
  sir_type_id_t* params = NULL;
  if (ti->param_count) {
    params = (sir_type_id_t*)arena_alloc(&c->arena, (size_t)ti->param_count * sizeof(sir_type_id_t));
    if (!params) return false;
  }
  for (uint32_t i = 0; i < ti->param_count; i++) {
    const uint32_t pid = ti->params[i];
    if (pid == 0 || pid >= c->type_cap) return false;
    const type_info_t* pt = &c->types[pid];
    if (!pt->present || pt->is_fn) return false;
    const sir_type_id_t mt = mod_ty_for_prim(c, pt->prim);
    if (!mt) return false;
    params[i] = mt;
  }

  sir_type_id_t* results = NULL;
  uint32_t result_count = 0;
  if (ti->ret) {
    const uint32_t rid = ti->ret;
    if (rid == 0 || rid >= c->type_cap) return false;
    const type_info_t* rt = &c->types[rid];
    if (!rt->present || rt->is_fn) return false;
    if (rt->prim != SIR_PRIM_VOID) {
      const sir_type_id_t mt = mod_ty_for_prim(c, rt->prim);
      if (!mt) return false;
      results = (sir_type_id_t*)arena_alloc(&c->arena, sizeof(sir_type_id_t));
      if (!results) return false;
      results[0] = mt;
      result_count = 1;
    }
  }

  out_sig->params = params;
  out_sig->param_count = ti->param_count;
  out_sig->results = results;
  out_sig->result_count = result_count;
  return true;
}

static bool init_params_for_fn(sirj_ctx_t* c, uint32_t fn_node_id, uint32_t fn_type_id) {
  if (!c) return false;
  c->param_count = 0;
  c->next_slot = 0;
  reset_value_cache(c);
  c->defer_count = 0;
  c->cached_true_slot = 0;
  c->cached_false_slot = 0;
  c->in_cfg = false;

  if (fn_node_id >= c->node_cap || !c->nodes[fn_node_id].present) return false;
  const node_info_t* fnn = &c->nodes[fn_node_id];
  if (!fnn->fields_obj || fnn->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* pv = json_obj_get(fnn->fields_obj, "params");
  if (!pv) {
    // no params
    return true;
  }
  if (!json_is_array(pv)) return false;

  const type_info_t* ti = (fn_type_id < c->type_cap) ? &c->types[fn_type_id] : NULL;
  const uint32_t expected_n = (ti && ti->present && ti->is_fn) ? ti->param_count : 0;
  if (pv->v.arr.len != expected_n) return false;
  if (expected_n > (uint32_t)(sizeof(c->params) / sizeof(c->params[0]))) return false;

  for (uint32_t i = 0; i < expected_n; i++) {
    uint32_t pid = 0;
    if (!parse_ref_id(pv->v.arr.items[i], &pid)) return false;
    if (pid >= c->node_cap || !c->nodes[pid].present) return false;
    const node_info_t* pn = &c->nodes[pid];
    if (!pn->fields_obj || pn->fields_obj->type != JSON_OBJECT) return false;
    const char* nm = json_get_string(json_obj_get(pn->fields_obj, "name"));
    if (!nm) return false;

    val_kind_t k = VK_INVALID;
    const uint32_t param_type_id = ti->params[i];
    if (param_type_id == 0 || param_type_id >= c->type_cap) return false;
    const type_info_t* pt = &c->types[param_type_id];
    if (!pt->present || pt->is_fn) return false;
    if (pt->prim == SIR_PRIM_I1) k = VK_I1;
    else if (pt->prim == SIR_PRIM_I8) k = VK_I8;
    else if (pt->prim == SIR_PRIM_I16) k = VK_I16;
    else if (pt->prim == SIR_PRIM_I32) k = VK_I32;
    else if (pt->prim == SIR_PRIM_I64) k = VK_I64;
    else if (pt->prim == SIR_PRIM_PTR) k = VK_PTR;
    else if (pt->prim == SIR_PRIM_BOOL) k = VK_BOOL;
    else if (pt->prim == SIR_PRIM_F32) k = VK_F32;
    else if (pt->prim == SIR_PRIM_F64) k = VK_F64;
    else return false;

    c->params[i].name = nm;
    c->params[i].slot = i;
    c->params[i].kind = k;
    c->param_count++;
    c->next_slot = i + 1;
  }

  return true;
}

static bool lower_globals(sirj_ctx_t* c) {
  if (!c || !c->mb) return false;
  if (!c->syms || c->symrec_cap == 0) return true;

  for (uint32_t i = 0; i < c->symrec_cap; i++) {
    sym_info_t* s = &c->syms[i];
    if (!s->present) continue;
    if (!s->name || s->name[0] == '\0') continue;
    if (!s->kind) continue;
    if (strcmp(s->kind, "var") != 0 && strcmp(s->kind, "const") != 0) continue;
    if (s->type_ref == 0) {
      sirj_diag_setf(c, "sem.global.missing_type", c->cur_path, s->loc_line, i, "sym", "global sym %s missing type_ref", s->name);
      return false;
    }

    uint32_t size = 0, align = 1;
    if (!type_layout(c, s->type_ref, &size, &align)) {
      sirj_diag_setf(c, "sem.global.bad_type", c->cur_path, s->loc_line, i, "sym", "unsupported global type_ref=%u for %s",
                     (unsigned)s->type_ref, s->name);
      return false;
    }

    uint8_t* init_bytes = NULL;
    uint32_t init_len = 0;
    if (s->init_kind == SYM_INIT_NUM) {
      // Only support numeric init for primitive globals in MVP.
      if (s->type_ref >= c->type_cap || !c->types[s->type_ref].present || c->types[s->type_ref].is_fn || c->types[s->type_ref].is_array) {
        sirj_diag_setf(c, "sem.global.init_num_type", c->cur_path, s->loc_line, i, "sym", "numeric init only supported for primitive globals");
        return false;
      }
      init_bytes = (uint8_t*)arena_alloc(&c->arena, size);
      if (!init_bytes) return false;
      memset(init_bytes, 0, size);
      const sir_prim_type_t p = c->types[s->type_ref].prim;
      if (p == SIR_PRIM_I8) {
        init_bytes[0] = (uint8_t)s->init_num;
      } else if (p == SIR_PRIM_I16) {
        const uint16_t x = (uint16_t)s->init_num;
        memcpy(init_bytes, &x, 2);
      } else if (p == SIR_PRIM_I32) {
        const int32_t x = (int32_t)s->init_num;
        memcpy(init_bytes, &x, 4);
      } else if (p == SIR_PRIM_I64) {
        const int64_t x = (int64_t)s->init_num;
        memcpy(init_bytes, &x, 8);
      } else if (p == SIR_PRIM_PTR) {
        const zi_ptr_t x = (zi_ptr_t)s->init_num;
        memcpy(init_bytes, &x, sizeof(x));
      } else if (p == SIR_PRIM_BOOL) {
        init_bytes[0] = (s->init_num != 0) ? 1 : 0;
      } else {
        return false;
      }
      init_len = size;
    } else if (s->init_kind == SYM_INIT_NODE) {
      if (!build_const_bytes(c, s->init_node, s->type_ref, &init_bytes, &init_len)) {
        sirj_diag_setf(c, "sem.global.init_const", c->cur_path, s->loc_line, i, "sym", "unsupported global initializer for %s", s->name);
        return false;
      }
    } else {
      // zero-init
      init_bytes = NULL;
      init_len = 0;
    }

    const sir_global_id_t gid = sir_mb_global(c->mb, s->name, size, align, init_bytes, init_len);
    if (!gid) {
      sirj_diag_setf(c, "sem.oom", c->cur_path, s->loc_line, i, "sym", "out of memory");
      return false;
    }
    s->gid = gid;
  }

  return true;
}

static const char* sem_zi_err_name(int32_t rc) {
  switch (rc) {
    case -1:
      return "ZI_E_INVALID";
    case -2:
      return "ZI_E_BOUNDS";
    case -3:
      return "ZI_E_NOENT";
    case -4:
      return "ZI_E_DENIED";
    case -5:
      return "ZI_E_CLOSED";
    case -6:
      return "ZI_E_AGAIN";
    case -7:
      return "ZI_E_NOSYS";
    case -8:
      return "ZI_E_OOM";
    case -9:
      return "ZI_E_IO";
    case -10:
      return "ZI_E_INTERNAL";
    default:
      return "ZI_E_UNKNOWN";
  }
}

static int sem_run_or_verify_sir_jsonl_impl(const char* path, const sem_cap_t* caps, uint32_t cap_count, const char* fs_root,
                                           sem_diag_format_t diag_format, bool diag_all, bool do_run, int* out_prog_rc,
                                           const sir_exec_event_sink_t* sink, void (*post_run)(void* user, const sir_module_t* m, int32_t exec_rc),
                                           void* post_user) {
  if (!path) return 2;

  sirj_ctx_t c;
  memset(&c, 0, sizeof(c));
  arena_init(&c.arena);
  c.diag_format = diag_format;
  c.cur_path = path;
  c.diag_all = diag_all;

  if (!parse_file(&c, path)) {
    if (!c.diag.set) sirj_diag_setf(&c, "sem.parse", path, 0, 0, NULL, "failed to parse: %s", path);
    sem_print_diag(&c);
    ctx_dispose(&c);
    return 1;
  }

  uint32_t entry_fn_node_id = 0;
  if (!find_entry_fn(&c, &entry_fn_node_id)) {
    sirj_diag_setf(&c, "sem.no_entry_fn", path, 0, 0, NULL, "no entry fn (expected fn name zir_main or main)");
    sem_print_diag(&c);
    ctx_dispose(&c);
    return 1;
  }

  c.mb = sir_mb_new();
  if (!c.mb) {
    sirj_diag_setf(&c, "sem.oom", path, 0, 0, NULL, "out of memory");
    sem_print_diag(&c);
    ctx_dispose(&c);
    return 1;
  }
  if (!ensure_prim_types(&c)) {
    sirj_diag_setf(&c, "sem.oom", path, 0, 0, NULL, "out of memory");
    sem_print_diag(&c);
    ctx_dispose(&c);
    return 1;
  }

  if (!lower_globals(&c)) {
    if (!c.diag.set) sirj_diag_setf(&c, "sem.global", path, 0, 0, NULL, "failed to lower globals");
    sem_print_diag(&c);
    ctx_dispose(&c);
    return 1;
  }

  // Create module funcs for all SIR fn nodes so ptr.sym can resolve them.
  uint32_t entry_fid = 0;
  for (uint32_t i = 0; i < c.node_cap; i++) {
    if (!c.nodes[i].present) continue;
    if (!c.nodes[i].tag || strcmp(c.nodes[i].tag, "fn") != 0) continue;
    if (!c.nodes[i].fields_obj || c.nodes[i].fields_obj->type != JSON_OBJECT) continue;
    const char* nm = json_get_string(json_obj_get(c.nodes[i].fields_obj, "name"));
    if (!nm) continue;
    const sir_func_id_t fid = sir_mb_func_begin(c.mb, nm);
    if (!fid) {
      sirj_diag_setf(&c, "sem.oom", path, c.nodes[i].loc_line, i, "fn", "out of memory");
      sem_print_diag(&c);
      ctx_dispose(&c);
      return 1;
    }
    c.func_by_node[i] = fid;

    uint32_t fty = c.nodes[i].type_ref;
    sir_sig_t sig = {0};
    if (fty && build_fn_sig(&c, fty, &sig)) {
      if (i == entry_fn_node_id) {
        // `sir_module_run` executes the entry function as a process, not as a callable,
        // so it does not accept a return-value contract. Entry should EXIT/EXIT_VAL.
        sig.results = NULL;
        sig.result_count = 0;
      }
      if (!sir_mb_func_set_sig(c.mb, fid, sig)) {
        sirj_diag_setf(&c, "sem.oom", path, c.nodes[i].loc_line, i, "fn", "out of memory");
        sem_print_diag(&c);
        ctx_dispose(&c);
        return 1;
      }
    }

    if (i == entry_fn_node_id) entry_fid = fid;
  }
  if (!entry_fid) {
    sirj_diag_setf(&c, "sem.internal", path, 0, 0, NULL, "failed to map entry function");
    sem_print_diag(&c);
    ctx_dispose(&c);
    return 1;
  }
  if (!sir_mb_func_set_entry(c.mb, entry_fid)) {
    sirj_diag_setf(&c, "sem.internal", path, 0, 0, NULL, "failed to init module func");
    sem_print_diag(&c);
    ctx_dispose(&c);
    return 1;
  }

  // Lower each function body.
  for (uint32_t i = 0; i < c.node_cap; i++) {
    const sir_func_id_t fid = (i < c.func_by_node_cap) ? c.func_by_node[i] : 0;
    if (!fid) continue;
    const node_info_t* fnn = &c.nodes[i];
    if (!fnn->fields_obj || fnn->fields_obj->type != JSON_OBJECT) {
      sirj_diag_setf(&c, "sem.internal", path, fnn->loc_line, i, "fn", "fn fields malformed");
      sem_print_diag(&c);
      ctx_dispose(&c);
      return 1;
    }
    const uint32_t fty = fnn->type_ref;

    if (!init_params_for_fn(&c, i, fty)) {
      sirj_diag_setf(&c, "sem.unsupported.fn_params", path, fnn->loc_line, i, "fn", "unsupported fn params");
      sem_print_diag(&c);
      ctx_dispose(&c);
      return 1;
    }
    c.fn = fid;
    const bool is_entry = (fid == entry_fid);
    if (!lower_fn_body(&c, i, is_entry)) {
      if (!c.diag.set) {
        const char* nm = json_get_string(json_obj_get(fnn->fields_obj, "name"));
        sirj_diag_setf(&c, "sem.unsupported", path, fnn->loc_line, i, "fn", "unsupported SIR subset in fn=%s", nm ? nm : "?");
      }
      sem_print_diag(&c);
      ctx_dispose(&c);
      return 1;
    }
    if (!sir_mb_func_set_value_count(c.mb, fid, c.next_slot)) {
      sirj_diag_setf(&c, "sem.internal", path, fnn->loc_line, i, "fn", "failed to set value count");
      sem_print_diag(&c);
      ctx_dispose(&c);
      return 1;
    }
  }

  sir_module_t* m = sir_mb_finalize(c.mb);
  if (!m) {
    sirj_diag_setf(&c, "sem.internal", path, 0, 0, NULL, "failed to finalize module");
    sem_print_diag(&c);
    ctx_dispose(&c);
    return 1;
  }

  sir_validate_diag_t vd = {0};
  if (!sir_module_validate_ex(m, &vd)) {
    sir_module_free(m);
    const uint32_t diag_line = vd.src_line ? vd.src_line : 0;
    const uint32_t diag_node = vd.src_node_id ? vd.src_node_id : 0;
    if (vd.fid && vd.op != SIR_INST_INVALID) {
      const char* op = sir_inst_kind_name(vd.op);
      sirj_diag_setf_ex(&c, vd.code ? vd.code : "sem.validate", path, diag_line, diag_node, NULL, (uint32_t)vd.fid, (uint32_t)vd.ip, op,
                        "module validate failed: %s", vd.message[0] ? vd.message : "invalid");
    } else {
      sirj_diag_setf(&c, vd.code ? vd.code : "sem.validate", path, diag_line, diag_node, NULL, "module validate failed: %s",
                     vd.message[0] ? vd.message : "invalid");
    }
    sem_print_diag(&c);
    ctx_dispose(&c);
    return 1;
  }

  if (!do_run) {
    sir_module_free(m);
    ctx_dispose(&c);
    if (out_prog_rc) *out_prog_rc = 0;
    return 0;
  }

  sir_hosted_zabi_t hz;
  if (!sir_hosted_zabi_init(
          &hz, (sir_hosted_zabi_cfg_t){.abi_version = 0x00020005u,
                                       .guest_mem_cap = 16u * 1024u * 1024u,
                                       .guest_mem_base = 0x10000ull,
                                       .caps = caps,
                                       .cap_count = cap_count,
                                       .fs_root = fs_root})) {
    sir_module_free(m);
    sirj_diag_setf(&c, "sem.runtime_init", path, 0, 0, NULL, "failed to init runtime");
    sem_print_diag(&c);
    ctx_dispose(&c);
    return 1;
  }

  const sir_host_t host = sem_hosted_make_host(&hz);
  sem_wrap_sink_t wrap = {.inner = sink};
  const sir_exec_event_sink_t wrap_sink = {
      .user = &wrap,
      .on_step = sem_wrap_on_step,
      .on_mem = sem_wrap_on_mem,
      .on_hostcall = sem_wrap_on_hostcall,
  };
  const sir_exec_event_sink_t* sink2 = (sink || diag_format == SEM_DIAG_JSON) ? &wrap_sink : NULL;
  const int32_t rc = sir_module_run_ex(m, hz.mem, host, sink2);
  if (post_run) post_run(post_user, m, rc);

  sir_hosted_zabi_dispose(&hz);
  sir_module_free(m);
  ctx_dispose(&c);

  if (rc < 0) {
    // Execution errors come from sircore (ZI_E_*).
    if (diag_format == SEM_DIAG_JSON) {
      fprintf(stderr, "{\"tool\":\"sem\",\"code\":\"sem.exec\",\"message\":\"execution failed\",\"rc\":%d,\"rc_name\":\"%s\"", (int)rc,
              sem_zi_err_name(rc));
      if (sink2) {
        if (wrap.last.node_id) fprintf(stderr, ",\"node\":%u", (unsigned)wrap.last.node_id);
        if (wrap.last.line) fprintf(stderr, ",\"line\":%u", (unsigned)wrap.last.line);
        if (wrap.last.fid) {
          fprintf(stderr, ",\"fid\":%u", (unsigned)wrap.last.fid);
          fprintf(stderr, ",\"ip\":%u", (unsigned)wrap.last.ip);
          fprintf(stderr, ",\"op\":\"%s\"", sir_inst_kind_name(wrap.last.op));
        }
      }
      fprintf(stderr, "}\n");
    } else {
      fprintf(stderr, "sem: execution failed: %s (%d)\n", sem_zi_err_name(rc), (int)rc);
      if (sink2 && wrap.last.fid) {
        fprintf(stderr, "sem:   at fid=%u ip=%u op=%s\n", (unsigned)wrap.last.fid, (unsigned)wrap.last.ip, sir_inst_kind_name(wrap.last.op));
      }
      if (sink2 && (wrap.last.node_id || wrap.last.line)) {
        fprintf(stderr, "sem:   at node=%u line=%u\n", (unsigned)wrap.last.node_id, (unsigned)wrap.last.line);
      }
    }
    return 1;
  }
  if (out_prog_rc) *out_prog_rc = (int)rc;
  return 0;
}

int sem_run_sir_jsonl(const char* path, const sem_cap_t* caps, uint32_t cap_count, const char* fs_root) {
  int prog_rc = 0;
  const int tool_rc =
      sem_run_or_verify_sir_jsonl_impl(path, caps, cap_count, fs_root, SEM_DIAG_TEXT, false, true, &prog_rc, NULL, NULL, NULL);
  if (tool_rc != 0) return tool_rc;
  return prog_rc;
}

int sem_run_sir_jsonl_ex(const char* path, const sem_cap_t* caps, uint32_t cap_count, const char* fs_root, sem_diag_format_t diag_format,
                         bool diag_all) {
  int prog_rc = 0;
  const int tool_rc = sem_run_or_verify_sir_jsonl_impl(path, caps, cap_count, fs_root, diag_format, diag_all, true, &prog_rc, NULL, NULL, NULL);
  if (tool_rc != 0) return tool_rc;
  return prog_rc;
}

int sem_run_sir_jsonl_capture_ex(const char* path, const sem_cap_t* caps, uint32_t cap_count, const char* fs_root, sem_diag_format_t diag_format,
                                 bool diag_all, int* out_prog_rc) {
  int prog_rc = 0;
  const int tool_rc = sem_run_or_verify_sir_jsonl_impl(path, caps, cap_count, fs_root, diag_format, diag_all, true, &prog_rc, NULL, NULL, NULL);
  if (tool_rc != 0) return tool_rc;
  if (out_prog_rc) *out_prog_rc = prog_rc;
  return 0;
}

typedef struct sem_trace_ctx {
  FILE* out;
  const char* func_filter; // exact match on function name when non-NULL
  const char* op_filter;   // exact match on sir_inst_kind_name when non-NULL (step records only)
} sem_trace_ctx_t;

typedef struct sem_cov_ctx {
  FILE* out;
  uint32_t* offsets; // len=func_count
  uint32_t* counts;  // len=offsets[func_count-1] + inst_count[last]
  uint32_t total_slots;
  uint32_t unique_steps;
  uint64_t total_steps;
} sem_cov_ctx_t;

typedef struct sem_events_ctx {
  sem_trace_ctx_t* trace;
  sem_cov_ctx_t* cov;
  bool cov_inited;
} sem_events_ctx_t;

static const char* sem_trace_func_name(const sir_module_t* m, sir_func_id_t fid) {
  if (!m || fid == 0 || fid > m->func_count) return "";
  const sir_func_t* f = &m->funcs[fid - 1];
  return f->name ? f->name : "";
}

static void sem_trace_write_src(FILE* out, const sir_module_t* m, sir_func_id_t fid, uint32_t ip) {
  if (!out || !m || fid == 0 || fid > m->func_count) return;
  const sir_func_t* f = &m->funcs[fid - 1];
  if (!f || ip >= f->inst_count) return;
  const uint32_t node_id = f->insts[ip].src_node_id;
  const uint32_t line = f->insts[ip].src_line;
  if (!node_id && !line) return;
  fprintf(out, ",\"node\":%u,\"line\":%u", (unsigned)node_id, (unsigned)line);
}

static void sem_trace_on_step(void* user, const sir_module_t* m, sir_func_id_t fid, uint32_t ip, sir_inst_kind_t k) {
  sem_trace_ctx_t* t = (sem_trace_ctx_t*)user;
  if (!t || !t->out) return;
  const char* fn = sem_trace_func_name(m, fid);
  if (t->func_filter && t->func_filter[0] && strcmp(fn, t->func_filter) != 0) return;
  if (t->op_filter && t->op_filter[0] && strcmp(sir_inst_kind_name(k), t->op_filter) != 0) return;
  fprintf(t->out, "{\"tool\":\"sem\",\"k\":\"trace_step\",\"fid\":%u,\"func\":\"", (unsigned)fid);
  sem_json_write_escaped(t->out, fn);
  fprintf(t->out, "\",\"ip\":%u,\"op\":\"%s\"", (unsigned)ip, sir_inst_kind_name(k));
  sem_trace_write_src(t->out, m, fid, ip);
  fprintf(t->out, "}\n");
}

static void sem_cov_on_step(void* user, const sir_module_t* m, sir_func_id_t fid, uint32_t ip, sir_inst_kind_t k) {
  (void)k;
  sem_cov_ctx_t* c = (sem_cov_ctx_t*)user;
  if (!c || !m) return;
  if (fid == 0 || fid > m->func_count) return;
  const uint32_t fidx = fid - 1;
  const sir_func_t* f = &m->funcs[fidx];
  if (ip >= f->inst_count) return;
  if (!c->offsets || !c->counts) return;
  const uint32_t slot = c->offsets[fidx] + ip;
  if (slot >= c->total_slots) return;
  if (c->counts[slot] == 0) c->unique_steps++;
  c->counts[slot]++;
  c->total_steps++;
}

static void sem_trace_on_mem(void* user, const sir_module_t* m, sir_func_id_t fid, uint32_t ip, sir_mem_event_kind_t k, zi_ptr_t addr,
                             uint32_t size) {
  sem_trace_ctx_t* t = (sem_trace_ctx_t*)user;
  if (!t || !t->out) return;
  const char* fn = sem_trace_func_name(m, fid);
  if (t->func_filter && t->func_filter[0] && strcmp(fn, t->func_filter) != 0) return;
  fprintf(t->out, "{\"tool\":\"sem\",\"k\":\"trace_mem\",\"fid\":%u,\"func\":\"", (unsigned)fid);
  sem_json_write_escaped(t->out, fn);
  fprintf(t->out, "\",\"ip\":%u,\"kind\":\"%s\",\"addr\":%" PRIu64 ",\"size\":%u", (unsigned)ip, (k == SIR_MEM_WRITE) ? "w" : "r",
          (uint64_t)addr, (unsigned)size);
  sem_trace_write_src(t->out, m, fid, ip);
  fprintf(t->out, "}\n");
}

static void sem_trace_on_hostcall(void* user, const sir_module_t* m, sir_func_id_t fid, uint32_t ip, const char* callee, int32_t rc) {
  sem_trace_ctx_t* t = (sem_trace_ctx_t*)user;
  if (!t || !t->out) return;
  const char* fn = sem_trace_func_name(m, fid);
  if (t->func_filter && t->func_filter[0] && strcmp(fn, t->func_filter) != 0) return;
  fprintf(t->out, "{\"tool\":\"sem\",\"k\":\"trace_hostcall\",\"fid\":%u,\"func\":\"", (unsigned)fid);
  sem_json_write_escaped(t->out, fn);
  fprintf(t->out, "\",\"ip\":%u,\"callee\":\"", (unsigned)ip);
  sem_json_write_escaped(t->out, callee ? callee : "");
  fprintf(t->out, "\",\"rc\":%d", (int)rc);
  sem_trace_write_src(t->out, m, fid, ip);
  fprintf(t->out, "}\n");
}

static void sem_events_on_step(void* user, const sir_module_t* m, sir_func_id_t fid, uint32_t ip, sir_inst_kind_t k) {
  sem_events_ctx_t* e = (sem_events_ctx_t*)user;
  if (!e) return;
  if (e->trace) sem_trace_on_step(e->trace, m, fid, ip, k);
  if (e->cov) {
    if (!e->cov_inited && m && m->func_count) {
      const uint32_t fn = m->func_count;
      e->cov->offsets = (uint32_t*)calloc(fn ? fn : 1u, sizeof(uint32_t));
      if (!e->cov->offsets) return;
      uint32_t total = 0;
      for (uint32_t i = 0; i < fn; i++) {
        e->cov->offsets[i] = total;
        const uint32_t n = m->funcs[i].inst_count;
        if (UINT32_MAX - total < n) {
          free(e->cov->offsets);
          e->cov->offsets = NULL;
          return;
        }
        total += n;
      }
      e->cov->counts = (uint32_t*)calloc(total ? total : 1u, sizeof(uint32_t));
      if (!e->cov->counts) {
        free(e->cov->offsets);
        e->cov->offsets = NULL;
        return;
      }
      e->cov->total_slots = total;
      e->cov_inited = true;
    }
    sem_cov_on_step(e->cov, m, fid, ip, k);
  }
}

static void sem_events_on_mem(void* user, const sir_module_t* m, sir_func_id_t fid, uint32_t ip, sir_mem_event_kind_t mk, zi_ptr_t addr,
                              uint32_t size) {
  sem_events_ctx_t* e = (sem_events_ctx_t*)user;
  if (!e) return;
  if (e->trace) sem_trace_on_mem(e->trace, m, fid, ip, mk, addr, size);
}

static void sem_events_on_hostcall(void* user, const sir_module_t* m, sir_func_id_t fid, uint32_t ip, const char* callee, int32_t rc) {
  sem_events_ctx_t* e = (sem_events_ctx_t*)user;
  if (!e) return;
  if (e->trace) sem_trace_on_hostcall(e->trace, m, fid, ip, callee, rc);
}

static void sem_events_post_run(void* user, const sir_module_t* m, int32_t exec_rc) {
  sem_events_ctx_t* e = (sem_events_ctx_t*)user;
  if (!e || !e->cov || !e->cov->out || !e->cov_inited || !m) return;

  FILE* out = e->cov->out;
  fprintf(out, "{\"tool\":\"sem\",\"k\":\"coverage\",\"format\":\"inst\",\"version\":1,\"exec_rc\":%d}\n", (int)exec_rc);
  for (uint32_t i = 0; i < m->func_count; i++) {
    const sir_func_t* f = &m->funcs[i];
    const sir_func_id_t fid = i + 1;
    const char* fn = f->name ? f->name : "";
    const uint32_t base = e->cov->offsets ? e->cov->offsets[i] : 0;
    for (uint32_t ip = 0; ip < f->inst_count; ip++) {
      const uint32_t slot = base + ip;
      if (!e->cov->counts || slot >= e->cov->total_slots) continue;
      const uint32_t hit = e->cov->counts[slot];
      if (!hit) continue;
      const sir_inst_kind_t opk = f->insts[ip].k;
      fprintf(out, "{\"tool\":\"sem\",\"k\":\"cov_step\",\"fid\":%u,\"func\":\"", (unsigned)fid);
      sem_json_write_escaped(out, fn);
      fprintf(out, "\",\"ip\":%u,\"op\":\"%s\",\"count\":%u", (unsigned)ip, sir_inst_kind_name(opk), (unsigned)hit);
      sem_trace_write_src(out, m, fid, ip);
      fprintf(out, "}\n");
    }
  }
  fprintf(out, "{\"tool\":\"sem\",\"k\":\"cov_summary\",\"unique_steps\":%u,\"total_steps\":%" PRIu64 "}\n", (unsigned)e->cov->unique_steps,
          (uint64_t)e->cov->total_steps);
}

int sem_run_sir_jsonl_events_ex(const char* path, const sem_cap_t* caps, uint32_t cap_count, const char* fs_root, sem_diag_format_t diag_format,
                                bool diag_all, const char* trace_jsonl_out_path, const char* coverage_jsonl_out_path, const char* trace_func_filter,
                                const char* trace_op_filter) {
  FILE* trace_out = NULL;
  FILE* cov_out = NULL;

  if (trace_jsonl_out_path && trace_jsonl_out_path[0]) {
    trace_out = fopen(trace_jsonl_out_path, "wb");
    if (!trace_out) {
      fprintf(stderr, "sem: failed to open trace output: %s\n", trace_jsonl_out_path);
      return 2;
    }
  }
  if (coverage_jsonl_out_path && coverage_jsonl_out_path[0]) {
    cov_out = fopen(coverage_jsonl_out_path, "wb");
    if (!cov_out) {
      if (trace_out) fclose(trace_out);
      fprintf(stderr, "sem: failed to open coverage output: %s\n", coverage_jsonl_out_path);
      return 2;
    }
  }

  sem_trace_ctx_t t = {.out = trace_out, .func_filter = trace_func_filter, .op_filter = trace_op_filter};
  sem_cov_ctx_t cov = {.out = cov_out};

  sem_events_ctx_t ev = {.trace = trace_out ? &t : NULL, .cov = cov_out ? &cov : NULL, .cov_inited = false};

  const sir_exec_event_sink_t sink = {
      .user = &ev,
      .on_step = sem_events_on_step,
      .on_mem = sem_events_on_mem,
      .on_hostcall = sem_events_on_hostcall,
  };

  int prog_rc = 0;
  const int tool_rc = sem_run_or_verify_sir_jsonl_impl(path, caps, cap_count, fs_root, diag_format, diag_all, true, &prog_rc,
                                                       (trace_out || cov_out) ? &sink : NULL, cov_out ? sem_events_post_run : NULL, &ev);

  if (trace_out) fclose(trace_out);
  if (cov_out) fclose(cov_out);
  free(cov.offsets);
  free(cov.counts);

  if (tool_rc != 0) return tool_rc;
  return prog_rc;
}

int sem_run_sir_jsonl_trace_ex(const char* path, const sem_cap_t* caps, uint32_t cap_count, const char* fs_root, sem_diag_format_t diag_format,
                               bool diag_all, const char* trace_jsonl_out_path) {
  if (!trace_jsonl_out_path || !trace_jsonl_out_path[0]) {
    fprintf(stderr, "sem: missing --trace-jsonl-out path\n");
    return 2;
  }
  return sem_run_sir_jsonl_events_ex(path, caps, cap_count, fs_root, diag_format, diag_all, trace_jsonl_out_path, NULL, NULL, NULL);
}

int sem_run_sir_jsonl_coverage_ex(const char* path, const sem_cap_t* caps, uint32_t cap_count, const char* fs_root, sem_diag_format_t diag_format,
                                  bool diag_all, const char* coverage_jsonl_out_path) {
  if (!coverage_jsonl_out_path || !coverage_jsonl_out_path[0]) {
    fprintf(stderr, "sem: missing --coverage-jsonl-out path\n");
    return 2;
  }
  return sem_run_sir_jsonl_events_ex(path, caps, cap_count, fs_root, diag_format, diag_all, NULL, coverage_jsonl_out_path, NULL, NULL);
}

int sem_verify_sir_jsonl(const char* path, sem_diag_format_t diag_format) {
  return sem_verify_sir_jsonl_ex(path, diag_format, false);
}

int sem_verify_sir_jsonl_ex(const char* path, sem_diag_format_t diag_format, bool diag_all) {
  return sem_run_or_verify_sir_jsonl_impl(path, NULL, 0, NULL, diag_format, diag_all, false, NULL, NULL, NULL, NULL);
}
