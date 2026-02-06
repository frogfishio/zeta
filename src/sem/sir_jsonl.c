#include "sir_jsonl.h"

#include "sem_hosted.h"
#include "sir_module.h"

#include "json.h"
#include "sircc.h"

#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct type_info {
  bool present;
  bool is_fn;
  sir_prim_type_t prim; // for prim
  uint32_t* params;     // for fn, arena-owned
  uint32_t param_count;
  uint32_t ret; // SIR type id
} type_info_t;

typedef struct node_info {
  bool present;
  const char* tag;       // arena-owned
  uint32_t type_ref;     // 0 if missing
  JsonValue* fields_obj; // object or NULL
} node_info_t;

typedef enum val_kind {
  VK_INVALID = 0,
  VK_I32,
  VK_I64,
  VK_PTR,
  VK_BOOL,
} val_kind_t;

typedef struct sirj_ctx {
  Arena arena;

  type_info_t* types; // indexed by type id (0..cap-1)
  uint32_t type_cap;

  node_info_t* nodes; // indexed by node id
  uint32_t node_cap;

  // Lowering maps
  sir_sym_id_t* sym_by_node; // indexed by node id; 0 means unset
  uint32_t sym_cap;

  sir_val_id_t* val_by_node; // indexed by node id; stores slot+1 (0 means unset)
  val_kind_t* kind_by_node;  // indexed by node id
  uint32_t val_cap;
  uint32_t kind_cap;

  sir_val_id_t next_slot;

  sir_module_builder_t* mb;
  sir_func_id_t fn;
  sir_func_id_t* func_by_node; // node_id -> func_id (0 if none)
  uint32_t func_by_node_cap;

  // Primitive module type ids
  sir_type_id_t ty_i32;
  sir_type_id_t ty_i64;
  sir_type_id_t ty_ptr;
  sir_type_id_t ty_bool;

  // Current-function param bindings (name -> slot).
  struct {
    const char* name;
    sir_val_id_t slot;
    val_kind_t kind;
  } params[32];
  uint32_t param_count;
} sirj_ctx_t;

static void ctx_dispose(sirj_ctx_t* c) {
  if (!c) return;
  if (c->mb) {
    sir_mb_free(c->mb);
    c->mb = NULL;
  }
  free(c->types);
  free(c->nodes);
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

static bool json_get_u32(const JsonValue* v, uint32_t* out) {
  if (!out) return false;
  int64_t i = 0;
  if (!json_get_i64(v, &i)) return false;
  if (i < 0 || i > 0x7FFFFFFFll) return false;
  *out = (uint32_t)i;
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

static sir_prim_type_t prim_from_string(const char* s) {
  if (!s) return SIR_PRIM_INVALID;
  if (strcmp(s, "i32") == 0) return SIR_PRIM_I32;
  if (strcmp(s, "i64") == 0) return SIR_PRIM_I64;
  if (strcmp(s, "ptr") == 0) return SIR_PRIM_PTR;
  if (strcmp(s, "bool") == 0) return SIR_PRIM_BOOL;
  return SIR_PRIM_INVALID;
}

static sir_type_id_t mod_ty_for_prim(sirj_ctx_t* c, sir_prim_type_t prim) {
  if (!c) return 0;
  switch (prim) {
    case SIR_PRIM_I32:
      return c->ty_i32;
    case SIR_PRIM_I64:
      return c->ty_i64;
    case SIR_PRIM_PTR:
      return c->ty_ptr;
    case SIR_PRIM_BOOL:
      return c->ty_bool;
    default:
      return 0;
  }
}

static bool ensure_prim_types(sirj_ctx_t* c) {
  if (!c || !c->mb) return false;
  if (!c->ty_i32) c->ty_i32 = sir_mb_type_prim(c->mb, SIR_PRIM_I32);
  if (!c->ty_i64) c->ty_i64 = sir_mb_type_prim(c->mb, SIR_PRIM_I64);
  if (!c->ty_ptr) c->ty_ptr = sir_mb_type_prim(c->mb, SIR_PRIM_PTR);
  if (!c->ty_bool) c->ty_bool = sir_mb_type_prim(c->mb, SIR_PRIM_BOOL);
  return c->ty_i32 && c->ty_i64 && c->ty_ptr && c->ty_bool;
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
    const sir_type_id_t mt = mod_ty_for_prim(c, c->types[sir_rid].prim);
    if (!mt) return false;
    results[0] = mt;
    result_count = 1;
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

static bool eval_node(sirj_ctx_t* c, uint32_t node_id, sir_val_id_t* out_slot, val_kind_t* out_kind);

static bool eval_const_i32(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* vv = json_obj_get(n->fields_obj, "value");
  int64_t i = 0;
  if (!json_get_i64(vv, &i)) return false;
  if (i < INT32_MIN || i > INT32_MAX) return false;
  const sir_val_id_t slot = alloc_slot(c, VK_I32);
  if (!sir_mb_emit_const_i32(c->mb, c->fn, slot, (int32_t)i)) return false;
  if (!set_node_val(c, node_id, slot, VK_I32)) return false;
  *out_slot = slot;
  *out_kind = VK_I32;
  return true;
}

static bool eval_const_i64(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* vv = json_obj_get(n->fields_obj, "value");
  int64_t i = 0;
  if (!json_get_i64(vv, &i)) return false;
  const sir_val_id_t slot = alloc_slot(c, VK_I64);
  if (!sir_mb_emit_const_i64(c->mb, c->fn, slot, i)) return false;
  if (!set_node_val(c, node_id, slot, VK_I64)) return false;
  *out_slot = slot;
  *out_kind = VK_I64;
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
  const sir_val_id_t dst = alloc_slot(c, VK_I32);
  if (!sir_mb_emit_i32_add(c->mb, c->fn, dst, a_slot, b_slot)) return false;
  if (!set_node_val(c, node_id, dst, VK_I32)) return false;
  *out_slot = dst;
  *out_kind = VK_I32;
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
  if (!sir_mb_emit_i32_add(c->mb, c->fn, dst, a_slot, b_slot)) return false;
  if (!set_node_val(c, node_id, dst, VK_I32)) return false;
  *out_slot = dst;
  *out_kind = VK_I32;
  return true;
}

static bool eval_ptr_to_i64_passthrough(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  (void)node_id;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len != 1) return false;
  uint32_t arg_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &arg_id)) return false;
  // MVP: treat ptr.to_i64 as passthrough for host calls.
  return eval_node(c, arg_id, out_slot, out_kind);
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

static bool eval_call_indirect(sirj_ctx_t* c, uint32_t node_id, const node_info_t* n, sir_val_id_t* out_slot, val_kind_t* out_kind) {
  if (!c || !n || !out_slot || !out_kind) return false;
  if (!n->fields_obj || n->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* av = json_obj_get(n->fields_obj, "args");
  if (!json_is_array(av) || av->v.arr.len < 1) return false;

  uint32_t callee_id = 0;
  if (!parse_ref_id(av->v.arr.items[0], &callee_id)) return false;

  sir_sym_id_t callee_sym = 0;
  sir_func_id_t callee_fn = 0;
  if (callee_id >= c->node_cap || !c->nodes[callee_id].present) return false;
  const node_info_t* cn = &c->nodes[callee_id];
  if (cn->tag && strcmp(cn->tag, "decl.fn") == 0) {
    if (!resolve_decl_fn_sym(c, callee_id, &callee_sym)) return false;
  } else if (cn->tag && strcmp(cn->tag, "ptr.sym") == 0) {
    if (!cn->fields_obj || cn->fields_obj->type != JSON_OBJECT) return false;
    const char* nm = json_get_string(json_obj_get(cn->fields_obj, "name"));
    if (!nm) return false;
    if (!resolve_internal_func_by_name(c, nm, &callee_fn)) {
      return false;
    }
  } else {
    return false;
  }

  sir_val_id_t args_slots[16];
  const uint32_t argc = (uint32_t)(av->v.arr.len - 1);
  if (argc > (uint32_t)(sizeof(args_slots) / sizeof(args_slots[0]))) return false;

  for (uint32_t i = 0; i < argc; i++) {
    uint32_t arg_node_id = 0;
    if (!parse_ref_id(av->v.arr.items[i + 1], &arg_node_id)) return false;
    val_kind_t ak = VK_INVALID;
    if (!eval_node(c, arg_node_id, &args_slots[i], &ak)) return false;
    (void)ak;
  }

  // Determine return arity from the callee signature.
  // (We only support 0 or 1 return in the sir_module MVP.)
  // Use the SIR `sig` field when present (points to a type id).
  uint32_t sig_tid = 0;
  const JsonValue* sigv = json_obj_get(n->fields_obj, "sig");
  if (sigv) {
    if (!parse_ref_id(sigv, &sig_tid)) {
      return false;
    }
  }
  uint32_t ret_tid = 0;
  if (sig_tid && sig_tid < c->type_cap && c->types[sig_tid].present && c->types[sig_tid].is_fn) {
    ret_tid = c->types[sig_tid].ret;
  }

  uint8_t result_count = 0;
  sir_val_id_t res_slots[1];
  val_kind_t rk = VK_INVALID;
  if (ret_tid != 0) {
    if (ret_tid >= c->type_cap || !c->types[ret_tid].present || c->types[ret_tid].is_fn) return false;
    const sir_prim_type_t rp = c->types[ret_tid].prim;
    if (rp == SIR_PRIM_I32) rk = VK_I32;
    else if (rp == SIR_PRIM_I64) rk = VK_I64;
    else if (rp == SIR_PRIM_PTR) rk = VK_PTR;
    else if (rp == SIR_PRIM_BOOL) rk = VK_BOOL;
    else return false;
    res_slots[0] = alloc_slot(c, rk);
    result_count = 1;
  }

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

  if (strcmp(n->tag, "const.i32") == 0) return eval_const_i32(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "const.i64") == 0) return eval_const_i64(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "cstr") == 0) return eval_cstr(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "name") == 0) return eval_name(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "ptr.to_i64") == 0) return eval_ptr_to_i64_passthrough(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "i32.add") == 0) return eval_i32_add_mnemonic(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "binop.add") == 0) return eval_binop_add(c, node_id, n, out_slot, out_kind);
  if (strcmp(n->tag, "call.indirect") == 0) return eval_call_indirect(c, node_id, n, out_slot, out_kind);

  return false;
}

static bool exec_stmt(sirj_ctx_t* c, uint32_t stmt_id, bool* out_did_return, sir_val_id_t* out_exit_slot) {
  if (!c || !out_did_return || !out_exit_slot) return false;
  *out_did_return = false;
  *out_exit_slot = 0;
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
    *out_did_return = true;
    *out_exit_slot = slot;
    (void)k;
    return true;
  }

  return false;
}

static bool lower_fn_body(sirj_ctx_t* c, uint32_t fn_node_id, bool is_entry) {
  if (!c) return false;
  if (fn_node_id >= c->node_cap || !c->nodes[fn_node_id].present) return false;
  const node_info_t* fnn = &c->nodes[fn_node_id];
  if (!fnn->fields_obj || fnn->fields_obj->type != JSON_OBJECT) return false;
  const JsonValue* bodyv = json_obj_get(fnn->fields_obj, "body");
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
    if (!exec_stmt(c, sid, &did_ret, &exit_slot)) return false;
    if (did_ret) {
      if (is_entry) {
        if (!sir_mb_emit_exit_val(c->mb, c->fn, exit_slot)) return false;
      } else {
        if (!sir_mb_emit_ret_val(c->mb, c->fn, exit_slot)) return false;
      }
      return true;
    }
  }

  // Implicit return 0.
  const sir_val_id_t slot = alloc_slot(c, VK_I32);
  if (!sir_mb_emit_const_i32(c->mb, c->fn, slot, 0)) return false;
  if (is_entry) {
    if (!sir_mb_emit_exit_val(c->mb, c->fn, slot)) return false;
  } else {
    if (!sir_mb_emit_ret_val(c->mb, c->fn, slot)) return false;
  }
  return true;
}

static bool parse_file(sirj_ctx_t* c, const char* path) {
  if (!c || !path) return false;
  FILE* f = fopen(path, "rb");
  if (!f) return false;

  char* line = NULL;
  size_t cap = 0;
  size_t len = 0;
  int ch = 0;

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

      JsonValue* root = NULL;
      JsonError err = {0};
      if (!json_parse(&c->arena, line, &root, &err) || !root) {
        fprintf(stderr, "sem: json parse error at %zu: %s\n", err.offset, err.msg ? err.msg : "error");
        free(line);
        fclose(f);
        return false;
      }
      if (!json_is_object(root)) {
        fprintf(stderr, "sem: record is not an object\n");
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
        uint32_t id = 0;
        if (!json_get_u32(json_obj_get(root, "id"), &id)) {
          fprintf(stderr, "sem: type.id missing/invalid\n");
          free(line);
          fclose(f);
          return false;
        }
        if (!ensure_type_cap(c, id)) {
          free(line);
          fclose(f);
          return false;
        }
        const char* kind = json_get_string(json_obj_get(root, "kind"));
        if (!kind) {
          fprintf(stderr, "sem: type.kind missing\n");
          free(line);
          fclose(f);
          return false;
        }

        type_info_t ti = {0};
        ti.present = true;
        if (strcmp(kind, "prim") == 0) {
          const char* prim = json_get_string(json_obj_get(root, "prim"));
          ti.prim = prim_from_string(prim);
          if (ti.prim == SIR_PRIM_INVALID) {
            fprintf(stderr, "sem: unsupported prim: %s\n", prim ? prim : "(null)");
            free(line);
            fclose(f);
            return false;
          }
        } else if (strcmp(kind, "fn") == 0) {
          ti.is_fn = true;
          const JsonValue* pv = obj_req(root, "params");
          if (!parse_u32_array(pv, &ti.params, &ti.param_count, &c->arena)) {
            fprintf(stderr, "sem: bad fn params\n");
            free(line);
            fclose(f);
            return false;
          }
          if (!json_get_u32(json_obj_get(root, "ret"), &ti.ret)) {
            fprintf(stderr, "sem: bad fn ret\n");
            free(line);
            fclose(f);
            return false;
          }
        } else {
          // ignore other kinds for now
          memset(&ti, 0, sizeof(ti));
          ti.present = true;
        }
        c->types[id] = ti;
      } else if (strcmp(k, "node") == 0) {
        uint32_t id = 0;
        if (!json_get_u32(json_obj_get(root, "id"), &id)) {
          fprintf(stderr, "sem: node.id missing/invalid\n");
          free(line);
          fclose(f);
          return false;
        }
        if (!ensure_node_cap(c, id)) {
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
  sir_type_id_t params[16];
  for (uint32_t i = 0; i < ti->param_count; i++) {
    const uint32_t pid = ti->params[i];
    if (pid == 0 || pid >= c->type_cap) return false;
    const type_info_t* pt = &c->types[pid];
    if (!pt->present || pt->is_fn) return false;
    const sir_type_id_t mt = mod_ty_for_prim(c, pt->prim);
    if (!mt) return false;
    params[i] = mt;
  }

  sir_type_id_t results[1];
  uint32_t result_count = 0;
  if (ti->ret) {
    const uint32_t rid = ti->ret;
    if (rid == 0 || rid >= c->type_cap) return false;
    const type_info_t* rt = &c->types[rid];
    if (!rt->present || rt->is_fn) return false;
    const sir_type_id_t mt = mod_ty_for_prim(c, rt->prim);
    if (!mt) return false;
    results[0] = mt;
    result_count = 1;
  }

  out_sig->params = params;
  out_sig->param_count = ti->param_count;
  out_sig->results = result_count ? results : NULL;
  out_sig->result_count = result_count;
  return true;
}

static bool init_params_for_fn(sirj_ctx_t* c, uint32_t fn_node_id, uint32_t fn_type_id) {
  if (!c) return false;
  c->param_count = 0;
  c->next_slot = 0;
  reset_value_cache(c);

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
    if (pt->prim == SIR_PRIM_I32) k = VK_I32;
    else if (pt->prim == SIR_PRIM_I64) k = VK_I64;
    else if (pt->prim == SIR_PRIM_PTR) k = VK_PTR;
    else if (pt->prim == SIR_PRIM_BOOL) k = VK_BOOL;
    else return false;

    c->params[i].name = nm;
    c->params[i].slot = i;
    c->params[i].kind = k;
    c->param_count++;
    c->next_slot = i + 1;
  }

  return true;
}

int sem_run_sir_jsonl(const char* path, const sem_cap_t* caps, uint32_t cap_count, const char* fs_root) {
  if (!path) return 2;

  sirj_ctx_t c;
  memset(&c, 0, sizeof(c));
  arena_init(&c.arena);

  if (!parse_file(&c, path)) {
    ctx_dispose(&c);
    fprintf(stderr, "sem: failed to parse: %s\n", path);
    return 1;
  }

  uint32_t entry_fn_node_id = 0;
  if (!find_entry_fn(&c, &entry_fn_node_id)) {
    ctx_dispose(&c);
    fprintf(stderr, "sem: no entry fn (expected fn name zir_main or main)\n");
    return 1;
  }

  c.mb = sir_mb_new();
  if (!c.mb) {
    ctx_dispose(&c);
    fprintf(stderr, "sem: OOM\n");
    return 1;
  }
  if (!ensure_prim_types(&c)) {
    ctx_dispose(&c);
    fprintf(stderr, "sem: OOM\n");
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
      ctx_dispose(&c);
      fprintf(stderr, "sem: OOM\n");
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
        ctx_dispose(&c);
        fprintf(stderr, "sem: OOM\n");
        return 1;
      }
    }

    if (i == entry_fn_node_id) entry_fid = fid;
  }
  if (!entry_fid) {
    ctx_dispose(&c);
    fprintf(stderr, "sem: internal: failed to map entry function\n");
    return 1;
  }
  if (!sir_mb_func_set_entry(c.mb, entry_fid)) {
    ctx_dispose(&c);
    fprintf(stderr, "sem: internal: failed to init module func\n");
    return 1;
  }

  // Lower each function body.
  for (uint32_t i = 0; i < c.node_cap; i++) {
    const sir_func_id_t fid = (i < c.func_by_node_cap) ? c.func_by_node[i] : 0;
    if (!fid) continue;
    const node_info_t* fnn = &c.nodes[i];
    if (!fnn->fields_obj || fnn->fields_obj->type != JSON_OBJECT) {
      ctx_dispose(&c);
      fprintf(stderr, "sem: internal: fn fields malformed (node_id=%u)\n", (unsigned)i);
      return 1;
    }
    const uint32_t fty = fnn->type_ref;

    if (!init_params_for_fn(&c, i, fty)) {
      ctx_dispose(&c);
      fprintf(stderr, "sem: unsupported fn params (node_id=%u)\n", (unsigned)i);
      return 1;
    }
    c.fn = fid;
    const bool is_entry = (fid == entry_fid);
    if (!lower_fn_body(&c, i, is_entry)) {
      ctx_dispose(&c);
      const char* nm = json_get_string(json_obj_get(fnn->fields_obj, "name"));
      fprintf(stderr, "sem: unsupported SIR subset in %s (fn=%s node_id=%u)\n", path, nm ? nm : "?", (unsigned)i);
      return 1;
    }
    if (!sir_mb_func_set_value_count(c.mb, fid, c.next_slot)) {
      ctx_dispose(&c);
      fprintf(stderr, "sem: internal: failed to set value count\n");
      return 1;
    }
  }

  sir_module_t* m = sir_mb_finalize(c.mb);
  if (!m) {
    ctx_dispose(&c);
    fprintf(stderr, "sem: internal: failed to finalize module\n");
    return 1;
  }

  char verr[160];
  if (!sir_module_validate(m, verr, sizeof(verr))) {
    sir_module_free(m);
    ctx_dispose(&c);
    fprintf(stderr, "sem: validate failed: %s\n", verr[0] ? verr : "invalid");
    return 1;
  }

  sir_hosted_zabi_t hz;
  if (!sir_hosted_zabi_init(&hz, (sir_hosted_zabi_cfg_t){.abi_version = 0x00020005u, .guest_mem_cap = 16u * 1024u * 1024u, .guest_mem_base = 0x10000ull, .caps = caps, .cap_count = cap_count, .fs_root = fs_root})) {
    sir_module_free(m);
    ctx_dispose(&c);
    fprintf(stderr, "sem: failed to init runtime\n");
    return 1;
  }

  const sir_host_t host = sem_hosted_make_host(&hz);
  const int32_t rc = sir_module_run(m, hz.mem, host);
  sir_hosted_zabi_dispose(&hz);
  sir_module_free(m);
  ctx_dispose(&c);

  if (rc < 0) {
    fprintf(stderr, "sem: execution failed: %d\n", rc);
    return 1;
  }
  return (int)rc;
}
