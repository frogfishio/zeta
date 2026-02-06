// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool validate_cfg_fn(SirProgram* p, NodeRec* fn);

static bool is_prim_named(SirProgram* p, int64_t type_id, const char* prim) {
  if (!p || !prim || type_id == 0) return false;
  TypeRec* t = get_type(p, type_id);
  return t && t->kind == TYPE_PRIM && t->prim && strcmp(t->prim, prim) == 0;
}

static bool is_ptr_type_id(SirProgram* p, int64_t type_id) {
  if (!p || type_id == 0) return false;
  TypeRec* t = get_type(p, type_id);
  return t && t->kind == TYPE_PTR;
}

static bool is_fn_type_id(SirProgram* p, int64_t type_id, TypeRec** out_fn) {
  if (out_fn) *out_fn = NULL;
  if (!p || type_id == 0) return false;
  TypeRec* t = get_type(p, type_id);
  if (!t || t->kind != TYPE_FN) return false;
  if (out_fn) *out_fn = t;
  return true;
}

static bool fn_sig_eq_derived(TypeRec* have_sig, int64_t want_env_ty, TypeRec* want_call_sig) {
  if (!have_sig || have_sig->kind != TYPE_FN) return false;
  if (!want_call_sig || want_call_sig->kind != TYPE_FN) return false;
  if (have_sig->varargs != want_call_sig->varargs) return false;
  if (have_sig->ret != want_call_sig->ret) return false;
  if (have_sig->param_len != want_call_sig->param_len + 1) return false;
  if (have_sig->params[0] != want_env_ty) return false;
  for (size_t i = 0; i < want_call_sig->param_len; i++) {
    if (have_sig->params[i + 1] != want_call_sig->params[i]) return false;
  }
  return true;
}

static bool validate_fun_node(SirProgram* p, NodeRec* n) {
  if (!p || !n) return false;
  if (!p->feat_fun_v1) return true;
  if (!(strcmp(n->tag, "call.fun") == 0 || strncmp(n->tag, "fun.", 4) == 0)) return true;

  SirDiagSaved saved = sir_diag_push_node(p, n);

  JsonValue* args = (n->fields && n->fields->type == JSON_OBJECT) ? json_obj_get(n->fields, "args") : NULL;

  if (strcmp(n->tag, "call.fun") == 0) {
    if (!n->fields) {
      err_codef(p, "sircc.call.fun.missing_fields", "sircc: call.fun node %lld missing fields", (long long)n->id);
      goto bad;
    }
    if (!args || args->type != JSON_ARRAY || args->v.arr.len < 1) {
      err_codef(p, "sircc.call.fun.args_bad", "sircc: call.fun node %lld requires args:[callee, ...]", (long long)n->id);
      goto bad;
    }
    int64_t callee_id = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &callee_id)) {
      err_codef(p, "sircc.call.fun.callee_ref_bad", "sircc: call.fun node %lld args[0] must be callee fun ref", (long long)n->id);
      goto bad;
    }
    NodeRec* callee_n = get_node(p, callee_id);
    if (!callee_n || callee_n->type_ref == 0) {
      err_codef(p, "sircc.call.fun.callee_missing_type_ref", "sircc: call.fun node %lld callee must have a fun type_ref", (long long)n->id);
      goto bad;
    }
    TypeRec* callee_ty = get_type(p, callee_n->type_ref);
    if (!callee_ty || callee_ty->kind != TYPE_FUN || callee_ty->sig == 0) {
      err_codef(p, "sircc.call.fun.callee_type_bad", "sircc: call.fun node %lld callee must be a fun type", (long long)n->id);
      goto bad;
    }
    TypeRec* sig = get_type(p, callee_ty->sig);
    if (!sig || sig->kind != TYPE_FN) {
      err_codef(p, "sircc.call.fun.sig_bad", "sircc: call.fun node %lld callee fun.sig must reference a fn type", (long long)n->id);
      goto bad;
    }

    size_t argc = args->v.arr.len - 1;
    if (!sig->varargs && argc != sig->param_len) {
      err_codef(p, "sircc.call.fun.argc_mismatch", "sircc: fun call arg count mismatch (got %zu, want %zu)", argc, sig->param_len);
      goto bad;
    }
    if (argc < sig->param_len) {
      err_codef(p, "sircc.call.fun.argc_missing", "sircc: fun call missing required args (got %zu, want >= %zu)", argc, sig->param_len);
      goto bad;
    }
    for (size_t i = 0; i < sig->param_len; i++) {
      int64_t aid = 0;
      if (!parse_node_ref_id(p, args->v.arr.items[i + 1], &aid)) {
        err_codef(p, "sircc.call.fun.arg_ref_bad", "sircc: call.fun node %lld arg[%zu] must be node ref", (long long)n->id, i);
        goto bad;
      }
      NodeRec* an = get_node(p, aid);
      if (!an || an->type_ref == 0) {
        err_codef(p, "sircc.call.fun.arg_type_mismatch", "sircc: fun call arg[%zu] missing type_ref", i);
        goto bad;
      }
      int64_t want = sig->params[i];
      int64_t got = an->type_ref;
      if (want == got) continue;
      if (is_ptr_type_id(p, want) && is_ptr_type_id(p, got)) continue; // pointer bitcast allowed
      err_codef(p, "sircc.call.fun.arg_type_mismatch", "sircc: fun call arg[%zu] type mismatch (want=%lld, got=%lld)", i, (long long)want,
                (long long)got);
      goto bad;
    }
    if (n->type_ref && n->type_ref != sig->ret) {
      err_codef(p, "sircc.call.fun.ret_type_mismatch", "sircc: fun call return type mismatch (want=%lld, got=%lld)", (long long)n->type_ref,
                (long long)sig->ret);
      goto bad;
    }
    goto ok;
  }

  // fun.* nodes
  const char* op = n->tag + 4;
  if (strcmp(op, "sym") == 0) {
    if (!n->fields) {
      err_codef(p, "sircc.fun.sym.missing_fields", "sircc: fun.sym node %lld missing fields", (long long)n->id);
      goto bad;
    }
    if (n->type_ref == 0) {
      err_codef(p, "sircc.fun.sym.missing_type", "sircc: fun.sym node %lld missing type_ref (fun type)", (long long)n->id);
      goto bad;
    }
    TypeRec* fty = get_type(p, n->type_ref);
    if (!fty || fty->kind != TYPE_FUN || fty->sig == 0) {
      err_codef(p, "sircc.fun.sym.type_ref.bad", "sircc: fun.sym node %lld type_ref must be a fun type", (long long)n->id);
      goto bad;
    }
    TypeRec* sig = get_type(p, fty->sig);
    if (!sig || sig->kind != TYPE_FN) {
      err_codef(p, "sircc.fun.sym.sig.bad", "sircc: fun.sym node %lld fun.sig must reference a fn type", (long long)n->id);
      goto bad;
    }
    const char* name = json_get_string(json_obj_get(n->fields, "name"));
    if (!name || !is_ident(name)) {
      err_codef(p, "sircc.fun.sym.name.bad", "sircc: fun.sym node %lld requires fields.name Ident", (long long)n->id);
      goto bad;
    }
    SymRec* s = find_sym_by_name(p, name);
    if (s && s->kind && (strcmp(s->kind, "var") == 0 || strcmp(s->kind, "const") == 0)) {
      err_codef(p, "sircc.fun.sym.conflict_sym", "sircc: fun.sym '%s' references a data symbol (expected function)", name);
      goto bad;
    }
    NodeRec* fn_node = find_fn_node_by_name(p, name);
    if (fn_node && fn_node->type_ref != fty->sig) {
      err_codef(p, "sircc.fun.sym.sig_mismatch", "sircc: fun.sym '%s' signature mismatch vs fn node type_ref", name);
      goto bad;
    }
    NodeRec* decl_node = find_decl_fn_node_by_name(p, name);
    if (decl_node) {
      int64_t decl_sig_id = decl_node->type_ref;
      if (decl_sig_id == 0) {
        if (!parse_type_ref_id(p, json_obj_get(decl_node->fields, "sig"), &decl_sig_id)) {
          err_codef(p, "sircc.fun.sym.decl.sig.bad", "sircc: fun.sym '%s' has decl.fn without a signature", name);
          goto bad;
        }
      }
      if (decl_sig_id != fty->sig) {
        err_codef(p, "sircc.fun.sym.sig_mismatch", "sircc: fun.sym '%s' signature mismatch vs decl.fn", name);
        goto bad;
      }
    }
    if (!fn_node && !decl_node) {
      err_codef(p, "sircc.fun.sym.undefined", "sircc: fun.sym '%s' requires a prior fn or decl.fn of matching signature (producer rule)", name);
      goto bad;
    }
    goto ok;
  }

  if (strcmp(op, "cmp.eq") == 0 || strcmp(op, "cmp.ne") == 0) {
    if (!n->fields) {
      err_codef(p, "sircc.fun.cmp.missing_fields", "sircc: %s node %lld missing fields", n->tag, (long long)n->id);
      goto bad;
    }
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) {
      err_codef(p, "sircc.fun.cmp.args_bad", "sircc: %s node %lld requires fields.args:[a,b]", n->tag, (long long)n->id);
      goto bad;
    }
    int64_t a_id = 0, b_id = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &a_id) || !parse_node_ref_id(p, args->v.arr.items[1], &b_id)) {
      err_codef(p, "sircc.fun.cmp.arg_ref_bad", "sircc: %s node %lld args must be node refs", n->tag, (long long)n->id);
      goto bad;
    }
    NodeRec* a = get_node(p, a_id);
    NodeRec* b = get_node(p, b_id);
    if (!a || !b || a->type_ref == 0 || b->type_ref == 0) {
      err_codef(p, "sircc.fun.cmp.operand_bad", "sircc: %s node %lld operands must be function values", n->tag, (long long)n->id);
      goto bad;
    }
    if (a->type_ref != b->type_ref) {
      err_codef(p, "sircc.fun.cmp.type_mismatch", "sircc: %s node %lld requires both operands to have same fun type", n->tag, (long long)n->id);
      goto bad;
    }
    TypeRec* ta = get_type(p, a->type_ref);
    if (!ta || ta->kind != TYPE_FUN) {
      err_codef(p, "sircc.fun.cmp.operand_bad", "sircc: %s node %lld operands must be function values", n->tag, (long long)n->id);
      goto bad;
    }
    goto ok;
  }

  goto ok;

bad:
  sir_diag_pop(p, saved);
  return false;
ok:
  sir_diag_pop(p, saved);
  return true;
}

static bool validate_closure_node(SirProgram* p, NodeRec* n) {
  if (!p || !n) return false;
  if (!p->feat_closure_v1) return true;
  if (!(strcmp(n->tag, "call.closure") == 0 || strncmp(n->tag, "closure.", 8) == 0)) return true;

  SirDiagSaved saved = sir_diag_push_node(p, n);

  JsonValue* args = (n->fields && n->fields->type == JSON_OBJECT) ? json_obj_get(n->fields, "args") : NULL;

  if (strcmp(n->tag, "call.closure") == 0) {
    if (!n->fields) {
      err_codef(p, "sircc.call.closure.missing_fields", "sircc: call.closure node %lld missing fields", (long long)n->id);
      goto bad;
    }
    if (!args || args->type != JSON_ARRAY || args->v.arr.len < 1) {
      err_codef(p, "sircc.call.closure.args_bad", "sircc: call.closure node %lld requires args:[callee, ...]", (long long)n->id);
      goto bad;
    }
    int64_t callee_id = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &callee_id)) {
      err_codef(p, "sircc.call.closure.callee_ref_bad", "sircc: call.closure node %lld args[0] must be callee closure ref", (long long)n->id);
      goto bad;
    }
    NodeRec* callee_n = get_node(p, callee_id);
    if (!callee_n || callee_n->type_ref == 0) {
      err_codef(p, "sircc.call.closure.callee_missing_type_ref",
                "sircc: call.closure node %lld callee must have a closure type_ref", (long long)n->id);
      goto bad;
    }
    TypeRec* cty = get_type(p, callee_n->type_ref);
    if (!cty || cty->kind != TYPE_CLOSURE || cty->call_sig == 0 || cty->env_ty == 0) {
      err_codef(p, "sircc.call.closure.callee_type_bad", "sircc: call.closure node %lld callee must be a closure type", (long long)n->id);
      goto bad;
    }
    TypeRec* cs = get_type(p, cty->call_sig);
    if (!cs || cs->kind != TYPE_FN) {
      err_codef(p, "sircc.call.closure.sig_bad", "sircc: call.closure node %lld could not derive closure code signature", (long long)n->id);
      goto bad;
    }

    size_t argc = args->v.arr.len - 1;
    if (!cs->varargs && argc != cs->param_len) {
      err_codef(p, "sircc.call.closure.argc_mismatch", "sircc: closure call arg count mismatch (got %zu, want %zu)", argc, cs->param_len);
      goto bad;
    }
    if (argc < cs->param_len) {
      err_codef(p, "sircc.call.closure.argc_missing", "sircc: closure call missing required args (got %zu, want >= %zu)", argc, cs->param_len);
      goto bad;
    }
    for (size_t i = 0; i < cs->param_len; i++) {
      int64_t aid = 0;
      if (!parse_node_ref_id(p, args->v.arr.items[i + 1], &aid)) {
        err_codef(p, "sircc.call.closure.arg_ref_bad", "sircc: call.closure node %lld arg[%zu] must be node ref", (long long)n->id, i);
        goto bad;
      }
      NodeRec* an = get_node(p, aid);
      if (!an || an->type_ref == 0) {
        err_codef(p, "sircc.call.closure.arg_type_mismatch", "sircc: closure call arg[%zu] missing type_ref", i);
        goto bad;
      }
      int64_t want = cs->params[i];
      int64_t got = an->type_ref;
      if (want == got) continue;
      if (is_ptr_type_id(p, want) && is_ptr_type_id(p, got)) continue;
      err_codef(p, "sircc.call.closure.arg_type_mismatch", "sircc: closure call arg[%zu] type mismatch (want=%lld, got=%lld)", i, (long long)want,
                (long long)got);
      goto bad;
    }
    if (n->type_ref && n->type_ref != cs->ret) {
      err_codef(p, "sircc.call.closure.ret_type_mismatch", "sircc: closure call return type mismatch (want=%lld, got=%lld)", (long long)n->type_ref,
                (long long)cs->ret);
      goto bad;
    }
    goto ok;
  }

  const char* op = n->tag + 8;

  if (strcmp(op, "make") == 0) {
    if (!n->fields) {
      err_codef(p, "sircc.closure.make.missing_fields", "sircc: closure.make node %lld missing fields", (long long)n->id);
      goto bad;
    }
    if (n->type_ref == 0) {
      err_codef(p, "sircc.closure.make.missing_type_ref", "sircc: closure.make node %lld missing type_ref (closure type)", (long long)n->id);
      goto bad;
    }
    TypeRec* cty = get_type(p, n->type_ref);
    if (!cty || cty->kind != TYPE_CLOSURE || cty->call_sig == 0 || cty->env_ty == 0) {
      err_codef(p, "sircc.closure.make.type_ref.bad", "sircc: closure.make node %lld type_ref must be a closure type", (long long)n->id);
      goto bad;
    }
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) {
      err_codef(p, "sircc.closure.make.args_bad", "sircc: closure.make node %lld requires fields.args:[code, env]", (long long)n->id);
      goto bad;
    }
    int64_t code_id = 0, env_id = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &code_id) || !parse_node_ref_id(p, args->v.arr.items[1], &env_id)) {
      err_codef(p, "sircc.closure.make.arg_ref_bad", "sircc: closure.make node %lld args must be node refs", (long long)n->id);
      goto bad;
    }
    NodeRec* code_n = get_node(p, code_id);
    if (!code_n || code_n->type_ref == 0) {
      err_codef(p, "sircc.closure.make.code.missing_type", "sircc: closure.make code must have a fun type_ref");
      goto bad;
    }
    TypeRec* code_ty = get_type(p, code_n->type_ref);
    if (!code_ty || code_ty->kind != TYPE_FUN || code_ty->sig == 0) {
      err_codef(p, "sircc.closure.make.code.not_fun", "sircc: closure.make code must be a fun value");
      goto bad;
    }
    TypeRec* have_sig = get_type(p, code_ty->sig);
    TypeRec* call_sig = get_type(p, cty->call_sig);
    if (!have_sig || have_sig->kind != TYPE_FN || !call_sig || call_sig->kind != TYPE_FN) {
      err_codef(p, "sircc.closure.make.callSig.bad", "sircc: closure.make closure.callSig must reference fn type");
      goto bad;
    }
    if (!fn_sig_eq_derived(have_sig, cty->env_ty, call_sig)) {
      err_codef(p, "sircc.closure.make.code.sig_mismatch", "sircc: closure.make code signature does not match derived codeSig");
      goto bad;
    }
    NodeRec* env_n = get_node(p, env_id);
    if (!env_n || env_n->type_ref == 0 || env_n->type_ref != cty->env_ty) {
      err_codef(p, "sircc.closure.make.env.type_mismatch", "sircc: closure.make env type does not match closure env type");
      goto bad;
    }
    goto ok;
  }

  if (strcmp(op, "sym") == 0) {
    if (!n->fields) {
      err_codef(p, "sircc.closure.sym.missing_fields", "sircc: closure.sym node %lld missing fields", (long long)n->id);
      goto bad;
    }
    if (n->type_ref == 0) {
      err_codef(p, "sircc.closure.sym.missing_type_ref", "sircc: closure.sym node %lld missing type_ref (closure type)", (long long)n->id);
      goto bad;
    }
    TypeRec* cty = get_type(p, n->type_ref);
    if (!cty || cty->kind != TYPE_CLOSURE || cty->call_sig == 0 || cty->env_ty == 0) {
      err_codef(p, "sircc.closure.sym.type_ref.bad", "sircc: closure.sym node %lld type_ref must be a closure type", (long long)n->id);
      goto bad;
    }
    const char* name = json_get_string(json_obj_get(n->fields, "name"));
    if (!name || !is_ident(name)) {
      err_codef(p, "sircc.closure.sym.name.bad", "sircc: closure.sym node %lld requires fields.name Ident", (long long)n->id);
      goto bad;
    }
    int64_t env_id = 0;
    if (!parse_node_ref_id(p, json_obj_get(n->fields, "env"), &env_id)) {
      err_codef(p, "sircc.closure.sym.env.ref.missing", "sircc: closure.sym node %lld missing fields.env ref", (long long)n->id);
      goto bad;
    }
    NodeRec* env_n = get_node(p, env_id);
    if (!env_n || env_n->type_ref == 0 || env_n->type_ref != cty->env_ty) {
      err_codef(p, "sircc.closure.sym.env.type_mismatch", "sircc: closure.sym env type does not match closure env type");
      goto bad;
    }
    TypeRec* cs = get_type(p, cty->call_sig);
    if (!cs || cs->kind != TYPE_FN) {
      err_codef(p, "sircc.closure.sym.callSig.bad", "sircc: closure.sym node %lld closure.callSig must reference fn type", (long long)n->id);
      goto bad;
    }
    goto ok;
  }

  if (strcmp(op, "code") == 0 || strcmp(op, "env") == 0) {
    if (!n->fields) {
      err_codef(p, "sircc.closure.access.missing_fields", "sircc: %s node %lld missing fields", n->tag, (long long)n->id);
      goto bad;
    }
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
      err_codef(p, "sircc.closure.access.args_bad", "sircc: %s node %lld requires fields.args:[c]", n->tag, (long long)n->id);
      goto bad;
    }
    int64_t cid = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &cid)) {
      err_codef(p, "sircc.closure.access.arg_ref_bad", "sircc: %s node %lld arg must be node ref", n->tag, (long long)n->id);
      goto bad;
    }
    NodeRec* c = get_node(p, cid);
    if (!c || c->type_ref == 0) goto ok; // best-effort
    TypeRec* cty = get_type(p, c->type_ref);
    if (!cty || cty->kind != TYPE_CLOSURE) goto ok; // best-effort
    if (strcmp(op, "env") == 0) {
      if (n->type_ref && n->type_ref != cty->env_ty) {
        err_codef(p, "sircc.closure.env.type_mismatch", "sircc: closure.env result type_ref mismatch");
        goto bad;
      }
    }
    goto ok;
  }

  if (strcmp(op, "cmp.eq") == 0 || strcmp(op, "cmp.ne") == 0) {
    if (!n->fields) {
      err_codef(p, "sircc.closure.cmp.missing_fields", "sircc: %s node %lld missing fields", n->tag, (long long)n->id);
      goto bad;
    }
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) {
      err_codef(p, "sircc.closure.cmp.args_bad", "sircc: %s node %lld requires fields.args:[a,b]", n->tag, (long long)n->id);
      goto bad;
    }
    int64_t a_id = 0, b_id = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &a_id) || !parse_node_ref_id(p, args->v.arr.items[1], &b_id)) {
      err_codef(p, "sircc.closure.cmp.arg_ref_bad", "sircc: %s node %lld args must be node refs", n->tag, (long long)n->id);
      goto bad;
    }
    NodeRec* a = get_node(p, a_id);
    NodeRec* b = get_node(p, b_id);
    if (!a || !b || a->type_ref == 0 || b->type_ref == 0 || a->type_ref != b->type_ref) {
      err_codef(p, "sircc.closure.cmp.type_mismatch", "sircc: %s node %lld requires both operands to have same closure type", n->tag,
                (long long)n->id);
      goto bad;
    }
    TypeRec* cty = get_type(p, a->type_ref);
    if (!cty || cty->kind != TYPE_CLOSURE) {
      err_codef(p, "sircc.closure.cmp.operand_bad", "sircc: %s node %lld operands must be closure values", n->tag, (long long)n->id);
      goto bad;
    }
    TypeRec* envt = get_type(p, cty->env_ty);
    if (envt && envt->kind != TYPE_PTR && envt->kind != TYPE_PRIM) {
      err_codef(p, "sircc.closure.cmp.env_unsupported", "sircc: %s env equality unsupported for non-integer/non-pointer env type", n->tag);
      goto bad;
    }
    if (envt && envt->kind == TYPE_PRIM) {
      if (!envt->prim) {
        err_codef(p, "sircc.closure.cmp.env_unsupported", "sircc: %s env equality unsupported for env type", n->tag);
        goto bad;
      }
      bool ok = (strcmp(envt->prim, "i8") == 0 || strcmp(envt->prim, "i16") == 0 || strcmp(envt->prim, "i32") == 0 || strcmp(envt->prim, "i64") == 0 ||
                 strcmp(envt->prim, "bool") == 0 || strcmp(envt->prim, "i1") == 0);
      if (!ok) {
        err_codef(p, "sircc.closure.cmp.env_unsupported", "sircc: %s env equality unsupported for env type '%s'", n->tag, envt->prim);
        goto bad;
      }
    }
    goto ok;
  }

  goto ok;

bad:
  sir_diag_pop(p, saved);
  return false;
ok:
  sir_diag_pop(p, saved);
  return true;
}

static bool validate_adt_node(SirProgram* p, NodeRec* n) {
  if (!p || !n) return false;
  if (!p->feat_adt_v1) return true;
  if (strncmp(n->tag, "adt.", 4) != 0) return true;

  SirDiagSaved saved = sir_diag_push_node(p, n);

  if (!n->fields) {
    err_codef(p, "sircc.adt.missing_fields", "sircc: %s node %lld missing fields", n->tag, (long long)n->id);
    goto bad;
  }

  const char* op = n->tag + 4;
  JsonValue* args = json_obj_get(n->fields, "args");
  JsonValue* flags = json_obj_get(n->fields, "flags");

  if (strcmp(op, "tag") == 0) {
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
      err_codef(p, "sircc.adt.tag.args_bad", "sircc: adt.tag node %lld requires fields.args:[v]", (long long)n->id);
      goto bad;
    }
    int64_t vid = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &vid)) {
      err_codef(p, "sircc.adt.tag.arg_ref_bad", "sircc: adt.tag node %lld arg must be node ref", (long long)n->id);
      goto bad;
    }
    NodeRec* v = get_node(p, vid);
    if (v && v->type_ref) {
      TypeRec* sty = get_type(p, v->type_ref);
      if (!sty || sty->kind != TYPE_SUM) {
        err_codef(p, "sircc.adt.tag.arg_type_bad", "sircc: adt.tag node %lld arg must be sum type", (long long)n->id);
        goto bad;
      }
    }
    goto ok;
  }

  if (strcmp(op, "is") == 0) {
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
      err_codef(p, "sircc.adt.is.args_bad", "sircc: adt.is node %lld requires fields.args:[v]", (long long)n->id);
      goto bad;
    }
    if (!flags || flags->type != JSON_OBJECT) {
      err_codef(p, "sircc.adt.is.flags_missing", "sircc: adt.is node %lld missing fields.flags", (long long)n->id);
      goto bad;
    }
    int64_t variant = 0;
    if (!must_i64(p, json_obj_get(flags, "variant"), &variant, "adt.is.flags.variant")) goto bad;
    int64_t vid = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &vid)) {
      err_codef(p, "sircc.adt.is.arg_ref_bad", "sircc: adt.is node %lld arg must be node ref", (long long)n->id);
      goto bad;
    }
    (void)variant;
    goto ok;
  }

  if (strcmp(op, "make") == 0) {
    if (n->type_ref == 0) {
      err_codef(p, "sircc.adt.make.missing_type_ref", "sircc: adt.make node %lld missing type_ref (sum type)", (long long)n->id);
      goto bad;
    }
    TypeRec* sty = get_type(p, n->type_ref);
    if (!sty || sty->kind != TYPE_SUM) {
      err_codef(p, "sircc.adt.make.type_ref.bad", "sircc: adt.make node %lld type_ref must be a sum type", (long long)n->id);
      goto bad;
    }
    if (!flags || flags->type != JSON_OBJECT) {
      err_codef(p, "sircc.adt.make.flags_missing", "sircc: adt.make node %lld missing fields.flags", (long long)n->id);
      goto bad;
    }
    int64_t variant = -1;
    if (!must_i64(p, json_obj_get(flags, "variant"), &variant, "adt.make.flags.variant")) goto bad;
    int64_t eff = (variant < 0 || (size_t)variant >= sty->variant_len) ? 0 : variant;
    int64_t pay_ty_id = (sty->variant_len && (size_t)eff < sty->variant_len) ? sty->variants[(size_t)eff].ty : 0;

    size_t argc = 0;
    if (args) {
      if (args->type != JSON_ARRAY) {
        err_codef(p, "sircc.adt.make.args_type_bad", "sircc: adt.make node %lld fields.args must be array when present", (long long)n->id);
        goto bad;
      }
      argc = args->v.arr.len;
    }

    if (pay_ty_id == 0) {
      if (argc != 0) {
        err_codef(p, "sircc.adt.make.args_nullary_bad", "sircc: adt.make node %lld variant %lld is nullary; args must be empty",
                  (long long)n->id, (long long)variant);
        goto bad;
      }
    } else {
      if (argc != 1) {
        err_codef(p, "sircc.adt.make.args_payload_bad", "sircc: adt.make node %lld variant %lld requires one payload arg",
                  (long long)n->id, (long long)variant);
        goto bad;
      }
      int64_t pid = 0;
      if (!parse_node_ref_id(p, args->v.arr.items[0], &pid)) {
        err_codef(p, "sircc.adt.make.arg_ref_bad", "sircc: adt.make node %lld payload arg must be node ref", (long long)n->id);
        goto bad;
      }
      NodeRec* pn = get_node(p, pid);
      if (!pn || pn->type_ref == 0 || pn->type_ref != pay_ty_id) {
        err_codef(p, "sircc.adt.make.payload.type_mismatch", "sircc: adt.make node %lld payload type mismatch", (long long)n->id);
        goto bad;
      }
    }
    goto ok;
  }

  if (strcmp(op, "get") == 0) {
    int64_t sum_ty_id = 0;
    if (!parse_type_ref_id(p, json_obj_get(n->fields, "ty"), &sum_ty_id)) {
      err_codef(p, "sircc.adt.get.missing_ty", "sircc: adt.get node %lld missing fields.ty (sum type)", (long long)n->id);
      goto bad;
    }
    TypeRec* sty = get_type(p, sum_ty_id);
    if (!sty || sty->kind != TYPE_SUM) {
      err_codef(p, "sircc.adt.get.ty.bad", "sircc: adt.get node %lld fields.ty must reference a sum type", (long long)n->id);
      goto bad;
    }
    if (!flags || flags->type != JSON_OBJECT) {
      err_codef(p, "sircc.adt.get.flags_missing", "sircc: adt.get node %lld missing fields.flags", (long long)n->id);
      goto bad;
    }
    int64_t variant = -1;
    if (!must_i64(p, json_obj_get(flags, "variant"), &variant, "adt.get.flags.variant")) goto bad;
    int64_t eff = (variant < 0 || (size_t)variant >= sty->variant_len) ? 0 : variant;
    int64_t pay_ty_id = (sty->variant_len && (size_t)eff < sty->variant_len) ? sty->variants[(size_t)eff].ty : 0;
    if (pay_ty_id == 0) {
      err_codef(p, "sircc.adt.get.nullary", "sircc: adt.get node %lld variant %lld is nullary (no payload)", (long long)n->id, (long long)variant);
      goto bad;
    }
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
      err_codef(p, "sircc.adt.get.args_bad", "sircc: adt.get node %lld requires fields.args:[v]", (long long)n->id);
      goto bad;
    }
    int64_t vid = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &vid)) {
      err_codef(p, "sircc.adt.get.arg_ref_bad", "sircc: adt.get node %lld arg must be node ref", (long long)n->id);
      goto bad;
    }
    NodeRec* v = get_node(p, vid);
    if (v && v->type_ref && v->type_ref != sum_ty_id) {
      err_codef(p, "sircc.adt.get.arg_type_bad", "sircc: adt.get node %lld arg must match fields.ty sum type", (long long)n->id);
      goto bad;
    }
    if (n->type_ref && n->type_ref != pay_ty_id) {
      err_codef(p, "sircc.adt.get.ret_type_bad", "sircc: adt.get node %lld type_ref must match payload type", (long long)n->id);
      goto bad;
    }
    goto ok;
  }

  goto ok;

bad:
  sir_diag_pop(p, saved);
  return false;
ok:
  sir_diag_pop(p, saved);
  return true;
}

static bool branch_result_type(SirProgram* p, JsonValue* br, int64_t want_ty_id, int64_t payload_ty_id, bool allow_payload_arg,
                               int64_t* out_ty) {
  if (!p || !br || br->type != JSON_OBJECT || !out_ty) return false;
  *out_ty = 0;
  const char* kind = json_get_string(json_obj_get(br, "kind"));
  if (!kind) {
    err_codef(p, "sircc.sem.branch.kind.missing", "sircc: sem branch operand missing kind");
    return false;
  }
  if (strcmp(kind, "val") == 0) {
    int64_t vid = 0;
    if (!parse_node_ref_id(p, json_obj_get(br, "v"), &vid)) return false;
    NodeRec* v = get_node(p, vid);
    if (!v || v->type_ref == 0) {
      err_codef(p, "sircc.sem.branch.val.type_mismatch", "sircc: branch value missing type_ref");
      return false;
    }
    if (want_ty_id && v->type_ref != want_ty_id) {
      err_codef(p, "sircc.sem.branch.val.type_mismatch", "sircc: branch value type mismatch (want=%lld, got=%lld)", (long long)want_ty_id,
                (long long)v->type_ref);
      return false;
    }
    *out_ty = v->type_ref;
    return true;
  }
  if (strcmp(kind, "thunk") == 0) {
    int64_t fid = 0;
    if (!parse_node_ref_id(p, json_obj_get(br, "f"), &fid)) return false;
    NodeRec* fn = get_node(p, fid);
    if (!fn || fn->type_ref == 0) return false;
    TypeRec* t = get_type(p, fn->type_ref);
    if (!t) return false;
    TypeRec* sig = NULL;
    if (t->kind == TYPE_FUN) {
      if (!is_fn_type_id(p, t->sig, &sig)) {
        err_codef(p, "sircc.sem.thunk.kind.bad", "sircc: thunk must be fun or closure");
        return false;
      }
    } else if (t->kind == TYPE_CLOSURE) {
      if (!is_fn_type_id(p, t->call_sig, &sig)) {
        err_codef(p, "sircc.sem.thunk.kind.bad", "sircc: thunk must be fun or closure");
        return false;
      }
    } else {
      err_codef(p, "sircc.sem.thunk.kind.bad", "sircc: thunk must be fun or closure");
      return false;
    }

    // Arity rules:
    if (!allow_payload_arg) {
      if (sig->param_len != 0) {
        err_codef(p, "sircc.sem.thunk.arity.bad", "sircc: thunk %s must have () -> T signature", (t->kind == TYPE_CLOSURE) ? "closure" : "fun");
        return false;
      }
    } else {
      if (!(sig->param_len == 0 || sig->param_len == 1)) {
        err_codef(p, "sircc.sem.thunk.arity.bad", "sircc: thunk %s must have () -> T or (A) -> T signature",
                  (t->kind == TYPE_CLOSURE) ? "closure" : "fun");
        return false;
      }
      if (sig->param_len == 1) {
        if (payload_ty_id == 0) {
          err_codef(p, "sircc.sem.match_sum.case_payload_unexpected",
                    "sircc: sem.match_sum case body expects payload but variant is nullary");
          return false;
        }
        if (sig->params[0] != payload_ty_id) {
          err_codef(p, "sircc.sem.match_sum.thunk.param.bad", "sircc: sem.match_sum thunk parameter type must match payload type");
          return false;
        }
      }
    }

    if (want_ty_id && sig->ret != want_ty_id) {
      err_codef(p, "sircc.sem.thunk.ret.bad", "sircc: thunk return type mismatch");
      return false;
    }
    *out_ty = sig->ret;
    return true;
  }
  err_codef(p, "sircc.sem.branch.kind.bad", "sircc: sem branch operand kind must be 'val' or 'thunk'");
  return false;
}

static bool validate_sem_node(SirProgram* p, NodeRec* n) {
  if (!p || !n) return false;
  if (!p->feat_sem_v1) return true;
  if (strncmp(n->tag, "sem.", 4) != 0) return true;

  SirDiagSaved saved = sir_diag_push_node(p, n);

  if (!n->fields) {
    err_codef(p, "sircc.sem.missing_fields", "sircc: %s node %lld missing fields", n->tag, (long long)n->id);
    goto bad;
  }

  JsonValue* args = json_obj_get(n->fields, "args");

  if (strcmp(n->tag, "sem.if") == 0) {
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) {
      err_codef(p, "sircc.sem.if.args_bad", "sircc: sem.if node %lld requires args:[cond, thenBranch, elseBranch]", (long long)n->id);
      goto bad;
    }
    int64_t cond_id = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &cond_id)) {
      err_codef(p, "sircc.sem.if.cond_ref_bad", "sircc: sem.if node %lld cond must be node ref", (long long)n->id);
      goto bad;
    }
    NodeRec* cond = get_node(p, cond_id);
    if (!cond || cond->type_ref == 0 || !(is_prim_named(p, cond->type_ref, "bool") || is_prim_named(p, cond->type_ref, "i1"))) {
      err_codef(p, "sircc.sem.if.cond_type_bad", "sircc: sem.if node %lld cond must be bool", (long long)n->id);
      goto bad;
    }
    int64_t want = n->type_ref;
    int64_t t_then = 0, t_else = 0;
    if (!branch_result_type(p, args->v.arr.items[1], want, 0, false, &t_then)) goto bad;
    if (want == 0) want = t_then;
    if (!branch_result_type(p, args->v.arr.items[2], want, 0, false, &t_else)) goto bad;
    if (want == 0) want = t_else;
    if (t_then && want && t_then != want) {
      err_codef(p, "sircc.sem.branch.val.type_mismatch", "sircc: branch value type mismatch");
      goto bad;
    }
    if (t_else && want && t_else != want) {
      err_codef(p, "sircc.sem.branch.val.type_mismatch", "sircc: branch value type mismatch");
      goto bad;
    }
    if (n->type_ref && want && n->type_ref != want) {
      err_codef(p, "sircc.sem.if.ret_type_bad", "sircc: sem.if type_ref mismatch");
      goto bad;
    }
    goto ok;
  }

  if (strcmp(n->tag, "sem.and_sc") == 0 || strcmp(n->tag, "sem.or_sc") == 0) {
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) {
      err_codef(p, "sircc.sem.sc.args_bad", "sircc: %s node %lld requires args:[lhs, rhsBranch]", n->tag, (long long)n->id);
      goto bad;
    }
    int64_t lhs_id = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &lhs_id)) {
      err_codef(p, "sircc.sem.sc.lhs_ref_bad", "sircc: %s lhs must be node ref", n->tag);
      goto bad;
    }
    NodeRec* lhs = get_node(p, lhs_id);
    if (!lhs || lhs->type_ref == 0 || !(is_prim_named(p, lhs->type_ref, "bool") || is_prim_named(p, lhs->type_ref, "i1"))) {
      err_codef(p, "sircc.sem.sc.lhs_type_bad", "sircc: %s lhs must be bool", n->tag);
      goto bad;
    }
    int64_t bty = lhs->type_ref;
    int64_t tr = 0;
    if (!branch_result_type(p, args->v.arr.items[1], bty, 0, false, &tr)) goto bad;
    if (n->type_ref && n->type_ref != bty) {
      err_codef(p, "sircc.sem.sc.ret_type_bad", "sircc: %s type_ref must be bool", n->tag);
      goto bad;
    }
    goto ok;
  }

  if (strcmp(n->tag, "sem.match_sum") == 0) {
    int64_t sum_ty_id = 0;
    if (!parse_type_ref_id(p, json_obj_get(n->fields, "sum"), &sum_ty_id)) {
      err_codef(p, "sircc.sem.match_sum.sum_missing", "sircc: sem.match_sum node %lld missing fields.sum (sum type)", (long long)n->id);
      goto bad;
    }
    TypeRec* sty = get_type(p, sum_ty_id);
    if (!sty || sty->kind != TYPE_SUM) {
      err_codef(p, "sircc.sem.match_sum.sum_bad", "sircc: sem.match_sum fields.sum must reference a sum type");
      goto bad;
    }
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
      err_codef(p, "sircc.sem.match_sum.args_bad", "sircc: sem.match_sum node %lld requires args:[scrut]", (long long)n->id);
      goto bad;
    }
    int64_t scrut_id = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &scrut_id)) {
      err_codef(p, "sircc.sem.match_sum.scrut_ref_bad", "sircc: sem.match_sum scrut must be node ref");
      goto bad;
    }
    NodeRec* scrut = get_node(p, scrut_id);
    if (scrut && scrut->type_ref && scrut->type_ref != sum_ty_id) {
      err_codef(p, "sircc.sem.match_sum.scrut_type_bad", "sircc: sem.match_sum scrut type_ref must match fields.sum");
      goto bad;
    }

    JsonValue* cases = json_obj_get(n->fields, "cases");
    JsonValue* def = json_obj_get(n->fields, "default");
    if (!cases || cases->type != JSON_ARRAY || !def || def->type != JSON_OBJECT) {
      err_codef(p, "sircc.sem.match_sum.cases_bad",
                "sircc: sem.match_sum node %lld requires fields.cases array and fields.default branch", (long long)n->id);
      goto bad;
    }

    int64_t want = n->type_ref;
    int64_t tmp = 0;
    if (!branch_result_type(p, def, want, 0, false, &tmp)) goto bad;
    if (want == 0) want = tmp;

    for (size_t i = 0; i < cases->v.arr.len; i++) {
      JsonValue* co = cases->v.arr.items[i];
      if (!co || co->type != JSON_OBJECT) {
        err_codef(p, "sircc.sem.match_sum.case_obj_bad", "sircc: sem.match_sum cases[%zu] must be object", i);
        goto bad;
      }
      int64_t variant = 0;
      if (!must_i64(p, json_obj_get(co, "variant"), &variant, "sem.match_sum.cases.variant")) goto bad;
      JsonValue* body = json_obj_get(co, "body");
      if (!body || body->type != JSON_OBJECT) {
        err_codef(p, "sircc.sem.match_sum.case_body_missing", "sircc: sem.match_sum cases[%zu] missing body branch", i);
        goto bad;
      }
      int64_t pay_ty_id = 0;
      if (variant >= 0 && (size_t)variant < sty->variant_len) {
        pay_ty_id = sty->variants[(size_t)variant].ty;
      }
      int64_t rty = 0;
      if (!branch_result_type(p, body, want, pay_ty_id, true, &rty)) goto bad;
      if (want == 0) want = rty;
    }
    if (n->type_ref && want && n->type_ref != want) {
      err_codef(p, "sircc.sem.match_sum.ret_type_bad", "sircc: sem.match_sum type_ref mismatch");
      goto bad;
    }
    goto ok;
  }

  goto ok;

bad:
  sir_diag_pop(p, saved);
  return false;
ok:
  sir_diag_pop(p, saved);
  return true;
}

static bool is_vec_type_id(SirProgram* p, int64_t type_id, TypeRec** out_vec, TypeRec** out_lane) {
  if (out_vec) *out_vec = NULL;
  if (out_lane) *out_lane = NULL;
  if (!p || type_id == 0) return false;
  TypeRec* v = get_type(p, type_id);
  if (!v || v->kind != TYPE_VEC || v->lane_ty == 0) return false;
  TypeRec* lane = get_type(p, v->lane_ty);
  if (!lane || lane->kind != TYPE_PRIM || !lane->prim) return false;
  if (out_vec) *out_vec = v;
  if (out_lane) *out_lane = lane;
  return true;
}

static bool lane_is_bool(TypeRec* lane) {
  if (!lane || lane->kind != TYPE_PRIM || !lane->prim) return false;
  return strcmp(lane->prim, "bool") == 0 || strcmp(lane->prim, "i1") == 0;
}

static int64_t find_bool_vec_type_id(SirProgram* p, int64_t lanes) {
  if (!p || lanes <= 0) return 0;
  for (size_t i = 0; i < p->types_cap; i++) {
    TypeRec* t = p->types[i];
    if (!t || t->kind != TYPE_VEC) continue;
    if (t->lanes != lanes) continue;
    TypeRec* lane = get_type(p, t->lane_ty);
    if (lane_is_bool(lane)) return (int64_t)i;
  }
  return 0;
}

static bool validate_simd_node(SirProgram* p, NodeRec* n) {
  if (!p || !n) return false;
  if (!p->feat_simd_v1) return true;
  if (!(strncmp(n->tag, "vec.", 4) == 0 || strcmp(n->tag, "load.vec") == 0 || strcmp(n->tag, "store.vec") == 0)) return true;

  SirDiagSaved saved = sir_diag_push_node(p, n);

  // Helper: fetch args array.
  JsonValue* args = (n->fields && n->fields->type == JSON_OBJECT) ? json_obj_get(n->fields, "args") : NULL;

  if (strcmp(n->tag, "vec.splat") == 0) {
    if (n->type_ref == 0) {
      err_codef(p, "sircc.vec.splat.missing_type", "sircc: vec.splat node %lld missing type_ref (vec type)", (long long)n->id);
      goto bad;
    }
    TypeRec* vec = NULL;
    TypeRec* lane = NULL;
    if (!is_vec_type_id(p, n->type_ref, &vec, &lane)) {
      err_codef(p, "sircc.vec.splat.type.bad", "sircc: vec.splat node %lld type_ref must be a vec type", (long long)n->id);
      goto bad;
    }
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
      err_codef(p, "sircc.vec.splat.args.bad", "sircc: vec.splat node %lld requires args:[x]", (long long)n->id);
      goto bad;
    }
    int64_t xid = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &xid)) {
      err_codef(p, "sircc.vec.splat.args.ref_bad", "sircc: vec.splat node %lld args[0] must be a node ref", (long long)n->id);
      goto bad;
    }
    NodeRec* x = get_node(p, xid);
    if (!x || x->type_ref != vec->lane_ty) {
      err_codef(p, "sircc.vec.splat.lane.type_mismatch", "sircc: vec.splat node %lld arg type must match lane type", (long long)n->id);
      goto bad;
    }
    goto ok;
  }

  if (strcmp(n->tag, "vec.extract") == 0) {
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) {
      err_codef(p, "sircc.vec.extract.args.bad", "sircc: vec.extract node %lld requires args:[v, idx]", (long long)n->id);
      goto bad;
    }
    int64_t vid = 0, idxid = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &vid) || !parse_node_ref_id(p, args->v.arr.items[1], &idxid)) {
      err_codef(p, "sircc.vec.extract.args.ref_bad", "sircc: vec.extract node %lld args must be node refs", (long long)n->id);
      goto bad;
    }
    NodeRec* v = get_node(p, vid);
    NodeRec* idx = get_node(p, idxid);
    TypeRec* vec = NULL;
    TypeRec* lane = NULL;
    if (!v || !is_vec_type_id(p, v->type_ref, &vec, &lane)) {
      err_codef(p, "sircc.vec.extract.v.type.bad", "sircc: vec.extract node %lld v must be a vec", (long long)n->id);
      goto bad;
    }
    if (!idx || !is_prim_named(p, idx->type_ref, "i32")) {
      err_codef(p, "sircc.vec.extract.idx.type.bad", "sircc: vec.extract node %lld idx must be i32", (long long)n->id);
      goto bad;
    }
    if (n->type_ref && n->type_ref != vec->lane_ty) {
      err_codef(p, "sircc.vec.extract.type.bad", "sircc: vec.extract node %lld type_ref must match lane type", (long long)n->id);
      goto bad;
    }
    goto ok;
  }

  if (strcmp(n->tag, "vec.replace") == 0) {
    if (n->type_ref == 0) {
      err_codef(p, "sircc.vec.replace.missing_type", "sircc: vec.replace node %lld missing type_ref (vec type)", (long long)n->id);
      goto bad;
    }
    TypeRec* vec = NULL;
    TypeRec* lane = NULL;
    if (!is_vec_type_id(p, n->type_ref, &vec, &lane)) {
      err_codef(p, "sircc.vec.replace.type.bad", "sircc: vec.replace node %lld type_ref must be a vec type", (long long)n->id);
      goto bad;
    }
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) {
      err_codef(p, "sircc.vec.replace.args.bad", "sircc: vec.replace node %lld requires args:[v, idx, x]", (long long)n->id);
      goto bad;
    }
    int64_t vid = 0, idxid = 0, xid = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &vid) || !parse_node_ref_id(p, args->v.arr.items[1], &idxid) ||
        !parse_node_ref_id(p, args->v.arr.items[2], &xid)) {
      err_codef(p, "sircc.vec.replace.args.ref_bad", "sircc: vec.replace node %lld args must be node refs", (long long)n->id);
      goto bad;
    }
    NodeRec* v = get_node(p, vid);
    NodeRec* idx = get_node(p, idxid);
    NodeRec* x = get_node(p, xid);
    if (!v || v->type_ref != n->type_ref) {
      err_codef(p, "sircc.vec.replace.v.type.bad", "sircc: vec.replace node %lld v must match type_ref", (long long)n->id);
      goto bad;
    }
    if (!idx || !is_prim_named(p, idx->type_ref, "i32")) {
      err_codef(p, "sircc.vec.replace.idx.type.bad", "sircc: vec.replace node %lld idx must be i32", (long long)n->id);
      goto bad;
    }
    if (!x || x->type_ref != vec->lane_ty) {
      err_codef(p, "sircc.vec.replace.x.type.bad", "sircc: vec.replace node %lld x must match lane type", (long long)n->id);
      goto bad;
    }
    goto ok;
  }

  if (strcmp(n->tag, "vec.shuffle") == 0) {
    if (n->type_ref == 0) {
      err_codef(p, "sircc.vec.shuffle.missing_type", "sircc: vec.shuffle node %lld missing type_ref (vec type)", (long long)n->id);
      goto bad;
    }
    TypeRec* vec = NULL;
    TypeRec* lane = NULL;
    if (!is_vec_type_id(p, n->type_ref, &vec, &lane)) {
      err_codef(p, "sircc.vec.shuffle.type.bad", "sircc: vec.shuffle node %lld type_ref must be a vec type", (long long)n->id);
      goto bad;
    }
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) {
      err_codef(p, "sircc.vec.shuffle.args.bad", "sircc: vec.shuffle node %lld requires args:[a,b]", (long long)n->id);
      goto bad;
    }
    int64_t aid = 0, bid = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &aid) || !parse_node_ref_id(p, args->v.arr.items[1], &bid)) {
      err_codef(p, "sircc.vec.shuffle.args.ref_bad", "sircc: vec.shuffle node %lld args must be node refs", (long long)n->id);
      goto bad;
    }
    NodeRec* a = get_node(p, aid);
    NodeRec* b = get_node(p, bid);
    if (!a || !b || a->type_ref != n->type_ref || b->type_ref != n->type_ref) {
      err_codef(p, "sircc.vec.shuffle.ab.type.bad", "sircc: vec.shuffle node %lld requires a,b of the same vec type", (long long)n->id);
      goto bad;
    }
    JsonValue* flags = json_obj_get(n->fields, "flags");
    JsonValue* idxs = (flags && flags->type == JSON_OBJECT) ? json_obj_get(flags, "idx") : NULL;
    if (!idxs || idxs->type != JSON_ARRAY || idxs->v.arr.len != (size_t)vec->lanes) {
      err_codef(p, "sircc.vec.shuffle.idx.len_bad", "sircc: vec.shuffle node %lld flags.idx length must equal lanes", (long long)n->id);
      goto bad;
    }
    for (size_t i = 0; i < idxs->v.arr.len; i++) {
      int64_t x = 0;
      if (!json_get_i64(idxs->v.arr.items[i], &x)) {
        err_codef(p, "sircc.vec.shuffle.idx.elem_bad", "sircc: vec.shuffle node %lld flags.idx[%zu] must be an integer", (long long)n->id, i);
        goto bad;
      }
    }
    goto ok;
  }

  if (strcmp(n->tag, "load.vec") == 0) {
    if (n->type_ref == 0 || !is_vec_type_id(p, n->type_ref, NULL, NULL)) {
      err_codef(p, "sircc.load.vec.type.bad", "sircc: load.vec node %lld type_ref must be a vec type", (long long)n->id);
      goto bad;
    }
    JsonValue* addr = n->fields ? json_obj_get(n->fields, "addr") : NULL;
    int64_t aid = 0;
    if (!parse_node_ref_id(p, addr, &aid)) {
      err_codef(p, "sircc.load.vec.addr.ref_bad", "sircc: load.vec node %lld missing fields.addr ref", (long long)n->id);
      goto bad;
    }
    NodeRec* a = get_node(p, aid);
    if (!a) {
      err_codef(p, "sircc.load.vec.addr.ref_bad", "sircc: load.vec node %lld addr references unknown node %lld", (long long)n->id, (long long)aid);
      goto bad;
    }
    if (a->type_ref) {
      TypeRec* at = get_type(p, a->type_ref);
      if (!at || at->kind != TYPE_PTR) {
        err_codef(p, "sircc.load.vec.addr.not_ptr", "sircc: load.vec node %lld requires pointer addr", (long long)n->id);
        goto bad;
      }
    }
    goto ok;
  }

  if (strcmp(n->tag, "store.vec") == 0) {
    JsonValue* addr = n->fields ? json_obj_get(n->fields, "addr") : NULL;
    JsonValue* val = n->fields ? json_obj_get(n->fields, "value") : NULL;
    int64_t aid = 0, vid = 0;
    if (!parse_node_ref_id(p, addr, &aid) || !parse_node_ref_id(p, val, &vid)) {
      err_codef(p, "sircc.store.vec.addr_value.ref_bad", "sircc: store.vec node %lld requires fields.addr and fields.value refs", (long long)n->id);
      goto bad;
    }
    NodeRec* a = get_node(p, aid);
    if (!a) {
      err_codef(p, "sircc.store.vec.addr_value.ref_bad", "sircc: store.vec node %lld addr references unknown node %lld", (long long)n->id, (long long)aid);
      goto bad;
    }
    if (a->type_ref) {
      TypeRec* at = get_type(p, a->type_ref);
      if (!at || at->kind != TYPE_PTR) {
        err_codef(p, "sircc.store.vec.addr.not_ptr", "sircc: store.vec node %lld requires pointer addr", (long long)n->id);
        goto bad;
      }
    }
    NodeRec* v = get_node(p, vid);
    int64_t vec_ty = v ? v->type_ref : 0;
    if (vec_ty == 0) {
      (void)parse_type_ref_id(p, n->fields ? json_obj_get(n->fields, "ty") : NULL, &vec_ty);
    }
    if (!vec_ty || !is_vec_type_id(p, vec_ty, NULL, NULL)) {
      err_codef(p, "sircc.store.vec.type.bad", "sircc: store.vec node %lld requires vec type (value.type_ref or fields.ty)", (long long)n->id);
      goto bad;
    }
    if (v && v->type_ref && v->type_ref != vec_ty) {
      err_codef(p, "sircc.store.vec.type.mismatch", "sircc: store.vec node %lld value vec type does not match fields.ty", (long long)n->id);
      goto bad;
    }
    goto ok;
  }

  if (strcmp(n->tag, "vec.bitcast") == 0) {
    if (!n->fields) {
      err_codef(p, "sircc.vec.bitcast.missing_fields", "sircc: vec.bitcast node %lld missing fields", (long long)n->id);
      goto bad;
    }
    int64_t from_id = 0, to_id = 0;
    if (!parse_type_ref_id(p, json_obj_get(n->fields, "from"), &from_id) || !parse_type_ref_id(p, json_obj_get(n->fields, "to"), &to_id)) {
      err_codef(p, "sircc.vec.bitcast.from_to.bad", "sircc: vec.bitcast node %lld requires fields.from and fields.to type refs", (long long)n->id);
      goto bad;
    }
    if (!is_vec_type_id(p, from_id, NULL, NULL) || !is_vec_type_id(p, to_id, NULL, NULL)) {
      err_codef(p, "sircc.vec.bitcast.type.bad", "sircc: vec.bitcast node %lld from/to must be vec types", (long long)n->id);
      goto bad;
    }
    int64_t from_sz = 0, from_al = 0, to_sz = 0, to_al = 0;
    if (!type_size_align(p, from_id, &from_sz, &from_al) || !type_size_align(p, to_id, &to_sz, &to_al) || from_sz != to_sz) {
      err_codef(p, "sircc.vec.bitcast.size_mismatch", "sircc: vec.bitcast node %lld requires sizeof(from)==sizeof(to)", (long long)n->id);
      goto bad;
    }
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
      err_codef(p, "sircc.vec.bitcast.args.bad", "sircc: vec.bitcast node %lld requires args:[v]", (long long)n->id);
      goto bad;
    }
    int64_t vid = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &vid)) {
      err_codef(p, "sircc.vec.bitcast.args.ref_bad", "sircc: vec.bitcast node %lld args[0] must be a node ref", (long long)n->id);
      goto bad;
    }
    NodeRec* v = get_node(p, vid);
    if (!v || v->type_ref != from_id) {
      err_codef(p, "sircc.vec.bitcast.v.type.bad", "sircc: vec.bitcast node %lld value must have type from", (long long)n->id);
      goto bad;
    }
    goto ok;
  }

  // Remaining vec.* families: validate arity + type shape (best-effort).
  if (strncmp(n->tag, "vec.cmp.", 8) == 0 || strcmp(n->tag, "vec.select") == 0 || strcmp(n->tag, "vec.add") == 0 || strcmp(n->tag, "vec.sub") == 0 ||
      strcmp(n->tag, "vec.mul") == 0 || strcmp(n->tag, "vec.and") == 0 || strcmp(n->tag, "vec.or") == 0 || strcmp(n->tag, "vec.xor") == 0 ||
      strcmp(n->tag, "vec.not") == 0) {
    if (!args || args->type != JSON_ARRAY) {
      err_codef(p, "sircc.vec.op.args.bad", "sircc: %s node %lld requires args array", n->tag, (long long)n->id);
      goto bad;
    }
    // Defer deep type checking to lowering for now; but keep arity tight.
    size_t want = 0;
    if (strcmp(n->tag, "vec.not") == 0) want = 1;
    else if (strncmp(n->tag, "vec.cmp.", 8) == 0) want = 2;
    else if (strcmp(n->tag, "vec.select") == 0) want = 3;
    else want = 2;
    if (args->v.arr.len != want) {
      err_codef(p, "sircc.vec.op.arity_bad", "sircc: %s node %lld requires %zu args", n->tag, (long long)n->id, want);
      goto bad;
    }

    // For vec.cmp.*, ensure there is a bool vec type when type_ref is absent.
    if (strncmp(n->tag, "vec.cmp.", 8) == 0 && n->type_ref == 0) {
      int64_t aid = 0;
      if (parse_node_ref_id(p, args->v.arr.items[0], &aid)) {
        NodeRec* a = get_node(p, aid);
        TypeRec* src = NULL;
        TypeRec* lane = NULL;
        if (a && is_vec_type_id(p, a->type_ref, &src, &lane)) {
          int64_t bty = find_bool_vec_type_id(p, src->lanes);
          if (bty == 0) {
            err_codef(p, "sircc.vec.cmp.bool_ty_missing",
                      "sircc: %s node %lld requires a vec(bool,%lld) type definition to exist in the stream", n->tag, (long long)n->id,
                      (long long)src->lanes);
            goto bad;
          }
        }
      }
    }
    goto ok;
  }

ok:
  sir_diag_pop(p, saved);
  return true;
bad:
  sir_diag_pop(p, saved);
  return false;
}

bool validate_program(SirProgram* p) {
  // Validate CFG-form functions even under --verify-only.
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if (strcmp(n->tag, "fn") != 0) continue;
    if (!n->fields) continue;
    JsonValue* blocks = json_obj_get(n->fields, "blocks");
    JsonValue* entry = json_obj_get(n->fields, "entry");
    if (blocks || entry) {
      if (!validate_cfg_fn(p, n)) return false;
    }
  }

  // Feature gates for node-based streams (meta.ext.features can appear anywhere, so do this post-parse).
  if (p->feat_closure_v1 && !p->feat_fun_v1) {
    err_codef(p, "sircc.feature.dep", "sircc: feature closure:v1 requires fun:v1");
    return false;
  }

  for (size_t i = 0; i < p->types_cap; i++) {
    TypeRec* t = p->types[i];
    if (!t) continue;
    if (t->kind == TYPE_VEC && !p->feat_simd_v1) {
      SirDiagSaved saved = sir_diag_push(p, "type", t->id, "vec");
      err_codef(p, "sircc.feature.gate", "sircc: type kind 'vec' requires feature simd:v1 (enable via meta.ext.features)");
      sir_diag_pop(p, saved);
      return false;
    }
    if (t->kind == TYPE_FUN && !p->feat_fun_v1) {
      SirDiagSaved saved = sir_diag_push(p, "type", t->id, "fun");
      err_codef(p, "sircc.feature.gate", "sircc: type kind 'fun' requires feature fun:v1 (enable via meta.ext.features)");
      sir_diag_pop(p, saved);
      return false;
    }
    if (t->kind == TYPE_CLOSURE && !p->feat_closure_v1) {
      SirDiagSaved saved = sir_diag_push(p, "type", t->id, "closure");
      err_codef(p, "sircc.feature.gate", "sircc: type kind 'closure' requires feature closure:v1 (enable via meta.ext.features)");
      sir_diag_pop(p, saved);
      return false;
    }
    if (t->kind == TYPE_SUM && !p->feat_adt_v1) {
      SirDiagSaved saved = sir_diag_push(p, "type", t->id, "sum");
      err_codef(p, "sircc.feature.gate", "sircc: type kind 'sum' requires feature adt:v1 (enable via meta.ext.features)");
      sir_diag_pop(p, saved);
      return false;
    }

    if (t->kind == TYPE_VEC) {
      TypeRec* lane = get_type(p, t->lane_ty);
      if (!lane || lane->kind != TYPE_PRIM || !lane->prim) {
        SirDiagSaved saved = sir_diag_push(p, "type", t->id, "vec");
        err_codef(p, "sircc.type.vec.lane.bad", "sircc: type.vec lane must reference a primitive lane type");
        sir_diag_pop(p, saved);
        return false;
      }
      const char* lp = lane->prim;
      bool ok = (strcmp(lp, "i8") == 0 || strcmp(lp, "i16") == 0 || strcmp(lp, "i32") == 0 || strcmp(lp, "i64") == 0 ||
                 strcmp(lp, "f32") == 0 || strcmp(lp, "f64") == 0 || strcmp(lp, "bool") == 0 || strcmp(lp, "i1") == 0);
      if (!ok) {
        SirDiagSaved saved = sir_diag_push(p, "type", t->id, "vec");
        err_codef(p, "sircc.type.vec.lane.unsupported", "sircc: type.vec lane must be one of i8/i16/i32/i64/f32/f64/bool");
        sir_diag_pop(p, saved);
        return false;
      }
      if (t->lanes <= 0) {
        SirDiagSaved saved = sir_diag_push(p, "type", t->id, "vec");
        err_codef(p, "sircc.type.vec.lanes.bad", "sircc: type.vec lanes must be > 0");
        sir_diag_pop(p, saved);
        return false;
      }
    }
  }

  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if ((strncmp(n->tag, "vec.", 4) == 0 || strcmp(n->tag, "load.vec") == 0 || strcmp(n->tag, "store.vec") == 0) && !p->feat_simd_v1) {
      SirDiagSaved saved = sir_diag_push_node(p, n);
      err_codef(p, "sircc.feature.gate", "sircc: mnemonic '%s' requires feature simd:v1 (enable via meta.ext.features)", n->tag);
      sir_diag_pop(p, saved);
      return false;
    }
    if ((strcmp(n->tag, "call.fun") == 0 || strncmp(n->tag, "fun.", 4) == 0) && !p->feat_fun_v1) {
      SirDiagSaved saved = sir_diag_push_node(p, n);
      err_codef(p, "sircc.feature.gate", "sircc: mnemonic '%s' requires feature fun:v1 (enable via meta.ext.features)", n->tag);
      sir_diag_pop(p, saved);
      return false;
    }
    if ((strcmp(n->tag, "call.closure") == 0 || strncmp(n->tag, "closure.", 8) == 0) && !p->feat_closure_v1) {
      SirDiagSaved saved = sir_diag_push_node(p, n);
      err_codef(p, "sircc.feature.gate", "sircc: mnemonic '%s' requires feature closure:v1 (enable via meta.ext.features)", n->tag);
      sir_diag_pop(p, saved);
      return false;
    }
    if ((strncmp(n->tag, "adt.", 4) == 0) && !p->feat_adt_v1) {
      SirDiagSaved saved = sir_diag_push_node(p, n);
      err_codef(p, "sircc.feature.gate", "sircc: mnemonic '%s' requires feature adt:v1 (enable via meta.ext.features)", n->tag);
      sir_diag_pop(p, saved);
      return false;
    }
    if ((strncmp(n->tag, "sem.", 4) == 0) && !p->feat_sem_v1) {
      SirDiagSaved saved = sir_diag_push_node(p, n);
      err_codef(p, "sircc.feature.gate", "sircc: mnemonic '%s' requires feature sem:v1 (enable via meta.ext.features)", n->tag);
      sir_diag_pop(p, saved);
      return false;
    }
    if (strcmp(n->tag, "sem.match_sum") == 0 && p->feat_sem_v1 && !p->feat_adt_v1) {
      SirDiagSaved saved = sir_diag_push_node(p, n);
      err_codef(p, "sircc.feature.dep", "sircc: sem.match_sum requires adt:v1");
      sir_diag_pop(p, saved);
      return false;
    }
  }

  // SIMD semantic checks (close the "verify-only vs lowering" delta).
  if (p->feat_simd_v1) {
    for (size_t i = 0; i < p->nodes_cap; i++) {
      NodeRec* n = p->nodes[i];
      if (!n) continue;
      if (!validate_simd_node(p, n)) return false;
    }
  }

  // fun/closure/adt/sem semantic checks (close the "verify-only vs lowering" delta).
  if (p->feat_fun_v1) {
    for (size_t i = 0; i < p->nodes_cap; i++) {
      NodeRec* n = p->nodes[i];
      if (!n) continue;
      if (!validate_fun_node(p, n)) return false;
    }
  }
  if (p->feat_closure_v1) {
    for (size_t i = 0; i < p->nodes_cap; i++) {
      NodeRec* n = p->nodes[i];
      if (!n) continue;
      if (!validate_closure_node(p, n)) return false;
    }
  }
  if (p->feat_adt_v1) {
    for (size_t i = 0; i < p->nodes_cap; i++) {
      NodeRec* n = p->nodes[i];
      if (!n) continue;
      if (!validate_adt_node(p, n)) return false;
    }
  }
  if (p->feat_sem_v1) {
    for (size_t i = 0; i < p->nodes_cap; i++) {
      NodeRec* n = p->nodes[i];
      if (!n) continue;
      if (!validate_sem_node(p, n)) return false;
    }
  }

  return true;
}

static size_t block_param_count(SirProgram* p, int64_t block_id) {
  NodeRec* b = get_node(p, block_id);
  if (!b || strcmp(b->tag, "block") != 0 || !b->fields) return 0;
  JsonValue* params = json_obj_get(b->fields, "params");
  if (!params) return 0;
  if (params->type != JSON_ARRAY) return (size_t)-1;
  return params->v.arr.len;
}

static bool validate_block_params(SirProgram* p, int64_t block_id) {
  NodeRec* b = get_node(p, block_id);
  if (!b || strcmp(b->tag, "block") != 0) {
    SirDiagSaved saved = sir_diag_push(p, "node", block_id, b ? b->tag : NULL);
    err_codef(p, "sircc.cfg.block.not_block", "sircc: block ref %lld is not a block node", (long long)block_id);
    sir_diag_pop(p, saved);
    return false;
  }
  JsonValue* params = b->fields ? json_obj_get(b->fields, "params") : NULL;
  if (!params) return true;
  if (params->type != JSON_ARRAY) {
    SirDiagSaved saved = sir_diag_push_node(p, b);
    err_codef(p, "sircc.cfg.block.params.not_array", "sircc: block %lld params must be an array", (long long)block_id);
    sir_diag_pop(p, saved);
    return false;
  }
  for (size_t i = 0; i < params->v.arr.len; i++) {
    int64_t pid = 0;
    if (!parse_node_ref_id(p, params->v.arr.items[i], &pid)) {
      SirDiagSaved saved = sir_diag_push_node(p, b);
      err_codef(p, "sircc.cfg.block.param.not_ref", "sircc: block %lld params[%zu] must be node refs", (long long)block_id, i);
      sir_diag_pop(p, saved);
      return false;
    }
    NodeRec* pn = get_node(p, pid);
    if (!pn || strcmp(pn->tag, "bparam") != 0) {
      SirDiagSaved saved = sir_diag_push_node(p, b);
      err_codef(p, "sircc.cfg.block.param.not_bparam", "sircc: block %lld params[%zu] must reference bparam nodes", (long long)block_id,
                i);
      sir_diag_pop(p, saved);
      return false;
    }
    if (pn->type_ref == 0) {
      SirDiagSaved saved = sir_diag_push_node(p, pn);
      err_codef(p, "sircc.cfg.bparam.missing_type", "sircc: bparam node %lld missing type_ref", (long long)pid);
      sir_diag_pop(p, saved);
      return false;
    }
  }
  return true;
}

static bool validate_branch_args(SirProgram* p, int64_t to_block_id, JsonValue* args) {
  size_t pc = block_param_count(p, to_block_id);
  if (pc == (size_t)-1) {
    NodeRec* b = get_node(p, to_block_id);
    SirDiagSaved saved = sir_diag_push(p, "node", to_block_id, b ? b->tag : NULL);
    err_codef(p, "sircc.cfg.block.params.not_array", "sircc: block %lld params must be an array", (long long)to_block_id);
    sir_diag_pop(p, saved);
    return false;
  }
  size_t ac = 0;
  if (args) {
    if (args->type != JSON_ARRAY) {
      err_codef(p, "sircc.cfg.branch.args.not_array", "sircc: branch args must be an array");
      return false;
    }
    ac = args->v.arr.len;
  }
  if (pc != ac) {
    NodeRec* b = get_node(p, to_block_id);
    SirDiagSaved saved = sir_diag_push(p, "node", to_block_id, b ? b->tag : NULL);
    err_codef(p, "sircc.cfg.branch.args.count_mismatch", "sircc: block %lld param/arg count mismatch (params=%zu, args=%zu)",
              (long long)to_block_id, pc, ac);
    sir_diag_pop(p, saved);
    return false;
  }
  for (size_t i = 0; i < ac; i++) {
    int64_t aid = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[i], &aid)) {
      err_codef(p, "sircc.cfg.branch.arg.not_ref", "sircc: branch args[%zu] must be node refs", i);
      return false;
    }
    if (!get_node(p, aid)) {
      err_codef(p, "sircc.cfg.branch.arg.unknown_node", "sircc: branch args[%zu] references unknown node %lld", i, (long long)aid);
      return false;
    }
  }
  return true;
}

static bool validate_terminator(SirProgram* p, int64_t term_id) {
  NodeRec* t = get_node(p, term_id);
  if (!t) {
    SirDiagSaved saved = sir_diag_push(p, "node", term_id, NULL);
    err_codef(p, "sircc.cfg.term.unknown", "sircc: block terminator references unknown node %lld", (long long)term_id);
    sir_diag_pop(p, saved);
    return false;
  }
  SirDiagSaved saved = sir_diag_push_node(p, t);
  if (strncmp(t->tag, "term.", 5) != 0 && strcmp(t->tag, "return") != 0) {
    err_codef(p, "sircc.cfg.term.not_terminator", "sircc: block must end with a terminator (got '%s')", t->tag);
    sir_diag_pop(p, saved);
    return false;
  }

  if (strcmp(t->tag, "term.br") == 0) {
    if (!t->fields) {
      err_codef(p, "sircc.cfg.term.missing_fields", "sircc: term.br missing fields");
      sir_diag_pop(p, saved);
      return false;
    }
    int64_t to_id = 0;
    if (!parse_node_ref_id(p, json_obj_get(t->fields, "to"), &to_id)) {
      err_codef(p, "sircc.cfg.term.br.missing_to", "sircc: term.br missing to ref");
      sir_diag_pop(p, saved);
      return false;
    }
    if (!validate_block_params(p, to_id)) {
      sir_diag_pop(p, saved);
      return false;
    }
    bool ok = validate_branch_args(p, to_id, json_obj_get(t->fields, "args"));
    sir_diag_pop(p, saved);
    return ok;
  }

  if (strcmp(t->tag, "term.cbr") == 0 || strcmp(t->tag, "term.condbr") == 0) {
    if (!t->fields) {
      err_codef(p, "sircc.cfg.term.missing_fields", "sircc: %s missing fields", t->tag);
      sir_diag_pop(p, saved);
      return false;
    }
    int64_t cond_id = 0;
    if (!parse_node_ref_id(p, json_obj_get(t->fields, "cond"), &cond_id)) {
      err_codef(p, "sircc.cfg.term.cbr.missing_cond", "sircc: %s missing cond ref", t->tag);
      sir_diag_pop(p, saved);
      return false;
    }
    if (!get_node(p, cond_id)) {
      err_codef(p, "sircc.cfg.term.cbr.cond.unknown_node", "sircc: %s cond references unknown node %lld", t->tag, (long long)cond_id);
      sir_diag_pop(p, saved);
      return false;
    }
    JsonValue* thenb = json_obj_get(t->fields, "then");
    JsonValue* elseb = json_obj_get(t->fields, "else");
    if (!thenb || thenb->type != JSON_OBJECT || !elseb || elseb->type != JSON_OBJECT) {
      err_codef(p, "sircc.cfg.term.cbr.missing_branches", "sircc: %s requires then/else objects", t->tag);
      sir_diag_pop(p, saved);
      return false;
    }
    int64_t then_id = 0, else_id = 0;
    if (!parse_node_ref_id(p, json_obj_get(thenb, "to"), &then_id) || !parse_node_ref_id(p, json_obj_get(elseb, "to"), &else_id)) {
      err_codef(p, "sircc.cfg.term.cbr.missing_to", "sircc: %s then/else missing to ref", t->tag);
      sir_diag_pop(p, saved);
      return false;
    }
    if (!validate_block_params(p, then_id) || !validate_block_params(p, else_id)) {
      sir_diag_pop(p, saved);
      return false;
    }
    if (!validate_branch_args(p, then_id, json_obj_get(thenb, "args"))) {
      sir_diag_pop(p, saved);
      return false;
    }
    if (!validate_branch_args(p, else_id, json_obj_get(elseb, "args"))) {
      sir_diag_pop(p, saved);
      return false;
    }
    sir_diag_pop(p, saved);
    return true;
  }

  if (strcmp(t->tag, "term.switch") == 0) {
    if (!t->fields) {
      err_codef(p, "sircc.cfg.term.missing_fields", "sircc: term.switch missing fields");
      sir_diag_pop(p, saved);
      return false;
    }
    int64_t scrut_id = 0;
    if (!parse_node_ref_id(p, json_obj_get(t->fields, "scrut"), &scrut_id)) {
      err_codef(p, "sircc.cfg.term.switch.missing_scrut", "sircc: term.switch missing scrut ref");
      sir_diag_pop(p, saved);
      return false;
    }
    if (!get_node(p, scrut_id)) {
      err_codef(p, "sircc.cfg.term.switch.scrut.unknown_node", "sircc: term.switch scrut references unknown node %lld",
                (long long)scrut_id);
      sir_diag_pop(p, saved);
      return false;
    }
    JsonValue* def = json_obj_get(t->fields, "default");
    if (!def || def->type != JSON_OBJECT) {
      err_codef(p, "sircc.cfg.term.switch.missing_default", "sircc: term.switch missing default branch");
      sir_diag_pop(p, saved);
      return false;
    }
    int64_t def_id = 0;
    if (!parse_node_ref_id(p, json_obj_get(def, "to"), &def_id)) {
      err_codef(p, "sircc.cfg.term.switch.default.missing_to", "sircc: term.switch default missing to ref");
      sir_diag_pop(p, saved);
      return false;
    }
    if (!validate_block_params(p, def_id)) {
      sir_diag_pop(p, saved);
      return false;
    }
    if (!validate_branch_args(p, def_id, json_obj_get(def, "args"))) {
      sir_diag_pop(p, saved);
      return false;
    }
    JsonValue* cases = json_obj_get(t->fields, "cases");
    if (!cases || cases->type != JSON_ARRAY) {
      err_codef(p, "sircc.cfg.term.switch.cases.not_array", "sircc: term.switch missing cases array");
      sir_diag_pop(p, saved);
      return false;
    }
    for (size_t i = 0; i < cases->v.arr.len; i++) {
      JsonValue* c = cases->v.arr.items[i];
      if (!c || c->type != JSON_OBJECT) {
        err_codef(p, "sircc.cfg.term.switch.case.not_object", "sircc: term.switch case[%zu] must be object", i);
        sir_diag_pop(p, saved);
        return false;
      }
      int64_t to_id = 0;
      if (!parse_node_ref_id(p, json_obj_get(c, "to"), &to_id)) {
        err_codef(p, "sircc.cfg.term.switch.case.missing_to", "sircc: term.switch case[%zu] missing to ref", i);
        sir_diag_pop(p, saved);
        return false;
      }
      if (!validate_block_params(p, to_id)) {
        sir_diag_pop(p, saved);
        return false;
      }
      if (!validate_branch_args(p, to_id, json_obj_get(c, "args"))) {
        sir_diag_pop(p, saved);
        return false;
      }
      int64_t lit_id = 0;
      if (!parse_node_ref_id(p, json_obj_get(c, "lit"), &lit_id)) {
        err_codef(p, "sircc.cfg.term.switch.case.missing_lit", "sircc: term.switch case[%zu] missing lit ref", i);
        sir_diag_pop(p, saved);
        return false;
      }
      NodeRec* litn = get_node(p, lit_id);
      if (!litn || strncmp(litn->tag, "const.", 6) != 0) {
        err_codef(p, "sircc.cfg.term.switch.case.bad_lit", "sircc: term.switch case[%zu] lit must be const.* node", i);
        sir_diag_pop(p, saved);
        return false;
      }
    }
    sir_diag_pop(p, saved);
    return true;
  }

  sir_diag_pop(p, saved);
  return true;
}

static bool validate_cfg_fn(SirProgram* p, NodeRec* fn) {
  SirDiagSaved saved = sir_diag_push_node(p, fn);
  JsonValue* blocks = json_obj_get(fn->fields, "blocks");
  JsonValue* entry = json_obj_get(fn->fields, "entry");
  if (!blocks || blocks->type != JSON_ARRAY || !entry) {
    err_codef(p, "sircc.cfg.fn.missing_fields", "sircc: fn %lld CFG form requires fields.blocks (array) and fields.entry (ref)",
              (long long)fn->id);
    sir_diag_pop(p, saved);
    return false;
  }
  int64_t entry_id = 0;
  if (!parse_node_ref_id(p, entry, &entry_id)) {
    err_codef(p, "sircc.cfg.fn.entry.bad_ref", "sircc: fn %lld entry must be a block ref", (long long)fn->id);
    sir_diag_pop(p, saved);
    return false;
  }

  // mark blocks in this fn for quick membership
  unsigned char* in_fn = (unsigned char*)calloc(p->nodes_cap ? p->nodes_cap : 1, 1);
  if (!in_fn) {
    sir_diag_pop(p, saved);
    return false;
  }
  for (size_t i = 0; i < blocks->v.arr.len; i++) {
    int64_t bid = 0;
    if (!parse_node_ref_id(p, blocks->v.arr.items[i], &bid)) {
      err_codef(p, "sircc.cfg.fn.blocks.bad_ref", "sircc: fn %lld blocks[%zu] must be block refs", (long long)fn->id, i);
      free(in_fn);
      sir_diag_pop(p, saved);
      return false;
    }
    if (bid >= 0 && (size_t)bid < p->nodes_cap) in_fn[bid] = 1;
    if (!validate_block_params(p, bid)) {
      free(in_fn);
      sir_diag_pop(p, saved);
      return false;
    }
  }
  if (entry_id < 0 || (size_t)entry_id >= p->nodes_cap || !in_fn[entry_id]) {
    err_codef(p, "sircc.cfg.fn.entry.not_in_blocks", "sircc: fn %lld entry block %lld not in blocks list", (long long)fn->id,
              (long long)entry_id);
    free(in_fn);
    sir_diag_pop(p, saved);
    return false;
  }

  for (size_t i = 0; i < blocks->v.arr.len; i++) {
    int64_t bid = 0;
    (void)parse_node_ref_id(p, blocks->v.arr.items[i], &bid);
    NodeRec* b = get_node(p, bid);
    if (!b || strcmp(b->tag, "block") != 0) {
      SirDiagSaved bsaved = sir_diag_push(p, "node", bid, b ? b->tag : NULL);
      err_codef(p, "sircc.cfg.fn.blocks.not_block", "sircc: fn %lld blocks[%zu] references non-block %lld", (long long)fn->id, i,
                (long long)bid);
      sir_diag_pop(p, bsaved);
      free(in_fn);
      sir_diag_pop(p, saved);
      return false;
    }
    JsonValue* stmts = b->fields ? json_obj_get(b->fields, "stmts") : NULL;
    if (!stmts || stmts->type != JSON_ARRAY || stmts->v.arr.len == 0) {
      SirDiagSaved bsaved = sir_diag_push_node(p, b);
      err_codef(p, "sircc.cfg.block.stmts.not_array", "sircc: block %lld must have non-empty stmts array", (long long)bid);
      sir_diag_pop(p, bsaved);
      free(in_fn);
      sir_diag_pop(p, saved);
      return false;
    }
    for (size_t si = 0; si < stmts->v.arr.len; si++) {
      int64_t sid = 0;
      if (!parse_node_ref_id(p, stmts->v.arr.items[si], &sid)) {
        SirDiagSaved bsaved = sir_diag_push_node(p, b);
        err_codef(p, "sircc.cfg.block.stmt.not_ref", "sircc: block %lld stmts[%zu] must be node refs", (long long)bid, si);
        sir_diag_pop(p, bsaved);
        free(in_fn);
        sir_diag_pop(p, saved);
        return false;
      }
      NodeRec* sn = get_node(p, sid);
      if (!sn) {
        SirDiagSaved bsaved = sir_diag_push_node(p, b);
        err_codef(p, "sircc.cfg.block.stmt.unknown_node", "sircc: block %lld stmts[%zu] references unknown node %lld", (long long)bid, si,
                  (long long)sid);
        sir_diag_pop(p, bsaved);
        free(in_fn);
        sir_diag_pop(p, saved);
        return false;
      }
      bool is_term = (strncmp(sn->tag, "term.", 5) == 0) || (strcmp(sn->tag, "return") == 0);
      if (is_term && si + 1 != stmts->v.arr.len) {
        SirDiagSaved ssaved = sir_diag_push_node(p, sn);
        err_codef(p, "sircc.cfg.block.term.not_last", "sircc: block %lld has terminator before end (stmt %zu)", (long long)bid, si);
        sir_diag_pop(p, ssaved);
        free(in_fn);
        sir_diag_pop(p, saved);
        return false;
      }
      if (si + 1 == stmts->v.arr.len) {
        if (!is_term) {
          SirDiagSaved ssaved = sir_diag_push_node(p, sn);
          err_codef(p, "sircc.cfg.block.term.missing", "sircc: block %lld must end with a terminator (got '%s')", (long long)bid, sn->tag);
          sir_diag_pop(p, ssaved);
          free(in_fn);
          sir_diag_pop(p, saved);
          return false;
        }
        if (!validate_terminator(p, sid)) {
          free(in_fn);
          sir_diag_pop(p, saved);
          return false;
        }
      }
    }
  }

  free(in_fn);
  sir_diag_pop(p, saved);
  return true;
}
