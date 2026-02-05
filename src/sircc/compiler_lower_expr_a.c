// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_lower_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LLVMTypeRef build_closure_code_sig(FunctionCtx* f, TypeRec* cty) {
  if (!f || !cty || cty->kind != TYPE_CLOSURE) return NULL;
  TypeRec* cs = get_type(f->p, cty->call_sig);
  if (!cs || cs->kind != TYPE_FN) return NULL;
  LLVMTypeRef env = lower_type(f->p, f->ctx, cty->env_ty);
  LLVMTypeRef ret = lower_type(f->p, f->ctx, cs->ret);
  if (!env || !ret) return NULL;
  size_t nparams = cs->param_len + 1;
  if (nparams > UINT_MAX) return NULL;
  LLVMTypeRef* params = (LLVMTypeRef*)malloc(nparams * sizeof(LLVMTypeRef));
  if (!params) return NULL;
  params[0] = env;
  bool ok = true;
  for (size_t i = 0; i < cs->param_len; i++) {
    params[i + 1] = lower_type(f->p, f->ctx, cs->params[i]);
    if (!params[i + 1]) {
      ok = false;
      break;
    }
  }
  LLVMTypeRef out = NULL;
  if (ok) out = LLVMFunctionType(ret, params, (unsigned)nparams, cs->varargs ? 1 : 0);
  free(params);
  return out;
}

static LLVMValueRef call_fun_value(FunctionCtx* f, int64_t callee_id, LLVMValueRef* argv, size_t argc, LLVMTypeRef want_ret) {
  NodeRec* callee_n = get_node(f->p, callee_id);
  if (!callee_n || callee_n->type_ref == 0) {
    errf(f->p, "sircc: expected fun callee with type_ref");
    return NULL;
  }
  TypeRec* callee_ty = get_type(f->p, callee_n->type_ref);
  if (!callee_ty || callee_ty->kind != TYPE_FUN || callee_ty->sig == 0) {
    errf(f->p, "sircc: expected fun callee type");
    return NULL;
  }
  LLVMTypeRef callee_fty = lower_type(f->p, f->ctx, callee_ty->sig);
  if (!callee_fty || LLVMGetTypeKind(callee_fty) != LLVMFunctionTypeKind) return NULL;
  LLVMValueRef callee = lower_expr(f, callee_id);
  if (!callee) return NULL;

  unsigned param_count = LLVMCountParamTypes(callee_fty);
  bool is_varargs = LLVMIsFunctionVarArg(callee_fty) != 0;
  if (!is_varargs && (unsigned)argc != param_count) {
    errf(f->p, "sircc: fun call arg count mismatch (got %zu, want %u)", argc, param_count);
    return NULL;
  }
  if ((unsigned)argc < param_count) {
    errf(f->p, "sircc: fun call missing required args (got %zu, want >= %u)", argc, param_count);
    return NULL;
  }
  if (param_count) {
    LLVMTypeRef* params = (LLVMTypeRef*)malloc(param_count * sizeof(LLVMTypeRef));
    if (!params) return NULL;
    LLVMGetParamTypes(callee_fty, params);
    for (unsigned i = 0; i < param_count; i++) {
      LLVMTypeRef want = params[i];
      LLVMTypeRef got = LLVMTypeOf(argv[i]);
      if (want == got) continue;
      if (LLVMGetTypeKind(want) == LLVMPointerTypeKind && LLVMGetTypeKind(got) == LLVMPointerTypeKind) {
        argv[i] = LLVMBuildBitCast(f->builder, argv[i], want, "arg.cast");
        continue;
      }
      free(params);
      errf(f->p, "sircc: fun call arg[%u] type mismatch", i);
      return NULL;
    }
    free(params);
  }

  LLVMValueRef out = LLVMBuildCall2(f->builder, callee_fty, callee, argv, (unsigned)argc, "call");
  if (out && want_ret && LLVMTypeOf(out) != want_ret) {
    errf(f->p, "sircc: fun call return type mismatch");
    return NULL;
  }
  return out;
}

static LLVMValueRef call_closure_value(FunctionCtx* f, int64_t callee_id, LLVMValueRef* user_argv, size_t user_argc, LLVMTypeRef want_ret) {
  NodeRec* callee_n = get_node(f->p, callee_id);
  if (!callee_n || callee_n->type_ref == 0) {
    errf(f->p, "sircc: expected closure callee with type_ref");
    return NULL;
  }
  TypeRec* callee_ty = get_type(f->p, callee_n->type_ref);
  if (!callee_ty || callee_ty->kind != TYPE_CLOSURE) {
    errf(f->p, "sircc: expected closure callee type");
    return NULL;
  }

  LLVMValueRef callee = lower_expr(f, callee_id);
  if (!callee) return NULL;
  LLVMValueRef code = LLVMBuildExtractValue(f->builder, callee, 0, "clo.code");
  LLVMValueRef env = LLVMBuildExtractValue(f->builder, callee, 1, "clo.env");
  if (!code || !env) return NULL;

  LLVMTypeRef code_sig = build_closure_code_sig(f, callee_ty);
  if (!code_sig || LLVMGetTypeKind(code_sig) != LLVMFunctionTypeKind) return NULL;

  size_t argc = user_argc + 1;
  LLVMValueRef* argv = (LLVMValueRef*)malloc(argc * sizeof(LLVMValueRef));
  if (!argv) return NULL;
  argv[0] = env;
  for (size_t i = 0; i < user_argc; i++) argv[i + 1] = user_argv[i];

  unsigned param_count = LLVMCountParamTypes(code_sig);
  bool is_varargs = LLVMIsFunctionVarArg(code_sig) != 0;
  if (!is_varargs && (unsigned)argc != param_count) {
    free(argv);
    errf(f->p, "sircc: closure call arg count mismatch (got %zu, want %u)", argc, param_count);
    return NULL;
  }
  if ((unsigned)argc < param_count) {
    free(argv);
    errf(f->p, "sircc: closure call missing required args (got %zu, want >= %u)", argc, param_count);
    return NULL;
  }
  if (param_count) {
    LLVMTypeRef* params = (LLVMTypeRef*)malloc(param_count * sizeof(LLVMTypeRef));
    if (!params) {
      free(argv);
      return NULL;
    }
    LLVMGetParamTypes(code_sig, params);
    for (unsigned i = 0; i < param_count; i++) {
      LLVMTypeRef want = params[i];
      LLVMTypeRef got = LLVMTypeOf(argv[i]);
      if (want == got) continue;
      if (LLVMGetTypeKind(want) == LLVMPointerTypeKind && LLVMGetTypeKind(got) == LLVMPointerTypeKind) {
        argv[i] = LLVMBuildBitCast(f->builder, argv[i], want, "arg.cast");
        continue;
      }
      free(params);
      free(argv);
      errf(f->p, "sircc: closure call arg[%u] type mismatch", i);
      return NULL;
    }
    free(params);
  }

  LLVMValueRef out = LLVMBuildCall2(f->builder, code_sig, code, argv, (unsigned)argc, "call");
  free(argv);
  if (out && want_ret && LLVMTypeOf(out) != want_ret) {
    errf(f->p, "sircc: closure call return type mismatch");
    return NULL;
  }
  return out;
}

static bool eval_branch_operand(FunctionCtx* f, JsonValue* br, LLVMTypeRef want_ty, LLVMValueRef* out) {
  if (!f || !br || br->type != JSON_OBJECT || !out) return false;
  const char* kind = json_get_string(json_obj_get(br, "kind"));
  if (!kind) return false;

  if (strcmp(kind, "val") == 0) {
    int64_t vid = 0;
    if (!parse_node_ref_id(f->p, json_obj_get(br, "v"), &vid)) return false;
    LLVMValueRef v = lower_expr(f, vid);
    if (!v) return false;
    if (want_ty && LLVMTypeOf(v) != want_ty) {
      errf(f->p, "sircc: branch value type mismatch");
      return false;
    }
    *out = v;
    return true;
  }

  if (strcmp(kind, "thunk") == 0) {
    int64_t fid = 0;
    if (!parse_node_ref_id(f->p, json_obj_get(br, "f"), &fid)) return false;
    NodeRec* fn = get_node(f->p, fid);
    if (!fn || fn->type_ref == 0) return false;
    TypeRec* t = get_type(f->p, fn->type_ref);
    if (!t) return false;

    // Only 0-arg thunks here.
    if (t->kind == TYPE_FUN) {
      TypeRec* sig = get_type(f->p, t->sig);
      if (!sig || sig->kind != TYPE_FN || sig->param_len != 0) {
        errf(f->p, "sircc: thunk fun must have () -> T signature");
        return false;
      }
      LLVMValueRef v = call_fun_value(f, fid, NULL, 0, want_ty);
      if (!v) return false;
      *out = v;
      return true;
    }
    if (t->kind == TYPE_CLOSURE) {
      TypeRec* sig = get_type(f->p, t->call_sig);
      if (!sig || sig->kind != TYPE_FN || sig->param_len != 0) {
        errf(f->p, "sircc: thunk closure must have () -> T signature");
        return false;
      }
      LLVMValueRef v = call_closure_value(f, fid, NULL, 0, want_ty);
      if (!v) return false;
      *out = v;
      return true;
    }
    errf(f->p, "sircc: thunk must be fun or closure");
    return false;
  }

  return false;
}

static LLVMValueRef sum_payload_load(FunctionCtx* f, TypeRec* sty, int64_t sum_ty_id, LLVMValueRef scrut, int64_t payload_ty_id) {
  if (!f || !sty || sty->kind != TYPE_SUM) return NULL;
  if (!scrut) return NULL;

  LLVMTypeRef sum_llvm = lower_type(f->p, f->ctx, sum_ty_id);
  if (!sum_llvm) return NULL;

  int64_t sum_sz = 0, sum_al = 0;
  if (!type_size_align(f->p, sum_ty_id, &sum_sz, &sum_al)) return NULL;

  LLVMValueRef slot = LLVMBuildAlloca(f->builder, sum_llvm, "sum.tmp");
  if (sum_al > 0 && sum_al <= 4096) LLVMSetAlignment(slot, (unsigned)sum_al);
  LLVMBuildStore(f->builder, scrut, slot);

  // Compute payload field index based on alignment.
  int64_t payload_size = 0;
  int64_t payload_align = 1;
  for (size_t i = 0; i < sty->variant_len; i++) {
    int64_t vty = sty->variants[i].ty;
    if (vty == 0) continue;
    int64_t vsz = 0;
    int64_t val = 0;
    if (type_size_align(f->p, vty, &vsz, &val)) {
      if (vsz > payload_size) payload_size = vsz;
      if (val > payload_align) payload_align = val;
    }
  }
  if (payload_align < 1) payload_align = 1;
  int64_t payload_off = 4;
  int64_t rem = payload_off % payload_align;
  if (rem) payload_off += (payload_align - rem);
  unsigned payload_field = (payload_off > 4) ? 2u : 1u;

  LLVMValueRef payp = LLVMBuildStructGEP2(f->builder, sum_llvm, slot, payload_field, "payloadp");
  LLVMTypeRef pay_ty = lower_type(f->p, f->ctx, payload_ty_id);
  if (!pay_ty) return NULL;
  LLVMValueRef castp = LLVMBuildBitCast(f->builder, payp, LLVMPointerType(pay_ty, 0), "pay.castp");
  LLVMValueRef ld = LLVMBuildLoad2(f->builder, pay_ty, castp, "payload");
  int64_t psz = 0, pal = 0;
  if (type_size_align(f->p, payload_ty_id, &psz, &pal) && pal > 0 && pal <= 4096) {
    LLVMSetAlignment(ld, (unsigned)pal);
  }
  return ld;
}

LLVMValueRef lower_expr(FunctionCtx* f, int64_t node_id) {
  NodeRec* n = get_node(f->p, node_id);
  if (!n) {
    errf(f->p, "sircc: unknown node id %lld", (long long)node_id);
    return NULL;
  }
  if ((strcmp(n->tag, "param") == 0 || strcmp(n->tag, "bparam") == 0) && n->llvm_value) {
    return n->llvm_value;
  }
  if (n->llvm_value) return n->llvm_value;
  if (n->resolving) {
    errf(f->p, "sircc: cyclic node reference at %lld", (long long)node_id);
    return NULL;
  }
  n->resolving = true;

  LLVMValueRef out = NULL;

  if (strcmp(n->tag, "name") == 0) {
    const char* name = NULL;
    if (n->fields) name = json_get_string(json_obj_get(n->fields, "name"));
    if (!name) {
      errf(f->p, "sircc: name node %lld missing fields.name", (long long)node_id);
      goto done;
    }
    out = bind_get(f, name);
    if (!out) errf(f->p, "sircc: unknown name '%s' in node %lld", name, (long long)node_id);
    goto done;
  }

  if (strcmp(n->tag, "decl.fn") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: decl.fn node %lld missing fields", (long long)node_id);
      goto done;
    }
    const char* name = json_get_string(json_obj_get(n->fields, "name"));
    if (!name || !is_ident(name)) {
      errf(f->p, "sircc: decl.fn node %lld requires fields.name Ident", (long long)node_id);
      goto done;
    }

    int64_t sig_id = n->type_ref;
    if (sig_id == 0) {
      if (!parse_type_ref_id(f->p, json_obj_get(n->fields, "sig"), &sig_id)) {
        errf(f->p, "sircc: decl.fn node %lld requires type_ref or fields.sig (fn type ref)", (long long)node_id);
        goto done;
      }
    }
    LLVMTypeRef fnty = lower_type(f->p, f->ctx, sig_id);
    if (!fnty || LLVMGetTypeKind(fnty) != LLVMFunctionTypeKind) {
      errf(f->p, "sircc: decl.fn node %lld signature must be a fn type (type %lld)", (long long)node_id, (long long)sig_id);
      goto done;
    }

    LLVMValueRef fn = LLVMGetNamedFunction(f->mod, name);
    if (!fn) {
      fn = LLVMAddFunction(f->mod, name, fnty);
      LLVMSetLinkage(fn, LLVMExternalLinkage);
    } else {
      LLVMTypeRef have = LLVMGlobalGetValueType(fn);
      if (have != fnty) {
        errf(f->p, "sircc: decl.fn '%s' type mismatch vs existing declaration/definition", name);
        goto done;
      }
    }
    out = fn;
    goto done;
  }

  if (strcmp(n->tag, "cstr") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: cstr node %lld missing fields", (long long)node_id);
      goto done;
    }
    const char* s = json_get_string(json_obj_get(n->fields, "value"));
    if (!s) {
      errf(f->p, "sircc: cstr node %lld requires fields.value string", (long long)node_id);
      goto done;
    }

    size_t len = strlen(s);
    LLVMValueRef init = LLVMConstStringInContext2(f->ctx, s, len, 0);
    LLVMTypeRef aty = LLVMTypeOf(init); // [len+1 x i8]

    char gname[64];
    snprintf(gname, sizeof(gname), ".str.%lld", (long long)node_id);
    LLVMValueRef g = LLVMGetNamedGlobal(f->mod, gname);
    if (!g) {
      g = LLVMAddGlobal(f->mod, aty, gname);
      LLVMSetInitializer(g, init);
      LLVMSetGlobalConstant(g, 1);
      LLVMSetLinkage(g, LLVMPrivateLinkage);
      LLVMSetUnnamedAddress(g, LLVMGlobalUnnamedAddr);
      LLVMSetAlignment(g, 1);
    }

    LLVMTypeRef i32 = LLVMInt32TypeInContext(f->ctx);
    LLVMValueRef idxs[2] = {LLVMConstInt(i32, 0, 0), LLVMConstInt(i32, 0, 0)};
    LLVMValueRef p = LLVMBuildInBoundsGEP2(f->builder, aty, g, idxs, 2, "cstr");

    LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
    out = LLVMBuildBitCast(f->builder, p, i8p, "cstr.ptr");
    goto done;
  }

  if (strcmp(n->tag, "binop.add") == 0) {
    JsonValue* lhs = n->fields ? json_obj_get(n->fields, "lhs") : NULL;
    JsonValue* rhs = n->fields ? json_obj_get(n->fields, "rhs") : NULL;
    int64_t lhs_id = 0, rhs_id = 0;
    if (!parse_node_ref_id(f->p, lhs, &lhs_id) || !parse_node_ref_id(f->p, rhs, &rhs_id)) {
      errf(f->p, "sircc: binop.add node %lld missing lhs/rhs refs", (long long)node_id);
      goto done;
    }
    LLVMValueRef a = lower_expr(f, lhs_id);
    LLVMValueRef b = lower_expr(f, rhs_id);
    if (!a || !b) goto done;
    LLVMTypeRef ty = LLVMTypeOf(a);
    if (LLVMGetTypeKind(ty) == LLVMIntegerTypeKind) {
      out = LLVMBuildAdd(f->builder, a, b, "add");
    } else {
      out = LLVMBuildFAdd(f->builder, a, b, "addf");
    }
    goto done;
  }

  if (strncmp(n->tag, "i", 1) == 0) {
    // Mnemonic-style integer ops: i8.add, i16.sub, i32.mul, etc.
    const char* dot = strchr(n->tag, '.');
    if (dot) {
      char wbuf[8];
      size_t wlen = (size_t)(dot - n->tag);
      if (wlen < sizeof(wbuf)) {
        memcpy(wbuf, n->tag, wlen);
        wbuf[wlen] = 0;
        int width = 0;
          if (sscanf(wbuf, "i%d", &width) == 1 && (width == 8 || width == 16 || width == 32 || width == 64)) {
            const char* op = dot + 1;
          JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
          int64_t a_id = 0, b_id = 0;
          // Extract operands.
          LLVMValueRef a = NULL;
          LLVMValueRef b = NULL;

          if (args && args->type == JSON_ARRAY) {
            if (args->v.arr.len == 1) {
              if (!parse_node_ref_id(f->p, args->v.arr.items[0], &a_id)) {
                errf(f->p, "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
                goto done;
              }
              a = lower_expr(f, a_id);
              if (!a) goto done;
            } else if (args->v.arr.len == 2) {
              if (!parse_node_ref_id(f->p, args->v.arr.items[0], &a_id) || !parse_node_ref_id(f->p, args->v.arr.items[1], &b_id)) {
                errf(f->p, "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
                goto done;
              }
              a = lower_expr(f, a_id);
              b = lower_expr(f, b_id);
              if (!a || !b) goto done;
            } else {
              errf(f->p, "sircc: %s node %lld args must have arity 1 or 2", n->tag, (long long)node_id);
              goto done;
            }
          } else {
            // Back-compat: allow lhs/rhs form for binary operators.
            JsonValue* lhs = n->fields ? json_obj_get(n->fields, "lhs") : NULL;
            JsonValue* rhs = n->fields ? json_obj_get(n->fields, "rhs") : NULL;
            if (parse_node_ref_id(f->p, lhs, &a_id) && parse_node_ref_id(f->p, rhs, &b_id)) {
              a = lower_expr(f, a_id);
              b = lower_expr(f, b_id);
              if (!a || !b) goto done;
            } else {
              errf(f->p, "sircc: %s node %lld missing args", n->tag, (long long)node_id);
              goto done;
            }
          }

          // Lower ops.
          if (strcmp(op, "add") == 0) {
            out = LLVMBuildAdd(f->builder, a, b, "iadd");
            goto done;
          }
          if (strcmp(op, "sub") == 0) {
            out = LLVMBuildSub(f->builder, a, b, "isub");
            goto done;
          }
          if (strcmp(op, "mul") == 0) {
            out = LLVMBuildMul(f->builder, a, b, "imul");
            goto done;
          }
          if (strcmp(op, "and") == 0) {
            out = LLVMBuildAnd(f->builder, a, b, "iand");
            goto done;
          }
          if (strcmp(op, "or") == 0) {
            out = LLVMBuildOr(f->builder, a, b, "ior");
            goto done;
          }
          if (strcmp(op, "xor") == 0) {
            out = LLVMBuildXor(f->builder, a, b, "ixor");
            goto done;
          }
          if (strcmp(op, "not") == 0) {
            out = LLVMBuildNot(f->builder, a, "inot");
            goto done;
          }
          if (strcmp(op, "neg") == 0) {
            out = LLVMBuildNeg(f->builder, a, "ineg");
            goto done;
          }
          if (strcmp(op, "eqz") == 0) {
            if (b) {
              errf(f->p, "sircc: %s node %lld requires 1 arg", n->tag, (long long)node_id);
              goto done;
            }
            LLVMTypeRef aty = LLVMTypeOf(a);
            if (LLVMGetTypeKind(aty) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(aty) != (unsigned)width) {
              errf(f->p, "sircc: %s requires i%d operand", n->tag, width);
              goto done;
            }
            LLVMValueRef zero = LLVMConstInt(aty, 0, 0);
            out = LLVMBuildICmp(f->builder, LLVMIntEQ, a, zero, "eqz");
            goto done;
          }
          if (strcmp(op, "min.s") == 0 || strcmp(op, "min.u") == 0 || strcmp(op, "max.s") == 0 || strcmp(op, "max.u") == 0) {
            if (!b) {
              errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
              goto done;
            }
            LLVMTypeRef aty = LLVMTypeOf(a);
            LLVMTypeRef bty = LLVMTypeOf(b);
            if (LLVMGetTypeKind(aty) != LLVMIntegerTypeKind || LLVMGetTypeKind(bty) != LLVMIntegerTypeKind ||
                LLVMGetIntTypeWidth(aty) != (unsigned)width || LLVMGetIntTypeWidth(bty) != (unsigned)width) {
              errf(f->p, "sircc: %s requires i%d operands", n->tag, width);
              goto done;
            }
            bool is_min = (strncmp(op, "min.", 4) == 0);
            bool is_signed = (op[4] == 's');
            LLVMIntPredicate pred;
            if (is_min) pred = is_signed ? LLVMIntSLE : LLVMIntULE;
            else pred = is_signed ? LLVMIntSGE : LLVMIntUGE;
            LLVMValueRef cmp = LLVMBuildICmp(f->builder, pred, a, b, "minmax.cmp");
            out = LLVMBuildSelect(f->builder, cmp, a, b, "minmax");
            goto done;
          }
          if (strcmp(op, "shl") == 0 || strcmp(op, "shr.s") == 0 || strcmp(op, "shr.u") == 0) {
            if (!b) {
              errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
              goto done;
            }
            LLVMTypeRef xty = LLVMTypeOf(a);
            if (LLVMGetTypeKind(xty) != LLVMIntegerTypeKind) {
              errf(f->p, "sircc: %s node %lld requires integer lhs", n->tag, (long long)node_id);
              goto done;
            }

            LLVMTypeRef sty = LLVMTypeOf(b);
            if (LLVMGetTypeKind(sty) != LLVMIntegerTypeKind) {
              errf(f->p, "sircc: %s node %lld requires integer shift amount", n->tag, (long long)node_id);
              goto done;
            }

            LLVMValueRef shift = b;
            if (LLVMGetIntTypeWidth(sty) != LLVMGetIntTypeWidth(xty)) {
              shift = build_zext_or_trunc(f->builder, b, xty, "shift.cast");
            }
            unsigned mask = (unsigned)(width - 1);
            LLVMValueRef maskv = LLVMConstInt(xty, mask, 0);
            shift = LLVMBuildAnd(f->builder, shift, maskv, "shift.mask");

            if (strcmp(op, "shl") == 0) {
              out = LLVMBuildShl(f->builder, a, shift, "shl");
              goto done;
            }
            if (strcmp(op, "shr.s") == 0) {
              out = LLVMBuildAShr(f->builder, a, shift, "ashr");
              goto done;
            }
            out = LLVMBuildLShr(f->builder, a, shift, "lshr");
            goto done;
          }

          if (strcmp(op, "div.s.trap") == 0 || strcmp(op, "div.u.trap") == 0 || strcmp(op, "rem.s.trap") == 0 ||
              strcmp(op, "rem.u.trap") == 0) {
            if (!b) {
              errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
              goto done;
            }
            LLVMTypeRef aty = LLVMTypeOf(a);
            LLVMTypeRef bty = LLVMTypeOf(b);
            if (LLVMGetTypeKind(aty) != LLVMIntegerTypeKind || LLVMGetTypeKind(bty) != LLVMIntegerTypeKind ||
                LLVMGetIntTypeWidth(aty) != (unsigned)width || LLVMGetIntTypeWidth(bty) != (unsigned)width) {
              errf(f->p, "sircc: %s requires i%d operands", n->tag, width);
              goto done;
            }
            LLVMValueRef zero = LLVMConstInt(aty, 0, 0);
            LLVMValueRef b_is_zero = LLVMBuildICmp(f->builder, LLVMIntEQ, b, zero, "b.iszero");
            LLVMValueRef trap_cond = b_is_zero;

            bool is_div = (strncmp(op, "div.", 4) == 0);
            bool is_signed = (op[4] == 's');
            if (is_div && is_signed) {
              unsigned long long min_bits = 1ULL << (unsigned)(width - 1);
              LLVMValueRef minv = LLVMConstInt(aty, min_bits, 0);
              LLVMValueRef neg1 = LLVMConstAllOnes(aty);
              LLVMValueRef a_is_min = LLVMBuildICmp(f->builder, LLVMIntEQ, a, minv, "a.ismin");
              LLVMValueRef b_is_neg1 = LLVMBuildICmp(f->builder, LLVMIntEQ, b, neg1, "b.isneg1");
              LLVMValueRef ov = LLVMBuildAnd(f->builder, a_is_min, b_is_neg1, "div.ov");
              trap_cond = LLVMBuildOr(f->builder, trap_cond, ov, "trap.cond");
            }
            if (!emit_trap_if(f, trap_cond)) goto done;

            if (is_div) {
              out = is_signed ? LLVMBuildSDiv(f->builder, a, b, "div") : LLVMBuildUDiv(f->builder, a, b, "div");
            } else {
              out = is_signed ? LLVMBuildSRem(f->builder, a, b, "rem") : LLVMBuildURem(f->builder, a, b, "rem");
            }
            goto done;
          }

          if (strncmp(op, "trunc_sat_f", 11) == 0) {
            // iN.trunc_sat_f32.s / iN.trunc_sat_f32.u (and f64.*)
            if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
              errf(f->p, "sircc: %s node %lld requires args:[x]", n->tag, (long long)node_id);
              goto done;
            }
            int srcw = 0;
            char su = 0;
            if (sscanf(op, "trunc_sat_f%d.%c", &srcw, &su) != 2 || (srcw != 32 && srcw != 64) || (su != 's' && su != 'u')) {
              errf(f->p, "sircc: unsupported trunc_sat form '%s' in %s", op, n->tag);
              goto done;
            }
            int64_t x_id = 0;
            if (!parse_node_ref_id(f->p, args->v.arr.items[0], &x_id)) {
              errf(f->p, "sircc: %s node %lld arg must be node ref", n->tag, (long long)node_id);
              goto done;
            }
            LLVMValueRef x = lower_expr(f, x_id);
            if (!x) goto done;

            LLVMTypeRef ity = LLVMIntTypeInContext(f->ctx, (unsigned)width);
            LLVMTypeRef fty = (srcw == 32) ? LLVMFloatTypeInContext(f->ctx) : LLVMDoubleTypeInContext(f->ctx);
            if (LLVMTypeOf(x) != fty) {
              errf(f->p, "sircc: %s requires f%d operand", n->tag, srcw);
              goto done;
            }
            if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(f->builder))) goto done;

            LLVMBasicBlockRef bb_nan = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.nan");
            LLVMBasicBlockRef bb_chk1 = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.chk1");
            LLVMBasicBlockRef bb_min = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.min");
            LLVMBasicBlockRef bb_chk2 = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.chk2");
            LLVMBasicBlockRef bb_max = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.max");
            LLVMBasicBlockRef bb_conv = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.conv");
            LLVMBasicBlockRef bb_merge = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.merge");

            LLVMValueRef isnan = LLVMBuildFCmp(f->builder, LLVMRealUNO, x, x, "isnan");
            LLVMBuildCondBr(f->builder, isnan, bb_nan, bb_chk1);

            LLVMPositionBuilderAtEnd(f->builder, bb_nan);
            LLVMValueRef z = LLVMConstInt(ity, 0, 0);
            LLVMBuildBr(f->builder, bb_merge);

            LLVMPositionBuilderAtEnd(f->builder, bb_chk1);
            LLVMValueRef min_i = NULL;
            LLVMValueRef max_i = NULL;
            if (su == 's') {
              unsigned long long min_bits = 1ULL << (unsigned)(width - 1);
              min_i = LLVMConstInt(ity, min_bits, 0);
              max_i = LLVMConstInt(ity, min_bits - 1ULL, 0);
              LLVMValueRef min_f = LLVMBuildSIToFP(f->builder, min_i, fty, "min.f");
              LLVMValueRef too_low = LLVMBuildFCmp(f->builder, LLVMRealOLT, x, min_f, "too_low");
              LLVMBuildCondBr(f->builder, too_low, bb_min, bb_chk2);
            } else {
              min_i = LLVMConstInt(ity, 0, 0);
              max_i = LLVMConstAllOnes(ity);
              LLVMValueRef zf = LLVMConstReal(fty, 0.0);
              LLVMValueRef too_low = LLVMBuildFCmp(f->builder, LLVMRealOLE, x, zf, "too_low");
              LLVMBuildCondBr(f->builder, too_low, bb_min, bb_chk2);
            }

            LLVMPositionBuilderAtEnd(f->builder, bb_min);
            LLVMBuildBr(f->builder, bb_merge);

            LLVMPositionBuilderAtEnd(f->builder, bb_chk2);
            LLVMValueRef max_f = (su == 's') ? LLVMBuildSIToFP(f->builder, max_i, fty, "max.f") : LLVMBuildUIToFP(f->builder, max_i, fty, "max.f");
            LLVMValueRef too_high = LLVMBuildFCmp(f->builder, LLVMRealOGE, x, max_f, "too_high");
            LLVMBuildCondBr(f->builder, too_high, bb_max, bb_conv);

            LLVMPositionBuilderAtEnd(f->builder, bb_max);
            LLVMBuildBr(f->builder, bb_merge);

            LLVMPositionBuilderAtEnd(f->builder, bb_conv);
            LLVMValueRef conv = (su == 's') ? LLVMBuildFPToSI(f->builder, x, ity, "fptosi") : LLVMBuildFPToUI(f->builder, x, ity, "fptoui");
            LLVMBuildBr(f->builder, bb_merge);

            LLVMPositionBuilderAtEnd(f->builder, bb_merge);
            LLVMValueRef phi = LLVMBuildPhi(f->builder, ity, "trunc_sat");
            LLVMValueRef inc_vals[4] = {z, min_i, max_i, conv};
            LLVMBasicBlockRef inc_bbs[4] = {bb_nan, bb_min, bb_max, bb_conv};
            LLVMAddIncoming(phi, inc_vals, inc_bbs, 4);
            out = phi;
            goto done;
          }

          if (strcmp(op, "div.s.sat") == 0 || strcmp(op, "div.u.sat") == 0 || strcmp(op, "rem.s.sat") == 0 ||
              strcmp(op, "rem.u.sat") == 0) {
            if (!b) {
              errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
              goto done;
            }
            LLVMTypeRef aty = LLVMTypeOf(a);
            LLVMTypeRef bty = LLVMTypeOf(b);
            if (LLVMGetTypeKind(aty) != LLVMIntegerTypeKind || LLVMGetTypeKind(bty) != LLVMIntegerTypeKind ||
                LLVMGetIntTypeWidth(aty) != (unsigned)width || LLVMGetIntTypeWidth(bty) != (unsigned)width) {
              errf(f->p, "sircc: %s requires i%d operands", n->tag, width);
              goto done;
            }
            if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(f->builder))) goto done;

            bool is_div = (strncmp(op, "div.", 4) == 0);
            bool is_signed = (op[4] == 's');

            LLVMBasicBlockRef cur = LLVMGetInsertBlock(f->builder);
            LLVMBasicBlockRef bb_zero = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.zero");
            LLVMBasicBlockRef bb_chk = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.chk");
            LLVMBasicBlockRef bb_norm = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.norm");
            LLVMBasicBlockRef bb_over = NULL;
            LLVMBasicBlockRef bb_merge = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.merge");

            LLVMValueRef zero = LLVMConstInt(aty, 0, 0);
            LLVMValueRef b_is_zero = LLVMBuildICmp(f->builder, LLVMIntEQ, b, zero, "b.iszero");
            LLVMBuildCondBr(f->builder, b_is_zero, bb_zero, bb_chk);

            // b==0 case: result 0
            LLVMPositionBuilderAtEnd(f->builder, bb_zero);
            LLVMBuildBr(f->builder, bb_merge);

            // check overflow (signed div only), otherwise jump to normal
            LLVMPositionBuilderAtEnd(f->builder, bb_chk);
            if (is_div && is_signed) {
              bb_over = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.over");
              unsigned long long min_bits = 1ULL << (unsigned)(width - 1);
              LLVMValueRef minv = LLVMConstInt(aty, min_bits, 0);
              LLVMValueRef neg1 = LLVMConstAllOnes(aty);
              LLVMValueRef a_is_min = LLVMBuildICmp(f->builder, LLVMIntEQ, a, minv, "a.ismin");
              LLVMValueRef b_is_neg1 = LLVMBuildICmp(f->builder, LLVMIntEQ, b, neg1, "b.isneg1");
              LLVMValueRef ov = LLVMBuildAnd(f->builder, a_is_min, b_is_neg1, "div.ov");
              LLVMBuildCondBr(f->builder, ov, bb_over, bb_norm);

              LLVMPositionBuilderAtEnd(f->builder, bb_over);
              LLVMBuildBr(f->builder, bb_merge);
            } else {
              LLVMBuildBr(f->builder, bb_norm);
            }

            // normal division/rem
            LLVMPositionBuilderAtEnd(f->builder, bb_norm);
            LLVMValueRef norm = NULL;
            if (is_div) {
              norm = is_signed ? LLVMBuildSDiv(f->builder, a, b, "div") : LLVMBuildUDiv(f->builder, a, b, "div");
            } else {
              norm = is_signed ? LLVMBuildSRem(f->builder, a, b, "rem") : LLVMBuildURem(f->builder, a, b, "rem");
            }
            LLVMBuildBr(f->builder, bb_merge);

            // merge
            LLVMPositionBuilderAtEnd(f->builder, bb_merge);
            LLVMValueRef phi = LLVMBuildPhi(f->builder, aty, "sat");
            LLVMValueRef inc_vals[3];
            LLVMBasicBlockRef inc_bbs[3];
            unsigned inc_n = 0;
            inc_vals[inc_n] = zero;
            inc_bbs[inc_n] = bb_zero;
            inc_n++;
            if (bb_over) {
              unsigned long long min_bits = 1ULL << (unsigned)(width - 1);
              LLVMValueRef minv = LLVMConstInt(aty, min_bits, 0);
              inc_vals[inc_n] = minv;
              inc_bbs[inc_n] = bb_over;
              inc_n++;
            }
            inc_vals[inc_n] = norm;
            inc_bbs[inc_n] = bb_norm;
            inc_n++;
            LLVMAddIncoming(phi, inc_vals, inc_bbs, inc_n);
            (void)cur;
            out = phi;
            goto done;
          }

          if (strcmp(op, "rotl") == 0 || strcmp(op, "rotr") == 0) {
            if (!b) {
              errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
              goto done;
            }
            LLVMTypeRef xty = LLVMTypeOf(a);
            if (LLVMGetTypeKind(xty) != LLVMIntegerTypeKind) {
              errf(f->p, "sircc: %s node %lld requires integer lhs", n->tag, (long long)node_id);
              goto done;
            }
            LLVMTypeRef sty = LLVMTypeOf(b);
            if (LLVMGetTypeKind(sty) != LLVMIntegerTypeKind) {
              errf(f->p, "sircc: %s node %lld requires integer rotate amount", n->tag, (long long)node_id);
              goto done;
            }
            LLVMValueRef amt = b;
            if (LLVMGetIntTypeWidth(sty) != LLVMGetIntTypeWidth(xty)) {
              amt = build_zext_or_trunc(f->builder, b, xty, "rot.cast");
            }
            unsigned mask = (unsigned)(width - 1);
            LLVMValueRef maskv = LLVMConstInt(xty, mask, 0);
            amt = LLVMBuildAnd(f->builder, amt, maskv, "rot.mask");

            char full[32];
            snprintf(full, sizeof(full), "llvm.%s.i%d", (strcmp(op, "rotl") == 0) ? "fshl" : "fshr", width);
            LLVMTypeRef params[3] = {xty, xty, xty};
            LLVMValueRef fn = get_or_declare_intrinsic(f->mod, full, xty, params, 3);
            LLVMValueRef argv[3] = {a, a, amt};
            out = LLVMBuildCall2(f->builder, LLVMGlobalGetValueType(fn), fn, argv, 3, "rot");
            goto done;
          }

          if (strncmp(op, "cmp.", 4) == 0) {
            if (!b) {
              errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
              goto done;
            }
            const char* cc = op + 4;
            LLVMIntPredicate pred;
            if (strcmp(cc, "eq") == 0) pred = LLVMIntEQ;
            else if (strcmp(cc, "ne") == 0) pred = LLVMIntNE;
            else if (strcmp(cc, "slt") == 0) pred = LLVMIntSLT;
            else if (strcmp(cc, "sle") == 0) pred = LLVMIntSLE;
            else if (strcmp(cc, "sgt") == 0) pred = LLVMIntSGT;
            else if (strcmp(cc, "sge") == 0) pred = LLVMIntSGE;
            else if (strcmp(cc, "ult") == 0) pred = LLVMIntULT;
            else if (strcmp(cc, "ule") == 0) pred = LLVMIntULE;
            else if (strcmp(cc, "ugt") == 0) pred = LLVMIntUGT;
            else if (strcmp(cc, "uge") == 0) pred = LLVMIntUGE;
            else {
              errf(f->p, "sircc: unsupported integer compare '%s' in %s", cc, n->tag);
              goto done;
            }
            out = LLVMBuildICmp(f->builder, pred, a, b, "icmp");
            goto done;
          }

          if (strcmp(op, "clz") == 0 || strcmp(op, "ctz") == 0) {
            const char* iname = (strcmp(op, "clz") == 0) ? "llvm.ctlz" : "llvm.cttz";
            char full[32];
            snprintf(full, sizeof(full), "%s.i%d", iname, width);
            LLVMTypeRef ity = LLVMTypeOf(a);
            LLVMTypeRef i1 = LLVMInt1TypeInContext(f->ctx);
            LLVMTypeRef params[2] = {ity, i1};
            LLVMValueRef fn = get_or_declare_intrinsic(f->mod, full, ity, params, 2);
            LLVMValueRef argsv[2] = {a, LLVMConstInt(i1, 0, 0)};
            out = LLVMBuildCall2(f->builder, LLVMGlobalGetValueType(fn), fn, argsv, 2, op);
            goto done;
          }

          if (strcmp(op, "popc") == 0) {
            char full[32];
            snprintf(full, sizeof(full), "llvm.ctpop.i%d", width);
            LLVMTypeRef ity = LLVMTypeOf(a);
            LLVMTypeRef params[1] = {ity};
            LLVMValueRef fn = get_or_declare_intrinsic(f->mod, full, ity, params, 1);
            LLVMValueRef argsv[1] = {a};
            out = LLVMBuildCall2(f->builder, LLVMGlobalGetValueType(fn), fn, argsv, 1, "popc");
            goto done;
          }

          if (strncmp(op, "zext.i", 6) == 0 || strncmp(op, "sext.i", 6) == 0 || strncmp(op, "trunc.i", 7) == 0) {
            int src = 0;
            bool is_zext = strncmp(op, "zext.i", 6) == 0;
            bool is_sext = strncmp(op, "sext.i", 6) == 0;
            bool is_trunc = strncmp(op, "trunc.i", 7) == 0;
            const char* num = is_trunc ? (op + 7) : (op + 6);
            if (sscanf(num, "%d", &src) != 1 || !(src == 8 || src == 16 || src == 32 || src == 64)) {
              errf(f->p, "sircc: invalid cast mnemonic '%s'", n->tag);
              goto done;
            }

            if ((is_zext || is_sext) && width <= src) {
              errf(f->p, "sircc: %s requires dst width > src width", n->tag);
              goto done;
            }
            if (is_trunc && width >= src) {
              errf(f->p, "sircc: %s requires dst width < src width", n->tag);
              goto done;
            }

            LLVMTypeRef ity = LLVMTypeOf(a);
            if (LLVMGetTypeKind(ity) != LLVMIntegerTypeKind || (int)LLVMGetIntTypeWidth(ity) != src) {
              errf(f->p, "sircc: %s requires i%d operand", n->tag, src);
              goto done;
            }
            LLVMTypeRef dst = LLVMIntTypeInContext(f->ctx, (unsigned)width);
            if (is_zext) out = LLVMBuildZExt(f->builder, a, dst, "zext");
            else if (is_sext) out = LLVMBuildSExt(f->builder, a, dst, "sext");
            else out = LLVMBuildTrunc(f->builder, a, dst, "trunc");
            goto done;
          }
        }
      }
    }
  }

  if (strncmp(n->tag, "bool.", 5) == 0) {
    const char* op = n->tag + 5;
    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
    if (!args || args->type != JSON_ARRAY) {
      errf(f->p, "sircc: %s node %lld missing args array", n->tag, (long long)node_id);
      goto done;
    }

    if (strcmp(op, "not") == 0) {
      if (args->v.arr.len != 1) {
        errf(f->p, "sircc: bool.not node %lld requires 1 arg", (long long)node_id);
        goto done;
      }
      int64_t x_id = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &x_id)) {
        errf(f->p, "sircc: bool.not node %lld arg must be node ref", (long long)node_id);
        goto done;
      }
      LLVMValueRef x = lower_expr(f, x_id);
      if (!x) goto done;
      out = LLVMBuildNot(f->builder, x, "bnot");
      goto done;
    }

    if (strcmp(op, "and") == 0 || strcmp(op, "or") == 0 || strcmp(op, "xor") == 0) {
      if (args->v.arr.len != 2) {
        errf(f->p, "sircc: bool.%s node %lld requires 2 args", op, (long long)node_id);
        goto done;
      }
      int64_t a_id = 0, b_id = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &a_id) || !parse_node_ref_id(f->p, args->v.arr.items[1], &b_id)) {
        errf(f->p, "sircc: bool.%s node %lld args must be node refs", op, (long long)node_id);
        goto done;
      }
      LLVMValueRef a = lower_expr(f, a_id);
      LLVMValueRef b = lower_expr(f, b_id);
      if (!a || !b) goto done;
      if (strcmp(op, "and") == 0) out = LLVMBuildAnd(f->builder, a, b, "band");
      else if (strcmp(op, "or") == 0) out = LLVMBuildOr(f->builder, a, b, "bor");
      else out = LLVMBuildXor(f->builder, a, b, "bxor");
      goto done;
    }
  }

  if (strcmp(n->tag, "select") == 0) {
    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) {
      errf(f->p, "sircc: select node %lld requires args:[cond, then, else]", (long long)node_id);
      goto done;
    }
    int64_t ty_id = 0;
    bool has_ty = false;
    if (n->fields) {
      JsonValue* tyv = json_obj_get(n->fields, "ty");
      if (tyv && parse_type_ref_id(f->p, tyv, &ty_id)) has_ty = true;
    }
    int64_t c_id = 0, t_id = 0, e_id = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[0], &c_id) || !parse_node_ref_id(f->p, args->v.arr.items[1], &t_id) ||
        !parse_node_ref_id(f->p, args->v.arr.items[2], &e_id)) {
      errf(f->p, "sircc: select node %lld args must be node refs", (long long)node_id);
      goto done;
    }
    LLVMValueRef c = lower_expr(f, c_id);
    LLVMValueRef tv = lower_expr(f, t_id);
    LLVMValueRef ev = lower_expr(f, e_id);
    if (!c || !tv || !ev) goto done;
    if (LLVMGetTypeKind(LLVMTypeOf(c)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(c)) != 1) {
      errf(f->p, "sircc: select node %lld cond must be bool", (long long)node_id);
      goto done;
    }
    if (LLVMTypeOf(tv) != LLVMTypeOf(ev)) {
      errf(f->p, "sircc: select node %lld then/else types must match", (long long)node_id);
      goto done;
    }
    if (n->type_ref) {
      LLVMTypeRef want = lower_type(f->p, f->ctx, n->type_ref);
      if (!want || want != LLVMTypeOf(tv)) {
        errf(f->p, "sircc: select node %lld type_ref does not match operand type", (long long)node_id);
        goto done;
      }
    }
    if (has_ty) {
      LLVMTypeRef want = lower_type(f->p, f->ctx, ty_id);
      if (!want || want != LLVMTypeOf(tv)) {
        errf(f->p, "sircc: select node %lld ty does not match operand type", (long long)node_id);
        goto done;
      }
    }
    out = LLVMBuildSelect(f->builder, c, tv, ev, "select");
    goto done;
  }

  if (strcmp(n->tag, "call") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: call node %lld missing fields", (long long)node_id);
      goto done;
    }
    JsonValue* callee_v = json_obj_get(n->fields, "callee");
    int64_t callee_id = 0;
    if (!parse_node_ref_id(f->p, callee_v, &callee_id)) {
      errf(f->p, "sircc: call node %lld missing callee ref", (long long)node_id);
      goto done;
    }
    NodeRec* callee_n = get_node(f->p, callee_id);
    if (!callee_n || strcmp(callee_n->tag, "fn") != 0 || !callee_n->llvm_value) {
      errf(f->p, "sircc: call node %lld callee %lld is not a lowered fn", (long long)node_id, (long long)callee_id);
      goto done;
    }
    LLVMValueRef callee = callee_n->llvm_value;

    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY) {
      errf(f->p, "sircc: call node %lld missing args array", (long long)node_id);
      goto done;
    }
    size_t argc = args->v.arr.len;
    LLVMValueRef* argv = NULL;
    if (argc) {
      argv = (LLVMValueRef*)malloc(argc * sizeof(LLVMValueRef));
      if (!argv) goto done;
      for (size_t i = 0; i < argc; i++) {
        int64_t aid = 0;
        if (!parse_node_ref_id(f->p, args->v.arr.items[i], &aid)) {
          errf(f->p, "sircc: call node %lld arg[%zu] must be node ref", (long long)node_id, i);
          free(argv);
          goto done;
        }
        argv[i] = lower_expr(f, aid);
        if (!argv[i]) {
          free(argv);
          goto done;
        }
      }
    }

    LLVMTypeRef callee_fty = LLVMGlobalGetValueType(callee);
    if (LLVMGetTypeKind(callee_fty) != LLVMFunctionTypeKind) {
      errf(f->p, "sircc: call node %lld callee is not a function pointer", (long long)node_id);
      free(argv);
      goto done;
    }

    unsigned param_count = LLVMCountParamTypes(callee_fty);
    bool is_varargs = LLVMIsFunctionVarArg(callee_fty) != 0;
    if (!is_varargs && (unsigned)argc != param_count) {
      errf(f->p, "sircc: call node %lld arg count mismatch (got %zu, want %u)", (long long)node_id, argc, param_count);
      free(argv);
      goto done;
    }
    if ((unsigned)argc < param_count) {
      errf(f->p, "sircc: call node %lld missing required args (got %zu, want >= %u)", (long long)node_id, argc, param_count);
      free(argv);
      goto done;
    }

    if (param_count) {
      LLVMTypeRef* params = (LLVMTypeRef*)malloc(param_count * sizeof(LLVMTypeRef));
      if (!params) {
        free(argv);
        goto done;
      }
      LLVMGetParamTypes(callee_fty, params);
      for (unsigned i = 0; i < param_count; i++) {
        LLVMTypeRef want = params[i];
        LLVMTypeRef got = LLVMTypeOf(argv[i]);
        if (want == got) continue;
        if (LLVMGetTypeKind(want) == LLVMPointerTypeKind && LLVMGetTypeKind(got) == LLVMPointerTypeKind) {
          argv[i] = LLVMBuildBitCast(f->builder, argv[i], want, "arg.cast");
          continue;
        }
        free(params);
        errf(f->p, "sircc: call node %lld arg[%u] type mismatch", (long long)node_id, i);
        free(argv);
        goto done;
      }
      free(params);
    }

    out = LLVMBuildCall2(f->builder, callee_fty, callee, argv, (unsigned)argc, "call");
    free(argv);
    if (out && n->type_ref) {
      LLVMTypeRef want = lower_type(f->p, f->ctx, n->type_ref);
      if (want && want != LLVMTypeOf(out)) {
        errf(f->p, "sircc: call node %lld return type does not match type_ref", (long long)node_id);
        out = NULL;
        goto done;
      }
    }
    goto done;
  }

  if (strcmp(n->tag, "call.indirect") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: call.indirect node %lld missing fields", (long long)node_id);
      goto done;
    }

    int64_t sig_id = 0;
    if (!parse_type_ref_id(f->p, json_obj_get(n->fields, "sig"), &sig_id)) {
      errf(f->p, "sircc: call.indirect node %lld missing fields.sig (fn type ref)", (long long)node_id);
      goto done;
    }
    LLVMTypeRef callee_fty = lower_type(f->p, f->ctx, sig_id);
    if (!callee_fty || LLVMGetTypeKind(callee_fty) != LLVMFunctionTypeKind) {
      errf(f->p, "sircc: call.indirect node %lld fields.sig must reference a fn type", (long long)node_id);
      goto done;
    }

    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len < 1) {
      errf(f->p, "sircc: call.indirect node %lld requires args:[callee_ptr, ...]", (long long)node_id);
      goto done;
    }

    int64_t callee_id = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[0], &callee_id)) {
      errf(f->p, "sircc: call.indirect node %lld args[0] must be callee ptr ref", (long long)node_id);
      goto done;
    }
    NodeRec* callee_n = get_node(f->p, callee_id);
    if (callee_n && callee_n->type_ref) {
      TypeRec* t = get_type(f->p, callee_n->type_ref);
      if (t && (t->kind == TYPE_FUN || t->kind == TYPE_CLOSURE)) {
        errf(f->p, "sircc: call.indirect callee is an opaque %s value (use call.%s)", (t->kind == TYPE_CLOSURE) ? "closure" : "fun",
             (t->kind == TYPE_CLOSURE) ? "closure" : "fun");
        goto done;
      }
    }
    LLVMValueRef callee = lower_expr(f, callee_id);
    if (!callee) goto done;
    if (LLVMGetTypeKind(LLVMTypeOf(callee)) != LLVMPointerTypeKind) {
      errf(f->p, "sircc: call.indirect node %lld callee must be a ptr", (long long)node_id);
      goto done;
    }

    size_t argc = args->v.arr.len - 1;
    LLVMValueRef* argv = NULL;
    if (argc) {
      argv = (LLVMValueRef*)malloc(argc * sizeof(LLVMValueRef));
      if (!argv) goto done;
      for (size_t i = 0; i < argc; i++) {
        int64_t aid = 0;
        if (!parse_node_ref_id(f->p, args->v.arr.items[i + 1], &aid)) {
          errf(f->p, "sircc: call.indirect node %lld arg[%zu] must be node ref", (long long)node_id, i);
          free(argv);
          goto done;
        }
        argv[i] = lower_expr(f, aid);
        if (!argv[i]) {
          free(argv);
          goto done;
        }
      }
    }

    unsigned param_count = LLVMCountParamTypes(callee_fty);
    bool is_varargs = LLVMIsFunctionVarArg(callee_fty) != 0;
    if (!is_varargs && (unsigned)argc != param_count) {
      errf(f->p, "sircc: call.indirect node %lld arg count mismatch (got %zu, want %u)", (long long)node_id, argc, param_count);
      free(argv);
      goto done;
    }
    if ((unsigned)argc < param_count) {
      errf(f->p, "sircc: call.indirect node %lld missing required args (got %zu, want >= %u)", (long long)node_id, argc, param_count);
      free(argv);
      goto done;
    }

    if (param_count) {
      LLVMTypeRef* params = (LLVMTypeRef*)malloc(param_count * sizeof(LLVMTypeRef));
      if (!params) {
        free(argv);
        goto done;
      }
      LLVMGetParamTypes(callee_fty, params);
      for (unsigned i = 0; i < param_count; i++) {
        LLVMTypeRef want = params[i];
        LLVMTypeRef got = LLVMTypeOf(argv[i]);
        if (want == got) continue;
        if (LLVMGetTypeKind(want) == LLVMPointerTypeKind && LLVMGetTypeKind(got) == LLVMPointerTypeKind) {
          argv[i] = LLVMBuildBitCast(f->builder, argv[i], want, "arg.cast");
          continue;
        }
        free(params);
        errf(f->p, "sircc: call.indirect node %lld arg[%u] type mismatch", (long long)node_id, i);
        free(argv);
        goto done;
      }
      free(params);
    }

    out = LLVMBuildCall2(f->builder, callee_fty, callee, argv, (unsigned)argc, "call");
    free(argv);

    if (out && n->type_ref) {
      LLVMTypeRef want = lower_type(f->p, f->ctx, n->type_ref);
      if (want && want != LLVMTypeOf(out)) {
        errf(f->p, "sircc: call.indirect node %lld return type does not match type_ref", (long long)node_id);
        out = NULL;
        goto done;
      }
    }

    goto done;
  }

  if (strcmp(n->tag, "call.fun") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: call.fun node %lld missing fields", (long long)node_id);
      goto done;
    }

    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len < 1) {
      errf(f->p, "sircc: call.fun node %lld requires args:[callee, ...]", (long long)node_id);
      goto done;
    }

    int64_t callee_id = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[0], &callee_id)) {
      errf(f->p, "sircc: call.fun node %lld args[0] must be callee fun ref", (long long)node_id);
      goto done;
    }
    NodeRec* callee_n = get_node(f->p, callee_id);
    if (!callee_n || callee_n->type_ref == 0) {
      errf(f->p, "sircc: call.fun node %lld callee must have a fun type_ref", (long long)node_id);
      goto done;
    }
    TypeRec* callee_ty = get_type(f->p, callee_n->type_ref);
    if (!callee_ty || callee_ty->kind != TYPE_FUN || callee_ty->sig == 0) {
      errf(f->p, "sircc: call.fun node %lld callee must be a fun type", (long long)node_id);
      goto done;
    }
    LLVMTypeRef callee_fty = lower_type(f->p, f->ctx, callee_ty->sig);
    if (!callee_fty || LLVMGetTypeKind(callee_fty) != LLVMFunctionTypeKind) {
      errf(f->p, "sircc: call.fun node %lld callee fun.sig must reference a fn type", (long long)node_id);
      goto done;
    }

    LLVMValueRef callee = lower_expr(f, callee_id);
    if (!callee) goto done;

    size_t argc = args->v.arr.len - 1;
    LLVMValueRef* argv = NULL;
    if (argc) {
      argv = (LLVMValueRef*)malloc(argc * sizeof(LLVMValueRef));
      if (!argv) goto done;
      for (size_t i = 0; i < argc; i++) {
        int64_t aid = 0;
        if (!parse_node_ref_id(f->p, args->v.arr.items[i + 1], &aid)) {
          errf(f->p, "sircc: call.fun node %lld arg[%zu] must be node ref", (long long)node_id, i);
          free(argv);
          goto done;
        }
        argv[i] = lower_expr(f, aid);
        if (!argv[i]) {
          free(argv);
          goto done;
        }
      }
    }

    unsigned param_count = LLVMCountParamTypes(callee_fty);
    bool is_varargs = LLVMIsFunctionVarArg(callee_fty) != 0;
    if (!is_varargs && (unsigned)argc != param_count) {
      errf(f->p, "sircc: call.fun node %lld arg count mismatch (got %zu, want %u)", (long long)node_id, argc, param_count);
      free(argv);
      goto done;
    }
    if ((unsigned)argc < param_count) {
      errf(f->p, "sircc: call.fun node %lld missing required args (got %zu, want >= %u)", (long long)node_id, argc, param_count);
      free(argv);
      goto done;
    }

    if (param_count) {
      LLVMTypeRef* params = (LLVMTypeRef*)malloc(param_count * sizeof(LLVMTypeRef));
      if (!params) {
        free(argv);
        goto done;
      }
      LLVMGetParamTypes(callee_fty, params);
      for (unsigned i = 0; i < param_count; i++) {
        LLVMTypeRef want = params[i];
        LLVMTypeRef got = LLVMTypeOf(argv[i]);
        if (want == got) continue;
        if (LLVMGetTypeKind(want) == LLVMPointerTypeKind && LLVMGetTypeKind(got) == LLVMPointerTypeKind) {
          argv[i] = LLVMBuildBitCast(f->builder, argv[i], want, "arg.cast");
          continue;
        }
        free(params);
        errf(f->p, "sircc: call.fun node %lld arg[%u] type mismatch", (long long)node_id, i);
        free(argv);
        goto done;
      }
      free(params);
    }

    out = LLVMBuildCall2(f->builder, callee_fty, callee, argv, (unsigned)argc, "call");
    free(argv);

    if (out && n->type_ref) {
      LLVMTypeRef want = lower_type(f->p, f->ctx, n->type_ref);
      if (want && want != LLVMTypeOf(out)) {
        errf(f->p, "sircc: call.fun node %lld return type does not match type_ref", (long long)node_id);
        out = NULL;
        goto done;
      }
    }

    goto done;
  }

  if (strcmp(n->tag, "call.closure") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: call.closure node %lld missing fields", (long long)node_id);
      goto done;
    }

    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len < 1) {
      errf(f->p, "sircc: call.closure node %lld requires args:[callee, ...]", (long long)node_id);
      goto done;
    }

    int64_t callee_id = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[0], &callee_id)) {
      errf(f->p, "sircc: call.closure node %lld args[0] must be callee closure ref", (long long)node_id);
      goto done;
    }
    NodeRec* callee_n = get_node(f->p, callee_id);
    if (!callee_n || callee_n->type_ref == 0) {
      errf(f->p, "sircc: call.closure node %lld callee must have a closure type_ref", (long long)node_id);
      goto done;
    }
    TypeRec* callee_ty = get_type(f->p, callee_n->type_ref);
    if (!callee_ty || callee_ty->kind != TYPE_CLOSURE) {
      errf(f->p, "sircc: call.closure node %lld callee must be a closure type", (long long)node_id);
      goto done;
    }

    LLVMValueRef callee = lower_expr(f, callee_id);
    if (!callee) goto done;
    LLVMValueRef code = LLVMBuildExtractValue(f->builder, callee, 0, "clo.code");
    LLVMValueRef env = LLVMBuildExtractValue(f->builder, callee, 1, "clo.env");
    if (!code || !env) goto done;

    LLVMTypeRef code_sig = build_closure_code_sig(f, callee_ty);
    if (!code_sig || LLVMGetTypeKind(code_sig) != LLVMFunctionTypeKind) {
      errf(f->p, "sircc: call.closure node %lld could not derive closure code signature", (long long)node_id);
      goto done;
    }

    size_t user_argc = args->v.arr.len - 1;
    size_t argc = user_argc + 1; // include env
    LLVMValueRef* argv = NULL;
    if (argc) {
      argv = (LLVMValueRef*)malloc(argc * sizeof(LLVMValueRef));
      if (!argv) goto done;
      argv[0] = env;
      for (size_t i = 0; i < user_argc; i++) {
        int64_t aid = 0;
        if (!parse_node_ref_id(f->p, args->v.arr.items[i + 1], &aid)) {
          errf(f->p, "sircc: call.closure node %lld arg[%zu] must be node ref", (long long)node_id, i);
          free(argv);
          goto done;
        }
        argv[i + 1] = lower_expr(f, aid);
        if (!argv[i + 1]) {
          free(argv);
          goto done;
        }
      }
    }

    unsigned param_count = LLVMCountParamTypes(code_sig);
    bool is_varargs = LLVMIsFunctionVarArg(code_sig) != 0;
    if (!is_varargs && (unsigned)argc != param_count) {
      errf(f->p, "sircc: call.closure node %lld arg count mismatch (got %zu, want %u)", (long long)node_id, argc, param_count);
      free(argv);
      goto done;
    }
    if ((unsigned)argc < param_count) {
      errf(f->p, "sircc: call.closure node %lld missing required args (got %zu, want >= %u)", (long long)node_id, argc, param_count);
      free(argv);
      goto done;
    }

    if (param_count) {
      LLVMTypeRef* params = (LLVMTypeRef*)malloc(param_count * sizeof(LLVMTypeRef));
      if (!params) {
        free(argv);
        goto done;
      }
      LLVMGetParamTypes(code_sig, params);
      for (unsigned i = 0; i < param_count; i++) {
        LLVMTypeRef want = params[i];
        LLVMTypeRef got = LLVMTypeOf(argv[i]);
        if (want == got) continue;
        if (LLVMGetTypeKind(want) == LLVMPointerTypeKind && LLVMGetTypeKind(got) == LLVMPointerTypeKind) {
          argv[i] = LLVMBuildBitCast(f->builder, argv[i], want, "arg.cast");
          continue;
        }
        free(params);
        errf(f->p, "sircc: call.closure node %lld arg[%u] type mismatch", (long long)node_id, i);
        free(argv);
        goto done;
      }
      free(params);
    }

    out = LLVMBuildCall2(f->builder, code_sig, code, argv, (unsigned)argc, "call");
    free(argv);

    if (out && n->type_ref) {
      LLVMTypeRef want = lower_type(f->p, f->ctx, n->type_ref);
      if (want && want != LLVMTypeOf(out)) {
        errf(f->p, "sircc: call.closure node %lld return type does not match type_ref", (long long)node_id);
        out = NULL;
        goto done;
      }
    }

    goto done;
  }

  if (strcmp(n->tag, "sem.if") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: sem.if node %lld missing fields", (long long)node_id);
      goto done;
    }
    LLVMTypeRef want = NULL;
    if (n->type_ref) {
      want = lower_type(f->p, f->ctx, n->type_ref);
      if (!want) goto done;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) {
      errf(f->p, "sircc: sem.if node %lld requires args:[cond, thenBranch, elseBranch]", (long long)node_id);
      goto done;
    }
    int64_t cond_id = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[0], &cond_id)) {
      errf(f->p, "sircc: sem.if node %lld cond must be node ref", (long long)node_id);
      goto done;
    }
    LLVMValueRef cond = lower_expr(f, cond_id);
    if (!cond) goto done;
    if (LLVMGetTypeKind(LLVMTypeOf(cond)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(cond)) != 1) {
      errf(f->p, "sircc: sem.if node %lld cond must be bool", (long long)node_id);
      goto done;
    }
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(f->builder))) goto done;

    LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sem.then");
    LLVMBasicBlockRef else_bb = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sem.else");
    LLVMBasicBlockRef join_bb = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sem.join");
    LLVMBuildCondBr(f->builder, cond, then_bb, else_bb);

    LLVMPositionBuilderAtEnd(f->builder, then_bb);
    LLVMValueRef v_then = NULL;
    if (!eval_branch_operand(f, args->v.arr.items[1], want, &v_then) || !v_then) goto done;
    LLVMBuildBr(f->builder, join_bb);
    LLVMBasicBlockRef then_end = LLVMGetInsertBlock(f->builder);

    LLVMPositionBuilderAtEnd(f->builder, else_bb);
    LLVMValueRef v_else = NULL;
    if (!eval_branch_operand(f, args->v.arr.items[2], want, &v_else) || !v_else) goto done;
    LLVMBuildBr(f->builder, join_bb);
    LLVMBasicBlockRef else_end = LLVMGetInsertBlock(f->builder);

    LLVMPositionBuilderAtEnd(f->builder, join_bb);
    LLVMTypeRef phi_ty = want ? want : LLVMTypeOf(v_then);
    LLVMValueRef phi = LLVMBuildPhi(f->builder, phi_ty, "sem.if");
    LLVMAddIncoming(phi, &v_then, &then_end, 1);
    LLVMAddIncoming(phi, &v_else, &else_end, 1);
    out = phi;
    goto done;
  }

  if (strcmp(n->tag, "sem.and_sc") == 0 || strcmp(n->tag, "sem.or_sc") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: %s node %lld missing fields", n->tag, (long long)node_id);
      goto done;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) {
      errf(f->p, "sircc: %s node %lld requires args:[lhs, rhsBranch]", n->tag, (long long)node_id);
      goto done;
    }
    int64_t lhs_id = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[0], &lhs_id)) {
      errf(f->p, "sircc: %s lhs must be node ref", n->tag);
      goto done;
    }
    LLVMValueRef lhs = lower_expr(f, lhs_id);
    if (!lhs) goto done;
    if (LLVMGetTypeKind(LLVMTypeOf(lhs)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(lhs)) != 1) {
      errf(f->p, "sircc: %s lhs must be bool", n->tag);
      goto done;
    }
    LLVMTypeRef bty = LLVMInt1TypeInContext(f->ctx);
    LLVMValueRef v_then = NULL;
    LLVMValueRef v_else = NULL;

    // Rewrite to sem.if in-place:
    // and_sc: cond=lhs, then=rhsBranch, else=false
    // or_sc:  cond=lhs, then=true, else=rhsBranch
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(f->builder))) goto done;
    LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sem.then");
    LLVMBasicBlockRef else_bb = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sem.else");
    LLVMBasicBlockRef join_bb = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sem.join");
    LLVMBuildCondBr(f->builder, lhs, then_bb, else_bb);

    if (strcmp(n->tag, "sem.and_sc") == 0) {
      LLVMPositionBuilderAtEnd(f->builder, then_bb);
      if (!eval_branch_operand(f, args->v.arr.items[1], bty, &v_then) || !v_then) goto done;
      LLVMBuildBr(f->builder, join_bb);
      LLVMBasicBlockRef then_end = LLVMGetInsertBlock(f->builder);

      LLVMPositionBuilderAtEnd(f->builder, else_bb);
      v_else = LLVMConstInt(bty, 0, 0);
      LLVMBuildBr(f->builder, join_bb);
      LLVMBasicBlockRef else_end = LLVMGetInsertBlock(f->builder);

      LLVMPositionBuilderAtEnd(f->builder, join_bb);
      LLVMValueRef phi = LLVMBuildPhi(f->builder, bty, "sem.and");
      LLVMAddIncoming(phi, &v_then, &then_end, 1);
      LLVMAddIncoming(phi, &v_else, &else_end, 1);
      out = phi;
      goto done;
    } else {
      LLVMPositionBuilderAtEnd(f->builder, then_bb);
      v_then = LLVMConstInt(bty, 1, 0);
      LLVMBuildBr(f->builder, join_bb);
      LLVMBasicBlockRef then_end = LLVMGetInsertBlock(f->builder);

      LLVMPositionBuilderAtEnd(f->builder, else_bb);
      if (!eval_branch_operand(f, args->v.arr.items[1], bty, &v_else) || !v_else) goto done;
      LLVMBuildBr(f->builder, join_bb);
      LLVMBasicBlockRef else_end = LLVMGetInsertBlock(f->builder);

      LLVMPositionBuilderAtEnd(f->builder, join_bb);
      LLVMValueRef phi = LLVMBuildPhi(f->builder, bty, "sem.or");
      LLVMAddIncoming(phi, &v_then, &then_end, 1);
      LLVMAddIncoming(phi, &v_else, &else_end, 1);
      out = phi;
      goto done;
    }
  }

  if (strcmp(n->tag, "sem.match_sum") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: sem.match_sum node %lld missing fields", (long long)node_id);
      goto done;
    }
    LLVMTypeRef want = NULL;
    if (n->type_ref) {
      want = lower_type(f->p, f->ctx, n->type_ref);
      if (!want) goto done;
    }
    int64_t sum_ty_id = 0;
    if (!parse_type_ref_id(f->p, json_obj_get(n->fields, "sum"), &sum_ty_id)) {
      errf(f->p, "sircc: sem.match_sum node %lld missing fields.sum (sum type)", (long long)node_id);
      goto done;
    }
    TypeRec* sty = get_type(f->p, sum_ty_id);
    if (!sty || sty->kind != TYPE_SUM) {
      errf(f->p, "sircc: sem.match_sum fields.sum must reference a sum type");
      goto done;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
      errf(f->p, "sircc: sem.match_sum node %lld requires args:[scrut]", (long long)node_id);
      goto done;
    }
    int64_t scrut_id = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[0], &scrut_id)) {
      errf(f->p, "sircc: sem.match_sum scrut must be node ref");
      goto done;
    }
    LLVMValueRef scrut = lower_expr(f, scrut_id);
    if (!scrut) goto done;
    LLVMValueRef tag = LLVMBuildExtractValue(f->builder, scrut, 0, "tag");

    JsonValue* cases = json_obj_get(n->fields, "cases");
    JsonValue* def = json_obj_get(n->fields, "default");
    if (!cases || cases->type != JSON_ARRAY || !def || def->type != JSON_OBJECT) {
      errf(f->p, "sircc: sem.match_sum node %lld requires fields.cases array and fields.default branch", (long long)node_id);
      goto done;
    }
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(f->builder))) goto done;

    LLVMBasicBlockRef join_bb = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sem.join");
    LLVMBasicBlockRef def_bb = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sem.default");
    LLVMValueRef sw = LLVMBuildSwitch(f->builder, tag, def_bb, (unsigned)cases->v.arr.len);

    // Create case blocks first for stable ordering.
    LLVMBasicBlockRef* case_bbs = NULL;
    int64_t* case_variants = NULL;
    if (cases->v.arr.len) {
      case_bbs = (LLVMBasicBlockRef*)calloc(cases->v.arr.len, sizeof(LLVMBasicBlockRef));
      case_variants = (int64_t*)calloc(cases->v.arr.len, sizeof(int64_t));
      if (!case_bbs || !case_variants) goto done;
    }
    for (size_t i = 0; i < cases->v.arr.len; i++) {
      JsonValue* co = cases->v.arr.items[i];
      if (!co || co->type != JSON_OBJECT) {
        errf(f->p, "sircc: sem.match_sum cases[%zu] must be object", i);
        goto done;
      }
      int64_t variant = -1;
      if (!must_i64(f->p, json_obj_get(co, "variant"), &variant, "sem.match_sum.cases.variant")) goto done;
      case_variants[i] = variant;
      char namebuf[32];
      snprintf(namebuf, sizeof(namebuf), "sem.case.%lld", (long long)variant);
      case_bbs[i] = LLVMAppendBasicBlockInContext(f->ctx, f->fn, namebuf);
      LLVMValueRef lit = LLVMConstInt(LLVMInt32TypeInContext(f->ctx), (unsigned long long)variant, 0);
      LLVMAddCase(sw, lit, case_bbs[i]);
    }

    // Evaluate cases.
    LLVMValueRef* phi_vals = (LLVMValueRef*)calloc((cases->v.arr.len + 1), sizeof(LLVMValueRef));
    LLVMBasicBlockRef* phi_bbs = (LLVMBasicBlockRef*)calloc((cases->v.arr.len + 1), sizeof(LLVMBasicBlockRef));
    if (!phi_vals || !phi_bbs) goto done;
    size_t incoming = 0;

    for (size_t i = 0; i < cases->v.arr.len; i++) {
      JsonValue* co = cases->v.arr.items[i];
      JsonValue* body = json_obj_get(co, "body");
      if (!body || body->type != JSON_OBJECT) {
        errf(f->p, "sircc: sem.match_sum cases[%zu] missing body branch", i);
        goto done;
      }
      LLVMPositionBuilderAtEnd(f->builder, case_bbs[i]);

      // If body is thunk with 1-arg callable, pass payload.
      const char* kind = json_get_string(json_obj_get(body, "kind"));
      LLVMValueRef v = NULL;
      if (kind && strcmp(kind, "thunk") == 0) {
        int64_t fid = 0;
        if (!parse_node_ref_id(f->p, json_obj_get(body, "f"), &fid)) goto done;
        NodeRec* fn = get_node(f->p, fid);
        if (!fn || fn->type_ref == 0) goto done;
        TypeRec* t = get_type(f->p, fn->type_ref);
        if (!t) goto done;
        size_t arity = 0;
        if (t->kind == TYPE_FUN) {
          TypeRec* sig = get_type(f->p, t->sig);
          if (!sig || sig->kind != TYPE_FN) goto done;
          arity = sig->param_len;
        } else if (t->kind == TYPE_CLOSURE) {
          TypeRec* sig = get_type(f->p, t->call_sig);
          if (!sig || sig->kind != TYPE_FN) goto done;
          arity = sig->param_len;
        }
        if (arity == 1) {
          int64_t variant = case_variants[i];
          if (variant < 0 || (size_t)variant >= sty->variant_len) {
            LLVMValueRef bad = LLVMConstInt(LLVMInt1TypeInContext(f->ctx), 1, 0);
            if (!emit_trap_if(f, bad)) goto done;
            variant = 0;
          }
          int64_t pay_ty_id = sty->variants[(size_t)variant].ty;
          if (pay_ty_id == 0) {
            errf(f->p, "sircc: sem.match_sum case %lld body expects payload but variant is nullary", (long long)variant);
            goto done;
          }
          LLVMValueRef payload = sum_payload_load(f, sty, sum_ty_id, scrut, pay_ty_id);
          if (!payload) goto done;
          LLVMValueRef argv1[1] = {payload};
          if (t->kind == TYPE_FUN) v = call_fun_value(f, fid, argv1, 1, want);
          else v = call_closure_value(f, fid, argv1, 1, want);
        } else {
          if (!eval_branch_operand(f, body, want, &v)) goto done;
        }
      } else {
        if (!eval_branch_operand(f, body, want, &v)) goto done;
      }

      if (!v) goto done;
      LLVMBuildBr(f->builder, join_bb);
      phi_vals[incoming] = v;
      phi_bbs[incoming] = LLVMGetInsertBlock(f->builder);
      incoming++;
    }

    // Default.
    LLVMPositionBuilderAtEnd(f->builder, def_bb);
    LLVMValueRef vdef = NULL;
    if (!eval_branch_operand(f, def, want, &vdef) || !vdef) goto done;
    LLVMBuildBr(f->builder, join_bb);
    phi_vals[incoming] = vdef;
    phi_bbs[incoming] = LLVMGetInsertBlock(f->builder);
    incoming++;

    LLVMPositionBuilderAtEnd(f->builder, join_bb);
    LLVMTypeRef phi_ty = want ? want : LLVMTypeOf(phi_vals[0]);
    LLVMValueRef phi = LLVMBuildPhi(f->builder, phi_ty, "sem.match");
    LLVMAddIncoming(phi, phi_vals, phi_bbs, (unsigned)incoming);
    out = phi;
    free(case_bbs);
    free(case_variants);
    free(phi_vals);
    free(phi_bbs);
    goto done;
  }


  if (lower_expr_part_b(f, node_id, n, &out)) goto done;

  errf(f->p, "sircc: unsupported expr tag '%s' (node %lld)", n->tag, (long long)node_id);

done:
  n->llvm_value = out;
  n->resolving = false;
  return out;
}
