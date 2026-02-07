// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_lower_hl.h"

#include "compiler_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void json_write_value(FILE* out, const JsonValue* v);
static bool ensure_node_slot(SirProgram* p, int64_t id);

static void json_write_object(FILE* out, const JsonObject* obj) {
  fputc('{', out);
  if (obj) {
    for (size_t i = 0; i < obj->len; i++) {
      if (i) fputc(',', out);
      json_write_escaped(out, obj->items[i].key ? obj->items[i].key : "");
      fputc(':', out);
      json_write_value(out, obj->items[i].value);
    }
  }
  fputc('}', out);
}

static void json_write_array(FILE* out, const JsonArray* arr) {
  fputc('[', out);
  if (arr) {
    for (size_t i = 0; i < arr->len; i++) {
      if (i) fputc(',', out);
      json_write_value(out, arr->items[i]);
    }
  }
  fputc(']', out);
}

static void json_write_value(FILE* out, const JsonValue* v) {
  if (!out) return;
  if (!v) {
    fputs("null", out);
    return;
  }
  switch (v->type) {
    case JSON_NULL:
      fputs("null", out);
      return;
    case JSON_BOOL:
      fputs(v->v.b ? "true" : "false", out);
      return;
    case JSON_NUMBER:
      fprintf(out, "%lld", (long long)v->v.i);
      return;
    case JSON_STRING:
      json_write_escaped(out, v->v.s ? v->v.s : "");
      return;
    case JSON_ARRAY:
      json_write_array(out, &v->v.arr);
      return;
    case JSON_OBJECT:
      json_write_object(out, &v->v.obj);
      return;
    default:
      fputs("null", out);
      return;
  }
}

// Future: preserve original string ids by reverse-mapping through SirIdMap.

static JsonValue* jv_make(Arena* a, JsonType t) {
  JsonValue* v = (JsonValue*)arena_alloc(a, sizeof(*v));
  if (!v) return NULL;
  memset(v, 0, sizeof(*v));
  v->type = t;
  return v;
}

static JsonValue* jv_make_obj(Arena* a, size_t n) {
  JsonValue* v = jv_make(a, JSON_OBJECT);
  if (!v) return NULL;
  JsonObjectItem* items = NULL;
  if (n) {
    items = (JsonObjectItem*)arena_alloc(a, n * sizeof(*items));
    if (!items) return NULL;
    memset(items, 0, n * sizeof(*items));
  }
  v->v.obj.items = items;
  v->v.obj.len = n;
  return v;
}

static JsonValue* jv_make_arr(Arena* a, size_t n) {
  JsonValue* v = jv_make(a, JSON_ARRAY);
  if (!v) return NULL;
  JsonValue** items = NULL;
  if (n) {
    items = (JsonValue**)arena_alloc(a, n * sizeof(*items));
    if (!items) return NULL;
    memset(items, 0, n * sizeof(*items));
  }
  v->v.arr.items = items;
  v->v.arr.len = n;
  return v;
}

static bool lower_sem_if_to_select(SirProgram* p, NodeRec* n) {
  if (!p || !n || !n->fields) return false;
  JsonValue* args = json_obj_get(n->fields, "args");
  if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) return false;

  JsonValue* cond_ref = args->v.arr.items[0];
  JsonValue* br_then = args->v.arr.items[1];
  JsonValue* br_else = args->v.arr.items[2];
  if (!cond_ref || !br_then || !br_else) return false;
  if (br_then->type != JSON_OBJECT || br_else->type != JSON_OBJECT) return false;

  const char* k_then = json_get_string(json_obj_get(br_then, "kind"));
  const char* k_else = json_get_string(json_obj_get(br_else, "kind"));
  if (!k_then || !k_else) return false;

  if (strcmp(k_then, "val") != 0 || strcmp(k_else, "val") != 0) {
    // Not applicable: CFG lowering handles thunk branches (in return-position only for now).
    return false;
  }

  JsonValue* v_then = json_obj_get(br_then, "v");
  JsonValue* v_else = json_obj_get(br_else, "v");
  if (!v_then || !v_else) return false;

  JsonValue* new_args = jv_make_arr(&p->arena, 3);
  if (!new_args) {
    bump_exit_code(p, SIRCC_EXIT_INTERNAL);
    SIRCC_ERR_NODE(p, n, "sircc.oom", "sircc: out of memory");
    return false;
  }
  new_args->v.arr.items[0] = cond_ref;
  new_args->v.arr.items[1] = v_then;
  new_args->v.arr.items[2] = v_else;

  JsonValue* new_fields = jv_make_obj(&p->arena, 1);
  if (!new_fields) {
    bump_exit_code(p, SIRCC_EXIT_INTERNAL);
    SIRCC_ERR_NODE(p, n, "sircc.oom", "sircc: out of memory");
    return false;
  }
  new_fields->v.obj.items[0].key = "args";
  new_fields->v.obj.items[0].value = new_args;

  n->tag = "select";
  n->fields = new_fields;
  return true;
}

typedef enum BranchKind {
  BRANCH_INVALID = 0,
  BRANCH_VAL,
  BRANCH_THUNK,
} BranchKind;

typedef struct BranchOperand {
  BranchKind kind;
  int64_t node_id; // referenced node id for val.v or thunk.f
} BranchOperand;

static bool parse_branch_operand(SirProgram* p, const JsonValue* v, BranchOperand* out) {
  if (!p || !out) return false;
  memset(out, 0, sizeof(*out));
  if (!v || v->type != JSON_OBJECT) return false;
  const char* k = json_get_string(json_obj_get((JsonValue*)v, "kind"));
  if (!k) return false;
  if (strcmp(k, "val") == 0) {
    int64_t id = 0;
    if (!parse_node_ref_id(p, json_obj_get((JsonValue*)v, "v"), &id)) return false;
    out->kind = BRANCH_VAL;
    out->node_id = id;
    return true;
  }
  if (strcmp(k, "thunk") == 0) {
    int64_t id = 0;
    if (!parse_node_ref_id(p, json_obj_get((JsonValue*)v, "f"), &id)) return false;
    out->kind = BRANCH_THUNK;
    out->node_id = id;
    return true;
  }
  return false;
}

static JsonValue* jv_make_i64(Arena* a, int64_t x) {
  JsonValue* v = jv_make(a, JSON_NUMBER);
  if (!v) return NULL;
  v->v.i = x;
  return v;
}

static JsonValue* jv_make_str(Arena* a, const char* s) {
  JsonValue* v = jv_make(a, JSON_STRING);
  if (!v) return NULL;
  v->v.s = arena_strdup(a, s ? s : "");
  return v;
}

static JsonValue* jv_make_id_value(Arena* a, SirProgram* p, SirIdKind kind, int64_t id) {
  if (!a || !p || id == 0) return NULL;
  const char* s = sir_id_str_for_internal(p, kind, id);
  if (s) return jv_make_str(a, s);
  return jv_make_i64(a, id);
}

static JsonValue* jv_make_node_ref(SirProgram* p, int64_t id) {
  if (!p) return NULL;
  JsonValue* o = jv_make_obj(&p->arena, 2);
  if (!o) return NULL;
  o->v.obj.items[0].key = "t";
  o->v.obj.items[0].value = jv_make_str(&p->arena, "ref");
  o->v.obj.items[1].key = "id";
  o->v.obj.items[1].value = jv_make_id_value(&p->arena, p, SIR_ID_NODE, id);
  if (!o->v.obj.items[1].value) return NULL;
  return o;
}

#define jv_make_ref(_arena, _id) jv_make_node_ref(p, (_id))

static void emit_id_json(FILE* out, SirProgram* p, SirIdKind kind, int64_t id) {
  if (!out || !p) return;
  const char* s = sir_id_str_for_internal(p, kind, id);
  if (s) {
    json_write_escaped(out, s);
  } else {
    fprintf(out, "%lld", (long long)id);
  }
}

static const char* derived_id(SirProgram* p, SirIdKind kind, int64_t base_id, const char* suffix) {
  if (!p || base_id == 0 || !suffix) return NULL;
  const char* base_s = sir_id_str_for_internal(p, kind, base_id);
  char nb[64];
  if (!base_s) {
    (void)snprintf(nb, sizeof(nb), "%lld", (long long)base_id);
    base_s = nb;
  }
  const char* pre = "sircc:lower:";
  const size_t n = strlen(pre) + strlen(base_s) + 1 + strlen(suffix) + 1;
  char* s = (char*)arena_alloc(&p->arena, n);
  if (!s) return NULL;
  (void)snprintf(s, n, "%s%s:%s", pre, base_s, suffix);
  return s;
}

static int64_t alloc_node_id_from_str(SirProgram* p, const char* s, const char* ctx) {
  if (!p || !s || !*s) return 0;
  JsonValue tmp = {0};
  tmp.type = JSON_STRING;
  tmp.v.s = arena_strdup(&p->arena, s);
  if (!tmp.v.s) return 0;
  int64_t id = 0;
  if (!sir_intern_id(p, SIR_ID_NODE, &tmp, &id, ctx ? ctx : "derived node id")) return 0;
  if (!ensure_node_slot(p, id)) return 0;
  return id;
}

static NodeRec* make_node_stub(SirProgram* p, int64_t id, const char* tag, int64_t type_ref, JsonValue* fields) {
  if (!p || id <= 0 || id >= (int64_t)p->nodes_cap) return NULL;
  NodeRec* n = get_node(p, id);
  if (!n) return NULL;
  n->id = id;
  n->tag = tag;
  n->type_ref = type_ref;
  n->fields = fields;
  return n;
}

static bool ensure_node_slot(SirProgram* p, int64_t id) {
  if (!p) return false;
  if (id <= 0) return false;
  if ((size_t)id < p->nodes_cap && p->nodes && p->nodes[id]) return true;
  size_t need = (size_t)id + 1;
  if (need <= p->nodes_cap) {
    // slot exists but entry is NULL
  } else {
    size_t ncap = p->nodes_cap ? p->nodes_cap : 64;
    while (ncap < need) ncap *= 2;
    NodeRec** np = (NodeRec**)realloc(p->nodes, ncap * sizeof(NodeRec*));
    if (!np) return false;
    for (size_t i = p->nodes_cap; i < ncap; i++) np[i] = NULL;
    p->nodes = np;
    p->nodes_cap = ncap;
  }
  if (!p->nodes[id]) {
    NodeRec* nr = (NodeRec*)arena_alloc(&p->arena, sizeof(*nr));
    if (!nr) return false;
    memset(nr, 0, sizeof(*nr));
    nr->id = id;
    p->nodes[id] = nr;
  }
  return true;
}

static int64_t infer_branch_type(SirProgram* p, const BranchOperand* br) {
  if (!p || !br || br->node_id == 0) return 0;
  NodeRec* n = get_node(p, br->node_id);
  if (!n) return 0;
  if (br->kind == BRANCH_VAL) return n->type_ref;
  if (br->kind == BRANCH_THUNK) {
    if (n->type_ref == 0) return 0;
    TypeRec* t = get_type(p, n->type_ref);
    if (!t) return 0;
    if (t->kind == TYPE_FUN) {
      TypeRec* sig = get_type(p, t->sig);
      if (!sig || sig->kind != TYPE_FN) return 0;
      return sig->ret;
    }
    if (t->kind == TYPE_CLOSURE) {
      TypeRec* sig = get_type(p, t->call_sig);
      if (!sig || sig->kind != TYPE_FN) return 0;
      return sig->ret;
    }
  }
  return 0;
}

static int64_t find_prim_type_id(SirProgram* p, const char* prim) {
  if (!p || !prim) return 0;
  for (size_t i = 0; i < p->types_cap; i++) {
    TypeRec* t = p->types ? p->types[i] : NULL;
    if (!t) continue;
    if (t->kind != TYPE_PRIM || !t->prim) continue;
    if (strcmp(t->prim, prim) == 0) return t->id;
  }
  return 0;
}

static bool get_callable_sig(SirProgram* p, int64_t callee_node_id, TypeRec** out_sig, bool* out_is_closure) {
  if (out_sig) *out_sig = NULL;
  if (out_is_closure) *out_is_closure = false;
  if (!p) return false;
  NodeRec* callee = get_node(p, callee_node_id);
  if (!callee || callee->type_ref == 0) return false;
  TypeRec* ct = get_type(p, callee->type_ref);
  if (!ct) return false;
  if (ct->kind == TYPE_FUN) {
    TypeRec* sig = get_type(p, ct->sig);
    if (!sig || sig->kind != TYPE_FN) return false;
    if (out_sig) *out_sig = sig;
    if (out_is_closure) *out_is_closure = false;
    return true;
  }
  if (ct->kind == TYPE_CLOSURE) {
    TypeRec* sig = get_type(p, ct->call_sig);
    if (!sig || sig->kind != TYPE_FN) return false;
    if (out_sig) *out_sig = sig;
    if (out_is_closure) *out_is_closure = true;
    return true;
  }
  return false;
}

static bool make_call_thunk(SirProgram* p, int64_t base_id, const char* suffix, int64_t callee_node_id, int64_t result_ty,
                            int64_t* out_call_id) {
  if (!p || base_id == 0 || !suffix || !out_call_id) return false;
  *out_call_id = 0;
  NodeRec* callee = get_node(p, callee_node_id);
  if (!callee || callee->type_ref == 0) return false;
  TypeRec* ct = get_type(p, callee->type_ref);
  if (!ct) return false;

  const char* tag = NULL;
  if (ct->kind == TYPE_FUN) tag = "call.fun";
  else if (ct->kind == TYPE_CLOSURE) tag = "call.closure";
  else return false;

  const int64_t call_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, base_id, suffix), "call thunk id");
  if (!call_id) return false;

  JsonValue* args = jv_make_arr(&p->arena, 1);
  if (!args) return false;
  args->v.arr.items[0] = jv_make_ref(&p->arena, callee_node_id);
  if (!args->v.arr.items[0]) return false;

  JsonValue* fields = jv_make_obj(&p->arena, 1);
  if (!fields) return false;
  fields->v.obj.items[0].key = "args";
  fields->v.obj.items[0].value = args;

  make_node_stub(p, call_id, tag, result_ty, fields);
  *out_call_id = call_id;
  return true;
}

static bool make_call_thunk_with_payload(SirProgram* p, int64_t base_id, const char* suffix, int64_t callee_node_id, int64_t payload_node_id,
                                        int64_t result_ty, int64_t* out_call_id) {
  if (!p || base_id == 0 || !suffix || !out_call_id) return false;
  *out_call_id = 0;

  TypeRec* sig = NULL;
  bool is_closure = false;
  if (!get_callable_sig(p, callee_node_id, &sig, &is_closure)) return false;
  if (!sig) return false;

  size_t argc = 1;
  if (sig->param_len == 1) argc = 2;
  if (sig->param_len != 0 && sig->param_len != 1) return false;
  if (sig->param_len == 1 && payload_node_id == 0) return false;

  const char* tag = is_closure ? "call.closure" : "call.fun";

  const int64_t call_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, base_id, suffix), "call thunk id");
  if (!call_id) return false;

  JsonValue* args = jv_make_arr(&p->arena, argc);
  if (!args) return false;
  args->v.arr.items[0] = jv_make_ref(&p->arena, callee_node_id);
  if (!args->v.arr.items[0]) return false;
  if (argc == 2) {
    args->v.arr.items[1] = jv_make_ref(&p->arena, payload_node_id);
    if (!args->v.arr.items[1]) return false;
  }

  JsonValue* fields = jv_make_obj(&p->arena, 1);
  if (!fields) return false;
  fields->v.obj.items[0].key = "args";
  fields->v.obj.items[0].value = args;

  make_node_stub(p, call_id, tag, result_ty, fields);
  *out_call_id = call_id;
  return true;
}

static JsonValue* block_fields_with_stmts(SirProgram* p, JsonValue* old_fields, JsonValue* new_stmts, JsonValue* new_params) {
  if (!p || !new_stmts || new_stmts->type != JSON_ARRAY) return NULL;
  // Preserve existing keys other than stmts/params, then set params/stmts.
  size_t keep = 0;
  if (old_fields && old_fields->type == JSON_OBJECT) {
    for (size_t i = 0; i < old_fields->v.obj.len; i++) {
      const char* k = old_fields->v.obj.items[i].key;
      if (!k) continue;
      if (strcmp(k, "stmts") == 0) continue;
      if (strcmp(k, "params") == 0) continue;
      keep++;
    }
  }
  size_t add = 1; // stmts
  if (new_params) add++;
  JsonValue* o = jv_make_obj(&p->arena, keep + add);
  if (!o) return NULL;
  size_t wi = 0;
  if (old_fields && old_fields->type == JSON_OBJECT) {
    for (size_t i = 0; i < old_fields->v.obj.len; i++) {
      const char* k = old_fields->v.obj.items[i].key;
      if (!k) continue;
      if (strcmp(k, "stmts") == 0) continue;
      if (strcmp(k, "params") == 0) continue;
      o->v.obj.items[wi++] = old_fields->v.obj.items[i];
    }
  }
  if (new_params) {
    o->v.obj.items[wi].key = "params";
    o->v.obj.items[wi].value = new_params;
    wi++;
  }
  o->v.obj.items[wi].key = "stmts";
  o->v.obj.items[wi].value = new_stmts;
  wi++;
  o->v.obj.len = wi;
  return o;
}

static JsonValue* fn_fields_to_cfg(SirProgram* p, JsonValue* old_fields, int64_t entry_block_id, int64_t* block_ids, size_t block_len) {
  if (!p || !old_fields || old_fields->type != JSON_OBJECT) return NULL;
  size_t keep = 0;
  for (size_t i = 0; i < old_fields->v.obj.len; i++) {
    const char* k = old_fields->v.obj.items[i].key;
    if (!k) continue;
    if (strcmp(k, "body") == 0) continue;
    if (strcmp(k, "entry") == 0) continue;
    if (strcmp(k, "blocks") == 0) continue;
    keep++;
  }
  JsonValue* fields = jv_make_obj(&p->arena, keep + 2);
  if (!fields) return NULL;
  size_t wi = 0;
  for (size_t i = 0; i < old_fields->v.obj.len; i++) {
    const char* k = old_fields->v.obj.items[i].key;
    if (!k) continue;
    if (strcmp(k, "body") == 0) continue;
    if (strcmp(k, "entry") == 0) continue;
    if (strcmp(k, "blocks") == 0) continue;
    fields->v.obj.items[wi++] = old_fields->v.obj.items[i];
  }
  fields->v.obj.items[wi].key = "entry";
  fields->v.obj.items[wi].value = jv_make_ref(&p->arena, entry_block_id);
  wi++;
  JsonValue* blks = jv_make_arr(&p->arena, block_len);
  if (!blks) return NULL;
  for (size_t i = 0; i < block_len; i++) {
    blks->v.arr.items[i] = jv_make_ref(&p->arena, block_ids[i]);
    if (!blks->v.arr.items[i]) return NULL;
  }
  fields->v.obj.items[wi].key = "blocks";
  fields->v.obj.items[wi].value = blks;
  wi++;
  fields->v.obj.len = wi;
  return fields;
}

static JsonValue* fn_fields_with_blocks(SirProgram* p, JsonValue* old_fields, JsonValue* new_blocks) {
  if (!p || !old_fields || old_fields->type != JSON_OBJECT) return NULL;
  if (!new_blocks || new_blocks->type != JSON_ARRAY) return NULL;
  size_t keep = 0;
  bool have_blocks = false;
  for (size_t i = 0; i < old_fields->v.obj.len; i++) {
    const char* k = old_fields->v.obj.items[i].key;
    if (!k) continue;
    if (strcmp(k, "blocks") == 0) {
      have_blocks = true;
      keep++;
      continue;
    }
    keep++;
  }
  if (!have_blocks) return NULL;

  JsonValue* fields = jv_make_obj(&p->arena, keep);
  if (!fields) return NULL;
  size_t wi = 0;
  for (size_t i = 0; i < old_fields->v.obj.len; i++) {
    const char* k = old_fields->v.obj.items[i].key;
    if (!k) continue;
    if (strcmp(k, "blocks") == 0) {
      fields->v.obj.items[wi].key = "blocks";
      fields->v.obj.items[wi].value = new_blocks;
      wi++;
      continue;
    }
    fields->v.obj.items[wi++] = old_fields->v.obj.items[i];
  }
  fields->v.obj.len = wi;
  return fields;
}

static bool cfg_fn_append_blocks(SirProgram* p, NodeRec* fn, const int64_t* add_block_ids, size_t add_len) {
  if (!p || !fn || !fn->fields || !add_block_ids || add_len == 0) return false;
  JsonValue* blocks = json_obj_get(fn->fields, "blocks");
  if (!blocks || blocks->type != JSON_ARRAY) return false;
  JsonValue* new_blocks = jv_make_arr(&p->arena, blocks->v.arr.len + add_len);
  if (!new_blocks) return false;
  for (size_t i = 0; i < blocks->v.arr.len; i++) new_blocks->v.arr.items[i] = blocks->v.arr.items[i];
  for (size_t i = 0; i < add_len; i++) {
    new_blocks->v.arr.items[blocks->v.arr.len + i] = jv_make_ref(&p->arena, add_block_ids[i]);
    if (!new_blocks->v.arr.items[blocks->v.arr.len + i]) return false;
  }
  JsonValue* nf = fn_fields_with_blocks(p, fn->fields, new_blocks);
  if (!nf) return false;
  fn->fields = nf;
  return true;
}

static bool lower_sem_value_to_cfg_ret(SirProgram* p, NodeRec* fn, int64_t sem_node_id, const char* sem_tag, int64_t cond_id,
                                      const BranchOperand* br_then, const BranchOperand* br_else) {
  if (!p || !fn || !fn->fields) return false;

  // Only support non-CFG functions (fields.body) for MVP.
  int64_t body_id = 0;
  if (!parse_node_ref_id(p, json_obj_get(fn->fields, "body"), &body_id)) return false;
  NodeRec* body = get_node(p, body_id);
  if (!body || !body->fields) return false;

  JsonValue* stmts = json_obj_get(body->fields, "stmts");
  if (!stmts || stmts->type != JSON_ARRAY || stmts->v.arr.len == 0) return false;

  // Require last stmt is term.ret/return and returns sem_node_id.
  JsonValue* last = stmts->v.arr.items[stmts->v.arr.len - 1];
  int64_t term_id = 0;
  if (!parse_node_ref_id(p, last, &term_id)) return false;
  NodeRec* term = get_node(p, term_id);
  if (!term || !term->fields || !(strcmp(term->tag, "term.ret") == 0 || strcmp(term->tag, "return") == 0)) return false;
  int64_t got = 0;
  if (!parse_node_ref_id(p, json_obj_get(term->fields, "value"), &got)) return false;
  if (got != sem_node_id) return false;

  NodeRec* semn = get_node(p, sem_node_id);
  if (!semn) return false;
  int64_t result_ty = semn->type_ref;
  if (result_ty == 0) {
    result_ty = infer_branch_type(p, br_then);
    if (!result_ty) result_ty = infer_branch_type(p, br_else);
  }
  if (result_ty == 0) {
    SIRCC_ERR_NODE(p, semn, "sircc.lower_hl.sem.type_missing", "sircc: --lower-hl could not infer result type for %s", sem_tag);
    return false;
  }
  TypeRec* rty = get_type(p, result_ty);
  if (rty && rty->kind == TYPE_PRIM && rty->prim && strcmp(rty->prim, "void") == 0) {
    SIRCC_ERR_NODE(p, semn, "sircc.lower_hl.sem.void_unsupported", "sircc: --lower-hl does not support %s returning void yet", sem_tag);
    return false;
  }

  // Reuse sem node id as the join bparam (this strips sem.* from output).
  semn->tag = "bparam";
  semn->type_ref = result_ty;
  semn->fields = NULL;

  // Create join/then/else blocks.
  const int64_t then_bid = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, sem_node_id, "if.then.block"), "if then block id");
  const int64_t else_bid = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, sem_node_id, "if.else.block"), "if else block id");
  const int64_t join_bid = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, sem_node_id, "if.join.block"), "if join block id");
  if (!then_bid || !else_bid || !join_bid) return false;

  // Resolve then/else branch value node ids (materialize thunk calls inside the branch block via call nodes).
  int64_t then_val_id = 0, else_val_id = 0;
  if (br_then->kind == BRANCH_VAL) {
    then_val_id = br_then->node_id;
  } else {
    if (!make_call_thunk(p, sem_node_id, "if.then.call", br_then->node_id, result_ty, &then_val_id)) {
      SIRCC_ERR_NODE_ID(p, sem_node_id, sem_tag, "sircc.lower_hl.sem.thunk.bad", "sircc: invalid thunk in then branch");
      return false;
    }
  }
  if (br_else->kind == BRANCH_VAL) {
    else_val_id = br_else->node_id;
  } else {
    if (!make_call_thunk(p, sem_node_id, "if.else.call", br_else->node_id, result_ty, &else_val_id)) {
      SIRCC_ERR_NODE_ID(p, sem_node_id, sem_tag, "sircc.lower_hl.sem.thunk.bad", "sircc: invalid thunk in else branch");
      return false;
    }
  }

  // Create term.br in then/else to join, passing the value.
  const int64_t then_br_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, sem_node_id, "if.then.br"), "if then br id");
  const int64_t else_br_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, sem_node_id, "if.else.br"), "if else br id");
  if (!then_br_id || !else_br_id) return false;

  JsonValue* then_args = jv_make_arr(&p->arena, 1);
  JsonValue* else_args = jv_make_arr(&p->arena, 1);
  if (!then_args || !else_args) return false;
  then_args->v.arr.items[0] = jv_make_ref(&p->arena, then_val_id);
  else_args->v.arr.items[0] = jv_make_ref(&p->arena, else_val_id);
  if (!then_args->v.arr.items[0] || !else_args->v.arr.items[0]) return false;

  JsonValue* then_to = jv_make_obj(&p->arena, 1);
  JsonValue* else_to = jv_make_obj(&p->arena, 1);
  if (!then_to || !else_to) return false;
  then_to->v.obj.items[0].key = "to";
  then_to->v.obj.items[0].value = jv_make_ref(&p->arena, join_bid);
  else_to->v.obj.items[0].key = "to";
  else_to->v.obj.items[0].value = jv_make_ref(&p->arena, join_bid);
  if (!then_to->v.obj.items[0].value || !else_to->v.obj.items[0].value) return false;

  JsonValue* then_fields = jv_make_obj(&p->arena, 2);
  JsonValue* else_fields = jv_make_obj(&p->arena, 2);
  if (!then_fields || !else_fields) return false;
  then_fields->v.obj.items[0].key = "to";
  then_fields->v.obj.items[0].value = jv_make_ref(&p->arena, join_bid);
  then_fields->v.obj.items[1].key = "args";
  then_fields->v.obj.items[1].value = then_args;
  else_fields->v.obj.items[0].key = "to";
  else_fields->v.obj.items[0].value = jv_make_ref(&p->arena, join_bid);
  else_fields->v.obj.items[1].key = "args";
  else_fields->v.obj.items[1].value = else_args;

  make_node_stub(p, then_br_id, "term.br", 0, then_fields);
  make_node_stub(p, else_br_id, "term.br", 0, else_fields);

  // Then/else blocks with their terminators.
  JsonValue* then_stmts = jv_make_arr(&p->arena, 1);
  JsonValue* else_stmts = jv_make_arr(&p->arena, 1);
  if (!then_stmts || !else_stmts) return false;
  then_stmts->v.arr.items[0] = jv_make_ref(&p->arena, then_br_id);
  else_stmts->v.arr.items[0] = jv_make_ref(&p->arena, else_br_id);
  if (!then_stmts->v.arr.items[0] || !else_stmts->v.arr.items[0]) return false;

  JsonValue* then_block_fields = block_fields_with_stmts(p, NULL, then_stmts, NULL);
  JsonValue* else_block_fields = block_fields_with_stmts(p, NULL, else_stmts, NULL);
  if (!then_block_fields || !else_block_fields) return false;
  make_node_stub(p, then_bid, "block", 0, then_block_fields);
  make_node_stub(p, else_bid, "block", 0, else_block_fields);

  // Join block: params=[bparam], stmts=[term.ret(bparam)].
  JsonValue* join_params = jv_make_arr(&p->arena, 1);
  if (!join_params) return false;
  join_params->v.arr.items[0] = jv_make_ref(&p->arena, sem_node_id);
  if (!join_params->v.arr.items[0]) return false;

  // Rewrite the existing term.ret node to return the join param.
  JsonValue* ret_fields = jv_make_obj(&p->arena, 1);
  if (!ret_fields) return false;
  ret_fields->v.obj.items[0].key = "value";
  ret_fields->v.obj.items[0].value = jv_make_ref(&p->arena, sem_node_id);
  if (!ret_fields->v.obj.items[0].value) return false;
  term->tag = "term.ret";
  term->fields = ret_fields;

  JsonValue* join_stmts = jv_make_arr(&p->arena, 1);
  if (!join_stmts) return false;
  join_stmts->v.arr.items[0] = jv_make_ref(&p->arena, term_id);
  if (!join_stmts->v.arr.items[0]) return false;

  JsonValue* join_block_fields = block_fields_with_stmts(p, NULL, join_stmts, join_params);
  if (!join_block_fields) return false;
  make_node_stub(p, join_bid, "block", 0, join_block_fields);

  // Entry block: keep all stmts except the old return, then append term.cbr to then/else.
  const size_t prefix_n = stmts->v.arr.len - 1;
  JsonValue* new_entry_stmts = jv_make_arr(&p->arena, prefix_n + 1);
  if (!new_entry_stmts) return false;
  for (size_t i = 0; i < prefix_n; i++) new_entry_stmts->v.arr.items[i] = stmts->v.arr.items[i];

  const int64_t cbr_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, sem_node_id, "if.cbr"), "if cbr id");
  if (!cbr_id) return false;

  JsonValue* then_obj = jv_make_obj(&p->arena, 1);
  JsonValue* else_obj = jv_make_obj(&p->arena, 1);
  if (!then_obj || !else_obj) return false;
  then_obj->v.obj.items[0].key = "to";
  then_obj->v.obj.items[0].value = jv_make_ref(&p->arena, then_bid);
  else_obj->v.obj.items[0].key = "to";
  else_obj->v.obj.items[0].value = jv_make_ref(&p->arena, else_bid);
  if (!then_obj->v.obj.items[0].value || !else_obj->v.obj.items[0].value) return false;

  JsonValue* cbr_fields = jv_make_obj(&p->arena, 3);
  if (!cbr_fields) return false;
  cbr_fields->v.obj.items[0].key = "cond";
  cbr_fields->v.obj.items[0].value = jv_make_ref(&p->arena, cond_id);
  cbr_fields->v.obj.items[1].key = "then";
  cbr_fields->v.obj.items[1].value = then_obj;
  cbr_fields->v.obj.items[2].key = "else";
  cbr_fields->v.obj.items[2].value = else_obj;
  if (!cbr_fields->v.obj.items[0].value) return false;

  make_node_stub(p, cbr_id, "term.cbr", 0, cbr_fields);
  new_entry_stmts->v.arr.items[prefix_n] = jv_make_ref(&p->arena, cbr_id);
  if (!new_entry_stmts->v.arr.items[prefix_n]) return false;

  body->fields = block_fields_with_stmts(p, body->fields, new_entry_stmts, json_obj_get(body->fields, "params"));
  if (!body->fields) return false;

  // Rewrite fn to CFG form.
  int64_t blks[4] = {body_id, then_bid, else_bid, join_bid};
  fn->fields = fn_fields_to_cfg(p, fn->fields, body_id, blks, 4);
  if (!fn->fields) return false;
  return true;
}

static bool lower_sem_value_to_cfg_let(SirProgram* p, NodeRec* fn, int64_t sem_node_id, const char* sem_tag, int64_t cond_id,
                                      const BranchOperand* br_then, const BranchOperand* br_else, int64_t let_stmt_id) {
  if (!p || !fn || !fn->fields) return false;

  // Only support non-CFG functions (fields.body) for MVP.
  int64_t body_id = 0;
  if (!parse_node_ref_id(p, json_obj_get(fn->fields, "body"), &body_id)) return false;
  NodeRec* body = get_node(p, body_id);
  if (!body || !body->fields) return false;

  JsonValue* stmts = json_obj_get(body->fields, "stmts");
  if (!stmts || stmts->type != JSON_ARRAY || stmts->v.arr.len == 0) return false;

  // Find the let stmt index.
  size_t let_idx = (size_t)-1;
  for (size_t i = 0; i < stmts->v.arr.len; i++) {
    int64_t sid = 0;
    if (!parse_node_ref_id(p, stmts->v.arr.items[i], &sid)) continue;
    if (sid == let_stmt_id) {
      let_idx = i;
      break;
    }
  }
  if (let_idx == (size_t)-1) return false;
  if (let_idx == 0 && stmts->v.arr.len == 1) return false;

  NodeRec* letn = get_node(p, let_stmt_id);
  if (!letn || !letn->tag || strcmp(letn->tag, "let") != 0) return false;
  const char* let_name = letn->fields ? json_get_string(json_obj_get(letn->fields, "name")) : NULL;
  if (!let_name || !*let_name) return false;

  NodeRec* semn = get_node(p, sem_node_id);
  if (!semn) return false;
  int64_t result_ty = semn->type_ref;
  if (result_ty == 0) {
    result_ty = infer_branch_type(p, br_then);
    if (!result_ty) result_ty = infer_branch_type(p, br_else);
  }
  if (result_ty == 0) {
    SIRCC_ERR_NODE(p, semn, "sircc.lower_hl.sem.type_missing", "sircc: --lower-hl could not infer result type for %s", sem_tag);
    return false;
  }
  TypeRec* rty = get_type(p, result_ty);
  if (rty && rty->kind == TYPE_PRIM && rty->prim && strcmp(rty->prim, "void") == 0) {
    SIRCC_ERR_NODE(p, semn, "sircc.lower_hl.sem.void_unsupported", "sircc: --lower-hl does not support %s returning void yet", sem_tag);
    return false;
  }

  // Reuse sem node id as the continuation bparam (this strips sem.* from output).
  semn->tag = "bparam";
  semn->type_ref = result_ty;
  JsonValue* bpf = jv_make_obj(&p->arena, 1);
  if (!bpf) return false;
  bpf->v.obj.items[0].key = "name";
  bpf->v.obj.items[0].value = jv_make(&p->arena, JSON_STRING);
  if (!bpf->v.obj.items[0].value) return false;
  bpf->v.obj.items[0].value->v.s = arena_strdup(&p->arena, let_name);
  if (!bpf->v.obj.items[0].value->v.s) return false;
  semn->fields = bpf;

  // Create continuation/then/else blocks.
  const int64_t then_bid = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, sem_node_id, "if.then.block"), "if then block id");
  const int64_t else_bid = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, sem_node_id, "if.else.block"), "if else block id");
  const int64_t cont_bid = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, sem_node_id, "if.cont.block"), "if cont block id");
  if (!then_bid || !else_bid || !cont_bid) return false;

  // Continuation block params=[bparam], stmts = suffix (starting at let).
  JsonValue* cont_params = jv_make_arr(&p->arena, 1);
  if (!cont_params) return false;
  cont_params->v.arr.items[0] = jv_make_ref(&p->arena, sem_node_id);
  if (!cont_params->v.arr.items[0]) return false;

  // Suffix begins after the let statement, because the binding is provided by the block param name.
  const size_t suffix_n = (let_idx + 1 <= stmts->v.arr.len) ? (stmts->v.arr.len - (let_idx + 1)) : 0;
  JsonValue* cont_stmts = jv_make_arr(&p->arena, suffix_n);
  if (!cont_stmts) return false;
  for (size_t i = 0; i < suffix_n; i++) cont_stmts->v.arr.items[i] = stmts->v.arr.items[(let_idx + 1) + i];

  JsonValue* cont_fields = block_fields_with_stmts(p, NULL, cont_stmts, cont_params);
  if (!cont_fields) return false;
  make_node_stub(p, cont_bid, "block", 0, cont_fields);

  // Resolve then/else branch value node ids (materialize thunk calls inside the branch block).
  int64_t then_val_id = 0, else_val_id = 0;
  if (br_then->kind == BRANCH_VAL) then_val_id = br_then->node_id;
  else if (!make_call_thunk(p, sem_node_id, "if.then.call", br_then->node_id, result_ty, &then_val_id)) return false;
  if (br_else->kind == BRANCH_VAL) else_val_id = br_else->node_id;
  else if (!make_call_thunk(p, sem_node_id, "if.else.call", br_else->node_id, result_ty, &else_val_id)) return false;

  // then/else blocks: optional call stmt (if thunk), then term.br(to=cont,args=[v]).
  const int64_t then_br_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, sem_node_id, "if.then.br"), "if then br id");
  const int64_t else_br_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, sem_node_id, "if.else.br"), "if else br id");
  if (!then_br_id || !else_br_id) return false;

  JsonValue* then_br_args = jv_make_arr(&p->arena, 1);
  JsonValue* else_br_args = jv_make_arr(&p->arena, 1);
  if (!then_br_args || !else_br_args) return false;
  then_br_args->v.arr.items[0] = jv_make_ref(&p->arena, then_val_id);
  else_br_args->v.arr.items[0] = jv_make_ref(&p->arena, else_val_id);
  if (!then_br_args->v.arr.items[0] || !else_br_args->v.arr.items[0]) return false;

  JsonValue* then_br_fields = jv_make_obj(&p->arena, 2);
  JsonValue* else_br_fields = jv_make_obj(&p->arena, 2);
  if (!then_br_fields || !else_br_fields) return false;
  then_br_fields->v.obj.items[0].key = "to";
  then_br_fields->v.obj.items[0].value = jv_make_ref(&p->arena, cont_bid);
  then_br_fields->v.obj.items[1].key = "args";
  then_br_fields->v.obj.items[1].value = then_br_args;
  else_br_fields->v.obj.items[0].key = "to";
  else_br_fields->v.obj.items[0].value = jv_make_ref(&p->arena, cont_bid);
  else_br_fields->v.obj.items[1].key = "args";
  else_br_fields->v.obj.items[1].value = else_br_args;
  if (!then_br_fields->v.obj.items[0].value || !else_br_fields->v.obj.items[0].value) return false;

  make_node_stub(p, then_br_id, "term.br", 0, then_br_fields);
  make_node_stub(p, else_br_id, "term.br", 0, else_br_fields);

  const bool then_has_call = (br_then->kind == BRANCH_THUNK);
  const bool else_has_call = (br_else->kind == BRANCH_THUNK);
  JsonValue* then_stmts = jv_make_arr(&p->arena, then_has_call ? 2 : 1);
  JsonValue* else_stmts = jv_make_arr(&p->arena, else_has_call ? 2 : 1);
  if (!then_stmts || !else_stmts) return false;
  size_t ti = 0, ei = 0;
  if (then_has_call) {
    then_stmts->v.arr.items[ti++] = jv_make_ref(&p->arena, then_val_id);
    if (!then_stmts->v.arr.items[ti - 1]) return false;
  }
  then_stmts->v.arr.items[ti++] = jv_make_ref(&p->arena, then_br_id);
  if (!then_stmts->v.arr.items[ti - 1]) return false;
  if (else_has_call) {
    else_stmts->v.arr.items[ei++] = jv_make_ref(&p->arena, else_val_id);
    if (!else_stmts->v.arr.items[ei - 1]) return false;
  }
  else_stmts->v.arr.items[ei++] = jv_make_ref(&p->arena, else_br_id);
  if (!else_stmts->v.arr.items[ei - 1]) return false;

  JsonValue* then_fields = block_fields_with_stmts(p, NULL, then_stmts, NULL);
  JsonValue* else_fields = block_fields_with_stmts(p, NULL, else_stmts, NULL);
  if (!then_fields || !else_fields) return false;
  make_node_stub(p, then_bid, "block", 0, then_fields);
  make_node_stub(p, else_bid, "block", 0, else_fields);

  // Entry block: prefix stmts, then term.cbr.
  JsonValue* new_entry_stmts = jv_make_arr(&p->arena, let_idx + 1);
  if (!new_entry_stmts) return false;
  for (size_t i = 0; i < let_idx; i++) new_entry_stmts->v.arr.items[i] = stmts->v.arr.items[i];

  const int64_t cbr_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, sem_node_id, "if.cbr"), "if cbr id");
  if (!cbr_id) return false;

  JsonValue* then_obj = jv_make_obj(&p->arena, 1);
  JsonValue* else_obj = jv_make_obj(&p->arena, 1);
  if (!then_obj || !else_obj) return false;
  then_obj->v.obj.items[0].key = "to";
  then_obj->v.obj.items[0].value = jv_make_ref(&p->arena, then_bid);
  else_obj->v.obj.items[0].key = "to";
  else_obj->v.obj.items[0].value = jv_make_ref(&p->arena, else_bid);
  if (!then_obj->v.obj.items[0].value || !else_obj->v.obj.items[0].value) return false;

  JsonValue* cbr_fields = jv_make_obj(&p->arena, 3);
  if (!cbr_fields) return false;
  cbr_fields->v.obj.items[0].key = "cond";
  cbr_fields->v.obj.items[0].value = jv_make_ref(&p->arena, cond_id);
  cbr_fields->v.obj.items[1].key = "then";
  cbr_fields->v.obj.items[1].value = then_obj;
  cbr_fields->v.obj.items[2].key = "else";
  cbr_fields->v.obj.items[2].value = else_obj;
  if (!cbr_fields->v.obj.items[0].value) return false;

  make_node_stub(p, cbr_id, "term.cbr", 0, cbr_fields);
  new_entry_stmts->v.arr.items[let_idx] = jv_make_ref(&p->arena, cbr_id);
  if (!new_entry_stmts->v.arr.items[let_idx]) return false;

  body->fields = block_fields_with_stmts(p, body->fields, new_entry_stmts, json_obj_get(body->fields, "params"));
  if (!body->fields) return false;

  int64_t blks[4] = {body_id, then_bid, else_bid, cont_bid};
  fn->fields = fn_fields_to_cfg(p, fn->fields, body_id, blks, 4);
  if (!fn->fields) return false;
  return true;
}

static bool lower_sem_value_to_cfg_let_cfg(SirProgram* p, NodeRec* fn, int64_t block_id, int64_t sem_node_id, const char* sem_tag, int64_t cond_id,
                                          const BranchOperand* br_then, const BranchOperand* br_else, int64_t let_stmt_id) {
  if (!p || !fn || !fn->fields) return false;
  if (!json_obj_get(fn->fields, "entry")) return false;

  NodeRec* blk = get_node(p, block_id);
  if (!blk || !blk->fields || !blk->tag || strcmp(blk->tag, "block") != 0) return false;
  JsonValue* stmts = json_obj_get(blk->fields, "stmts");
  if (!stmts || stmts->type != JSON_ARRAY || stmts->v.arr.len == 0) return false;

  // Find the let stmt index.
  size_t let_idx = (size_t)-1;
  for (size_t i = 0; i < stmts->v.arr.len; i++) {
    int64_t sid = 0;
    if (!parse_node_ref_id(p, stmts->v.arr.items[i], &sid)) continue;
    if (sid == let_stmt_id) {
      let_idx = i;
      break;
    }
  }
  if (let_idx == (size_t)-1) return false;

  NodeRec* letn = get_node(p, let_stmt_id);
  if (!letn || !letn->tag || strcmp(letn->tag, "let") != 0) return false;
  const char* let_name = letn->fields ? json_get_string(json_obj_get(letn->fields, "name")) : NULL;
  if (!let_name || !*let_name) return false;

  NodeRec* semn = get_node(p, sem_node_id);
  if (!semn) return false;
  int64_t result_ty = semn->type_ref;
  if (result_ty == 0) {
    result_ty = infer_branch_type(p, br_then);
    if (!result_ty) result_ty = infer_branch_type(p, br_else);
  }
  if (result_ty == 0) {
    SIRCC_ERR_NODE(p, semn, "sircc.lower_hl.sem.type_missing", "sircc: --lower-hl could not infer result type for %s", sem_tag);
    return false;
  }

  // Reuse sem node id as the continuation bparam (this strips sem.* from output).
  semn->tag = "bparam";
  semn->type_ref = result_ty;
  JsonValue* bpf = jv_make_obj(&p->arena, 1);
  if (!bpf) return false;
  bpf->v.obj.items[0].key = "name";
  bpf->v.obj.items[0].value = jv_make(&p->arena, JSON_STRING);
  if (!bpf->v.obj.items[0].value) return false;
  bpf->v.obj.items[0].value->v.s = arena_strdup(&p->arena, let_name);
  if (!bpf->v.obj.items[0].value->v.s) return false;
  semn->fields = bpf;

  // Create continuation/then/else blocks.
  const int64_t then_bid = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, sem_node_id, "if.then.block"), "if then block id");
  const int64_t else_bid = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, sem_node_id, "if.else.block"), "if else block id");
  const int64_t cont_bid = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, sem_node_id, "if.cont.block"), "if cont block id");
  if (!then_bid || !else_bid || !cont_bid) return false;

  // Continuation block params=[bparam], stmts = suffix (after let).
  JsonValue* cont_params = jv_make_arr(&p->arena, 1);
  if (!cont_params) return false;
  cont_params->v.arr.items[0] = jv_make_ref(&p->arena, sem_node_id);
  if (!cont_params->v.arr.items[0]) return false;

  const size_t suffix_n = (let_idx + 1 <= stmts->v.arr.len) ? (stmts->v.arr.len - (let_idx + 1)) : 0;
  JsonValue* cont_stmts = jv_make_arr(&p->arena, suffix_n);
  if (!cont_stmts) return false;
  for (size_t i = 0; i < suffix_n; i++) cont_stmts->v.arr.items[i] = stmts->v.arr.items[(let_idx + 1) + i];

  JsonValue* cont_fields = block_fields_with_stmts(p, NULL, cont_stmts, cont_params);
  if (!cont_fields) return false;
  make_node_stub(p, cont_bid, "block", 0, cont_fields);

  // Resolve then/else branch value node ids (materialize thunk calls inside the branch block).
  int64_t then_val_id = 0, else_val_id = 0;
  if (br_then->kind == BRANCH_VAL) then_val_id = br_then->node_id;
  else if (!make_call_thunk(p, sem_node_id, "if.then.call", br_then->node_id, result_ty, &then_val_id)) return false;
  if (br_else->kind == BRANCH_VAL) else_val_id = br_else->node_id;
  else if (!make_call_thunk(p, sem_node_id, "if.else.call", br_else->node_id, result_ty, &else_val_id)) return false;

  const int64_t then_br_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, sem_node_id, "if.then.br"), "if then br id");
  const int64_t else_br_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, sem_node_id, "if.else.br"), "if else br id");
  if (!then_br_id || !else_br_id) return false;

  JsonValue* then_br_args = jv_make_arr(&p->arena, 1);
  JsonValue* else_br_args = jv_make_arr(&p->arena, 1);
  if (!then_br_args || !else_br_args) return false;
  then_br_args->v.arr.items[0] = jv_make_ref(&p->arena, then_val_id);
  else_br_args->v.arr.items[0] = jv_make_ref(&p->arena, else_val_id);
  if (!then_br_args->v.arr.items[0] || !else_br_args->v.arr.items[0]) return false;

  JsonValue* then_br_fields = jv_make_obj(&p->arena, 2);
  JsonValue* else_br_fields = jv_make_obj(&p->arena, 2);
  if (!then_br_fields || !else_br_fields) return false;
  then_br_fields->v.obj.items[0].key = "to";
  then_br_fields->v.obj.items[0].value = jv_make_ref(&p->arena, cont_bid);
  then_br_fields->v.obj.items[1].key = "args";
  then_br_fields->v.obj.items[1].value = then_br_args;
  else_br_fields->v.obj.items[0].key = "to";
  else_br_fields->v.obj.items[0].value = jv_make_ref(&p->arena, cont_bid);
  else_br_fields->v.obj.items[1].key = "args";
  else_br_fields->v.obj.items[1].value = else_br_args;
  if (!then_br_fields->v.obj.items[0].value || !else_br_fields->v.obj.items[0].value) return false;

  make_node_stub(p, then_br_id, "term.br", 0, then_br_fields);
  make_node_stub(p, else_br_id, "term.br", 0, else_br_fields);

  const bool then_has_call = (br_then->kind == BRANCH_THUNK);
  const bool else_has_call = (br_else->kind == BRANCH_THUNK);
  JsonValue* then_stmts = jv_make_arr(&p->arena, then_has_call ? 2 : 1);
  JsonValue* else_stmts = jv_make_arr(&p->arena, else_has_call ? 2 : 1);
  if (!then_stmts || !else_stmts) return false;
  size_t ti = 0, ei = 0;
  if (then_has_call) {
    then_stmts->v.arr.items[ti++] = jv_make_ref(&p->arena, then_val_id);
    if (!then_stmts->v.arr.items[ti - 1]) return false;
  }
  then_stmts->v.arr.items[ti++] = jv_make_ref(&p->arena, then_br_id);
  if (!then_stmts->v.arr.items[ti - 1]) return false;
  if (else_has_call) {
    else_stmts->v.arr.items[ei++] = jv_make_ref(&p->arena, else_val_id);
    if (!else_stmts->v.arr.items[ei - 1]) return false;
  }
  else_stmts->v.arr.items[ei++] = jv_make_ref(&p->arena, else_br_id);
  if (!else_stmts->v.arr.items[ei - 1]) return false;

  JsonValue* then_fields = block_fields_with_stmts(p, NULL, then_stmts, NULL);
  JsonValue* else_fields = block_fields_with_stmts(p, NULL, else_stmts, NULL);
  if (!then_fields || !else_fields) return false;
  make_node_stub(p, then_bid, "block", 0, then_fields);
  make_node_stub(p, else_bid, "block", 0, else_fields);

  // Entry block: prefix stmts (before let), then term.cbr.
  JsonValue* new_entry_stmts = jv_make_arr(&p->arena, let_idx + 1);
  if (!new_entry_stmts) return false;
  for (size_t i = 0; i < let_idx; i++) new_entry_stmts->v.arr.items[i] = stmts->v.arr.items[i];

  const int64_t cbr_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, sem_node_id, "if.cbr"), "if cbr id");
  if (!cbr_id) return false;

  JsonValue* then_obj = jv_make_obj(&p->arena, 1);
  JsonValue* else_obj = jv_make_obj(&p->arena, 1);
  if (!then_obj || !else_obj) return false;
  then_obj->v.obj.items[0].key = "to";
  then_obj->v.obj.items[0].value = jv_make_ref(&p->arena, then_bid);
  else_obj->v.obj.items[0].key = "to";
  else_obj->v.obj.items[0].value = jv_make_ref(&p->arena, else_bid);
  if (!then_obj->v.obj.items[0].value || !else_obj->v.obj.items[0].value) return false;

  JsonValue* cbr_fields = jv_make_obj(&p->arena, 3);
  if (!cbr_fields) return false;
  cbr_fields->v.obj.items[0].key = "cond";
  cbr_fields->v.obj.items[0].value = jv_make_ref(&p->arena, cond_id);
  cbr_fields->v.obj.items[1].key = "then";
  cbr_fields->v.obj.items[1].value = then_obj;
  cbr_fields->v.obj.items[2].key = "else";
  cbr_fields->v.obj.items[2].value = else_obj;
  if (!cbr_fields->v.obj.items[0].value) return false;

  make_node_stub(p, cbr_id, "term.cbr", 0, cbr_fields);
  new_entry_stmts->v.arr.items[let_idx] = jv_make_ref(&p->arena, cbr_id);
  if (!new_entry_stmts->v.arr.items[let_idx]) return false;

  blk->fields = block_fields_with_stmts(p, blk->fields, new_entry_stmts, json_obj_get(blk->fields, "params"));
  if (!blk->fields) return false;

  const int64_t add_blks[3] = {then_bid, else_bid, cont_bid};
  if (!cfg_fn_append_blocks(p, fn, add_blks, 3)) return false;
  return true;
}

static bool lower_sem_match_sum_to_cfg_ret(SirProgram* p, NodeRec* fn, int64_t match_node_id) {
  if (!p || !fn || !fn->fields) return false;

  int64_t body_id = 0;
  if (!parse_node_ref_id(p, json_obj_get(fn->fields, "body"), &body_id)) return false;
  NodeRec* body = get_node(p, body_id);
  if (!body || !body->fields) return false;

  JsonValue* stmts = json_obj_get(body->fields, "stmts");
  if (!stmts || stmts->type != JSON_ARRAY || stmts->v.arr.len == 0) return false;

  // Require last stmt is term.ret/return and returns match_node_id.
  JsonValue* last = stmts->v.arr.items[stmts->v.arr.len - 1];
  int64_t term_id = 0;
  if (!parse_node_ref_id(p, last, &term_id)) return false;
  NodeRec* term = get_node(p, term_id);
  if (!term || !term->fields || !(strcmp(term->tag, "term.ret") == 0 || strcmp(term->tag, "return") == 0)) return false;
  int64_t got = 0;
  if (!parse_node_ref_id(p, json_obj_get(term->fields, "value"), &got)) return false;
  if (got != match_node_id) return false;

  NodeRec* mn = get_node(p, match_node_id);
  if (!mn || !mn->fields || !mn->tag || strcmp(mn->tag, "sem.match_sum") != 0) return false;
  const int64_t result_ty = mn->type_ref;
  if (result_ty == 0) {
    SIRCC_ERR_NODE(p, mn, "sircc.lower_hl.sem.type_missing", "sircc: --lower-hl requires sem.match_sum node to have type_ref");
    return false;
  }
  TypeRec* rty = get_type(p, result_ty);
  if (rty && rty->kind == TYPE_PRIM && rty->prim && strcmp(rty->prim, "void") == 0) {
    SIRCC_ERR_NODE(p, mn, "sircc.lower_hl.sem.void_unsupported", "sircc: --lower-hl does not support sem.match_sum returning void yet");
    return false;
  }

  int64_t sum_ty_id = 0;
  if (!parse_type_ref_id(p, json_obj_get(mn->fields, "sum"), &sum_ty_id)) return false;
  TypeRec* sty = get_type(p, sum_ty_id);
  if (!sty || sty->kind != TYPE_SUM) return false;

  JsonValue* args = json_obj_get(mn->fields, "args");
  if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) return false;
  int64_t scrut_id = 0;
  if (!parse_node_ref_id(p, args->v.arr.items[0], &scrut_id)) return false;

  JsonValue* cases = json_obj_get(mn->fields, "cases");
  JsonValue* def = json_obj_get(mn->fields, "default");
  if (!cases || cases->type != JSON_ARRAY || !def || def->type != JSON_OBJECT) return false;

  const int64_t i32_ty = find_prim_type_id(p, "i32");
  if (!i32_ty) {
    SIRCC_ERR_NODE(p, mn, "sircc.lower_hl.sem.need_i32", "sircc: --lower-hl sem.match_sum requires an i32 primitive type in the module");
    return false;
  }

  // Reuse match node id as the join bparam (this strips sem.* from output).
  mn->tag = "bparam";
  mn->type_ref = result_ty;
  mn->fields = NULL;

  // Allocate blocks: one per case, plus default and join.
  const size_t case_n = cases->v.arr.len;
  int64_t* case_bids = NULL;
  if (case_n) {
    case_bids = (int64_t*)arena_alloc(&p->arena, case_n * sizeof(*case_bids));
    if (!case_bids) return false;
    memset(case_bids, 0, case_n * sizeof(*case_bids));
  }
  const int64_t def_bid = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, "match.default.block"), "match default block id");
  const int64_t join_bid = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, "match.join.block"), "match join block id");
  if (!def_bid || !join_bid) return false;

  // Build per-case blocks (materialize thunk calls inside the block if needed).
  for (size_t i = 0; i < case_n; i++) {
    JsonValue* co = cases->v.arr.items[i];
    if (!co || co->type != JSON_OBJECT) return false;
    int64_t variant = 0;
    if (!json_get_i64(json_obj_get(co, "variant"), &variant)) return false;
    if (variant < 0) return false;
    if (case_bids) {
      char suf[96];
      (void)snprintf(suf, sizeof(suf), "match.case.%lld.block", (long long)variant);
      if (case_bids[i] == 0) {
        case_bids[i] = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, suf), "match case block id");
        if (!case_bids[i]) return false;
      }
    }
    const size_t vix = (size_t)variant;
    if (vix >= sty->variant_len) return false;
    const int64_t pay_ty = sty->variants[vix].ty;

    JsonValue* body_br = json_obj_get(co, "body");
    if (!body_br || body_br->type != JSON_OBJECT) return false;
    BranchOperand br = {0};
    if (!parse_branch_operand(p, body_br, &br)) return false;

    int64_t val_id = 0;
    JsonValue* stm = NULL;
    size_t stm_n = 0;

    if (br.kind == BRANCH_VAL) {
      val_id = br.node_id;
      stm_n = 1;
      stm = jv_make_arr(&p->arena, stm_n);
      if (!stm) return false;
    } else {
      TypeRec* sig = NULL;
      bool is_closure = false;
      if (!get_callable_sig(p, br.node_id, &sig, &is_closure)) return false;
      if (!sig) return false;
      if (sig->param_len != 0 && sig->param_len != 1) return false;

      int64_t payload_id = 0;
      if (sig->param_len == 1) {
        if (pay_ty == 0) return false;
        char psuf[96];
        (void)snprintf(psuf, sizeof(psuf), "match.case.%lld.payload", (long long)variant);
        payload_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, psuf), "match payload id");
        if (!payload_id) return false;
        JsonValue* get_args = jv_make_arr(&p->arena, 1);
        if (!get_args) return false;
        get_args->v.arr.items[0] = jv_make_ref(&p->arena, scrut_id);
        if (!get_args->v.arr.items[0]) return false;
        JsonValue* flags = jv_make_obj(&p->arena, 1);
        if (!flags) return false;
        flags->v.obj.items[0].key = "variant";
        JsonValue* vn = jv_make(&p->arena, JSON_NUMBER);
        if (!vn) return false;
        vn->v.i = (int64_t)vix;
        flags->v.obj.items[0].value = vn;
        JsonValue* ty_ref = jv_make_obj(&p->arena, 3);
        if (!ty_ref) return false;
        ty_ref->v.obj.items[0].key = "t";
        ty_ref->v.obj.items[0].value = jv_make(&p->arena, JSON_STRING);
        if (!ty_ref->v.obj.items[0].value) return false;
        ty_ref->v.obj.items[0].value->v.s = "ref";
        ty_ref->v.obj.items[1].key = "k";
        ty_ref->v.obj.items[1].value = jv_make(&p->arena, JSON_STRING);
        if (!ty_ref->v.obj.items[1].value) return false;
        ty_ref->v.obj.items[1].value->v.s = "type";
        ty_ref->v.obj.items[2].key = "id";
        ty_ref->v.obj.items[2].value = jv_make(&p->arena, JSON_NUMBER);
        if (!ty_ref->v.obj.items[2].value) return false;
        ty_ref->v.obj.items[2].value->v.i = sum_ty_id;

        JsonValue* get_fields = jv_make_obj(&p->arena, 3);
        if (!get_fields) return false;
        get_fields->v.obj.items[0].key = "ty";
        get_fields->v.obj.items[0].value = ty_ref;
        get_fields->v.obj.items[1].key = "args";
        get_fields->v.obj.items[1].value = get_args;
        get_fields->v.obj.items[2].key = "flags";
        get_fields->v.obj.items[2].value = flags;
        make_node_stub(p, payload_id, "adt.get", pay_ty, get_fields);
      }

      char csuf[96];
      (void)snprintf(csuf, sizeof(csuf), "match.case.%lld.call", (long long)variant);
      if (!make_call_thunk_with_payload(p, match_node_id, csuf, br.node_id, payload_id, result_ty, &val_id)) return false;

      stm_n = (sig->param_len == 1) ? 3 : 2;
      stm = jv_make_arr(&p->arena, stm_n);
      if (!stm) return false;
      size_t wi = 0;
      if (sig->param_len == 1) {
        stm->v.arr.items[wi++] = jv_make_ref(&p->arena, payload_id);
        if (!stm->v.arr.items[wi - 1]) return false;
      }
      stm->v.arr.items[wi++] = jv_make_ref(&p->arena, val_id);
      if (!stm->v.arr.items[wi - 1]) return false;
    }

    char brsuf[96];
    (void)snprintf(brsuf, sizeof(brsuf), "match.case.%lld.br", (long long)variant);
    const int64_t br_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, brsuf), "match case br id");
    if (!br_id) return false;

    JsonValue* br_args = jv_make_arr(&p->arena, 1);
    if (!br_args) return false;
    br_args->v.arr.items[0] = jv_make_ref(&p->arena, val_id);
    if (!br_args->v.arr.items[0]) return false;

    JsonValue* br_fields = jv_make_obj(&p->arena, 2);
    if (!br_fields) return false;
    br_fields->v.obj.items[0].key = "to";
    br_fields->v.obj.items[0].value = jv_make_ref(&p->arena, join_bid);
    if (!br_fields->v.obj.items[0].value) return false;
    br_fields->v.obj.items[1].key = "args";
    br_fields->v.obj.items[1].value = br_args;

    make_node_stub(p, br_id, "term.br", 0, br_fields);

    // Append term.br to stmts.
    if (stm) {
      stm->v.arr.items[stm->v.arr.len - 1] = jv_make_ref(&p->arena, br_id);
      if (!stm->v.arr.items[stm->v.arr.len - 1]) return false;
    }

    JsonValue* block_fields = block_fields_with_stmts(p, NULL, stm, NULL);
    if (!block_fields) return false;
    make_node_stub(p, case_bids[i], "block", 0, block_fields);
  }

  // Default block.
  BranchOperand dbr = {0};
  if (!parse_branch_operand(p, def, &dbr)) return false;
  int64_t def_val_id = 0;
  JsonValue* def_stmts = NULL;
  if (dbr.kind == BRANCH_VAL) {
    def_val_id = dbr.node_id;
    def_stmts = jv_make_arr(&p->arena, 1);
    if (!def_stmts) return false;
  } else {
    if (!make_call_thunk(p, match_node_id, "match.default.call", dbr.node_id, result_ty, &def_val_id)) return false;
    def_stmts = jv_make_arr(&p->arena, 2);
    if (!def_stmts) return false;
    def_stmts->v.arr.items[0] = jv_make_ref(&p->arena, def_val_id);
    if (!def_stmts->v.arr.items[0]) return false;
  }
  const int64_t def_br_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, "match.default.br"), "match default br id");
  if (!def_br_id) return false;
  JsonValue* def_br_args = jv_make_arr(&p->arena, 1);
  if (!def_br_args) return false;
  def_br_args->v.arr.items[0] = jv_make_ref(&p->arena, def_val_id);
  if (!def_br_args->v.arr.items[0]) return false;
  JsonValue* def_br_fields = jv_make_obj(&p->arena, 2);
  if (!def_br_fields) return false;
  def_br_fields->v.obj.items[0].key = "to";
  def_br_fields->v.obj.items[0].value = jv_make_ref(&p->arena, join_bid);
  if (!def_br_fields->v.obj.items[0].value) return false;
  def_br_fields->v.obj.items[1].key = "args";
  def_br_fields->v.obj.items[1].value = def_br_args;
  make_node_stub(p, def_br_id, "term.br", 0, def_br_fields);
  def_stmts->v.arr.items[def_stmts->v.arr.len - 1] = jv_make_ref(&p->arena, def_br_id);
  if (!def_stmts->v.arr.items[def_stmts->v.arr.len - 1]) return false;
  JsonValue* def_block_fields = block_fields_with_stmts(p, NULL, def_stmts, NULL);
  if (!def_block_fields) return false;
  make_node_stub(p, def_bid, "block", 0, def_block_fields);

  // Join block: params=[bparam], stmts=[term.ret(bparam)].
  JsonValue* join_params = jv_make_arr(&p->arena, 1);
  if (!join_params) return false;
  join_params->v.arr.items[0] = jv_make_ref(&p->arena, match_node_id);
  if (!join_params->v.arr.items[0]) return false;

  JsonValue* ret_fields = jv_make_obj(&p->arena, 1);
  if (!ret_fields) return false;
  ret_fields->v.obj.items[0].key = "value";
  ret_fields->v.obj.items[0].value = jv_make_ref(&p->arena, match_node_id);
  if (!ret_fields->v.obj.items[0].value) return false;
  term->tag = "term.ret";
  term->fields = ret_fields;

  JsonValue* join_stmts = jv_make_arr(&p->arena, 1);
  if (!join_stmts) return false;
  join_stmts->v.arr.items[0] = jv_make_ref(&p->arena, term_id);
  if (!join_stmts->v.arr.items[0]) return false;
  JsonValue* join_block_fields = block_fields_with_stmts(p, NULL, join_stmts, join_params);
  if (!join_block_fields) return false;
  make_node_stub(p, join_bid, "block", 0, join_block_fields);

  // Entry: replace the old return with term.switch over adt.tag(scrut).
  const size_t prefix_n = stmts->v.arr.len - 1;
  JsonValue* new_entry_stmts = jv_make_arr(&p->arena, prefix_n + 1);
  if (!new_entry_stmts) return false;
  for (size_t i = 0; i < prefix_n; i++) new_entry_stmts->v.arr.items[i] = stmts->v.arr.items[i];

  // tag = adt.tag(scrut)
  const int64_t tag_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, "match.tag"), "match tag id");
  if (!tag_id) return false;
  JsonValue* tag_args = jv_make_arr(&p->arena, 1);
  if (!tag_args) return false;
  tag_args->v.arr.items[0] = jv_make_ref(&p->arena, scrut_id);
  if (!tag_args->v.arr.items[0]) return false;
  JsonValue* tag_fields = jv_make_obj(&p->arena, 1);
  if (!tag_fields) return false;
  tag_fields->v.obj.items[0].key = "args";
  tag_fields->v.obj.items[0].value = tag_args;
  make_node_stub(p, tag_id, "adt.tag", i32_ty, tag_fields);

  // Build switch cases.
  const int64_t sw_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, "match.switch"), "match switch id");
  if (!sw_id) return false;

  JsonValue* sw_cases = jv_make_arr(&p->arena, case_n);
  if (!sw_cases) return false;

  for (size_t i = 0; i < case_n; i++) {
    JsonValue* co = cases->v.arr.items[i];
    if (!co || co->type != JSON_OBJECT) return false;
    int64_t variant = 0;
    if (!json_get_i64(json_obj_get(co, "variant"), &variant)) return false;
    if (variant < 0) return false;

    char lsuf[96];
    (void)snprintf(lsuf, sizeof(lsuf), "match.case.%lld.lit", (long long)variant);
    const int64_t lit_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, lsuf), "match case lit id");
    if (!lit_id) return false;
    JsonValue* lit_fields = jv_make_obj(&p->arena, 1);
    if (!lit_fields) return false;
    lit_fields->v.obj.items[0].key = "value";
    JsonValue* ln = jv_make(&p->arena, JSON_NUMBER);
    if (!ln) return false;
    ln->v.i = variant;
    lit_fields->v.obj.items[0].value = ln;
    make_node_stub(p, lit_id, "const.i32", i32_ty, lit_fields);

    JsonValue* entry = jv_make_obj(&p->arena, 2);
    if (!entry) return false;
    entry->v.obj.items[0].key = "lit";
    entry->v.obj.items[0].value = jv_make_ref(&p->arena, lit_id);
    if (!entry->v.obj.items[0].value) return false;
    entry->v.obj.items[1].key = "to";
    entry->v.obj.items[1].value = jv_make_ref(&p->arena, case_bids[i]);
    if (!entry->v.obj.items[1].value) return false;
    sw_cases->v.arr.items[i] = entry;
  }

  JsonValue* sw_def = jv_make_obj(&p->arena, 1);
  if (!sw_def) return false;
  sw_def->v.obj.items[0].key = "to";
  sw_def->v.obj.items[0].value = jv_make_ref(&p->arena, def_bid);
  if (!sw_def->v.obj.items[0].value) return false;

  JsonValue* scrut_ref = jv_make_ref(&p->arena, tag_id);
  if (!scrut_ref) return false;

  JsonValue* sw_fields = jv_make_obj(&p->arena, 3);
  if (!sw_fields) return false;
  sw_fields->v.obj.items[0].key = "scrut";
  sw_fields->v.obj.items[0].value = scrut_ref;
  sw_fields->v.obj.items[1].key = "cases";
  sw_fields->v.obj.items[1].value = sw_cases;
  sw_fields->v.obj.items[2].key = "default";
  sw_fields->v.obj.items[2].value = sw_def;

  make_node_stub(p, sw_id, "term.switch", 0, sw_fields);
  new_entry_stmts->v.arr.items[prefix_n] = jv_make_ref(&p->arena, sw_id);
  if (!new_entry_stmts->v.arr.items[prefix_n]) return false;

  body->fields = block_fields_with_stmts(p, body->fields, new_entry_stmts, json_obj_get(body->fields, "params"));
  if (!body->fields) return false;

  // Rewrite fn to CFG form.
  const size_t blk_n = 1 + case_n + 2; // entry + cases + default + join
  int64_t* blks = (int64_t*)arena_alloc(&p->arena, blk_n * sizeof(*blks));
  if (!blks) return false;
  size_t wi = 0;
  blks[wi++] = body_id;
  for (size_t i = 0; i < case_n; i++) blks[wi++] = case_bids[i];
  blks[wi++] = def_bid;
  blks[wi++] = join_bid;
  fn->fields = fn_fields_to_cfg(p, fn->fields, body_id, blks, blk_n);
  if (!fn->fields) return false;

  return true;
}

static bool lower_sem_match_sum_to_cfg_let(SirProgram* p, NodeRec* fn, int64_t match_node_id, int64_t let_stmt_id) {
  if (!p || !fn || !fn->fields) return false;

  int64_t body_id = 0;
  if (!parse_node_ref_id(p, json_obj_get(fn->fields, "body"), &body_id)) return false;
  NodeRec* body = get_node(p, body_id);
  if (!body || !body->fields) return false;

  JsonValue* stmts = json_obj_get(body->fields, "stmts");
  if (!stmts || stmts->type != JSON_ARRAY || stmts->v.arr.len == 0) return false;

  // Find the let stmt index.
  size_t let_idx = (size_t)-1;
  for (size_t i = 0; i < stmts->v.arr.len; i++) {
    int64_t sid = 0;
    if (!parse_node_ref_id(p, stmts->v.arr.items[i], &sid)) continue;
    if (sid == let_stmt_id) {
      let_idx = i;
      break;
    }
  }
  if (let_idx == (size_t)-1) return false;
  NodeRec* letn = get_node(p, let_stmt_id);
  if (!letn || !letn->fields || !letn->tag || strcmp(letn->tag, "let") != 0) return false;
  const char* let_name = json_get_string(json_obj_get(letn->fields, "name"));
  if (!let_name || !*let_name) return false;

  NodeRec* mn = get_node(p, match_node_id);
  if (!mn || !mn->fields || !mn->tag || strcmp(mn->tag, "sem.match_sum") != 0) return false;
  const int64_t result_ty = mn->type_ref;
  if (result_ty == 0) {
    SIRCC_ERR_NODE(p, mn, "sircc.lower_hl.sem.type_missing", "sircc: --lower-hl requires sem.match_sum node to have type_ref");
    return false;
  }
  TypeRec* rty = get_type(p, result_ty);
  if (rty && rty->kind == TYPE_PRIM && rty->prim && strcmp(rty->prim, "void") == 0) {
    SIRCC_ERR_NODE(p, mn, "sircc.lower_hl.sem.void_unsupported", "sircc: --lower-hl does not support sem.match_sum returning void yet");
    return false;
  }

  int64_t sum_ty_id = 0;
  if (!parse_type_ref_id(p, json_obj_get(mn->fields, "sum"), &sum_ty_id)) return false;
  TypeRec* sty = get_type(p, sum_ty_id);
  if (!sty || sty->kind != TYPE_SUM) return false;

  JsonValue* args = json_obj_get(mn->fields, "args");
  if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) return false;
  int64_t scrut_id = 0;
  if (!parse_node_ref_id(p, args->v.arr.items[0], &scrut_id)) return false;

  JsonValue* cases = json_obj_get(mn->fields, "cases");
  JsonValue* def = json_obj_get(mn->fields, "default");
  if (!cases || cases->type != JSON_ARRAY || !def || def->type != JSON_OBJECT) return false;

  const int64_t i32_ty = find_prim_type_id(p, "i32");
  if (!i32_ty) {
    SIRCC_ERR_NODE(p, mn, "sircc.lower_hl.sem.need_i32", "sircc: --lower-hl sem.match_sum requires an i32 primitive type in the module");
    return false;
  }

  // Reuse match node id as the continuation bparam (this strips sem.* from output).
  mn->tag = "bparam";
  mn->type_ref = result_ty;
  JsonValue* bpf = jv_make_obj(&p->arena, 1);
  if (!bpf) return false;
  bpf->v.obj.items[0].key = "name";
  bpf->v.obj.items[0].value = jv_make(&p->arena, JSON_STRING);
  if (!bpf->v.obj.items[0].value) return false;
  bpf->v.obj.items[0].value->v.s = arena_strdup(&p->arena, let_name);
  if (!bpf->v.obj.items[0].value->v.s) return false;
  mn->fields = bpf;

  // Continuation block params=[bparam], stmts = suffix (starting at let).
  const int64_t cont_bid = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, "match.cont.block"), "match cont block id");
  if (!cont_bid) return false;

  JsonValue* cont_params = jv_make_arr(&p->arena, 1);
  if (!cont_params) return false;
  cont_params->v.arr.items[0] = jv_make_ref(&p->arena, match_node_id);
  if (!cont_params->v.arr.items[0]) return false;

  // Suffix begins after the let statement, because the binding is provided by the block param name.
  const size_t suffix_n = (let_idx + 1 <= stmts->v.arr.len) ? (stmts->v.arr.len - (let_idx + 1)) : 0;
  JsonValue* cont_stmts = jv_make_arr(&p->arena, suffix_n);
  if (!cont_stmts) return false;
  for (size_t i = 0; i < suffix_n; i++) cont_stmts->v.arr.items[i] = stmts->v.arr.items[(let_idx + 1) + i];

  JsonValue* cont_fields = block_fields_with_stmts(p, NULL, cont_stmts, cont_params);
  if (!cont_fields) return false;
  make_node_stub(p, cont_bid, "block", 0, cont_fields);

  // Allocate blocks: one per case, plus default.
  const size_t case_n = cases->v.arr.len;
  int64_t* case_bids = NULL;
  if (case_n) {
    case_bids = (int64_t*)arena_alloc(&p->arena, case_n * sizeof(*case_bids));
    if (!case_bids) return false;
    memset(case_bids, 0, case_n * sizeof(*case_bids));
  }
  for (size_t i = 0; i < case_n; i++) {
    // allocated per-case once we know the variant value
    case_bids[i] = 0;
  }
  const int64_t def_bid = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, "match.default.block"), "match default block id");
  if (!def_bid) return false;

  // Build per-case blocks.
  for (size_t i = 0; i < case_n; i++) {
    JsonValue* co = cases->v.arr.items[i];
    if (!co || co->type != JSON_OBJECT) return false;
    int64_t variant = 0;
    if (!json_get_i64(json_obj_get(co, "variant"), &variant)) return false;
    if (variant < 0) return false;
    if (case_bids) {
      char suf[96];
      (void)snprintf(suf, sizeof(suf), "match.case.%lld.block", (long long)variant);
      if (case_bids[i] == 0) {
        case_bids[i] = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, suf), "match case block id");
        if (!case_bids[i]) return false;
      }
    }
    const size_t vix = (size_t)variant;
    if (vix >= sty->variant_len) return false;
    const int64_t pay_ty = sty->variants[vix].ty;

    JsonValue* body_br = json_obj_get(co, "body");
    if (!body_br || body_br->type != JSON_OBJECT) return false;
    BranchOperand br = {0};
    if (!parse_branch_operand(p, body_br, &br)) return false;

    int64_t val_id = 0;
    JsonValue* stm = NULL;
    size_t stm_n = 0;

    if (br.kind == BRANCH_VAL) {
      val_id = br.node_id;
      stm_n = 1;
      stm = jv_make_arr(&p->arena, stm_n);
      if (!stm) return false;
    } else {
      TypeRec* sig = NULL;
      bool is_closure = false;
      if (!get_callable_sig(p, br.node_id, &sig, &is_closure)) return false;
      if (!sig) return false;
      if (sig->param_len != 0 && sig->param_len != 1) return false;

      int64_t payload_id = 0;
      if (sig->param_len == 1) {
        if (pay_ty == 0) return false;
        char psuf[96];
        (void)snprintf(psuf, sizeof(psuf), "match.case.%lld.payload", (long long)variant);
        payload_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, psuf), "match payload id");
        if (!payload_id) return false;
        JsonValue* get_args = jv_make_arr(&p->arena, 1);
        if (!get_args) return false;
        get_args->v.arr.items[0] = jv_make_ref(&p->arena, scrut_id);
        if (!get_args->v.arr.items[0]) return false;
        JsonValue* flags = jv_make_obj(&p->arena, 1);
        if (!flags) return false;
        flags->v.obj.items[0].key = "variant";
        JsonValue* vn = jv_make(&p->arena, JSON_NUMBER);
        if (!vn) return false;
        vn->v.i = (int64_t)vix;
        flags->v.obj.items[0].value = vn;
        JsonValue* ty_ref = jv_make_obj(&p->arena, 3);
        if (!ty_ref) return false;
        ty_ref->v.obj.items[0].key = "t";
        ty_ref->v.obj.items[0].value = jv_make(&p->arena, JSON_STRING);
        if (!ty_ref->v.obj.items[0].value) return false;
        ty_ref->v.obj.items[0].value->v.s = "ref";
        ty_ref->v.obj.items[1].key = "k";
        ty_ref->v.obj.items[1].value = jv_make(&p->arena, JSON_STRING);
        if (!ty_ref->v.obj.items[1].value) return false;
        ty_ref->v.obj.items[1].value->v.s = "type";
        ty_ref->v.obj.items[2].key = "id";
        ty_ref->v.obj.items[2].value = jv_make(&p->arena, JSON_NUMBER);
        if (!ty_ref->v.obj.items[2].value) return false;
        ty_ref->v.obj.items[2].value->v.i = sum_ty_id;

        JsonValue* get_fields = jv_make_obj(&p->arena, 3);
        if (!get_fields) return false;
        get_fields->v.obj.items[0].key = "ty";
        get_fields->v.obj.items[0].value = ty_ref;
        get_fields->v.obj.items[1].key = "args";
        get_fields->v.obj.items[1].value = get_args;
        get_fields->v.obj.items[2].key = "flags";
        get_fields->v.obj.items[2].value = flags;
        make_node_stub(p, payload_id, "adt.get", pay_ty, get_fields);
      }

      char csuf[96];
      (void)snprintf(csuf, sizeof(csuf), "match.case.%lld.call", (long long)variant);
      if (!make_call_thunk_with_payload(p, match_node_id, csuf, br.node_id, payload_id, result_ty, &val_id)) return false;

      stm_n = (sig->param_len == 1) ? 3 : 2;
      stm = jv_make_arr(&p->arena, stm_n);
      if (!stm) return false;
      size_t wi = 0;
      if (sig->param_len == 1) {
        stm->v.arr.items[wi++] = jv_make_ref(&p->arena, payload_id);
        if (!stm->v.arr.items[wi - 1]) return false;
      }
      stm->v.arr.items[wi++] = jv_make_ref(&p->arena, val_id);
      if (!stm->v.arr.items[wi - 1]) return false;
    }

    char brsuf[96];
    (void)snprintf(brsuf, sizeof(brsuf), "match.case.%lld.br", (long long)variant);
    const int64_t br_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, brsuf), "match case br id");
    if (!br_id) return false;
    JsonValue* br_args = jv_make_arr(&p->arena, 1);
    if (!br_args) return false;
    br_args->v.arr.items[0] = jv_make_ref(&p->arena, val_id);
    if (!br_args->v.arr.items[0]) return false;
    JsonValue* br_fields = jv_make_obj(&p->arena, 2);
    if (!br_fields) return false;
    br_fields->v.obj.items[0].key = "to";
    br_fields->v.obj.items[0].value = jv_make_ref(&p->arena, cont_bid);
    if (!br_fields->v.obj.items[0].value) return false;
    br_fields->v.obj.items[1].key = "args";
    br_fields->v.obj.items[1].value = br_args;
    make_node_stub(p, br_id, "term.br", 0, br_fields);

    stm->v.arr.items[stm->v.arr.len - 1] = jv_make_ref(&p->arena, br_id);
    if (!stm->v.arr.items[stm->v.arr.len - 1]) return false;

    JsonValue* block_fields = block_fields_with_stmts(p, NULL, stm, NULL);
    if (!block_fields) return false;
    make_node_stub(p, case_bids[i], "block", 0, block_fields);
  }

  // Default block.
  BranchOperand dbr = {0};
  if (!parse_branch_operand(p, def, &dbr)) return false;
  int64_t def_val_id = 0;
  JsonValue* def_stmts = NULL;
  if (dbr.kind == BRANCH_VAL) {
    def_val_id = dbr.node_id;
    def_stmts = jv_make_arr(&p->arena, 1);
    if (!def_stmts) return false;
  } else {
    if (!make_call_thunk(p, match_node_id, "match.default.call", dbr.node_id, result_ty, &def_val_id)) return false;
    def_stmts = jv_make_arr(&p->arena, 2);
    if (!def_stmts) return false;
    def_stmts->v.arr.items[0] = jv_make_ref(&p->arena, def_val_id);
    if (!def_stmts->v.arr.items[0]) return false;
  }
  const int64_t def_br_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, "match.default.br"), "match default br id");
  if (!def_br_id) return false;
  JsonValue* def_br_args = jv_make_arr(&p->arena, 1);
  if (!def_br_args) return false;
  def_br_args->v.arr.items[0] = jv_make_ref(&p->arena, def_val_id);
  if (!def_br_args->v.arr.items[0]) return false;
  JsonValue* def_br_fields = jv_make_obj(&p->arena, 2);
  if (!def_br_fields) return false;
  def_br_fields->v.obj.items[0].key = "to";
  def_br_fields->v.obj.items[0].value = jv_make_ref(&p->arena, cont_bid);
  if (!def_br_fields->v.obj.items[0].value) return false;
  def_br_fields->v.obj.items[1].key = "args";
  def_br_fields->v.obj.items[1].value = def_br_args;
  make_node_stub(p, def_br_id, "term.br", 0, def_br_fields);
  def_stmts->v.arr.items[def_stmts->v.arr.len - 1] = jv_make_ref(&p->arena, def_br_id);
  if (!def_stmts->v.arr.items[def_stmts->v.arr.len - 1]) return false;
  JsonValue* def_block_fields = block_fields_with_stmts(p, NULL, def_stmts, NULL);
  if (!def_block_fields) return false;
  make_node_stub(p, def_bid, "block", 0, def_block_fields);

  // Entry: prefix stmts, then term.switch over adt.tag(scrut).
  JsonValue* new_entry_stmts = jv_make_arr(&p->arena, let_idx + 1);
  if (!new_entry_stmts) return false;
  for (size_t i = 0; i < let_idx; i++) new_entry_stmts->v.arr.items[i] = stmts->v.arr.items[i];

  // tag = adt.tag(scrut)
  const int64_t tag_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, "match.tag"), "match tag id");
  if (!tag_id) return false;
  JsonValue* tag_args = jv_make_arr(&p->arena, 1);
  if (!tag_args) return false;
  tag_args->v.arr.items[0] = jv_make_ref(&p->arena, scrut_id);
  if (!tag_args->v.arr.items[0]) return false;
  JsonValue* tag_fields = jv_make_obj(&p->arena, 1);
  if (!tag_fields) return false;
  tag_fields->v.obj.items[0].key = "args";
  tag_fields->v.obj.items[0].value = tag_args;
  make_node_stub(p, tag_id, "adt.tag", i32_ty, tag_fields);

  // Build switch cases.
  const int64_t sw_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, "match.switch"), "match switch id");
  if (!sw_id) return false;

  JsonValue* sw_cases = jv_make_arr(&p->arena, case_n);
  if (!sw_cases) return false;

  for (size_t i = 0; i < case_n; i++) {
    JsonValue* co = cases->v.arr.items[i];
    if (!co || co->type != JSON_OBJECT) return false;
    int64_t variant = 0;
    if (!json_get_i64(json_obj_get(co, "variant"), &variant)) return false;
    if (variant < 0) return false;

    char lsuf[96];
    (void)snprintf(lsuf, sizeof(lsuf), "match.case.%lld.lit", (long long)variant);
    const int64_t lit_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, lsuf), "match case lit id");
    if (!lit_id) return false;
    JsonValue* lit_fields = jv_make_obj(&p->arena, 1);
    if (!lit_fields) return false;
    lit_fields->v.obj.items[0].key = "value";
    JsonValue* ln = jv_make(&p->arena, JSON_NUMBER);
    if (!ln) return false;
    ln->v.i = variant;
    lit_fields->v.obj.items[0].value = ln;
    make_node_stub(p, lit_id, "const.i32", i32_ty, lit_fields);

    JsonValue* entry = jv_make_obj(&p->arena, 2);
    if (!entry) return false;
    entry->v.obj.items[0].key = "lit";
    entry->v.obj.items[0].value = jv_make_ref(&p->arena, lit_id);
    if (!entry->v.obj.items[0].value) return false;
    entry->v.obj.items[1].key = "to";
    entry->v.obj.items[1].value = jv_make_ref(&p->arena, case_bids[i]);
    if (!entry->v.obj.items[1].value) return false;
    sw_cases->v.arr.items[i] = entry;
  }

  JsonValue* sw_def = jv_make_obj(&p->arena, 1);
  if (!sw_def) return false;
  sw_def->v.obj.items[0].key = "to";
  sw_def->v.obj.items[0].value = jv_make_ref(&p->arena, def_bid);
  if (!sw_def->v.obj.items[0].value) return false;

  JsonValue* sw_fields = jv_make_obj(&p->arena, 3);
  if (!sw_fields) return false;
  sw_fields->v.obj.items[0].key = "scrut";
  sw_fields->v.obj.items[0].value = jv_make_ref(&p->arena, tag_id);
  if (!sw_fields->v.obj.items[0].value) return false;
  sw_fields->v.obj.items[1].key = "cases";
  sw_fields->v.obj.items[1].value = sw_cases;
  sw_fields->v.obj.items[2].key = "default";
  sw_fields->v.obj.items[2].value = sw_def;

  make_node_stub(p, sw_id, "term.switch", 0, sw_fields);
  new_entry_stmts->v.arr.items[let_idx] = jv_make_ref(&p->arena, sw_id);
  if (!new_entry_stmts->v.arr.items[let_idx]) return false;

  body->fields = block_fields_with_stmts(p, body->fields, new_entry_stmts, json_obj_get(body->fields, "params"));
  if (!body->fields) return false;

  const size_t blk_n = 1 + case_n + 2; // entry + cases + default + cont
  int64_t* blks = (int64_t*)arena_alloc(&p->arena, blk_n * sizeof(*blks));
  if (!blks) return false;
  size_t wi = 0;
  blks[wi++] = body_id;
  for (size_t i = 0; i < case_n; i++) blks[wi++] = case_bids[i];
  blks[wi++] = def_bid;
  blks[wi++] = cont_bid;
  fn->fields = fn_fields_to_cfg(p, fn->fields, body_id, blks, blk_n);
  if (!fn->fields) return false;

  return true;
}

static bool lower_sem_match_sum_to_cfg_let_cfg(SirProgram* p, NodeRec* fn, int64_t block_id, int64_t match_node_id, int64_t let_stmt_id) {
  if (!p || !fn || !fn->fields) return false;
  if (!json_obj_get(fn->fields, "entry")) return false;

  NodeRec* blk = get_node(p, block_id);
  if (!blk || !blk->fields || !blk->tag || strcmp(blk->tag, "block") != 0) return false;
  JsonValue* stmts = json_obj_get(blk->fields, "stmts");
  if (!stmts || stmts->type != JSON_ARRAY || stmts->v.arr.len == 0) return false;

  // Find the let stmt index.
  size_t let_idx = (size_t)-1;
  for (size_t i = 0; i < stmts->v.arr.len; i++) {
    int64_t sid = 0;
    if (!parse_node_ref_id(p, stmts->v.arr.items[i], &sid)) continue;
    if (sid == let_stmt_id) {
      let_idx = i;
      break;
    }
  }
  if (let_idx == (size_t)-1) return false;

  NodeRec* letn = get_node(p, let_stmt_id);
  if (!letn || !letn->fields || !letn->tag || strcmp(letn->tag, "let") != 0) return false;
  const char* let_name = json_get_string(json_obj_get(letn->fields, "name"));
  if (!let_name || !*let_name) return false;

  NodeRec* mn = get_node(p, match_node_id);
  if (!mn || !mn->fields || !mn->tag || strcmp(mn->tag, "sem.match_sum") != 0) return false;
  const int64_t result_ty = mn->type_ref;
  if (result_ty == 0) return false;

  int64_t sum_ty_id = 0;
  if (!parse_type_ref_id(p, json_obj_get(mn->fields, "sum"), &sum_ty_id)) return false;
  TypeRec* sty = get_type(p, sum_ty_id);
  if (!sty || sty->kind != TYPE_SUM) return false;

  JsonValue* args = json_obj_get(mn->fields, "args");
  if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) return false;
  int64_t scrut_id = 0;
  if (!parse_node_ref_id(p, args->v.arr.items[0], &scrut_id)) return false;

  JsonValue* cases = json_obj_get(mn->fields, "cases");
  JsonValue* def = json_obj_get(mn->fields, "default");
  if (!cases || cases->type != JSON_ARRAY || !def || def->type != JSON_OBJECT) return false;

  const int64_t i32_ty = find_prim_type_id(p, "i32");
  if (!i32_ty) return false;

  // Reuse match node id as the continuation bparam (this strips sem.* from output).
  mn->tag = "bparam";
  mn->type_ref = result_ty;
  JsonValue* bpf = jv_make_obj(&p->arena, 1);
  if (!bpf) return false;
  bpf->v.obj.items[0].key = "name";
  bpf->v.obj.items[0].value = jv_make(&p->arena, JSON_STRING);
  if (!bpf->v.obj.items[0].value) return false;
  bpf->v.obj.items[0].value->v.s = arena_strdup(&p->arena, let_name);
  if (!bpf->v.obj.items[0].value->v.s) return false;
  mn->fields = bpf;

  // Continuation block.
  const int64_t cont_bid = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, "match.cont.block"), "match cont block id");
  if (!cont_bid) return false;

  JsonValue* cont_params = jv_make_arr(&p->arena, 1);
  if (!cont_params) return false;
  cont_params->v.arr.items[0] = jv_make_ref(&p->arena, match_node_id);
  if (!cont_params->v.arr.items[0]) return false;

  const size_t suffix_n = (let_idx + 1 <= stmts->v.arr.len) ? (stmts->v.arr.len - (let_idx + 1)) : 0;
  JsonValue* cont_stmts = jv_make_arr(&p->arena, suffix_n);
  if (!cont_stmts) return false;
  for (size_t i = 0; i < suffix_n; i++) cont_stmts->v.arr.items[i] = stmts->v.arr.items[(let_idx + 1) + i];

  JsonValue* cont_fields = block_fields_with_stmts(p, NULL, cont_stmts, cont_params);
  if (!cont_fields) return false;
  make_node_stub(p, cont_bid, "block", 0, cont_fields);

  // Allocate blocks: one per case, plus default.
  const size_t case_n = cases->v.arr.len;
  int64_t* case_bids = NULL;
  if (case_n) {
    case_bids = (int64_t*)arena_alloc(&p->arena, case_n * sizeof(*case_bids));
    if (!case_bids) return false;
    memset(case_bids, 0, case_n * sizeof(*case_bids));
  }
  for (size_t i = 0; i < case_n; i++) {
    case_bids[i] = 0; // allocated once variant is known
  }
  const int64_t def_bid = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, "match.default.block"), "match default block id");
  if (!def_bid) return false;

  // Build per-case blocks.
  for (size_t i = 0; i < case_n; i++) {
    JsonValue* co = cases->v.arr.items[i];
    if (!co || co->type != JSON_OBJECT) return false;
    int64_t variant = 0;
    if (!json_get_i64(json_obj_get(co, "variant"), &variant)) return false;
    if (variant < 0) return false;
    if (case_bids) {
      char suf[96];
      (void)snprintf(suf, sizeof(suf), "match.case.%lld.block", (long long)variant);
      if (case_bids[i] == 0) {
        case_bids[i] = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, suf), "match case block id");
        if (!case_bids[i]) return false;
      }
    }
    const size_t vix = (size_t)variant;
    if (vix >= sty->variant_len) return false;
    const int64_t pay_ty = sty->variants[vix].ty;

    JsonValue* body_br = json_obj_get(co, "body");
    if (!body_br || body_br->type != JSON_OBJECT) return false;
    BranchOperand br = {0};
    if (!parse_branch_operand(p, body_br, &br)) return false;

    int64_t val_id = 0;
    JsonValue* stm = NULL;
    size_t stm_n = 0;

    if (br.kind == BRANCH_VAL) {
      val_id = br.node_id;
      stm_n = 1;
      stm = jv_make_arr(&p->arena, stm_n);
      if (!stm) return false;
    } else {
      TypeRec* sig = NULL;
      bool is_closure = false;
      if (!get_callable_sig(p, br.node_id, &sig, &is_closure)) return false;
      if (!sig) return false;
      if (sig->param_len != 0 && sig->param_len != 1) return false;

      int64_t payload_id = 0;
      if (sig->param_len == 1) {
        if (pay_ty == 0) return false;
        char psuf[96];
        (void)snprintf(psuf, sizeof(psuf), "match.case.%lld.payload", (long long)variant);
        payload_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, psuf), "match payload id");
        if (!payload_id) return false;
        JsonValue* get_args = jv_make_arr(&p->arena, 1);
        if (!get_args) return false;
        get_args->v.arr.items[0] = jv_make_ref(&p->arena, scrut_id);
        if (!get_args->v.arr.items[0]) return false;
        JsonValue* flags = jv_make_obj(&p->arena, 1);
        if (!flags) return false;
        flags->v.obj.items[0].key = "variant";
        JsonValue* vn = jv_make(&p->arena, JSON_NUMBER);
        if (!vn) return false;
        vn->v.i = (int64_t)vix;
        flags->v.obj.items[0].value = vn;
        JsonValue* ty_ref = jv_make_obj(&p->arena, 3);
        if (!ty_ref) return false;
        ty_ref->v.obj.items[0].key = "t";
        ty_ref->v.obj.items[0].value = jv_make(&p->arena, JSON_STRING);
        if (!ty_ref->v.obj.items[0].value) return false;
        ty_ref->v.obj.items[0].value->v.s = "ref";
        ty_ref->v.obj.items[1].key = "k";
        ty_ref->v.obj.items[1].value = jv_make(&p->arena, JSON_STRING);
        if (!ty_ref->v.obj.items[1].value) return false;
        ty_ref->v.obj.items[1].value->v.s = "type";
        ty_ref->v.obj.items[2].key = "id";
        ty_ref->v.obj.items[2].value = jv_make(&p->arena, JSON_NUMBER);
        if (!ty_ref->v.obj.items[2].value) return false;
        ty_ref->v.obj.items[2].value->v.i = sum_ty_id;

        JsonValue* get_fields = jv_make_obj(&p->arena, 3);
        if (!get_fields) return false;
        get_fields->v.obj.items[0].key = "ty";
        get_fields->v.obj.items[0].value = ty_ref;
        get_fields->v.obj.items[1].key = "args";
        get_fields->v.obj.items[1].value = get_args;
        get_fields->v.obj.items[2].key = "flags";
        get_fields->v.obj.items[2].value = flags;
        make_node_stub(p, payload_id, "adt.get", pay_ty, get_fields);
      }

      char csuf[96];
      (void)snprintf(csuf, sizeof(csuf), "match.case.%lld.call", (long long)variant);
      if (!make_call_thunk_with_payload(p, match_node_id, csuf, br.node_id, payload_id, result_ty, &val_id)) return false;

      stm_n = (sig->param_len == 1) ? 3 : 2;
      stm = jv_make_arr(&p->arena, stm_n);
      if (!stm) return false;
      size_t wi = 0;
      if (sig->param_len == 1) {
        stm->v.arr.items[wi++] = jv_make_ref(&p->arena, payload_id);
        if (!stm->v.arr.items[wi - 1]) return false;
      }
      stm->v.arr.items[wi++] = jv_make_ref(&p->arena, val_id);
      if (!stm->v.arr.items[wi - 1]) return false;
    }

    char brsuf[96];
    (void)snprintf(brsuf, sizeof(brsuf), "match.case.%lld.br", (long long)variant);
    const int64_t br_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, brsuf), "match case br id");
    if (!br_id) return false;
    JsonValue* br_args = jv_make_arr(&p->arena, 1);
    if (!br_args) return false;
    br_args->v.arr.items[0] = jv_make_ref(&p->arena, val_id);
    if (!br_args->v.arr.items[0]) return false;
    JsonValue* br_fields = jv_make_obj(&p->arena, 2);
    if (!br_fields) return false;
    br_fields->v.obj.items[0].key = "to";
    br_fields->v.obj.items[0].value = jv_make_ref(&p->arena, cont_bid);
    if (!br_fields->v.obj.items[0].value) return false;
    br_fields->v.obj.items[1].key = "args";
    br_fields->v.obj.items[1].value = br_args;
    make_node_stub(p, br_id, "term.br", 0, br_fields);

    stm->v.arr.items[stm->v.arr.len - 1] = jv_make_ref(&p->arena, br_id);
    if (!stm->v.arr.items[stm->v.arr.len - 1]) return false;

    JsonValue* block_fields = block_fields_with_stmts(p, NULL, stm, NULL);
    if (!block_fields) return false;
    make_node_stub(p, case_bids[i], "block", 0, block_fields);
  }

  // Default block.
  BranchOperand dbr = {0};
  if (!parse_branch_operand(p, def, &dbr)) return false;
  int64_t def_val_id = 0;
  JsonValue* def_stmts = NULL;
  if (dbr.kind == BRANCH_VAL) {
    def_val_id = dbr.node_id;
    def_stmts = jv_make_arr(&p->arena, 1);
    if (!def_stmts) return false;
  } else {
    if (!make_call_thunk(p, match_node_id, "match.default.call", dbr.node_id, result_ty, &def_val_id)) return false;
    def_stmts = jv_make_arr(&p->arena, 2);
    if (!def_stmts) return false;
    def_stmts->v.arr.items[0] = jv_make_ref(&p->arena, def_val_id);
    if (!def_stmts->v.arr.items[0]) return false;
  }
  const int64_t def_br_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, "match.default.br"), "match default br id");
  if (!def_br_id) return false;
  JsonValue* def_br_args = jv_make_arr(&p->arena, 1);
  if (!def_br_args) return false;
  def_br_args->v.arr.items[0] = jv_make_ref(&p->arena, def_val_id);
  if (!def_br_args->v.arr.items[0]) return false;
  JsonValue* def_br_fields = jv_make_obj(&p->arena, 2);
  if (!def_br_fields) return false;
  def_br_fields->v.obj.items[0].key = "to";
  def_br_fields->v.obj.items[0].value = jv_make_ref(&p->arena, cont_bid);
  if (!def_br_fields->v.obj.items[0].value) return false;
  def_br_fields->v.obj.items[1].key = "args";
  def_br_fields->v.obj.items[1].value = def_br_args;
  make_node_stub(p, def_br_id, "term.br", 0, def_br_fields);
  def_stmts->v.arr.items[def_stmts->v.arr.len - 1] = jv_make_ref(&p->arena, def_br_id);
  if (!def_stmts->v.arr.items[def_stmts->v.arr.len - 1]) return false;
  JsonValue* def_block_fields = block_fields_with_stmts(p, NULL, def_stmts, NULL);
  if (!def_block_fields) return false;
  make_node_stub(p, def_bid, "block", 0, def_block_fields);

  // Entry: prefix stmts, then term.switch over adt.tag(scrut).
  JsonValue* new_entry_stmts = jv_make_arr(&p->arena, let_idx + 1);
  if (!new_entry_stmts) return false;
  for (size_t i = 0; i < let_idx; i++) new_entry_stmts->v.arr.items[i] = stmts->v.arr.items[i];

  const int64_t tag_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, "match.tag"), "match tag id");
  if (!tag_id) return false;
  JsonValue* tag_args = jv_make_arr(&p->arena, 1);
  if (!tag_args) return false;
  tag_args->v.arr.items[0] = jv_make_ref(&p->arena, scrut_id);
  if (!tag_args->v.arr.items[0]) return false;
  JsonValue* tag_fields = jv_make_obj(&p->arena, 1);
  if (!tag_fields) return false;
  tag_fields->v.obj.items[0].key = "args";
  tag_fields->v.obj.items[0].value = tag_args;
  make_node_stub(p, tag_id, "adt.tag", i32_ty, tag_fields);

  const int64_t sw_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, "match.switch"), "match switch id");
  if (!sw_id) return false;
  JsonValue* sw_cases = jv_make_arr(&p->arena, case_n);
  if (!sw_cases) return false;

  for (size_t i = 0; i < case_n; i++) {
    JsonValue* co = cases->v.arr.items[i];
    if (!co || co->type != JSON_OBJECT) return false;
    int64_t variant = 0;
    if (!json_get_i64(json_obj_get(co, "variant"), &variant)) return false;
    if (variant < 0) return false;

    char lsuf[96];
    (void)snprintf(lsuf, sizeof(lsuf), "match.case.%lld.lit", (long long)variant);
    const int64_t lit_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, match_node_id, lsuf), "match case lit id");
    if (!lit_id) return false;
    JsonValue* lit_fields = jv_make_obj(&p->arena, 1);
    if (!lit_fields) return false;
    lit_fields->v.obj.items[0].key = "value";
    JsonValue* ln = jv_make(&p->arena, JSON_NUMBER);
    if (!ln) return false;
    ln->v.i = variant;
    lit_fields->v.obj.items[0].value = ln;
    make_node_stub(p, lit_id, "const.i32", i32_ty, lit_fields);

    JsonValue* entry = jv_make_obj(&p->arena, 2);
    if (!entry) return false;
    entry->v.obj.items[0].key = "lit";
    entry->v.obj.items[0].value = jv_make_ref(&p->arena, lit_id);
    if (!entry->v.obj.items[0].value) return false;
    entry->v.obj.items[1].key = "to";
    entry->v.obj.items[1].value = jv_make_ref(&p->arena, case_bids[i]);
    if (!entry->v.obj.items[1].value) return false;
    sw_cases->v.arr.items[i] = entry;
  }

  JsonValue* sw_def = jv_make_obj(&p->arena, 1);
  if (!sw_def) return false;
  sw_def->v.obj.items[0].key = "to";
  sw_def->v.obj.items[0].value = jv_make_ref(&p->arena, def_bid);
  if (!sw_def->v.obj.items[0].value) return false;

  JsonValue* sw_fields = jv_make_obj(&p->arena, 3);
  if (!sw_fields) return false;
  sw_fields->v.obj.items[0].key = "scrut";
  sw_fields->v.obj.items[0].value = jv_make_ref(&p->arena, tag_id);
  if (!sw_fields->v.obj.items[0].value) return false;
  sw_fields->v.obj.items[1].key = "cases";
  sw_fields->v.obj.items[1].value = sw_cases;
  sw_fields->v.obj.items[2].key = "default";
  sw_fields->v.obj.items[2].value = sw_def;
  make_node_stub(p, sw_id, "term.switch", 0, sw_fields);

  new_entry_stmts->v.arr.items[let_idx] = jv_make_ref(&p->arena, sw_id);
  if (!new_entry_stmts->v.arr.items[let_idx]) return false;

  blk->fields = block_fields_with_stmts(p, blk->fields, new_entry_stmts, json_obj_get(blk->fields, "params"));
  if (!blk->fields) return false;

  // Append blocks (cont + cases + default).
  const size_t add_n = 1 + case_n + 1;
  int64_t* add = (int64_t*)arena_alloc(&p->arena, add_n * sizeof(*add));
  if (!add) return false;
  size_t wi = 0;
  add[wi++] = cont_bid;
  for (size_t i = 0; i < case_n; i++) add[wi++] = case_bids[i];
  add[wi++] = def_bid;
  if (!cfg_fn_append_blocks(p, fn, add, add_n)) return false;

  return true;
}

static bool lower_sem_sc_to_bool_bin(SirProgram* p, NodeRec* n, bool is_and) {
  if (!p || !n || !n->fields) return false;
  JsonValue* args = json_obj_get(n->fields, "args");
  if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) return false;

  JsonValue* lhs_ref = args->v.arr.items[0];
  JsonValue* rhs_branch = args->v.arr.items[1];
  if (!lhs_ref || !rhs_branch) return false;
  if (rhs_branch->type != JSON_OBJECT) return false;

  const char* k_rhs = json_get_string(json_obj_get(rhs_branch, "kind"));
  if (!k_rhs) return false;
  if (strcmp(k_rhs, "val") != 0) {
    // Not applicable: CFG lowering handles thunk RHS (in return-position only for now).
    return false;
  }

  JsonValue* v_rhs = json_obj_get(rhs_branch, "v");
  if (!v_rhs) return false;

  JsonValue* new_args = jv_make_arr(&p->arena, 2);
  if (!new_args) {
    bump_exit_code(p, SIRCC_EXIT_INTERNAL);
    SIRCC_ERR_NODE(p, n, "sircc.oom", "sircc: out of memory");
    return false;
  }
  new_args->v.arr.items[0] = lhs_ref;
  new_args->v.arr.items[1] = v_rhs;

  JsonValue* new_fields = jv_make_obj(&p->arena, 1);
  if (!new_fields) {
    bump_exit_code(p, SIRCC_EXIT_INTERNAL);
    SIRCC_ERR_NODE(p, n, "sircc.oom", "sircc: out of memory");
    return false;
  }
  new_fields->v.obj.items[0].key = "args";
  new_fields->v.obj.items[0].value = new_args;

  n->tag = is_and ? "bool.and" : "bool.or";
  n->fields = new_fields;
  return true;
}

typedef struct HoistLetList {
  int64_t* ids;
  size_t len;
  size_t cap;
} HoistLetList;

typedef struct SemHoistMapItem {
  int64_t sem_id;
  int64_t name_id;
} SemHoistMapItem;

typedef struct SemHoistMap {
  SemHoistMapItem* items;
  size_t len;
  size_t cap;
} SemHoistMap;

static int64_t sem_hoist_map_find(const SemHoistMap* m, int64_t sem_id) {
  if (!m || sem_id == 0) return 0;
  for (size_t i = 0; i < m->len; i++) {
    if (m->items[i].sem_id == sem_id) return m->items[i].name_id;
  }
  return 0;
}

static bool sem_hoist_map_put(SemHoistMap* m, int64_t sem_id, int64_t name_id) {
  if (!m || sem_id == 0 || name_id == 0) return false;
  if (m->len == m->cap) {
    const size_t ncap = m->cap ? (m->cap * 2) : 8;
    SemHoistMapItem* np = (SemHoistMapItem*)realloc(m->items, ncap * sizeof(*np));
    if (!np) return false;
    m->items = np;
    m->cap = ncap;
  }
  m->items[m->len].sem_id = sem_id;
  m->items[m->len].name_id = name_id;
  m->len++;
  return true;
}

static bool hoist_let_list_push(SirProgram* p, HoistLetList* l, int64_t id) {
  if (!p || !l || id == 0) return false;
  if (l->len == l->cap) {
    const size_t ncap = l->cap ? (l->cap * 2) : 8;
    int64_t* np = (int64_t*)realloc(l->ids, ncap * sizeof(*np));
    if (!np) return false;
    l->ids = np;
    l->cap = ncap;
  }
  l->ids[l->len++] = id;
  return true;
}

static bool hoist_sem_in_slot(SirProgram* p, JsonValue** slot, SemHoistMap* hoisted, HoistLetList* out_lets, uint8_t* visiting,
                              size_t visiting_cap) {
  if (!p || !slot || !*slot) return true;

  int64_t ref_id = 0;
  if (parse_node_ref_id(p, *slot, &ref_id)) {
    NodeRec* n = get_node(p, ref_id);
    if (!n || !n->tag) return true;

    if (strncmp(n->tag, "sem.", 4) == 0) {
      const int64_t already = sem_hoist_map_find(hoisted, ref_id);
      if (already) {
        *slot = jv_make_ref(&p->arena, already);
        if (!*slot) return false;
        return true;
      }

      const char* sem_tag = n->tag;
      if (n->type_ref == 0) {
        bump_exit_code(p, SIRCC_EXIT_ERROR);
        SIRCC_ERR_NODE(p, n, "sircc.lower_hl.sem.type_missing", "sircc: --lower-hl could not infer result type for %s", sem_tag);
        return false;
      }

      char buf[64];
      (void)snprintf(buf, sizeof(buf), "_sircc_sem_tmp_%lld", (long long)ref_id);
      const char* tmp_name = arena_strdup(&p->arena, buf);
      if (!tmp_name) return false;

      const int64_t name_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, ref_id, "hoist.name"), "hoist name id");
      if (!name_id) return false;
      JsonValue* name_fields = jv_make_obj(&p->arena, 1);
      if (!name_fields) return false;
      name_fields->v.obj.items[0].key = "name";
      name_fields->v.obj.items[0].value = jv_make_str(&p->arena, tmp_name);
      if (!name_fields->v.obj.items[0].value) return false;
      make_node_stub(p, name_id, "name", n->type_ref, name_fields);

      const int64_t let_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, ref_id, "hoist.let"), "hoist let id");
      if (!let_id) return false;
      JsonValue* let_fields = jv_make_obj(&p->arena, 2);
      if (!let_fields) return false;
      let_fields->v.obj.items[0].key = "name";
      let_fields->v.obj.items[0].value = jv_make_str(&p->arena, tmp_name);
      if (!let_fields->v.obj.items[0].value) return false;
      let_fields->v.obj.items[1].key = "value";
      let_fields->v.obj.items[1].value = *slot; // keep the sem ref
      make_node_stub(p, let_id, "let", 0, let_fields);

      if (!hoist_let_list_push(p, out_lets, let_id)) return false;
      if (!sem_hoist_map_put(hoisted, ref_id, name_id)) return false;

      *slot = jv_make_ref(&p->arena, name_id);
      if (!*slot) return false;
      return true;
    }

    // Recurse into referenced node fields to find sem nested inside expressions.
    if (ref_id > 0 && (size_t)ref_id < visiting_cap && visiting) {
      if (visiting[ref_id]) return true;
      visiting[ref_id] = 1;
    }
    if (n->fields && n->fields->type == JSON_OBJECT) {
      for (size_t i = 0; i < n->fields->v.obj.len; i++) {
        if (!hoist_sem_in_slot(p, &n->fields->v.obj.items[i].value, hoisted, out_lets, visiting, visiting_cap)) return false;
      }
    } else if (n->fields && n->fields->type == JSON_ARRAY) {
      for (size_t i = 0; i < n->fields->v.arr.len; i++) {
        if (!hoist_sem_in_slot(p, &n->fields->v.arr.items[i], hoisted, out_lets, visiting, visiting_cap)) return false;
      }
    }
    return true;
  }

  if ((*slot)->type == JSON_ARRAY) {
    for (size_t i = 0; i < (*slot)->v.arr.len; i++) {
      if (!hoist_sem_in_slot(p, &(*slot)->v.arr.items[i], hoisted, out_lets, visiting, visiting_cap)) return false;
    }
  } else if ((*slot)->type == JSON_OBJECT) {
    for (size_t i = 0; i < (*slot)->v.obj.len; i++) {
      if (!hoist_sem_in_slot(p, &(*slot)->v.obj.items[i].value, hoisted, out_lets, visiting, visiting_cap)) return false;
    }
  }
  return true;
}

typedef struct RefVec {
  JsonValue** items;
  size_t len;
  size_t cap;
} RefVec;

static bool ref_vec_push(RefVec* v, JsonValue* it) {
  if (!v || !it) return false;
  if (v->len == v->cap) {
    const size_t ncap = v->cap ? (v->cap * 2) : 16;
    JsonValue** np = (JsonValue**)realloc(v->items, ncap * sizeof(*np));
    if (!np) return false;
    v->items = np;
    v->cap = ncap;
  }
  v->items[v->len++] = it;
  return true;
}

static bool hoist_sem_uses_in_body_fn(SirProgram* p, NodeRec* fn, bool* out_did) {
  if (out_did) *out_did = false;
  if (!p || !fn || !fn->fields || fn->fields->type != JSON_OBJECT) return false;
  if (json_obj_get(fn->fields, "entry")) return true; // not body-form

  int64_t body_id = 0;
  if (!parse_node_ref_id(p, json_obj_get(fn->fields, "body"), &body_id)) return true;
  NodeRec* body = get_node(p, body_id);
  if (!body || !body->fields) return true;
  JsonValue* stmts = json_obj_get(body->fields, "stmts");
  if (!stmts || stmts->type != JSON_ARRAY || stmts->v.arr.len == 0) return true;

  uint8_t* visiting = (uint8_t*)arena_alloc(&p->arena, p->nodes_cap ? p->nodes_cap : 1);
  if (!visiting) return false;
  memset(visiting, 0, p->nodes_cap ? p->nodes_cap : 1);

  HoistLetList lets = {0};
  SemHoistMap hoisted = {0};
  bool did = false;

  RefVec vec = {0};

  for (size_t si = 0; si < stmts->v.arr.len; si++) {
    int64_t sid = 0;
    if (!parse_node_ref_id(p, stmts->v.arr.items[si], &sid)) continue;
    NodeRec* s = get_node(p, sid);
    if (!s || !s->tag || !s->fields) continue;

    // Hoist sem.* used inside this statement, but preserve `let name = sem.*` as-is.
    if (strcmp(s->tag, "let") == 0) {
      // Recurse into the RHS node (and its children) without rewriting the let.value ref itself.
      int64_t vid = 0;
      if (parse_node_ref_id(p, json_obj_get(s->fields, "value"), &vid)) {
        NodeRec* v = get_node(p, vid);
        if (v && v->fields) {
          if (!hoist_sem_in_slot(p, &v->fields, &hoisted, &lets, visiting, p->nodes_cap)) return false;
        }
      }
    } else if (s->fields->type == JSON_OBJECT) {
      for (size_t i = 0; i < s->fields->v.obj.len; i++) {
        if (!hoist_sem_in_slot(p, &s->fields->v.obj.items[i].value, &hoisted, &lets, visiting, p->nodes_cap)) return false;
      }
    }

    if (lets.len) {
      for (size_t li = 0; li < lets.len; li++) {
        if (!ref_vec_push(&vec, jv_make_ref(&p->arena, lets.ids[li]))) return false;
      }
      lets.len = 0;
      did = true;
    }

    // Append original statement.
    if (!ref_vec_push(&vec, stmts->v.arr.items[si])) return false;
  }

  if (did) {
    JsonValue* new_stmts = jv_make_arr(&p->arena, vec.len);
    if (!new_stmts) return false;
    for (size_t i = 0; i < vec.len; i++) new_stmts->v.arr.items[i] = vec.items[i];
    body->fields = block_fields_with_stmts(p, body->fields, new_stmts, json_obj_get(body->fields, "params"));
    if (!body->fields) return false;
    if (out_did) *out_did = true;
  }

  free(vec.items);
  free(lets.ids);
  free(hoisted.items);
  return true;
}

static bool hoist_sem_uses_in_cfg_fn(SirProgram* p, NodeRec* fn, bool* out_did) {
  if (out_did) *out_did = false;
  if (!p || !fn || !fn->fields || fn->fields->type != JSON_OBJECT) return false;
  if (!json_obj_get(fn->fields, "entry")) return true;

  JsonValue* blocks = json_obj_get(fn->fields, "blocks");
  if (!blocks || blocks->type != JSON_ARRAY) return true;

  for (size_t bi = 0; bi < blocks->v.arr.len; bi++) {
    bool blk_did = false;
    int64_t bid = 0;
    if (!parse_node_ref_id(p, blocks->v.arr.items[bi], &bid)) continue;
    NodeRec* blk = get_node(p, bid);
    if (!blk || !blk->fields || !blk->tag || strcmp(blk->tag, "block") != 0) continue;

    JsonValue* stmts = json_obj_get(blk->fields, "stmts");
    if (!stmts || stmts->type != JSON_ARRAY) continue;

    uint8_t* visiting = (uint8_t*)arena_alloc(&p->arena, p->nodes_cap ? p->nodes_cap : 1);
    if (!visiting) return false;
    memset(visiting, 0, p->nodes_cap ? p->nodes_cap : 1);

    HoistLetList lets = {0};
    SemHoistMap hoisted = {0};
    RefVec vec = {0};

    for (size_t si = 0; si < stmts->v.arr.len; si++) {
      int64_t sid = 0;
      if (!parse_node_ref_id(p, stmts->v.arr.items[si], &sid)) continue;
      NodeRec* s = get_node(p, sid);
      if (!s || !s->tag || !s->fields) continue;

      if (strcmp(s->tag, "let") == 0) {
        int64_t vid = 0;
        if (parse_node_ref_id(p, json_obj_get(s->fields, "value"), &vid)) {
          NodeRec* v = get_node(p, vid);
          if (v && v->fields) {
            if (!hoist_sem_in_slot(p, &v->fields, &hoisted, &lets, visiting, p->nodes_cap)) return false;
          }
        }
      } else if (s->fields->type == JSON_OBJECT) {
        for (size_t i = 0; i < s->fields->v.obj.len; i++) {
          if (!hoist_sem_in_slot(p, &s->fields->v.obj.items[i].value, &hoisted, &lets, visiting, p->nodes_cap)) return false;
        }
      }

      if (lets.len) {
        for (size_t li = 0; li < lets.len; li++) {
          if (!ref_vec_push(&vec, jv_make_ref(&p->arena, lets.ids[li]))) return false;
        }
        lets.len = 0;
        blk_did = true;
      }

      if (!ref_vec_push(&vec, stmts->v.arr.items[si])) return false;
    }

    // Hoist sem uses inside the terminator (append lets to stmts tail).
    JsonValue* term_ref = json_obj_get(blk->fields, "term");
    if (term_ref) {
      int64_t tid = 0;
      if (parse_node_ref_id(p, term_ref, &tid)) {
        NodeRec* t = get_node(p, tid);
        if (t && t->fields) {
          if (!hoist_sem_in_slot(p, &t->fields, &hoisted, &lets, visiting, p->nodes_cap)) return false;
        }
      }
    }
    if (lets.len) {
      for (size_t li = 0; li < lets.len; li++) {
        if (!ref_vec_push(&vec, jv_make_ref(&p->arena, lets.ids[li]))) return false;
      }
      lets.len = 0;
      blk_did = true;
    }

    if (blk_did) {
      JsonValue* new_stmts = jv_make_arr(&p->arena, vec.len);
      if (!new_stmts) return false;
      for (size_t i = 0; i < vec.len; i++) new_stmts->v.arr.items[i] = vec.items[i];
      blk->fields = block_fields_with_stmts(p, blk->fields, new_stmts, json_obj_get(blk->fields, "params"));
      if (!blk->fields) return false;
      if (out_did) *out_did = true;
    }

    free(vec.items);
    free(lets.ids);
    free(hoisted.items);
  }

  return true;
}

static bool lower_one_sem_in_body_fn(SirProgram* p, NodeRec* fn, bool* out_did) {
  if (out_did) *out_did = false;
  if (!p || !fn || !fn->fields || fn->fields->type != JSON_OBJECT) return false;
  if (json_obj_get(fn->fields, "entry")) return true; // not body-form

  int64_t body_id = 0;
  if (!parse_node_ref_id(p, json_obj_get(fn->fields, "body"), &body_id)) return true;
  NodeRec* body = get_node(p, body_id);
  if (!body || !body->fields) return true;
  JsonValue* stmts = json_obj_get(body->fields, "stmts");
  if (!stmts || stmts->type != JSON_ARRAY || stmts->v.arr.len == 0) return true;

  // Let-position lowering: find `let name = sem.*(...)`.
  for (size_t si = 0; si < stmts->v.arr.len; si++) {
    int64_t sid = 0;
    if (!parse_node_ref_id(p, stmts->v.arr.items[si], &sid)) continue;
    NodeRec* s = get_node(p, sid);
    if (!s || !s->tag || strcmp(s->tag, "let") != 0) continue;
    if (!s->fields || s->fields->type != JSON_OBJECT) continue;
    int64_t vid = 0;
    if (!parse_node_ref_id(p, json_obj_get(s->fields, "value"), &vid)) continue;
    NodeRec* v = get_node(p, vid);
    if (!v || !v->tag) continue;
    if (strncmp(v->tag, "sem.", 4) != 0) continue;

    if (strcmp(v->tag, "sem.if") == 0) {
      JsonValue* args = json_obj_get(v->fields, "args");
      if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) continue;
      int64_t cond_id = 0;
      if (!parse_node_ref_id(p, args->v.arr.items[0], &cond_id)) continue;
      BranchOperand bt = {0}, be = {0};
      if (!parse_branch_operand(p, args->v.arr.items[1], &bt) || !parse_branch_operand(p, args->v.arr.items[2], &be)) continue;
      if (!lower_sem_value_to_cfg_let(p, fn, vid, "sem.if", cond_id, &bt, &be, sid)) return false;
      if (out_did) *out_did = true;
      return true;
    }

    if (strcmp(v->tag, "sem.match_sum") == 0) {
      if (!lower_sem_match_sum_to_cfg_let(p, fn, vid, sid)) return false;
      if (out_did) *out_did = true;
      return true;
    }

    if (strcmp(v->tag, "sem.and_sc") == 0 || strcmp(v->tag, "sem.or_sc") == 0) {
      JsonValue* args = json_obj_get(v->fields, "args");
      if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) continue;
      int64_t lhs_id = 0;
      if (!parse_node_ref_id(p, args->v.arr.items[0], &lhs_id)) continue;
      BranchOperand rhs = {0};
      if (!parse_branch_operand(p, args->v.arr.items[1], &rhs)) continue;

      NodeRec* lhsn = get_node(p, lhs_id);
      const int64_t bool_ty = lhsn ? lhsn->type_ref : 0;
      if (!bool_ty) continue;
      const int64_t c_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, vid, "sc.const"), "sc const id");
      if (!c_id) return false;
      JsonValue* c_fields = jv_make_obj(&p->arena, 1);
      if (!c_fields) return false;
      c_fields->v.obj.items[0].key = "value";
      JsonValue* lit = jv_make(&p->arena, JSON_NUMBER);
      if (!lit) return false;
      const bool is_and = (strcmp(v->tag, "sem.and_sc") == 0);
      lit->v.i = is_and ? 0 : 1;
      c_fields->v.obj.items[0].value = lit;
      make_node_stub(p, c_id, "const.bool", bool_ty, c_fields);

      BranchOperand bt = {0}, be = {0};
      if (is_and) {
        bt = rhs;
        be.kind = BRANCH_VAL;
        be.node_id = c_id;
      } else {
        bt.kind = BRANCH_VAL;
        bt.node_id = c_id;
        be = rhs;
      }
      if (!lower_sem_value_to_cfg_let(p, fn, vid, v->tag, lhs_id, &bt, &be, sid)) return false;
      if (out_did) *out_did = true;
      return true;
    }
  }

  // Return-position lowering for body-form.
  JsonValue* last = stmts->v.arr.items[stmts->v.arr.len - 1];
  int64_t term_id = 0;
  if (!parse_node_ref_id(p, last, &term_id)) return true;
  NodeRec* term = get_node(p, term_id);
  if (!term || !term->fields) return true;
  if (!(strcmp(term->tag, "term.ret") == 0 || strcmp(term->tag, "return") == 0)) return true;
  int64_t rid = 0;
  if (!parse_node_ref_id(p, json_obj_get(term->fields, "value"), &rid)) return true;
  NodeRec* rnode = get_node(p, rid);
  if (!rnode || !rnode->tag) return true;

  if (strcmp(rnode->tag, "sem.if") == 0) {
    JsonValue* args = json_obj_get(rnode->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) return true;
    int64_t cond_id = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &cond_id)) return true;
    BranchOperand bt = {0}, be = {0};
    if (!parse_branch_operand(p, args->v.arr.items[1], &bt) || !parse_branch_operand(p, args->v.arr.items[2], &be)) return true;
    if (!lower_sem_value_to_cfg_ret(p, fn, rid, "sem.if", cond_id, &bt, &be)) return false;
    if (out_did) *out_did = true;
    return true;
  }

  if (strcmp(rnode->tag, "sem.match_sum") == 0) {
    if (!lower_sem_match_sum_to_cfg_ret(p, fn, rid)) return false;
    if (out_did) *out_did = true;
    return true;
  }

  if (strcmp(rnode->tag, "sem.and_sc") == 0 || strcmp(rnode->tag, "sem.or_sc") == 0) {
    JsonValue* args = json_obj_get(rnode->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) return true;
    int64_t lhs_id = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &lhs_id)) return true;
    BranchOperand rhs = {0};
    if (!parse_branch_operand(p, args->v.arr.items[1], &rhs)) return true;

    NodeRec* lhsn = get_node(p, lhs_id);
    const int64_t bool_ty = lhsn ? lhsn->type_ref : 0;
    if (!bool_ty) return true;
    const int64_t c_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, rid, "sc.const"), "sc const id");
    if (!c_id) return false;
    JsonValue* c_fields = jv_make_obj(&p->arena, 1);
    if (!c_fields) return false;
    c_fields->v.obj.items[0].key = "value";
    JsonValue* lit = jv_make(&p->arena, JSON_NUMBER);
    if (!lit) return false;
    const bool is_and = (strcmp(rnode->tag, "sem.and_sc") == 0);
    lit->v.i = is_and ? 0 : 1;
    c_fields->v.obj.items[0].value = lit;
    make_node_stub(p, c_id, "const.bool", bool_ty, c_fields);

    BranchOperand bt = {0}, be = {0};
    if (is_and) {
      bt = rhs;
      be.kind = BRANCH_VAL;
      be.node_id = c_id;
    } else {
      bt.kind = BRANCH_VAL;
      bt.node_id = c_id;
      be = rhs;
    }
    if (!lower_sem_value_to_cfg_ret(p, fn, rid, rnode->tag, lhs_id, &bt, &be)) return false;
    if (out_did) *out_did = true;
    return true;
  }

  return true;
}

static bool lower_one_sem_in_cfg_fn(SirProgram* p, NodeRec* fn, bool* out_did) {
  if (out_did) *out_did = false;
  if (!p || !fn || !fn->fields || fn->fields->type != JSON_OBJECT) return false;
  if (!json_obj_get(fn->fields, "entry")) return true;

  JsonValue* blocks = json_obj_get(fn->fields, "blocks");
  if (!blocks || blocks->type != JSON_ARRAY) return true;

  for (size_t bi = 0; bi < blocks->v.arr.len; bi++) {
    int64_t bid = 0;
    if (!parse_node_ref_id(p, blocks->v.arr.items[bi], &bid)) continue;
    NodeRec* blk = get_node(p, bid);
    if (!blk || !blk->fields || !blk->tag || strcmp(blk->tag, "block") != 0) continue;
    JsonValue* stmts = json_obj_get(blk->fields, "stmts");
    if (!stmts || stmts->type != JSON_ARRAY || stmts->v.arr.len == 0) continue;

    for (size_t si = 0; si < stmts->v.arr.len; si++) {
      int64_t sid = 0;
      if (!parse_node_ref_id(p, stmts->v.arr.items[si], &sid)) continue;
      NodeRec* s = get_node(p, sid);
      if (!s || !s->tag || strcmp(s->tag, "let") != 0) continue;
      if (!s->fields || s->fields->type != JSON_OBJECT) continue;
      int64_t vid = 0;
      if (!parse_node_ref_id(p, json_obj_get(s->fields, "value"), &vid)) continue;
      NodeRec* v = get_node(p, vid);
      if (!v || !v->tag) continue;
      if (strncmp(v->tag, "sem.", 4) != 0) continue;

      if (strcmp(v->tag, "sem.if") == 0) {
        JsonValue* args = json_obj_get(v->fields, "args");
        if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) continue;
        int64_t cond_id = 0;
        if (!parse_node_ref_id(p, args->v.arr.items[0], &cond_id)) continue;
        BranchOperand bt = {0}, be = {0};
        if (!parse_branch_operand(p, args->v.arr.items[1], &bt) || !parse_branch_operand(p, args->v.arr.items[2], &be)) continue;
        if (!lower_sem_value_to_cfg_let_cfg(p, fn, bid, vid, "sem.if", cond_id, &bt, &be, sid)) return false;
        if (out_did) *out_did = true;
        return true;
      }

      if (strcmp(v->tag, "sem.match_sum") == 0) {
        if (!lower_sem_match_sum_to_cfg_let_cfg(p, fn, bid, vid, sid)) return false;
        if (out_did) *out_did = true;
        return true;
      }

      if (strcmp(v->tag, "sem.and_sc") == 0 || strcmp(v->tag, "sem.or_sc") == 0) {
        JsonValue* args = json_obj_get(v->fields, "args");
        if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) continue;
        int64_t lhs_id = 0;
        if (!parse_node_ref_id(p, args->v.arr.items[0], &lhs_id)) continue;
        BranchOperand rhs = {0};
        if (!parse_branch_operand(p, args->v.arr.items[1], &rhs)) continue;

        NodeRec* lhsn = get_node(p, lhs_id);
        const int64_t bool_ty = lhsn ? lhsn->type_ref : 0;
        if (!bool_ty) continue;
        const int64_t c_id = alloc_node_id_from_str(p, derived_id(p, SIR_ID_NODE, vid, "sc.const"), "sc const id");
        if (!c_id) return false;
        JsonValue* c_fields = jv_make_obj(&p->arena, 1);
        if (!c_fields) return false;
        c_fields->v.obj.items[0].key = "value";
        JsonValue* lit = jv_make(&p->arena, JSON_NUMBER);
        if (!lit) return false;
        const bool is_and = (strcmp(v->tag, "sem.and_sc") == 0);
        lit->v.i = is_and ? 0 : 1;
        c_fields->v.obj.items[0].value = lit;
        make_node_stub(p, c_id, "const.bool", bool_ty, c_fields);

        BranchOperand bt = {0}, be = {0};
        if (is_and) {
          bt = rhs;
          be.kind = BRANCH_VAL;
          be.node_id = c_id;
        } else {
          bt.kind = BRANCH_VAL;
          bt.node_id = c_id;
          be = rhs;
        }
        if (!lower_sem_value_to_cfg_let_cfg(p, fn, bid, vid, v->tag, lhs_id, &bt, &be, sid)) return false;
        if (out_did) *out_did = true;
        return true;
      }
    }
  }
  return true;
}

static bool lower_sem_nodes(SirProgram* p) {
  if (!p) return false;
  if (!p->feat_sem_v1) return true;

  // 1) First, rewrite the pure/val cases in-place (these can appear anywhere).
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes ? p->nodes[i] : NULL;
    if (!n || !n->tag) continue;
    if (strcmp(n->tag, "sem.if") == 0) {
      // Try the simple val/val -> select rewrite. If it doesn't apply, leave it for CFG lowering.
      (void)lower_sem_if_to_select(p, n);
    } else if (strcmp(n->tag, "sem.and_sc") == 0) {
      (void)lower_sem_sc_to_bool_bin(p, n, true);
    } else if (strcmp(n->tag, "sem.or_sc") == 0) {
      (void)lower_sem_sc_to_bool_bin(p, n, false);
    }
  }

  // 2) Handle remaining sem.* by iteratively lowering per-function until fixed point.
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* fn = p->nodes ? p->nodes[i] : NULL;
    if (!fn || !fn->tag || strcmp(fn->tag, "fn") != 0) continue;
    if (!fn->fields || fn->fields->type != JSON_OBJECT) continue;

    for (;;) {
      bool did = false;
      // First, hoist sem.* used in expression positions into lets so the lowering pass
      // can treat them uniformly.
      if (json_obj_get(fn->fields, "entry")) {
        if (!hoist_sem_uses_in_cfg_fn(p, fn, &did)) return false;
        if (did) continue;
        if (!lower_one_sem_in_cfg_fn(p, fn, &did)) return false;
      } else {
        if (!hoist_sem_uses_in_body_fn(p, fn, &did)) return false;
        if (did) continue;
        if (!lower_one_sem_in_body_fn(p, fn, &did)) return false;
      }
      if (!did) break;
    }
  }

  // 3) If any sem.* remains, we don't know how to lower it yet.
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes ? p->nodes[i] : NULL;
    if (!n || !n->tag) continue;
    if (strncmp(n->tag, "sem.", 4) != 0) continue;
    SIRCC_ERR_NODE(p, n, "sircc.lower_hl.sem.unsupported", "sircc: --lower-hl does not support lowering %s yet", n->tag);
    return false;
  }

  // If we've eliminated all sem.* nodes, we can drop the feature gate in the output meta.
  p->feat_sem_v1 = false;

  return true;
}

static const char* type_kind_str(TypeKind k) {
  switch (k) {
    case TYPE_PRIM:
      return "prim";
    case TYPE_PTR:
      return "ptr";
    case TYPE_ARRAY:
      return "array";
    case TYPE_FN:
      return "fn";
    case TYPE_STRUCT:
      return "struct";
    case TYPE_VEC:
      return "vec";
    case TYPE_FUN:
      return "fun";
    case TYPE_CLOSURE:
      return "closure";
    case TYPE_SUM:
      return "sum";
    default:
      return NULL;
  }
}

static void emit_features(FILE* out, const SirProgram* p) {
  bool first = true;
  fputc('[', out);
  if (p->feat_atomics_v1) {
    if (!first) fputc(',', out);
    json_write_escaped(out, "atomics:v1");
    first = false;
  }
  if (p->feat_simd_v1) {
    if (!first) fputc(',', out);
    json_write_escaped(out, "simd:v1");
    first = false;
  }
  if (p->feat_adt_v1) {
    if (!first) fputc(',', out);
    json_write_escaped(out, "adt:v1");
    first = false;
  }
  if (p->feat_fun_v1) {
    if (!first) fputc(',', out);
    json_write_escaped(out, "fun:v1");
    first = false;
  }
  if (p->feat_closure_v1) {
    if (!first) fputc(',', out);
    json_write_escaped(out, "closure:v1");
    first = false;
  }
  if (p->feat_coro_v1) {
    if (!first) fputc(',', out);
    json_write_escaped(out, "coro:v1");
    first = false;
  }
  if (p->feat_eh_v1) {
    if (!first) fputc(',', out);
    json_write_escaped(out, "eh:v1");
    first = false;
  }
  if (p->feat_gc_v1) {
    if (!first) fputc(',', out);
    json_write_escaped(out, "gc:v1");
    first = false;
  }
  if (p->feat_sem_v1) {
    if (!first) fputc(',', out);
    json_write_escaped(out, "sem:v1");
    first = false;
  }
  if (p->feat_data_v1) {
    if (!first) fputc(',', out);
    json_write_escaped(out, "data:v1");
    first = false;
  }
  fputc(']', out);
}

static bool emit_meta(FILE* out, const SirProgram* p) {
  if (!out || !p) return false;
  fputs("{\"ir\":\"sir-v1.0\",\"k\":\"meta\",\"producer\":\"sircc-lower-hl\"", out);
  if (p->unit_name) {
    fputs(",\"unit\":", out);
    json_write_escaped(out, p->unit_name);
  }
  fputs(",\"ext\":{", out);

  fputs("\"features\":", out);
  emit_features(out, p);

  if (p->target_triple || p->target_cpu || p->target_features) {
    fputs(",\"target\":{", out);
    bool first = true;
    if (p->target_triple) {
      if (!first) fputc(',', out);
      fputs("\"triple\":", out);
      json_write_escaped(out, p->target_triple);
      first = false;
    }
    if (p->target_cpu) {
      if (!first) fputc(',', out);
      fputs("\"cpu\":", out);
      json_write_escaped(out, p->target_cpu);
      first = false;
    }
    if (p->target_features) {
      if (!first) fputc(',', out);
      fputs("\"features\":", out);
      json_write_escaped(out, p->target_features);
      first = false;
    }
    fputs("}", out);
  }

  fputs("}}\n", out);
  return true;
}

static bool emit_types(FILE* out, SirProgram* p) {
  if (!out || !p) return false;
  for (size_t i = 0; i < p->types_cap; i++) {
    TypeRec* t = p->types ? p->types[i] : NULL;
    if (!t) continue;
    const char* k = type_kind_str(t->kind);
    if (!k) continue;

    fputs("{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":", out);
    emit_id_json(out, p, SIR_ID_TYPE, t->id);
    fputs(",\"kind\":", out);
    json_write_escaped(out, k);

    if (t->kind == TYPE_PRIM) {
      fputs(",\"prim\":", out);
      json_write_escaped(out, t->prim ? t->prim : "");
    } else if (t->kind == TYPE_PTR) {
      fputs(",\"of\":", out);
      emit_id_json(out, p, SIR_ID_TYPE, t->of);
    } else if (t->kind == TYPE_ARRAY) {
      fputs(",\"of\":", out);
      emit_id_json(out, p, SIR_ID_TYPE, t->of);
      fputs(",\"len\":", out);
      fprintf(out, "%lld", (long long)t->len);
    } else if (t->kind == TYPE_FN) {
      fputs(",\"params\":[", out);
      for (size_t pi = 0; pi < t->param_len; pi++) {
        if (pi) fputc(',', out);
        emit_id_json(out, p, SIR_ID_TYPE, t->params[pi]);
      }
      fputs("],\"ret\":", out);
      emit_id_json(out, p, SIR_ID_TYPE, t->ret);
      if (t->varargs) fputs(",\"varargs\":true", out);
    } else if (t->kind == TYPE_STRUCT) {
      if (t->name) {
        fputs(",\"name\":", out);
        json_write_escaped(out, t->name);
      }
      fputs(",\"fields\":[", out);
      for (size_t fi = 0; fi < t->field_len; fi++) {
        if (fi) fputc(',', out);
        fputs("{\"name\":", out);
        json_write_escaped(out, t->fields[fi].name ? t->fields[fi].name : "");
        fputs(",\"type_ref\":", out);
        emit_id_json(out, p, SIR_ID_TYPE, t->fields[fi].type_ref);
        fputs("}", out);
      }
      fputs("]", out);
    } else if (t->kind == TYPE_VEC) {
      fputs(",\"lane\":", out);
      emit_id_json(out, p, SIR_ID_TYPE, t->lane_ty);
      fputs(",\"lanes\":", out);
      fprintf(out, "%lld", (long long)t->lanes);
    } else if (t->kind == TYPE_FUN) {
      fputs(",\"sig\":", out);
      emit_id_json(out, p, SIR_ID_TYPE, t->sig);
    } else if (t->kind == TYPE_CLOSURE) {
      fputs(",\"callSig\":", out);
      emit_id_json(out, p, SIR_ID_TYPE, t->call_sig);
      fputs(",\"env\":", out);
      emit_id_json(out, p, SIR_ID_TYPE, t->env_ty);
    } else if (t->kind == TYPE_SUM) {
      fputs(",\"variants\":[", out);
      for (size_t vi = 0; vi < t->variant_len; vi++) {
        if (vi) fputc(',', out);
        fputs("{", out);
        bool first = true;
        if (t->variants[vi].name) {
          fputs("\"name\":", out);
          json_write_escaped(out, t->variants[vi].name);
          first = false;
        }
        if (t->variants[vi].ty) {
          if (!first) fputc(',', out);
          fputs("\"ty\":", out);
          emit_id_json(out, p, SIR_ID_TYPE, t->variants[vi].ty);
        }
        fputs("}", out);
      }
      fputs("]", out);
    }

    fputs("}\n", out);
  }
  return true;
}

static bool emit_syms(FILE* out, SirProgram* p) {
  if (!out || !p) return false;
  for (size_t i = 0; i < p->syms_cap; i++) {
    SymRec* s = p->syms ? p->syms[i] : NULL;
    if (!s) continue;
    fputs("{\"ir\":\"sir-v1.0\",\"k\":\"sym\",\"id\":", out);
    emit_id_json(out, p, SIR_ID_SYM, s->id);
    if (s->name) {
      fputs(",\"name\":", out);
      json_write_escaped(out, s->name);
    }
    if (s->kind) {
      fputs(",\"kind\":", out);
      json_write_escaped(out, s->kind);
    }
    if (s->linkage) {
      fputs(",\"linkage\":", out);
      json_write_escaped(out, s->linkage);
    }
    if (s->type_ref) {
      fputs(",\"type_ref\":", out);
      emit_id_json(out, p, SIR_ID_TYPE, s->type_ref);
    }
    if (s->value) {
      fputs(",\"value\":", out);
      json_write_value(out, s->value);
    }
    fputs("}\n", out);
  }
  return true;
}

static bool emit_nodes(FILE* out, SirProgram* p) {
  if (!out || !p) return false;
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes ? p->nodes[i] : NULL;
    if (!n || !n->tag) continue;
    fputs("{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":", out);
    emit_id_json(out, p, SIR_ID_NODE, n->id);
    fputs(",\"tag\":", out);
    json_write_escaped(out, n->tag);
    if (n->type_ref) {
      fputs(",\"type_ref\":", out);
      emit_id_json(out, p, SIR_ID_TYPE, n->type_ref);
    }
    if (n->fields) {
      fputs(",\"fields\":", out);
      json_write_value(out, n->fields);
    }
    fputs("}\n", out);
  }
  return true;
}

bool lower_hl_and_emit_sir_core(SirProgram* p, const char* out_path) {
  if (!p || !out_path || !out_path[0]) return false;

  if (!lower_hl_in_place(p)) return false;

  FILE* out = fopen(out_path, "wb");
  if (!out) {
    err_codef(p, "sircc.io.open_failed", "sircc: failed to open --emit-sir-core output: %s", strerror(errno));
    return false;
  }

  bool ok = true;
  ok = ok && emit_meta(out, p);
  ok = ok && emit_types(out, p);
  ok = ok && emit_syms(out, p);
  ok = ok && emit_nodes(out, p);
  fclose(out);
  return ok;
}

bool lower_hl_in_place(SirProgram* p) {
  if (!p) return false;
  // Currently, SIR-HL is the `sem:v1` intent family.
  // Future HL constructs should also be lowered here so normal codegen and
  // `--lower-hl` share the same pipeline.
  return lower_sem_nodes(p);
}
