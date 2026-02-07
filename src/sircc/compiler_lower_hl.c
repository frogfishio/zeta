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

static int64_t max_node_id(SirProgram* p) {
  int64_t max = 0;
  if (!p) return 0;
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes ? p->nodes[i] : NULL;
    if (!n) continue;
    if (n->id > max) max = n->id;
  }
  return max;
}

static int64_t alloc_new_node_id(SirProgram* p, int64_t* next) {
  if (!p || !next) return 0;
  if (*next <= 0) *next = max_node_id(p) + 1;
  return (*next)++;
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

static JsonValue* jv_make_ref(Arena* a, int64_t id) {
  JsonValue* o = jv_make_obj(a, 2);
  if (!o) return NULL;
  o->v.obj.items[0].key = "t";
  o->v.obj.items[0].value = jv_make_str(a, "ref");
  o->v.obj.items[1].key = "id";
  o->v.obj.items[1].value = jv_make_i64(a, id);
  return o;
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

static bool make_call_thunk(SirProgram* p, int64_t* next_id, int64_t callee_node_id, int64_t result_ty, int64_t* out_call_id) {
  if (!p || !next_id || !out_call_id) return false;
  *out_call_id = 0;
  NodeRec* callee = get_node(p, callee_node_id);
  if (!callee || callee->type_ref == 0) return false;
  TypeRec* ct = get_type(p, callee->type_ref);
  if (!ct) return false;

  const char* tag = NULL;
  if (ct->kind == TYPE_FUN) tag = "call.fun";
  else if (ct->kind == TYPE_CLOSURE) tag = "call.closure";
  else return false;

  const int64_t call_id = alloc_new_node_id(p, next_id);
  if (!ensure_node_slot(p, call_id)) return false;

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

  int64_t next_id = 0;

  // Reuse sem node id as the join bparam (this strips sem.* from output).
  semn->tag = "bparam";
  semn->type_ref = result_ty;
  semn->fields = NULL;

  // Create join/then/else blocks.
  const int64_t then_bid = alloc_new_node_id(p, &next_id);
  const int64_t else_bid = alloc_new_node_id(p, &next_id);
  const int64_t join_bid = alloc_new_node_id(p, &next_id);
  if (!ensure_node_slot(p, then_bid) || !ensure_node_slot(p, else_bid) || !ensure_node_slot(p, join_bid)) {
    bump_exit_code(p, SIRCC_EXIT_INTERNAL);
    err_codef(p, "sircc.oom", "sircc: out of memory");
    return false;
  }

  // Resolve then/else branch value node ids (materialize thunk calls inside the branch block via call nodes).
  int64_t then_val_id = 0, else_val_id = 0;
  if (br_then->kind == BRANCH_VAL) {
    then_val_id = br_then->node_id;
  } else {
    if (!make_call_thunk(p, &next_id, br_then->node_id, result_ty, &then_val_id)) {
      SIRCC_ERR_NODE_ID(p, sem_node_id, sem_tag, "sircc.lower_hl.sem.thunk.bad", "sircc: invalid thunk in then branch");
      return false;
    }
  }
  if (br_else->kind == BRANCH_VAL) {
    else_val_id = br_else->node_id;
  } else {
    if (!make_call_thunk(p, &next_id, br_else->node_id, result_ty, &else_val_id)) {
      SIRCC_ERR_NODE_ID(p, sem_node_id, sem_tag, "sircc.lower_hl.sem.thunk.bad", "sircc: invalid thunk in else branch");
      return false;
    }
  }

  // Create term.br in then/else to join, passing the value.
  const int64_t then_br_id = alloc_new_node_id(p, &next_id);
  const int64_t else_br_id = alloc_new_node_id(p, &next_id);
  if (!ensure_node_slot(p, then_br_id) || !ensure_node_slot(p, else_br_id)) return false;

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

  const int64_t cbr_id = alloc_new_node_id(p, &next_id);
  if (!ensure_node_slot(p, cbr_id)) return false;

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

  // 2) Handle remaining sem.* by converting simple return-position uses into CFG.
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* fn = p->nodes ? p->nodes[i] : NULL;
    if (!fn || !fn->tag || strcmp(fn->tag, "fn") != 0) continue;
    if (!fn->fields || fn->fields->type != JSON_OBJECT) continue;
    if (json_obj_get(fn->fields, "entry")) continue; // already CFG form; MVP: don't rewrite

    int64_t body_id = 0;
    if (!parse_node_ref_id(p, json_obj_get(fn->fields, "body"), &body_id)) continue;
    NodeRec* body = get_node(p, body_id);
    if (!body || !body->fields) continue;
    JsonValue* stmts = json_obj_get(body->fields, "stmts");
    if (!stmts || stmts->type != JSON_ARRAY || stmts->v.arr.len == 0) continue;
    JsonValue* last = stmts->v.arr.items[stmts->v.arr.len - 1];
    int64_t term_id = 0;
    if (!parse_node_ref_id(p, last, &term_id)) continue;
    NodeRec* term = get_node(p, term_id);
    if (!term || !term->fields) continue;
    if (!(strcmp(term->tag, "term.ret") == 0 || strcmp(term->tag, "return") == 0)) continue;
    int64_t rid = 0;
    if (!parse_node_ref_id(p, json_obj_get(term->fields, "value"), &rid)) continue;
    NodeRec* rnode = get_node(p, rid);
    if (!rnode || !rnode->tag) continue;

    if (strcmp(rnode->tag, "sem.if") == 0) {
      JsonValue* args = json_obj_get(rnode->fields, "args");
      if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) continue;
      int64_t cond_id = 0;
      if (!parse_node_ref_id(p, args->v.arr.items[0], &cond_id)) continue;
      BranchOperand bt = {0}, be = {0};
      if (!parse_branch_operand(p, args->v.arr.items[1], &bt) || !parse_branch_operand(p, args->v.arr.items[2], &be)) continue;
      if (!lower_sem_value_to_cfg_ret(p, fn, rid, "sem.if", cond_id, &bt, &be)) return false;
      continue;
    }

    if (strcmp(rnode->tag, "sem.and_sc") == 0 || strcmp(rnode->tag, "sem.or_sc") == 0) {
      JsonValue* args = json_obj_get(rnode->fields, "args");
      if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) continue;
      int64_t lhs_id = 0;
      if (!parse_node_ref_id(p, args->v.arr.items[0], &lhs_id)) continue;
      BranchOperand rhs = {0};
      if (!parse_branch_operand(p, args->v.arr.items[1], &rhs)) continue;

      // Build the missing constant branch in the entry region.
      NodeRec* lhsn = get_node(p, lhs_id);
      const int64_t bool_ty = lhsn ? lhsn->type_ref : 0;
      if (!bool_ty) continue;
      int64_t next_id = 0;
      const int64_t c_id = alloc_new_node_id(p, &next_id);
      if (!ensure_node_slot(p, c_id)) return false;
      JsonValue* c_fields = jv_make_obj(&p->arena, 1);
      if (!c_fields) return false;
      c_fields->v.obj.items[0].key = "value";
      // const.bool uses numeric 0/1 payloads.
      JsonValue* lit = jv_make(&p->arena, JSON_NUMBER);
      if (!lit) return false;
      const bool is_and = (strcmp(rnode->tag, "sem.and_sc") == 0);
      lit->v.i = is_and ? 0 : 1;
      c_fields->v.obj.items[0].value = lit;
      make_node_stub(p, c_id, "const.bool", bool_ty, c_fields);

      BranchOperand bt = {0}, be = {0};
      const bool is_and_sc = (strcmp(rnode->tag, "sem.and_sc") == 0);
      if (is_and_sc) {
        // if lhs then rhs else false
        bt = rhs;
        be.kind = BRANCH_VAL;
        be.node_id = c_id;
      } else {
        // if lhs then true else rhs
        bt.kind = BRANCH_VAL;
        bt.node_id = c_id;
        be = rhs;
      }
      if (!lower_sem_value_to_cfg_ret(p, fn, rid, rnode->tag, lhs_id, &bt, &be)) return false;
      continue;
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
    fprintf(out, "%lld", (long long)t->id);
    fputs(",\"kind\":", out);
    json_write_escaped(out, k);

    if (t->kind == TYPE_PRIM) {
      fputs(",\"prim\":", out);
      json_write_escaped(out, t->prim ? t->prim : "");
    } else if (t->kind == TYPE_PTR) {
      fputs(",\"of\":", out);
      fprintf(out, "%lld", (long long)t->of);
    } else if (t->kind == TYPE_ARRAY) {
      fputs(",\"of\":", out);
      fprintf(out, "%lld", (long long)t->of);
      fputs(",\"len\":", out);
      fprintf(out, "%lld", (long long)t->len);
    } else if (t->kind == TYPE_FN) {
      fputs(",\"params\":[", out);
      for (size_t pi = 0; pi < t->param_len; pi++) {
        if (pi) fputc(',', out);
        fprintf(out, "%lld", (long long)t->params[pi]);
      }
      fputs("],\"ret\":", out);
      fprintf(out, "%lld", (long long)t->ret);
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
        fprintf(out, "%lld", (long long)t->fields[fi].type_ref);
        fputs("}", out);
      }
      fputs("]", out);
    } else if (t->kind == TYPE_VEC) {
      fputs(",\"lane\":", out);
      fprintf(out, "%lld", (long long)t->lane_ty);
      fputs(",\"lanes\":", out);
      fprintf(out, "%lld", (long long)t->lanes);
    } else if (t->kind == TYPE_FUN) {
      fputs(",\"sig\":", out);
      fprintf(out, "%lld", (long long)t->sig);
    } else if (t->kind == TYPE_CLOSURE) {
      fputs(",\"callSig\":", out);
      fprintf(out, "%lld", (long long)t->call_sig);
      fputs(",\"env\":", out);
      fprintf(out, "%lld", (long long)t->env_ty);
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
          fprintf(out, "%lld", (long long)t->variants[vi].ty);
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
    fprintf(out, "%lld", (long long)s->id);
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
      fprintf(out, "%lld", (long long)s->type_ref);
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
    fprintf(out, "%lld", (long long)n->id);
    fputs(",\"tag\":", out);
    json_write_escaped(out, n->tag);
    if (n->type_ref) {
      fputs(",\"type_ref\":", out);
      fprintf(out, "%lld", (long long)n->type_ref);
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

  if (!lower_sem_nodes(p)) return false;

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
