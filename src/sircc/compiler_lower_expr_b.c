// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_lower_internal.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <llvm-c/Core.h>

static bool llvm_fn_type_eq(LLVMTypeRef a, LLVMTypeRef b) {
  if (!a || !b) return false;
  if (LLVMGetTypeKind(a) != LLVMFunctionTypeKind || LLVMGetTypeKind(b) != LLVMFunctionTypeKind) return false;
  if (LLVMIsFunctionVarArg(a) != LLVMIsFunctionVarArg(b)) return false;
  if (LLVMGetReturnType(a) != LLVMGetReturnType(b)) return false;
  unsigned ac = LLVMCountParamTypes(a);
  unsigned bc = LLVMCountParamTypes(b);
  if (ac != bc) return false;
  if (ac == 0) return true;
  LLVMTypeRef* ap = (LLVMTypeRef*)malloc(ac * sizeof(LLVMTypeRef));
  LLVMTypeRef* bp = (LLVMTypeRef*)malloc(bc * sizeof(LLVMTypeRef));
  if (!ap || !bp) {
    free(ap);
    free(bp);
    return false;
  }
  LLVMGetParamTypes(a, ap);
  LLVMGetParamTypes(b, bp);
  bool ok = true;
  for (unsigned i = 0; i < ac; i++) {
    if (ap[i] != bp[i]) {
      ok = false;
      break;
    }
  }
  free(ap);
  free(bp);
  return ok;
}

static bool is_opaque_callable_type(SirProgram* p, int64_t type_ref) {
  if (!p || type_ref == 0) return false;
  TypeRec* t = get_type(p, type_ref);
  if (!t) return false;
  return t->kind == TYPE_FUN || t->kind == TYPE_CLOSURE;
}

static bool reject_opaque_callable_operand(FunctionCtx* f, int64_t operand_node_id, const char* ctx_tag) {
  if (!f || !ctx_tag) return false;
  NodeRec* n = get_node(f->p, operand_node_id);
  if (!n) return false;
  if (!is_opaque_callable_type(f->p, n->type_ref)) return true;
  TypeRec* t = get_type(f->p, n->type_ref);
  const char* tk = (t && t->kind == TYPE_CLOSURE) ? "closure" : "fun";
  err_codef(f->p, "sircc.opaque_callable.ptr_op",
            "sircc: %s cannot operate on opaque %s values (use %s.* / call.%s)", ctx_tag, tk, tk, tk);
  return false;
}

bool lower_expr_part_b(FunctionCtx* f, int64_t node_id, NodeRec* n, LLVMValueRef* outp) {
  (void)node_id;
  if (!f || !n || !outp) return false;
  LLVMValueRef out = NULL;

  if (strncmp(n->tag, "vec.", 4) == 0 || strcmp(n->tag, "load.vec") == 0) {
    if (!lower_expr_simd(f, node_id, n, &out)) goto done;
    goto done;
  }

  if (strncmp(n->tag, "fun.", 4) == 0) {
    const char* op = n->tag + 4;

    if (strcmp(op, "sym") == 0) {
      if (!n->fields) {
        err_codef(f->p, "sircc.fun.sym.missing_fields", "sircc: fun.sym node %lld missing fields", (long long)node_id);
        goto done;
      }
      if (n->type_ref == 0) {
        err_codef(f->p, "sircc.fun.sym.missing_type", "sircc: fun.sym node %lld missing type_ref (fun type)", (long long)node_id);
        goto done;
      }
      TypeRec* fty = get_type(f->p, n->type_ref);
      if (!fty || fty->kind != TYPE_FUN || fty->sig == 0) {
        err_codef(f->p, "sircc.fun.sym.type_ref.bad", "sircc: fun.sym node %lld type_ref must be a fun type", (long long)node_id);
        goto done;
      }
      LLVMTypeRef sig = lower_type(f->p, f->ctx, fty->sig);
      if (!sig || LLVMGetTypeKind(sig) != LLVMFunctionTypeKind) {
        err_codef(f->p, "sircc.fun.sym.sig.bad", "sircc: fun.sym node %lld fun.sig must reference a fn type", (long long)node_id);
        goto done;
      }

      const char* name = json_get_string(json_obj_get(n->fields, "name"));
      if (!name || !is_ident(name)) {
        err_codef(f->p, "sircc.fun.sym.name.bad", "sircc: fun.sym node %lld requires fields.name Ident", (long long)node_id);
        goto done;
      }

      // A fun.sym must name a function symbol; reject collisions with globals or non-function syms.
      if (LLVMGetNamedGlobal(f->mod, name)) {
        err_codef(f->p, "sircc.fun.sym.conflict_global", "sircc: fun.sym '%s' conflicts with a global symbol", name);
        goto done;
      }
      SymRec* s = find_sym_by_name(f->p, name);
      if (s && s->kind && (strcmp(s->kind, "var") == 0 || strcmp(s->kind, "const") == 0)) {
        err_codef(f->p, "sircc.fun.sym.conflict_sym", "sircc: fun.sym '%s' references a data symbol (expected function)", name);
        goto done;
      }

      // Producer rule: the symbol should be declared/defined as a function in the stream.
      NodeRec* fn_node = find_fn_node_by_name(f->p, name);
      if (fn_node && fn_node->type_ref != fty->sig) {
        err_codef(f->p, "sircc.fun.sym.sig_mismatch", "sircc: fun.sym '%s' signature mismatch vs fn node type_ref", name);
        goto done;
      }
      NodeRec* decl_node = find_decl_fn_node_by_name(f->p, name);
      if (decl_node) {
        int64_t decl_sig_id = decl_node->type_ref;
        if (decl_sig_id == 0) {
          if (!parse_type_ref_id(f->p, json_obj_get(decl_node->fields, "sig"), &decl_sig_id)) {
            err_codef(f->p, "sircc.fun.sym.decl.sig.bad", "sircc: fun.sym '%s' has decl.fn without a signature", name);
            goto done;
          }
        }
        if (decl_sig_id != fty->sig) {
          err_codef(f->p, "sircc.fun.sym.sig_mismatch", "sircc: fun.sym '%s' signature mismatch vs decl.fn", name);
          goto done;
        }
      }

      LLVMValueRef fn = LLVMGetNamedFunction(f->mod, name);
      if (!fn && !fn_node && !decl_node) {
        err_codef(f->p, "sircc.fun.sym.undefined",
                  "sircc: fun.sym '%s' requires a prior fn or decl.fn of matching signature (producer rule)", name);
        goto done;
      }
      if (!fn) {
        fn = LLVMAddFunction(f->mod, name, sig);
        LLVMSetLinkage(fn, LLVMExternalLinkage);
      } else {
        LLVMTypeRef have = LLVMGlobalGetValueType(fn);
        if (have != sig) {
          err_codef(f->p, "sircc.fun.sym.sig_mismatch",
                    "sircc: fun.sym '%s' type mismatch vs existing declaration/definition", name);
          goto done;
        }
      }

      LLVMTypeRef want_ty = lower_type(f->p, f->ctx, n->type_ref);
      if (want_ty && LLVMTypeOf(fn) != want_ty) {
        if (LLVMGetTypeKind(want_ty) == LLVMPointerTypeKind) {
          out = LLVMBuildBitCast(f->builder, fn, want_ty, "fun.sym.cast");
          goto done;
        }
        err_codef(f->p, "sircc.fun.sym.llvm_type.bad", "sircc: fun.sym '%s' has unexpected LLVM type", name);
        goto done;
      }

      out = fn;
      goto done;
    }

    if (strcmp(op, "cmp.eq") == 0 || strcmp(op, "cmp.ne") == 0) {
      if (!n->fields) {
        err_codef(f->p, "sircc.fun.cmp.missing_fields", "sircc: %s node %lld missing fields", n->tag, (long long)node_id);
        goto done;
      }
      JsonValue* args = json_obj_get(n->fields, "args");
      if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) {
        err_codef(f->p, "sircc.fun.cmp.args_bad", "sircc: %s node %lld requires fields.args:[a,b]", n->tag, (long long)node_id);
        goto done;
      }
      int64_t a_id = 0, b_id = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &a_id) || !parse_node_ref_id(f->p, args->v.arr.items[1], &b_id)) {
        err_codef(f->p, "sircc.fun.cmp.arg_ref_bad", "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
        goto done;
      }
      LLVMValueRef a = lower_expr(f, a_id);
      LLVMValueRef b = lower_expr(f, b_id);
      if (!a || !b) goto done;
      if (LLVMTypeOf(a) != LLVMTypeOf(b)) {
        err_codef(f->p, "sircc.fun.cmp.type_mismatch",
                  "sircc: %s node %lld requires both operands to have same fun type", n->tag, (long long)node_id);
        goto done;
      }
      if (LLVMGetTypeKind(LLVMTypeOf(a)) != LLVMPointerTypeKind) {
        err_codef(f->p, "sircc.fun.cmp.operand_bad", "sircc: %s node %lld operands must be function values", n->tag,
                  (long long)node_id);
        goto done;
      }
      LLVMIntPredicate pred = (strcmp(op, "cmp.eq") == 0) ? LLVMIntEQ : LLVMIntNE;
      out = LLVMBuildICmp(f->builder, pred, a, b, "fun.cmp");
      goto done;
    }
  }

  if (strncmp(n->tag, "ptr.", 4) == 0) {
    const char* op = n->tag + 4;
    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;

    if (strcmp(op, "sym") == 0) {
      const char* name = NULL;
      if (n->fields) name = json_get_string(json_obj_get(n->fields, "name"));
      if (!name && args && args->type == JSON_ARRAY && args->v.arr.len == 1) {
        int64_t aid = 0;
        if (parse_node_ref_id(f->p, args->v.arr.items[0], &aid)) {
          NodeRec* an = get_node(f->p, aid);
          if (an && strcmp(an->tag, "name") == 0 && an->fields) {
            name = json_get_string(json_obj_get(an->fields, "name"));
          }
        }
      }
      if (!name) {
        err_codef(f->p, "sircc.ptr.sym.name.missing",
                  "sircc: ptr.sym node %lld requires fields.name or args:[name]", (long long)node_id);
        goto done;
      }
      LLVMValueRef fn = LLVMGetNamedFunction(f->mod, name);
      if (fn) {
        out = fn; // function values are pointers in LLVM
        goto done;
      }

      // If not a function, allow ptr.sym to name a global defined by a `sym` record (kind=var/const).
      LLVMValueRef g = LLVMGetNamedGlobal(f->mod, name);
      if (!g) {
        SymRec* s = find_sym_by_name(f->p, name);
        if (!s || !s->kind || (strcmp(s->kind, "var") != 0 && strcmp(s->kind, "const") != 0)) {
          err_codef(f->p, "sircc.ptr.sym.unknown",
                    "sircc: ptr.sym references unknown symbol '%s' (producer rule: ptr.sym must name an in-module declaration: "
                    "fn/decl.fn for functions, or sym(kind=var|const) for globals; extern calls should use decl.fn + call.indirect)",
                    name);
          goto done;
        }
        if (s->type_ref == 0) {
          err_codef(f->p, "sircc.sym.global.missing_type_ref", "sircc: sym '%s' missing type_ref for global definition", name);
          goto done;
        }
        LLVMTypeRef gty = lower_type(f->p, f->ctx, s->type_ref);
        if (!gty) {
          err_codef(f->p, "sircc.sym.global.type_ref.bad", "sircc: sym '%s' has invalid type_ref %lld", name,
                    (long long)s->type_ref);
          goto done;
        }
        g = LLVMAddGlobal(f->mod, gty, name);

        const char* linkage = s->linkage;
        if (linkage && strcmp(linkage, "local") == 0) LLVMSetLinkage(g, LLVMInternalLinkage);
        else if (linkage && strcmp(linkage, "public") == 0) LLVMSetLinkage(g, LLVMExternalLinkage);
        else if (linkage && strcmp(linkage, "extern") == 0) LLVMSetLinkage(g, LLVMExternalLinkage);
        else if (linkage && *linkage) {
          err_codef(f->p, "sircc.sym.global.linkage.bad",
                    "sircc: sym '%s' has unsupported linkage '%s' (use local/public/extern)", name, linkage);
          goto done;
        }

        if (s->kind && strcmp(s->kind, "const") == 0) {
          LLVMSetGlobalConstant(g, 1);
        }

        int64_t size = 0;
        int64_t align = 0;
        if (type_size_align(f->p, s->type_ref, &size, &align) && align > 0 && align <= 4096) {
          LLVMSetAlignment(g, (unsigned)align);
        }

        if (!linkage || strcmp(linkage, "extern") != 0) {
          LLVMValueRef init = NULL;
          if (s->value) {
            const char* vt = json_get_string(json_obj_get(s->value, "t"));
            if (vt && strcmp(vt, "num") == 0) {
              int64_t n0 = 0;
              (void)json_get_i64(json_obj_get(s->value, "v"), &n0);
              if (LLVMGetTypeKind(gty) == LLVMIntegerTypeKind) {
                init = LLVMConstInt(gty, (unsigned long long)n0, 1);
              } else if (LLVMGetTypeKind(gty) == LLVMPointerTypeKind && n0 == 0) {
                init = LLVMConstNull(gty);
              }
            } else if (vt && strcmp(vt, "ref") == 0) {
              int64_t cid = 0;
              if (!parse_node_ref_id(f->p, s->value, &cid)) {
                err_codef(f->p, "sircc.sym.global.init.ref.bad", "sircc: sym '%s' has invalid initializer ref", name);
                goto done;
              }
              NodeRec* cn = get_node(f->p, cid);
              if (!cn || !cn->tag || strncmp(cn->tag, "const.", 6) != 0) {
                err_codef(f->p, "sircc.sym.global.init.kind.bad", "sircc: sym '%s' initializer must be a const.* node", name);
                goto done;
              }
              LLVMValueRef cv = lower_expr(f, cid);
              if (!cv) goto done;
              if (!LLVMIsConstant(cv) || LLVMTypeOf(cv) != gty) {
                err_codef(f->p, "sircc.sym.global.init.type.bad",
                          "sircc: sym '%s' initializer type mismatch or not constant", name);
                goto done;
              }
              init = cv;
            }
            if (!init) {
              err_codef(f->p, "sircc.sym.global.init.unsupported",
                        "sircc: sym '%s' has unsupported global initializer value", name);
              goto done;
            }
          } else {
            init = LLVMConstNull(gty);
          }
          LLVMSetInitializer(g, init);
        }
      }

      out = g;
      goto done;
    }

    if (strcmp(op, "sizeof") == 0 || strcmp(op, "alignof") == 0 || strcmp(op, "offset") == 0) {
      if (!n->fields) {
        err_codef(f->p, "sircc.node.fields.missing", "sircc: %s node %lld missing fields", n->tag, (long long)node_id);
        goto done;
      }
      int64_t ty_id = 0;
      if (!parse_type_ref_id(f->p, json_obj_get(n->fields, "ty"), &ty_id)) {
        err_codef(f->p, "sircc.ptr.offset.ty.missing", "sircc: %s node %lld missing fields.ty (type ref)", n->tag, (long long)node_id);
        goto done;
      }
      int64_t size = 0;
      int64_t align = 0;
      if (!type_size_align(f->p, ty_id, &size, &align)) {
        err_codef(f->p, "sircc.ptr.offset.ty.bad", "sircc: %s node %lld has invalid/unsized type %lld", n->tag, (long long)node_id,
                  (long long)ty_id);
        goto done;
      }

      if (!args || args->type != JSON_ARRAY) {
        err_codef(f->p, "sircc.args.missing", "sircc: %s node %lld missing args array", n->tag, (long long)node_id);
        goto done;
      }

      if (strcmp(op, "sizeof") == 0) {
        if (args->v.arr.len != 0) {
          err_codef(f->p, "sircc.args.bad", "sircc: %s node %lld requires args:[]", n->tag, (long long)node_id);
          goto done;
        }
        out = LLVMConstInt(LLVMInt64TypeInContext(f->ctx), (unsigned long long)size, 0);
        goto done;
      }

      if (strcmp(op, "alignof") == 0) {
        if (args->v.arr.len != 0) {
          err_codef(f->p, "sircc.args.bad", "sircc: %s node %lld requires args:[]", n->tag, (long long)node_id);
          goto done;
        }
        out = LLVMConstInt(LLVMInt32TypeInContext(f->ctx), (unsigned long long)align, 0);
        goto done;
      }

      if (strcmp(op, "offset") == 0) {
        if (args->v.arr.len != 2) {
          err_codef(f->p, "sircc.args.bad", "sircc: %s node %lld requires args:[base,index]", n->tag, (long long)node_id);
          goto done;
        }
        int64_t base_id = 0, idx_id = 0;
        if (!parse_node_ref_id(f->p, args->v.arr.items[0], &base_id) || !parse_node_ref_id(f->p, args->v.arr.items[1], &idx_id)) {
          err_codef(f->p, "sircc.args.ref_bad", "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
          goto done;
        }
        LLVMValueRef base = lower_expr(f, base_id);
        LLVMValueRef idx = lower_expr(f, idx_id);
        if (!base || !idx) goto done;
        if (LLVMGetTypeKind(LLVMTypeOf(base)) != LLVMPointerTypeKind) {
          err_codef(f->p, "sircc.operand.type_bad", "sircc: %s requires ptr base", n->tag);
          goto done;
        }
        if (LLVMGetTypeKind(LLVMTypeOf(idx)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(idx)) != 64) {
          err_codef(f->p, "sircc.operand.type_bad", "sircc: %s requires i64 index", n->tag);
          goto done;
        }

        unsigned ptr_bits = f->p->ptr_bits ? f->p->ptr_bits : (unsigned)(sizeof(void*) * 8u);
        LLVMTypeRef ip = LLVMIntTypeInContext(f->ctx, ptr_bits);
        LLVMValueRef base_bits = LLVMBuildPtrToInt(f->builder, base, ip, "base.bits");
        LLVMValueRef idx_bits = LLVMBuildTruncOrBitCast(f->builder, idx, ip, "idx.bits");
        LLVMValueRef scale = LLVMConstInt(ip, (unsigned long long)size, 0);
        LLVMValueRef off_bits = LLVMBuildMul(f->builder, idx_bits, scale, "off.bits");
        LLVMValueRef sum_bits = LLVMBuildAdd(f->builder, base_bits, off_bits, "addr.bits");
        out = LLVMBuildIntToPtr(f->builder, sum_bits, LLVMTypeOf(base), "ptr.off");
        goto done;
      }
    }

    if (!args || args->type != JSON_ARRAY) {
      err_codef(f->p, "sircc.args.missing", "sircc: %s node %lld missing args array", n->tag, (long long)node_id);
      goto done;
    }

    if (strcmp(op, "cmp.eq") == 0 || strcmp(op, "cmp.ne") == 0) {
      if (args->v.arr.len != 2) {
        err_codef(f->p, "sircc.args.arity_bad", "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
        goto done;
      }
      int64_t a_id = 0, b_id = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &a_id) || !parse_node_ref_id(f->p, args->v.arr.items[1], &b_id)) {
        err_codef(f->p, "sircc.args.ref_bad", "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
        goto done;
      }
      if (!reject_opaque_callable_operand(f, a_id, n->tag) || !reject_opaque_callable_operand(f, b_id, n->tag)) goto done;
      LLVMValueRef a = lower_expr(f, a_id);
      LLVMValueRef b = lower_expr(f, b_id);
      if (!a || !b) goto done;
      if (LLVMGetTypeKind(LLVMTypeOf(a)) == LLVMPointerTypeKind && LLVMGetTypeKind(LLVMTypeOf(b)) == LLVMPointerTypeKind &&
          LLVMTypeOf(a) != LLVMTypeOf(b)) {
        LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
        a = LLVMBuildBitCast(f->builder, a, i8p, "pcmp.a");
        b = LLVMBuildBitCast(f->builder, b, i8p, "pcmp.b");
      }
      LLVMIntPredicate pred = (strcmp(op, "cmp.eq") == 0) ? LLVMIntEQ : LLVMIntNE;
      out = LLVMBuildICmp(f->builder, pred, a, b, "pcmp");
      goto done;
    }

    if (strcmp(op, "add") == 0 || strcmp(op, "sub") == 0) {
      if (args->v.arr.len != 2) {
        err_codef(f->p, "sircc.args.arity_bad", "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
        goto done;
      }
      int64_t p_id = 0, off_id = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &p_id) || !parse_node_ref_id(f->p, args->v.arr.items[1], &off_id)) {
        err_codef(f->p, "sircc.args.ref_bad", "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
        goto done;
      }
      if (!reject_opaque_callable_operand(f, p_id, n->tag)) goto done;
      LLVMValueRef pval = lower_expr(f, p_id);
      LLVMValueRef oval = lower_expr(f, off_id);
      if (!pval || !oval) goto done;
      LLVMTypeRef pty = LLVMTypeOf(pval);
      if (LLVMGetTypeKind(pty) != LLVMPointerTypeKind) {
        err_codef(f->p, "sircc.operand.type_bad", "sircc: %s requires pointer lhs", n->tag);
        goto done;
      }
      if (LLVMGetTypeKind(LLVMTypeOf(oval)) != LLVMIntegerTypeKind) {
        err_codef(f->p, "sircc.operand.type_bad", "sircc: %s requires integer byte offset rhs", n->tag);
        goto done;
      }
      LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
      LLVMValueRef p8 = LLVMBuildBitCast(f->builder, pval, i8p, "p8");
      LLVMValueRef off = oval;
      LLVMTypeRef i64 = LLVMInt64TypeInContext(f->ctx);
      if (LLVMGetIntTypeWidth(LLVMTypeOf(off)) != 64) {
        off = build_sext_or_trunc(f->builder, off, i64, "off64");
      }
      if (strcmp(op, "sub") == 0) {
        off = LLVMBuildNeg(f->builder, off, "off.neg");
      }
      LLVMValueRef idx[1] = {off};
      LLVMValueRef gep = LLVMBuildGEP2(f->builder, LLVMInt8TypeInContext(f->ctx), p8, idx, 1, "p.gep");
      out = LLVMBuildBitCast(f->builder, gep, pty, "p.cast");
      goto done;
    }

    if (strcmp(op, "to_i64") == 0 || strcmp(op, "from_i64") == 0) {
      if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
        err_codef(f->p, "sircc.args.bad", "sircc: %s node %lld requires args:[x]", n->tag, (long long)node_id);
        goto done;
      }
      int64_t x_id = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &x_id)) {
        err_codef(f->p, "sircc.args.ref_bad", "sircc: %s node %lld arg must be node ref", n->tag, (long long)node_id);
        goto done;
      }
      if (strcmp(op, "to_i64") == 0) {
        if (!reject_opaque_callable_operand(f, x_id, n->tag)) goto done;
      }
      LLVMValueRef x = lower_expr(f, x_id);
      if (!x) goto done;

      LLVMTypeRef i64 = LLVMInt64TypeInContext(f->ctx);
      unsigned ptr_bits = f->p->ptr_bits ? f->p->ptr_bits : (unsigned)(sizeof(void*) * 8u);
      LLVMTypeRef ip = LLVMIntTypeInContext(f->ctx, ptr_bits);
      LLVMTypeRef pty = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);

      if (strcmp(op, "to_i64") == 0) {
        if (LLVMGetTypeKind(LLVMTypeOf(x)) != LLVMPointerTypeKind) {
          err_codef(f->p, "sircc.ptr.to_i64.operand.type_bad", "sircc: ptr.to_i64 requires ptr operand");
          goto done;
        }
        LLVMValueRef bits = LLVMBuildPtrToInt(f->builder, x, ip, "ptr.bits");
        out = build_zext_or_trunc(f->builder, bits, i64, "ptr.i64");
        goto done;
      }

      if (LLVMGetTypeKind(LLVMTypeOf(x)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(x)) != 64) {
        err_codef(f->p, "sircc.ptr.from_i64.operand.type_bad", "sircc: ptr.from_i64 requires i64 operand");
        goto done;
      }
      LLVMValueRef bits = LLVMBuildTruncOrBitCast(f->builder, x, ip, "i64.ptrbits");
      out = LLVMBuildIntToPtr(f->builder, bits, pty, "ptr");
      goto done;
    }
  }

  if (strcmp(n->tag, "alloca") == 0) {
    if (!n->fields) {
      err_codef(f->p, "sircc.alloca.fields.missing", "sircc: alloca node %lld missing fields", (long long)node_id);
      goto done;
    }
    int64_t ty_id = 0;
    if (!parse_type_ref_id(f->p, json_obj_get(n->fields, "ty"), &ty_id)) {
      err_codef(f->p, "sircc.alloca.ty.missing", "sircc: alloca node %lld missing fields.ty (type ref)", (long long)node_id);
      goto done;
    }

    int64_t el_size = 0;
    int64_t el_align = 0;
    if (!type_size_align(f->p, ty_id, &el_size, &el_align)) {
      err_codef(f->p, "sircc.alloca.ty.bad", "sircc: alloca node %lld has invalid/unsized element type %lld", (long long)node_id,
                (long long)ty_id);
      goto done;
    }

    LLVMTypeRef el = lower_type(f->p, f->ctx, ty_id);
    if (!el) {
      err_codef(f->p, "sircc.alloca.ty.bad", "sircc: alloca node %lld has invalid element type %lld", (long long)node_id, (long long)ty_id);
      goto done;
    }

    // Parse flags: count?:i64, align?:i32, zero?:bool
    int64_t align_i64 = 0;
    bool align_present = false;
    bool zero_init = false;
    LLVMValueRef count_val = NULL;
    JsonValue* flags = json_obj_get(n->fields, "flags");
    if (flags && flags->type == JSON_OBJECT) {
      JsonValue* av = json_obj_get(flags, "align");
      if (av) {
        align_present = true;
        if (!json_get_i64(av, &align_i64)) {
          err_codef(f->p, "sircc.alloca.align.not_int", "sircc: alloca node %lld flags.align must be an integer", (long long)node_id);
          goto done;
        }
      }
      JsonValue* zv = json_obj_get(flags, "zero");
      if (zv && zv->type == JSON_BOOL) zero_init = zv->v.b;
    }
    JsonValue* countv = (flags && flags->type == JSON_OBJECT) ? json_obj_get(flags, "count") : NULL;
    if (!countv) countv = json_obj_get(n->fields, "count");
    JsonValue* alignv = json_obj_get(n->fields, "align");
    if (alignv) {
      align_present = true;
      if (!json_get_i64(alignv, &align_i64)) {
        err_codef(f->p, "sircc.alloca.align.not_int", "sircc: alloca node %lld align must be an integer", (long long)node_id);
        goto done;
      }
    }
    JsonValue* zerov = json_obj_get(n->fields, "zero");
    if (zerov && zerov->type == JSON_BOOL) zero_init = zerov->v.b;

    LLVMTypeRef i64 = LLVMInt64TypeInContext(f->ctx);
    if (!countv) {
      count_val = LLVMConstInt(i64, 1, 0);
    } else {
      int64_t c = 0;
      if (json_get_i64(countv, &c)) {
        if (c < 0) {
          err_codef(f->p, "sircc.alloca.count.range", "sircc: alloca node %lld count must be >= 0", (long long)node_id);
          goto done;
        }
        count_val = LLVMConstInt(i64, (unsigned long long)c, 0);
      } else {
        int64_t cid = 0;
        if (!parse_node_ref_id(f->p, countv, &cid)) {
          err_codef(f->p, "sircc.alloca.count.bad", "sircc: alloca node %lld count must be i64 or node ref", (long long)node_id);
          goto done;
        }
        count_val = lower_expr(f, cid);
        if (!count_val) goto done;
        if (LLVMGetTypeKind(LLVMTypeOf(count_val)) != LLVMIntegerTypeKind) {
          err_codef(f->p, "sircc.alloca.count.ref_type_bad", "sircc: alloca node %lld count ref must be integer", (long long)node_id);
          goto done;
        }
        if (LLVMGetIntTypeWidth(LLVMTypeOf(count_val)) != 64) {
          count_val = build_zext_or_trunc(f->builder, count_val, i64, "count.i64");
        }
      }
    }

    LLVMValueRef alloca_i = NULL;
    bool is_one = false;
    if (LLVMIsAConstantInt(count_val)) {
      unsigned long long z = LLVMConstIntGetZExtValue(count_val);
      is_one = (z == 1);
    }
    if (is_one) {
      alloca_i = LLVMBuildAlloca(f->builder, el, "alloca");
    } else {
      alloca_i = LLVMBuildArrayAlloca(f->builder, el, count_val, "alloca");
    }
    if (!alloca_i) goto done;

    unsigned align = 0;
    if (align_present) {
      if (align_i64 <= 0 || align_i64 > (int64_t)UINT_MAX) {
        err_codef(f->p, "sircc.alloca.align.range", "sircc: alloca node %lld align must be > 0", (long long)node_id);
        goto done;
      }
      align = (unsigned)align_i64;
    } else if (el_align > 0) {
      align = (unsigned)el_align;
    }
    if (align) LLVMSetAlignment(alloca_i, align);

    if (zero_init) {
      LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
      LLVMValueRef dst = LLVMBuildBitCast(f->builder, alloca_i, i8p, "alloca.i8p");
      LLVMValueRef byte = LLVMConstInt(LLVMInt8TypeInContext(f->ctx), 0, 0);
      LLVMValueRef bytes = LLVMConstInt(i64, (unsigned long long)el_size, 0);
      if (!is_one) bytes = LLVMBuildMul(f->builder, count_val, bytes, "alloca.bytes");
      LLVMBuildMemSet(f->builder, dst, byte, bytes, align ? align : 1);
    }

    // SIR mnemonic returns `ptr` (opaque). Represent as i8*.
    LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
    alloca_i = LLVMBuildBitCast(f->builder, alloca_i, i8p, "alloca.ptr");

    out = alloca_i;
    goto done;
  }

  if (strncmp(n->tag, "alloca.", 7) == 0) {
    const char* tname = n->tag + 7;
    LLVMTypeRef el = NULL;
    if (strcmp(tname, "ptr") == 0) {
      el = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
    } else {
      el = lower_type_prim(f->ctx, tname);
    }
    if (!el) {
      err_codef(f->p, "sircc.alloca.type_unsupported", "sircc: unsupported alloca type '%s'", tname);
      goto done;
    }
    out = LLVMBuildAlloca(f->builder, el, "alloca");
    goto done;
  }

  if (strncmp(n->tag, "load.", 5) == 0) {
    const char* tname = n->tag + 5;
    if (!n->fields) {
      err_codef(f->p, "sircc.load.fields.missing", "sircc: %s node %lld missing fields", n->tag, (long long)node_id);
      goto done;
    }
    JsonValue* addr = json_obj_get(n->fields, "addr");
    int64_t aid = 0;
    if (!parse_node_ref_id(f->p, addr, &aid)) {
      err_codef(f->p, "sircc.load.addr.ref_bad", "sircc: %s node %lld missing fields.addr ref", n->tag, (long long)node_id);
      goto done;
    }
    LLVMValueRef pval = lower_expr(f, aid);
    if (!pval) goto done;
    LLVMTypeRef pty = LLVMTypeOf(pval);
    if (LLVMGetTypeKind(pty) != LLVMPointerTypeKind) {
      err_codef(f->p, "sircc.load.addr.type_bad", "sircc: %s requires pointer addr", n->tag);
      goto done;
    }
    LLVMTypeRef el = NULL;
    if (strcmp(tname, "ptr") == 0) {
      el = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
    } else {
      el = lower_type_prim(f->ctx, tname);
    }
    if (!el) {
      err_codef(f->p, "sircc.load.type_unsupported", "sircc: unsupported load type '%s'", tname);
      goto done;
    }
    LLVMTypeRef want_ptr = LLVMPointerType(el, 0);
    if (want_ptr != pty) {
      pval = LLVMBuildBitCast(f->builder, pval, want_ptr, "ld.cast");
    }
    JsonValue* alignv = json_obj_get(n->fields, "align");
    unsigned align = 1;
    if (alignv) {
      int64_t a = 0;
      if (!json_get_i64(alignv, &a)) {
        err_codef(f->p, "sircc.load.align.not_int", "sircc: %s node %lld align must be an integer", n->tag, (long long)node_id);
        goto done;
      }
      if (a <= 0 || a > (int64_t)UINT_MAX) {
        err_codef(f->p, "sircc.load.align.range", "sircc: %s node %lld align must be > 0", n->tag, (long long)node_id);
        goto done;
      }
      align = (unsigned)a;
    }
    if ((align & (align - 1u)) != 0u) {
      err_codef(f->p, "sircc.load.align.not_pow2", "sircc: %s node %lld align must be a power of two", n->tag, (long long)node_id);
      goto done;
    }
    if (!emit_trap_if_misaligned(f, pval, align)) goto done;
    out = LLVMBuildLoad2(f->builder, el, pval, "load");
    LLVMSetAlignment(out, align);
    JsonValue* volv = json_obj_get(n->fields, "vol");
    if (volv && volv->type == JSON_BOOL) LLVMSetVolatile(out, volv->v.b ? 1 : 0);
    if (LLVMGetTypeKind(el) == LLVMFloatTypeKind || LLVMGetTypeKind(el) == LLVMDoubleTypeKind) {
      out = canonicalize_float(f, out);
    }
    goto done;
  }

  if (strncmp(n->tag, "f32.", 4) == 0 || strncmp(n->tag, "f64.", 4) == 0) {
    int width = (n->tag[1] == '3') ? 32 : 64;
    const char* op = n->tag + 4;

    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
    if (!args || args->type != JSON_ARRAY) {
      err_codef(f->p, "sircc.args.missing", "sircc: %s node %lld missing args array", n->tag, (long long)node_id);
      goto done;
    }

    LLVMValueRef a = NULL;
    LLVMValueRef b = NULL;

    if (args->v.arr.len == 1) {
      int64_t a_id = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &a_id)) {
        err_codef(f->p, "sircc.args.ref_bad", "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
        goto done;
      }
      a = lower_expr(f, a_id);
      if (!a) goto done;
    } else if (args->v.arr.len == 2) {
      int64_t a_id = 0, b_id = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &a_id) || !parse_node_ref_id(f->p, args->v.arr.items[1], &b_id)) {
        err_codef(f->p, "sircc.args.ref_bad", "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
        goto done;
      }
      a = lower_expr(f, a_id);
      b = lower_expr(f, b_id);
      if (!a || !b) goto done;
    } else {
      err_codef(f->p, "sircc.args.arity_bad", "sircc: %s node %lld args must have arity 1 or 2", n->tag, (long long)node_id);
      goto done;
    }

    // Conversions like f32.from_i32.s take integer operands, so handle those
    // before enforcing float operand types.
    if (strncmp(op, "from_i", 6) == 0) {
      if (!a || b) {
        err_codef(f->p, "sircc.args.bad", "sircc: %s requires args:[x]", n->tag);
        goto done;
      }
      int srcw = 0;
      char su = 0;
      if (sscanf(op, "from_i%d.%c", &srcw, &su) != 2 || (srcw != 32 && srcw != 64) || (su != 's' && su != 'u')) {
        err_codef(f->p, "sircc.conv.int_to_float.unsupported", "sircc: unsupported int->float conversion '%s' in %s", op, n->tag);
        goto done;
      }
      if (LLVMGetTypeKind(LLVMTypeOf(a)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(a)) != (unsigned)srcw) {
        err_codef(f->p, "sircc.operand.type_bad", "sircc: %s requires i%d operand", n->tag, srcw);
        goto done;
      }
      LLVMTypeRef fty = (width == 32) ? LLVMFloatTypeInContext(f->ctx) : LLVMDoubleTypeInContext(f->ctx);
      out = (su == 's') ? LLVMBuildSIToFP(f->builder, a, fty, "sitofp") : LLVMBuildUIToFP(f->builder, a, fty, "uitofp");
      goto done;
    }

    LLVMTypeRef fty = LLVMTypeOf(a);
    if (width == 32 && LLVMGetTypeKind(fty) != LLVMFloatTypeKind) {
      err_codef(f->p, "sircc.operand.type_bad", "sircc: %s expects f32 operands", n->tag);
      goto done;
    }
    if (width == 64 && LLVMGetTypeKind(fty) != LLVMDoubleTypeKind) {
      err_codef(f->p, "sircc.operand.type_bad", "sircc: %s expects f64 operands", n->tag);
      goto done;
    }

    if (strcmp(op, "add") == 0) {
      if (!b) {
        err_codef(f->p, "sircc.args.arity_bad", "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      out = canonicalize_float(f, LLVMBuildFAdd(f->builder, a, b, "fadd"));
      goto done;
    }
    if (strcmp(op, "sub") == 0) {
      if (!b) {
        err_codef(f->p, "sircc.args.arity_bad", "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      out = canonicalize_float(f, LLVMBuildFSub(f->builder, a, b, "fsub"));
      goto done;
    }
    if (strcmp(op, "mul") == 0) {
      if (!b) {
        err_codef(f->p, "sircc.args.arity_bad", "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      out = canonicalize_float(f, LLVMBuildFMul(f->builder, a, b, "fmul"));
      goto done;
    }
    if (strcmp(op, "div") == 0) {
      if (!b) {
        err_codef(f->p, "sircc.args.arity_bad", "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      out = canonicalize_float(f, LLVMBuildFDiv(f->builder, a, b, "fdiv"));
      goto done;
    }
    if (strcmp(op, "neg") == 0) {
      out = canonicalize_float(f, LLVMBuildFNeg(f->builder, a, "fneg"));
      goto done;
    }
    if (strcmp(op, "abs") == 0) {
      char full[32];
      snprintf(full, sizeof(full), "llvm.fabs.f%d", width);
      LLVMTypeRef params[1] = {fty};
      LLVMValueRef fn = get_or_declare_intrinsic(f->mod, full, fty, params, 1);
      LLVMValueRef argsv[1] = {a};
      out = canonicalize_float(f, LLVMBuildCall2(f->builder, LLVMGlobalGetValueType(fn), fn, argsv, 1, "fabs"));
      goto done;
    }
    if (strcmp(op, "sqrt") == 0) {
      char full[32];
      snprintf(full, sizeof(full), "llvm.sqrt.f%d", width);
      LLVMTypeRef params[1] = {fty};
      LLVMValueRef fn = get_or_declare_intrinsic(f->mod, full, fty, params, 1);
      LLVMValueRef argsv[1] = {a};
      out = canonicalize_float(f, LLVMBuildCall2(f->builder, LLVMGlobalGetValueType(fn), fn, argsv, 1, "fsqrt"));
      goto done;
    }

    if (strcmp(op, "min") == 0 || strcmp(op, "max") == 0) {
      if (!b) {
        err_codef(f->p, "sircc.args.arity_bad", "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      LLVMValueRef isnan_a = LLVMBuildFCmp(f->builder, LLVMRealUNO, a, a, "isnan.a");
      LLVMValueRef isnan_b = LLVMBuildFCmp(f->builder, LLVMRealUNO, b, b, "isnan.b");
      LLVMValueRef anynan = LLVMBuildOr(f->builder, isnan_a, isnan_b, "isnan.any");
      LLVMValueRef qnan = canonical_qnan(f, fty);

      LLVMRealPredicate pred = (strcmp(op, "min") == 0) ? LLVMRealOLT : LLVMRealOGT;
      LLVMValueRef cmp = LLVMBuildFCmp(f->builder, pred, a, b, "fcmp");
      LLVMValueRef sel = LLVMBuildSelect(f->builder, cmp, a, b, "fsel");
      out = LLVMBuildSelect(f->builder, anynan, qnan, sel, "fminmax");
      goto done;
    }

    if (strncmp(op, "cmp.", 4) == 0) {
      if (!b) {
        err_codef(f->p, "sircc.args.arity_bad", "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      const char* cc = op + 4;
      LLVMRealPredicate pred;
      if (strcmp(cc, "oeq") == 0) pred = LLVMRealOEQ;
      else if (strcmp(cc, "one") == 0) pred = LLVMRealONE;
      else if (strcmp(cc, "olt") == 0) pred = LLVMRealOLT;
      else if (strcmp(cc, "ole") == 0) pred = LLVMRealOLE;
      else if (strcmp(cc, "ogt") == 0) pred = LLVMRealOGT;
      else if (strcmp(cc, "oge") == 0) pred = LLVMRealOGE;
      else if (strcmp(cc, "ueq") == 0) pred = LLVMRealUEQ;
      else if (strcmp(cc, "une") == 0) pred = LLVMRealUNE;
      else if (strcmp(cc, "ult") == 0) pred = LLVMRealULT;
      else if (strcmp(cc, "ule") == 0) pred = LLVMRealULE;
      else if (strcmp(cc, "ugt") == 0) pred = LLVMRealUGT;
      else if (strcmp(cc, "uge") == 0) pred = LLVMRealUGE;
      else {
        err_codef(f->p, "sircc.cmp.float.cc.bad", "sircc: unsupported float compare '%s' in %s", cc, n->tag);
        goto done;
      }
      out = LLVMBuildFCmp(f->builder, pred, a, b, "fcmp");
      goto done;
    }

  }

  if (strncmp(n->tag, "closure.", 8) == 0) {
    const char* op = n->tag + 8;

    if (strcmp(op, "make") == 0) {
      if (!n->fields) {
        err_codef(f->p, "sircc.closure.make.missing_fields", "sircc: closure.make node %lld missing fields", (long long)node_id);
        goto done;
      }
      if (n->type_ref == 0) {
        err_codef(f->p, "sircc.closure.make.missing_type_ref",
                  "sircc: closure.make node %lld missing type_ref (closure type)", (long long)node_id);
        goto done;
      }
      TypeRec* cty = get_type(f->p, n->type_ref);
      if (!cty || cty->kind != TYPE_CLOSURE || cty->call_sig == 0 || cty->env_ty == 0) {
        err_codef(f->p, "sircc.closure.make.type_ref.bad",
                  "sircc: closure.make node %lld type_ref must be a closure type", (long long)node_id);
        goto done;
      }

      JsonValue* args = json_obj_get(n->fields, "args");
      if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) {
        err_codef(f->p, "sircc.closure.make.args_bad",
                  "sircc: closure.make node %lld requires fields.args:[code, env]", (long long)node_id);
        goto done;
      }
      int64_t code_id = 0, env_id = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &code_id) || !parse_node_ref_id(f->p, args->v.arr.items[1], &env_id)) {
        err_codef(f->p, "sircc.closure.make.arg_ref_bad",
                  "sircc: closure.make node %lld args must be node refs", (long long)node_id);
        goto done;
      }

      // Validate code/env types against closure type.
      NodeRec* code_n = get_node(f->p, code_id);
      if (!code_n || code_n->type_ref == 0) {
        err_codef(f->p, "sircc.closure.make.code.missing_type", "sircc: closure.make code must have a fun type_ref");
        goto done;
      }
      TypeRec* code_ty = get_type(f->p, code_n->type_ref);
      if (!code_ty || code_ty->kind != TYPE_FUN || code_ty->sig == 0) {
        err_codef(f->p, "sircc.closure.make.code.not_fun", "sircc: closure.make code must be a fun value");
        goto done;
      }
      LLVMTypeRef have_code_sig = lower_type(f->p, f->ctx, code_ty->sig);
      if (!have_code_sig || LLVMGetTypeKind(have_code_sig) != LLVMFunctionTypeKind) goto done;

      // Derive codeSig = (env, callSig.params...) -> callSig.ret
      TypeRec* cs = get_type(f->p, cty->call_sig);
      if (!cs || cs->kind != TYPE_FN) {
        err_codef(f->p, "sircc.closure.make.callSig.bad", "sircc: closure.make closure.callSig must reference fn type");
        goto done;
      }
      LLVMTypeRef env_ty = lower_type(f->p, f->ctx, cty->env_ty);
      LLVMTypeRef ret_ty = lower_type(f->p, f->ctx, cs->ret);
      if (!env_ty || !ret_ty) goto done;
      size_t nparams = cs->param_len + 1;
      if (nparams > UINT_MAX) goto done;
      LLVMTypeRef* params = (LLVMTypeRef*)malloc(nparams * sizeof(LLVMTypeRef));
      if (!params) goto done;
      params[0] = env_ty;
      bool ok = true;
      for (size_t i = 0; i < cs->param_len; i++) {
        params[i + 1] = lower_type(f->p, f->ctx, cs->params[i]);
        if (!params[i + 1]) {
          ok = false;
          break;
        }
      }
      LLVMTypeRef want_code_sig = NULL;
      if (ok) want_code_sig = LLVMFunctionType(ret_ty, params, (unsigned)nparams, cs->varargs ? 1 : 0);
      free(params);
      if (!want_code_sig || !llvm_fn_type_eq(have_code_sig, want_code_sig)) {
        err_codef(f->p, "sircc.closure.make.code.sig_mismatch", "sircc: closure.make code signature does not match derived codeSig");
        goto done;
      }

      LLVMValueRef code = lower_expr(f, code_id);
      LLVMValueRef env = lower_expr(f, env_id);
      if (!code || !env) goto done;
      if (env_ty && LLVMTypeOf(env) != env_ty) {
        err_codef(f->p, "sircc.closure.make.env.type_mismatch", "sircc: closure.make env type does not match closure env type");
        goto done;
      }

      LLVMTypeRef clo_ty = lower_type(f->p, f->ctx, n->type_ref);
      if (!clo_ty || LLVMGetTypeKind(clo_ty) != LLVMStructTypeKind) {
        err_codef(f->p, "sircc.closure.make.llvm_type.bad",
                  "sircc: closure.make node %lld invalid closure type_ref", (long long)node_id);
        goto done;
      }

      LLVMValueRef tmp = LLVMGetUndef(clo_ty);
      tmp = LLVMBuildInsertValue(f->builder, tmp, code, 0, "clo.code");
      tmp = LLVMBuildInsertValue(f->builder, tmp, env, 1, "clo.env");
      out = tmp;
      goto done;
    }

    if (strcmp(op, "sym") == 0) {
      if (!n->fields) {
        err_codef(f->p, "sircc.closure.sym.missing_fields", "sircc: closure.sym node %lld missing fields", (long long)node_id);
        goto done;
      }
      if (n->type_ref == 0) {
        err_codef(f->p, "sircc.closure.sym.missing_type_ref",
                  "sircc: closure.sym node %lld missing type_ref (closure type)", (long long)node_id);
        goto done;
      }
      TypeRec* cty = get_type(f->p, n->type_ref);
      if (!cty || cty->kind != TYPE_CLOSURE || cty->call_sig == 0 || cty->env_ty == 0) {
        err_codef(f->p, "sircc.closure.sym.type_ref.bad",
                  "sircc: closure.sym node %lld type_ref must be a closure type", (long long)node_id);
        goto done;
      }

      const char* name = json_get_string(json_obj_get(n->fields, "name"));
      if (!name || !is_ident(name)) {
        err_codef(f->p, "sircc.closure.sym.name.bad",
                  "sircc: closure.sym node %lld requires fields.name Ident", (long long)node_id);
        goto done;
      }
      int64_t env_id = 0;
      if (!parse_node_ref_id(f->p, json_obj_get(n->fields, "env"), &env_id)) {
        err_codef(f->p, "sircc.closure.sym.env.ref.missing",
                  "sircc: closure.sym node %lld missing fields.env ref", (long long)node_id);
        goto done;
      }
      LLVMValueRef env = lower_expr(f, env_id);
      if (!env) goto done;
      LLVMTypeRef want_env_ty = lower_type(f->p, f->ctx, cty->env_ty);
      if (want_env_ty && LLVMTypeOf(env) != want_env_ty) {
        err_codef(f->p, "sircc.closure.sym.env.type_mismatch", "sircc: closure.sym env type does not match closure env type");
        goto done;
      }

      TypeRec* cs = get_type(f->p, cty->call_sig);
      if (!cs || cs->kind != TYPE_FN) {
        err_codef(f->p, "sircc.closure.sym.callSig.bad",
                  "sircc: closure.sym node %lld closure.callSig must reference fn type", (long long)node_id);
        goto done;
      }
      LLVMTypeRef env_ty = lower_type(f->p, f->ctx, cty->env_ty);
      LLVMTypeRef ret_ty = lower_type(f->p, f->ctx, cs->ret);
      if (!env_ty || !ret_ty) goto done;
      size_t nparams = cs->param_len + 1;
      if (nparams > UINT_MAX) goto done;
      LLVMTypeRef* params = (LLVMTypeRef*)malloc(nparams * sizeof(LLVMTypeRef));
      if (!params) goto done;
      params[0] = env_ty;
      bool ok = true;
      for (size_t i = 0; i < cs->param_len; i++) {
        params[i + 1] = lower_type(f->p, f->ctx, cs->params[i]);
        if (!params[i + 1]) {
          ok = false;
          break;
        }
      }
      LLVMTypeRef code_sig = NULL;
      if (ok) code_sig = LLVMFunctionType(ret_ty, params, (unsigned)nparams, cs->varargs ? 1 : 0);
      free(params);
      if (!code_sig) goto done;

      LLVMValueRef fn = LLVMGetNamedFunction(f->mod, name);
      if (!fn) {
        fn = LLVMAddFunction(f->mod, name, code_sig);
        LLVMSetLinkage(fn, LLVMExternalLinkage);
      } else {
        LLVMTypeRef have = LLVMGlobalGetValueType(fn);
        if (have != code_sig) {
          err_codef(f->p, "sircc.closure.sym.sig_mismatch",
                    "sircc: closure.sym '%s' type mismatch vs existing declaration/definition", name);
          goto done;
        }
      }

      LLVMTypeRef clo_ty = lower_type(f->p, f->ctx, n->type_ref);
      if (!clo_ty || LLVMGetTypeKind(clo_ty) != LLVMStructTypeKind) goto done;

      LLVMValueRef tmp = LLVMGetUndef(clo_ty);
      tmp = LLVMBuildInsertValue(f->builder, tmp, fn, 0, "clo.code");
      tmp = LLVMBuildInsertValue(f->builder, tmp, env, 1, "clo.env");
      out = tmp;
      goto done;
    }

    if (strcmp(op, "code") == 0 || strcmp(op, "env") == 0) {
      if (!n->fields) {
        err_codef(f->p, "sircc.closure.access.missing_fields", "sircc: %s node %lld missing fields", n->tag, (long long)node_id);
        goto done;
      }
      JsonValue* args = json_obj_get(n->fields, "args");
      if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
        err_codef(f->p, "sircc.closure.access.args_bad",
                  "sircc: %s node %lld requires fields.args:[c]", n->tag, (long long)node_id);
        goto done;
      }
      int64_t cid = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &cid)) {
        err_codef(f->p, "sircc.closure.access.arg_ref_bad",
                  "sircc: %s node %lld arg must be node ref", n->tag, (long long)node_id);
        goto done;
      }
      LLVMValueRef c = lower_expr(f, cid);
      if (!c) goto done;
      unsigned idx = (strcmp(op, "code") == 0) ? 0u : 1u;
      out = LLVMBuildExtractValue(f->builder, c, idx, idx == 0 ? "clo.code" : "clo.env");
      goto done;
    }

    if (strcmp(op, "cmp.eq") == 0 || strcmp(op, "cmp.ne") == 0) {
      if (!n->fields) {
        err_codef(f->p, "sircc.closure.cmp.missing_fields", "sircc: %s node %lld missing fields", n->tag, (long long)node_id);
        goto done;
      }
      JsonValue* args = json_obj_get(n->fields, "args");
      if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) {
        err_codef(f->p, "sircc.closure.cmp.args_bad",
                  "sircc: %s node %lld requires fields.args:[a,b]", n->tag, (long long)node_id);
        goto done;
      }
      int64_t a_id = 0, b_id = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &a_id) || !parse_node_ref_id(f->p, args->v.arr.items[1], &b_id)) {
        err_codef(f->p, "sircc.closure.cmp.arg_ref_bad",
                  "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
        goto done;
      }
      LLVMValueRef a = lower_expr(f, a_id);
      LLVMValueRef b = lower_expr(f, b_id);
      if (!a || !b) goto done;

      LLVMValueRef acode = LLVMBuildExtractValue(f->builder, a, 0, "acode");
      LLVMValueRef bcode = LLVMBuildExtractValue(f->builder, b, 0, "bcode");
      LLVMValueRef aenv = LLVMBuildExtractValue(f->builder, a, 1, "aenv");
      LLVMValueRef benv = LLVMBuildExtractValue(f->builder, b, 1, "benv");
      if (!acode || !bcode || !aenv || !benv) goto done;

      LLVMValueRef code_eq = LLVMBuildICmp(f->builder, LLVMIntEQ, acode, bcode, "code.eq");

      LLVMTypeRef env_ty = LLVMTypeOf(aenv);
      LLVMValueRef env_eq = NULL;
      LLVMTypeKind k = LLVMGetTypeKind(env_ty);
      if (k == LLVMIntegerTypeKind || k == LLVMPointerTypeKind) {
        env_eq = LLVMBuildICmp(f->builder, LLVMIntEQ, aenv, benv, "env.eq");
      } else {
        err_codef(f->p, "sircc.closure.cmp.env_unsupported",
                  "sircc: %s env equality unsupported for non-integer/non-pointer env type", n->tag);
        goto done;
      }

      LLVMValueRef both = LLVMBuildAnd(f->builder, code_eq, env_eq, "clo.eq");
      if (strcmp(op, "cmp.eq") == 0) out = both;
      else out = LLVMBuildNot(f->builder, both, "clo.ne");
      goto done;
    }
  }

  if (strncmp(n->tag, "adt.", 4) == 0) {
    const char* op = n->tag + 4;

    // Helper: compute payload layout for a sum type (payload_size, payload_align, payload_off, payload_field_index).
    int64_t payload_size = 0;
    int64_t payload_align = 1;
    int64_t payload_off = 4;
    int payload_field = 1; // default: {tag,payload}

    TypeRec* sum_ty = NULL;
    if (strcmp(op, "make") == 0) {
      if (n->type_ref == 0) {
        err_codef(f->p, "sircc.adt.make.missing_type_ref", "sircc: adt.make node %lld missing type_ref (sum type)", (long long)node_id);
        goto done;
      }
      sum_ty = get_type(f->p, n->type_ref);
    } else if (strcmp(op, "get") == 0) {
      if (!n->fields) {
        err_codef(f->p, "sircc.adt.get.missing_fields", "sircc: adt.get node %lld missing fields", (long long)node_id);
        goto done;
      }
      int64_t ty_id = 0;
      if (!parse_type_ref_id(f->p, json_obj_get(n->fields, "ty"), &ty_id)) {
        err_codef(f->p, "sircc.adt.get.missing_ty", "sircc: adt.get node %lld missing fields.ty (sum type)", (long long)node_id);
        goto done;
      }
      sum_ty = get_type(f->p, ty_id);
    } else {
      // tag/is don't need explicit type_ref here; we derive from operand where required.
    }

    if (sum_ty && sum_ty->kind == TYPE_SUM) {
      for (size_t i = 0; i < sum_ty->variant_len; i++) {
        int64_t vty = sum_ty->variants[i].ty;
        if (vty == 0) continue;
        int64_t vsz = 0;
        int64_t val = 0;
        if (type_size_align(f->p, vty, &vsz, &val)) {
          if (vsz > payload_size) payload_size = vsz;
          if (val > payload_align) payload_align = val;
        }
      }
      if (payload_align < 1) payload_align = 1;
      int64_t rem = payload_off % payload_align;
      if (rem) payload_off += (payload_align - rem);
      if (payload_off > 4) payload_field = 2; // {tag,pad,payload}
    }

    if (strcmp(op, "tag") == 0) {
      if (!n->fields) {
        err_codef(f->p, "sircc.adt.tag.missing_fields", "sircc: adt.tag node %lld missing fields", (long long)node_id);
        goto done;
      }
      JsonValue* args = json_obj_get(n->fields, "args");
      if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
        err_codef(f->p, "sircc.adt.tag.args_bad", "sircc: adt.tag node %lld requires fields.args:[v]", (long long)node_id);
        goto done;
      }
      int64_t vid = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &vid)) {
        err_codef(f->p, "sircc.adt.tag.arg_ref_bad", "sircc: adt.tag node %lld arg must be node ref", (long long)node_id);
        goto done;
      }
      LLVMValueRef v = lower_expr(f, vid);
      if (!v) goto done;
      out = LLVMBuildExtractValue(f->builder, v, 0, "tag");
      goto done;
    }

    if (strcmp(op, "is") == 0) {
      if (!n->fields) {
        err_codef(f->p, "sircc.adt.is.missing_fields", "sircc: adt.is node %lld missing fields", (long long)node_id);
        goto done;
      }
      JsonValue* args = json_obj_get(n->fields, "args");
      if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
        err_codef(f->p, "sircc.adt.is.args_bad", "sircc: adt.is node %lld requires fields.args:[v]", (long long)node_id);
        goto done;
      }
      JsonValue* flags = json_obj_get(n->fields, "flags");
      if (!flags || flags->type != JSON_OBJECT) {
        err_codef(f->p, "sircc.adt.is.flags_missing", "sircc: adt.is node %lld missing fields.flags", (long long)node_id);
        goto done;
      }
      int64_t variant = -1;
      if (!must_i64(f->p, json_obj_get(flags, "variant"), &variant, "adt.is.flags.variant")) goto done;

      int64_t vid = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &vid)) {
        err_codef(f->p, "sircc.adt.is.arg_ref_bad", "sircc: adt.is node %lld arg must be node ref", (long long)node_id);
        goto done;
      }
      LLVMValueRef v = lower_expr(f, vid);
      if (!v) goto done;
      LLVMValueRef tag = LLVMBuildExtractValue(f->builder, v, 0, "tag");

      // Out of range => deterministic trap.
      // Derive sum type from operand if possible; otherwise treat variant as unchecked.
      NodeRec* vn = get_node(f->p, vid);
      TypeRec* sty = (vn && vn->type_ref) ? get_type(f->p, vn->type_ref) : NULL;
      if (sty && sty->kind == TYPE_SUM) {
        LLVMValueRef bad = LLVMConstInt(LLVMInt1TypeInContext(f->ctx),
                                       (variant < 0 || (size_t)variant >= sty->variant_len) ? 1 : 0, 0);
        if (!emit_trap_if(f, bad)) goto done;
      }

      LLVMValueRef want = LLVMConstInt(LLVMInt32TypeInContext(f->ctx), (unsigned long long)variant, 0);
      out = LLVMBuildICmp(f->builder, LLVMIntEQ, tag, want, "is");
      goto done;
    }

    if (strcmp(op, "make") == 0) {
      if (!n->fields) {
        err_codef(f->p, "sircc.adt.make.missing_fields", "sircc: adt.make node %lld missing fields", (long long)node_id);
        goto done;
      }
      if (n->type_ref == 0) {
        err_codef(f->p, "sircc.adt.make.missing_type_ref", "sircc: adt.make node %lld missing type_ref (sum type)", (long long)node_id);
        goto done;
      }
      TypeRec* sty = get_type(f->p, n->type_ref);
      if (!sty || sty->kind != TYPE_SUM) {
        err_codef(f->p, "sircc.adt.make.type_ref.bad", "sircc: adt.make node %lld type_ref must be a sum type", (long long)node_id);
        goto done;
      }
      JsonValue* flags = json_obj_get(n->fields, "flags");
      if (!flags || flags->type != JSON_OBJECT) {
        err_codef(f->p, "sircc.adt.make.flags_missing", "sircc: adt.make node %lld missing fields.flags", (long long)node_id);
        goto done;
      }
      int64_t variant = -1;
      if (!must_i64(f->p, json_obj_get(flags, "variant"), &variant, "adt.make.flags.variant")) goto done;

      LLVMValueRef bad = LLVMConstInt(LLVMInt1TypeInContext(f->ctx),
                                      (variant < 0 || (size_t)variant >= sty->variant_len) ? 1 : 0, 0);
      if (!emit_trap_if(f, bad)) goto done;
      if (variant < 0 || (size_t)variant >= sty->variant_len) variant = 0;

      int64_t pay_ty_id = sty->variants[(size_t)variant].ty;

      JsonValue* args = json_obj_get(n->fields, "args");
      size_t argc = 0;
      if (args) {
        if (args->type != JSON_ARRAY) {
          err_codef(f->p, "sircc.adt.make.args_type_bad",
                    "sircc: adt.make node %lld fields.args must be array when present", (long long)node_id);
          goto done;
        }
        argc = args->v.arr.len;
      }
      if (pay_ty_id == 0) {
        if (argc != 0) {
          err_codef(f->p, "sircc.adt.make.args_nullary_bad",
                    "sircc: adt.make node %lld variant %lld is nullary; args must be empty", (long long)node_id, (long long)variant);
          goto done;
        }
      } else {
        if (argc != 1) {
          err_codef(f->p, "sircc.adt.make.args_payload_bad",
                    "sircc: adt.make node %lld variant %lld requires one payload arg", (long long)node_id, (long long)variant);
          goto done;
        }
      }

      LLVMTypeRef sum_llvm = lower_type(f->p, f->ctx, n->type_ref);
      if (!sum_llvm) goto done;

      int64_t sum_sz = 0, sum_al = 0;
      if (!type_size_align(f->p, n->type_ref, &sum_sz, &sum_al)) {
        err_codef(f->p, "sircc.adt.layout.bad", "sircc: adt.make node %lld could not compute sum layout", (long long)node_id);
        goto done;
      }

      LLVMValueRef slot = LLVMBuildAlloca(f->builder, sum_llvm, "sum.tmp");
      if (sum_al > 0 && sum_al <= 4096) LLVMSetAlignment(slot, (unsigned)sum_al);

      LLVMValueRef zero = LLVMConstNull(sum_llvm);
      LLVMBuildStore(f->builder, zero, slot);

      LLVMValueRef tagp = LLVMBuildStructGEP2(f->builder, sum_llvm, slot, 0, "tagp");
      LLVMValueRef tagv = LLVMConstInt(LLVMInt32TypeInContext(f->ctx), (unsigned long long)variant, 0);
      LLVMBuildStore(f->builder, tagv, tagp);

      if (pay_ty_id != 0) {
        int64_t pid = 0;
        (void)parse_node_ref_id(f->p, args->v.arr.items[0], &pid);
        LLVMValueRef payload = lower_expr(f, pid);
        if (!payload) goto done;
        LLVMValueRef payp = LLVMBuildStructGEP2(f->builder, sum_llvm, slot, (unsigned)payload_field, "payloadp");
        LLVMTypeRef pay_ty = lower_type(f->p, f->ctx, pay_ty_id);
        if (!pay_ty) goto done;
        LLVMValueRef castp = LLVMBuildBitCast(f->builder, payp, LLVMPointerType(pay_ty, 0), "pay.castp");
        LLVMValueRef st = LLVMBuildStore(f->builder, payload, castp);
        int64_t psz = 0, pal = 0;
        if (type_size_align(f->p, pay_ty_id, &psz, &pal) && pal > 0 && pal <= 4096) {
          LLVMSetAlignment(st, (unsigned)pal);
        }
      }

      out = LLVMBuildLoad2(f->builder, sum_llvm, slot, "sum");
      goto done;
    }

    if (strcmp(op, "get") == 0) {
      if (!n->fields) {
        err_codef(f->p, "sircc.adt.get.missing_fields", "sircc: adt.get node %lld missing fields", (long long)node_id);
        goto done;
      }
      int64_t sum_ty_id = 0;
      if (!parse_type_ref_id(f->p, json_obj_get(n->fields, "ty"), &sum_ty_id)) {
        err_codef(f->p, "sircc.adt.get.missing_ty", "sircc: adt.get node %lld missing fields.ty (sum type)", (long long)node_id);
        goto done;
      }
      TypeRec* sty = get_type(f->p, sum_ty_id);
      if (!sty || sty->kind != TYPE_SUM) {
        err_codef(f->p, "sircc.adt.get.ty.bad",
                  "sircc: adt.get node %lld fields.ty must reference a sum type", (long long)node_id);
        goto done;
      }
      JsonValue* flags = json_obj_get(n->fields, "flags");
      if (!flags || flags->type != JSON_OBJECT) {
        err_codef(f->p, "sircc.adt.get.flags_missing", "sircc: adt.get node %lld missing fields.flags", (long long)node_id);
        goto done;
      }
      int64_t variant = -1;
      if (!must_i64(f->p, json_obj_get(flags, "variant"), &variant, "adt.get.flags.variant")) goto done;

      LLVMValueRef bad = LLVMConstInt(LLVMInt1TypeInContext(f->ctx),
                                      (variant < 0 || (size_t)variant >= sty->variant_len) ? 1 : 0, 0);
      if (!emit_trap_if(f, bad)) goto done;
      if (variant < 0 || (size_t)variant >= sty->variant_len) variant = 0;

      int64_t pay_ty_id = sty->variants[(size_t)variant].ty;
      if (pay_ty_id == 0) {
        err_codef(f->p, "sircc.adt.get.nullary",
                  "sircc: adt.get node %lld variant %lld is nullary (no payload)", (long long)node_id, (long long)variant);
        goto done;
      }

      JsonValue* args = json_obj_get(n->fields, "args");
      if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
        err_codef(f->p, "sircc.adt.get.args_bad", "sircc: adt.get node %lld requires fields.args:[v]", (long long)node_id);
        goto done;
      }
      int64_t vid = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &vid)) {
        err_codef(f->p, "sircc.adt.get.arg_ref_bad", "sircc: adt.get node %lld arg must be node ref", (long long)node_id);
        goto done;
      }
      LLVMValueRef v = lower_expr(f, vid);
      if (!v) goto done;
      LLVMValueRef tag = LLVMBuildExtractValue(f->builder, v, 0, "tag");
      LLVMValueRef want = LLVMConstInt(LLVMInt32TypeInContext(f->ctx), (unsigned long long)variant, 0);
      LLVMValueRef neq = LLVMBuildICmp(f->builder, LLVMIntNE, tag, want, "tag.ne");
      if (!emit_trap_if(f, neq)) goto done;

      // Store to a temp to read payload bytes as the payload type.
      LLVMTypeRef sum_llvm = lower_type(f->p, f->ctx, sum_ty_id);
      if (!sum_llvm) goto done;
      LLVMValueRef slot = LLVMBuildAlloca(f->builder, sum_llvm, "sum.tmp");
      int64_t sum_sz = 0, sum_al = 0;
      if (type_size_align(f->p, sum_ty_id, &sum_sz, &sum_al) && sum_al > 0 && sum_al <= 4096) {
        LLVMSetAlignment(slot, (unsigned)sum_al);
      }
      LLVMBuildStore(f->builder, v, slot);

      // Determine payload field index based on computed payload_off.
      payload_size = 0;
      payload_align = 1;
      payload_off = 4;
      payload_field = 1;
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
      int64_t rem = payload_off % payload_align;
      if (rem) payload_off += (payload_align - rem);
      if (payload_off > 4) payload_field = 2;

      LLVMValueRef payp = LLVMBuildStructGEP2(f->builder, sum_llvm, slot, (unsigned)payload_field, "payloadp");
      LLVMTypeRef pay_ty = lower_type(f->p, f->ctx, pay_ty_id);
      if (!pay_ty) goto done;
      LLVMValueRef castp = LLVMBuildBitCast(f->builder, payp, LLVMPointerType(pay_ty, 0), "pay.castp");
      LLVMValueRef ld = LLVMBuildLoad2(f->builder, pay_ty, castp, "payload");
      int64_t psz = 0, pal = 0;
      if (type_size_align(f->p, pay_ty_id, &psz, &pal) && pal > 0 && pal <= 4096) {
        LLVMSetAlignment(ld, (unsigned)pal);
      }
      out = ld;
      goto done;
    }
  }

  if (strncmp(n->tag, "const.", 6) == 0) {
    const char* tyname = n->tag + 6;
    if (!n->fields) goto done;

    // Structured constants (agg:v1-ish, sircc-defined node encoding).
    if (strcmp(tyname, "zero") == 0) {
      if (n->type_ref == 0) {
        err_codef(f->p, "sircc.const.zero.missing_type_ref", "sircc: const.zero node %lld missing type_ref", (long long)node_id);
        goto done;
      }
      LLVMTypeRef ty = lower_type(f->p, f->ctx, n->type_ref);
      if (!ty) {
        err_codef(f->p, "sircc.const.zero.type_ref.bad",
                  "sircc: const.zero node %lld has invalid type_ref %lld", (long long)node_id, (long long)n->type_ref);
        goto done;
      }
      out = LLVMConstNull(ty);
      goto done;
    }

    if (strcmp(tyname, "array") == 0 || strcmp(tyname, "repeat") == 0) {
      if (n->type_ref == 0) {
        err_codef(f->p, "sircc.const.array.missing_type_ref",
                  "sircc: const.%s node %lld missing type_ref", tyname, (long long)node_id);
        goto done;
      }
      TypeRec* tr = get_type(f->p, n->type_ref);
      if (!tr || tr->kind != TYPE_ARRAY) {
        err_codef(f->p, "sircc.const.array.type_ref.bad",
                  "sircc: const.%s node %lld type_ref must be an array type", tyname, (long long)node_id);
        goto done;
      }
      LLVMTypeRef aty = lower_type(f->p, f->ctx, n->type_ref);
      LLVMTypeRef elty = lower_type(f->p, f->ctx, tr->of);
      if (!aty || !elty) {
        err_codef(f->p, "sircc.const.array.elem.bad",
                  "sircc: const.%s node %lld has invalid array element type", tyname, (long long)node_id);
        goto done;
      }

      if (strcmp(tyname, "array") == 0) {
        JsonValue* elems = json_obj_get(n->fields, "elems");
        if (!elems || elems->type != JSON_ARRAY) {
          err_codef(f->p, "sircc.const.array.elems.missing",
                    "sircc: const.array node %lld requires fields.elems array", (long long)node_id);
          goto done;
        }
        if ((int64_t)elems->v.arr.len != tr->len) {
          err_codef(f->p, "sircc.const.array.elems.len.mismatch",
                    "sircc: const.array node %lld element count mismatch: have %zu, want %lld", (long long)node_id,
                    elems->v.arr.len, (long long)tr->len);
          goto done;
        }
        LLVMValueRef* elts = NULL;
        if (elems->v.arr.len) {
          elts = (LLVMValueRef*)malloc(elems->v.arr.len * sizeof(LLVMValueRef));
          if (!elts) {
            bump_exit_code(f->p, SIRCC_EXIT_INTERNAL);
            err_codef(f->p, "sircc.oom", "sircc: out of memory");
            goto done;
          }
        }
        for (size_t i = 0; i < elems->v.arr.len; i++) {
          int64_t cid = 0;
          if (!parse_node_ref_id(f->p, elems->v.arr.items[i], &cid)) {
            err_codef(f->p, "sircc.const.array.elem.ref.bad",
                      "sircc: const.array node %lld elems[%zu] must be node refs", (long long)node_id, i);
            free(elts);
            goto done;
          }
          LLVMValueRef cv = lower_expr(f, cid);
          if (!cv) {
            free(elts);
            goto done;
          }
          if (!LLVMIsConstant(cv)) {
            err_codef(f->p, "sircc.const.array.elem.not_const",
                      "sircc: const.array node %lld elems[%zu] is not a constant", (long long)node_id, i);
            free(elts);
            goto done;
          }
          if (LLVMTypeOf(cv) != elty) {
            err_codef(f->p, "sircc.const.array.elem.type.bad",
                      "sircc: const.array node %lld elems[%zu] type mismatch", (long long)node_id, i);
            free(elts);
            goto done;
          }
          elts[i] = cv;
        }
        out = LLVMConstArray(elty, elts, (unsigned)elems->v.arr.len);
        free(elts);
        goto done;
      }

      // repeat
      JsonValue* countv = json_obj_get(n->fields, "count");
      int64_t count = 0;
      if (!must_i64(f->p, countv, &count, "const.repeat.count")) goto done;
      if (count != tr->len) {
        err_codef(f->p, "sircc.const.repeat.count.mismatch",
                  "sircc: const.repeat node %lld count mismatch: have %lld, want %lld", (long long)node_id, (long long)count,
                  (long long)tr->len);
        goto done;
      }
      JsonValue* elemv = json_obj_get(n->fields, "elem");
      int64_t eid = 0;
      if (!parse_node_ref_id(f->p, elemv, &eid)) {
        err_codef(f->p, "sircc.const.repeat.elem.ref.bad",
                  "sircc: const.repeat node %lld requires fields.elem node ref", (long long)node_id);
        goto done;
      }
      LLVMValueRef ev = lower_expr(f, eid);
      if (!ev) goto done;
      if (!LLVMIsConstant(ev) || LLVMTypeOf(ev) != elty) {
        err_codef(f->p, "sircc.const.repeat.elem.bad",
                  "sircc: const.repeat node %lld elem must be a constant of element type", (long long)node_id);
        goto done;
      }
      if (tr->len < 0 || tr->len > (int64_t)UINT_MAX) {
        err_codef(f->p, "sircc.const.repeat.len.bad",
                  "sircc: const.repeat node %lld invalid array length", (long long)node_id);
        goto done;
      }
      unsigned nrep = (unsigned)tr->len;
      LLVMValueRef* elts = NULL;
      if (nrep) {
        elts = (LLVMValueRef*)malloc(nrep * sizeof(LLVMValueRef));
        if (!elts) {
          bump_exit_code(f->p, SIRCC_EXIT_INTERNAL);
          err_codef(f->p, "sircc.oom", "sircc: out of memory");
          goto done;
        }
        for (unsigned i = 0; i < nrep; i++) elts[i] = ev;
      }
      out = LLVMConstArray(elty, elts, nrep);
      free(elts);
      goto done;
    }

    if (strcmp(tyname, "struct") == 0) {
      if (n->type_ref == 0) {
        err_codef(f->p, "sircc.const.struct.missing_type_ref",
                  "sircc: const.struct node %lld missing type_ref", (long long)node_id);
        goto done;
      }
      TypeRec* tr = get_type(f->p, n->type_ref);
      if (!tr || tr->kind != TYPE_STRUCT) {
        err_codef(f->p, "sircc.const.struct.type_ref.bad",
                  "sircc: const.struct node %lld type_ref must be a struct type", (long long)node_id);
        goto done;
      }
      LLVMTypeRef sty = lower_type(f->p, f->ctx, n->type_ref);
      if (!sty) {
        err_codef(f->p, "sircc.const.struct.type_ref.bad",
                  "sircc: const.struct node %lld has invalid type_ref %lld", (long long)node_id, (long long)n->type_ref);
        goto done;
      }

      size_t nfields = tr->field_len;
      LLVMValueRef* elts = NULL;
      if (nfields) {
        elts = (LLVMValueRef*)malloc(nfields * sizeof(LLVMValueRef));
        if (!elts) {
          bump_exit_code(f->p, SIRCC_EXIT_INTERNAL);
          err_codef(f->p, "sircc.oom", "sircc: out of memory");
          goto done;
        }
        for (size_t i = 0; i < nfields; i++) {
          LLVMTypeRef fty = lower_type(f->p, f->ctx, tr->fields[i].type_ref);
          if (!fty) {
            err_codef(f->p, "sircc.const.struct.field.type.bad",
                      "sircc: const.struct node %lld has invalid field type", (long long)node_id);
            free(elts);
            goto done;
          }
          elts[i] = LLVMConstNull(fty);
        }
      }

      JsonValue* fields = json_obj_get(n->fields, "fields");
      if (!fields || fields->type != JSON_ARRAY) {
        err_codef(f->p, "sircc.const.struct.fields.bad",
                  "sircc: const.struct node %lld requires fields.fields array", (long long)node_id);
        free(elts);
        goto done;
      }

      int64_t last_i = -1;
      for (size_t j = 0; j < fields->v.arr.len; j++) {
        JsonValue* fo = fields->v.arr.items[j];
        if (!fo || fo->type != JSON_OBJECT) {
          err_codef(f->p, "sircc.const.struct.fields.item.bad",
                    "sircc: const.struct node %lld fields[%zu] must be an object", (long long)node_id, j);
          free(elts);
          goto done;
        }
        int64_t i = 0;
        if (!must_i64(f->p, json_obj_get(fo, "i"), &i, "const.struct.fields[i].i")) {
          free(elts);
          goto done;
        }
        if (i < 0 || (size_t)i >= nfields) {
          err_codef(f->p, "sircc.const.struct.field.index.bad",
                    "sircc: const.struct node %lld field index %lld out of range", (long long)node_id, (long long)i);
          free(elts);
          goto done;
        }
        if (i <= last_i) {
          err_codef(f->p, "sircc.const.struct.field.order.bad",
                    "sircc: const.struct node %lld fields must be strictly increasing by i", (long long)node_id);
          free(elts);
          goto done;
        }
        last_i = i;
        int64_t vid = 0;
        if (!parse_node_ref_id(f->p, json_obj_get(fo, "v"), &vid)) {
          err_codef(f->p, "sircc.const.struct.field.value.ref.bad",
                    "sircc: const.struct node %lld fields[%zu].v must be a node ref", (long long)node_id, j);
          free(elts);
          goto done;
        }
        LLVMValueRef cv = lower_expr(f, vid);
        if (!cv) {
          free(elts);
          goto done;
        }
        if (!LLVMIsConstant(cv)) {
          err_codef(f->p, "sircc.const.struct.field.value.not_const",
                    "sircc: const.struct node %lld fields[%zu] value is not a constant", (long long)node_id, j);
          free(elts);
          goto done;
        }
        LLVMTypeRef fty = lower_type(f->p, f->ctx, tr->fields[(size_t)i].type_ref);
        if (!fty || LLVMTypeOf(cv) != fty) {
          err_codef(f->p, "sircc.const.struct.field.value.type.bad",
                    "sircc: const.struct node %lld fields[%zu] type mismatch", (long long)node_id, j);
          free(elts);
          goto done;
        }
        elts[(size_t)i] = cv;
      }

      out = LLVMConstNamedStruct(sty, elts, (unsigned)nfields);
      free(elts);
      goto done;
    }

    LLVMTypeRef ty = lower_type_prim(f->ctx, tyname);
    if (!ty) {
      err_codef(f->p, "sircc.const.type.unsupported", "sircc: unsupported const type '%s'", tyname);
      goto done;
    }
    if (LLVMGetTypeKind(ty) == LLVMIntegerTypeKind) {
      int64_t value = 0;
      if (!must_i64(f->p, json_obj_get(n->fields, "value"), &value, "const.value")) goto done;
      out = LLVMConstInt(ty, (unsigned long long)value, 1);
      goto done;
    }
    if (LLVMGetTypeKind(ty) == LLVMFloatTypeKind || LLVMGetTypeKind(ty) == LLVMDoubleTypeKind) {
      // Prefer exact bit-pattern constants: fields.bits = "0x..." (hex).
      const char* bits = json_get_string(json_obj_get(n->fields, "bits"));
      if (!bits || strncmp(bits, "0x", 2) != 0) {
        err_codef(f->p, "sircc.const.float.bits.bad", "sircc: const.%s requires fields.bits hex string (0x...)", tyname);
        goto done;
      }
      char* end = NULL;
      unsigned long long raw = strtoull(bits + 2, &end, 16);
      if (!end || *end != 0) {
        err_codef(f->p, "sircc.const.float.bits.bad", "sircc: const.%s invalid bits '%s'", tyname, bits);
        goto done;
      }
      if (LLVMGetTypeKind(ty) == LLVMFloatTypeKind) {
        LLVMValueRef ib = LLVMConstInt(LLVMInt32TypeInContext(f->ctx), raw & 0xFFFFFFFFu, 0);
        out = LLVMConstBitCast(ib, ty);
      } else {
        LLVMValueRef ib = LLVMConstInt(LLVMInt64TypeInContext(f->ctx), raw, 0);
        out = LLVMConstBitCast(ib, ty);
      }
      goto done;
    }
  }

  return false;

done:
  *outp = out;
  return true;
}
