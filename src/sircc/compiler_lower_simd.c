// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_lower_internal.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool is_vec_type(SirProgram* p, int64_t ty_id, TypeRec** out_vec, TypeRec** out_lane) {
  if (out_vec) *out_vec = NULL;
  if (out_lane) *out_lane = NULL;
  if (!p || ty_id == 0) return false;
  TypeRec* t = get_type(p, ty_id);
  if (!t || t->kind != TYPE_VEC || t->lane_ty == 0) return false;
  TypeRec* lane = get_type(p, t->lane_ty);
  if (!lane || lane->kind != TYPE_PRIM || !lane->prim) return false;
  if (out_vec) *out_vec = t;
  if (out_lane) *out_lane = lane;
  return true;
}

static bool lane_is_bool(TypeRec* lane) {
  if (!lane || lane->kind != TYPE_PRIM || !lane->prim) return false;
  return (strcmp(lane->prim, "bool") == 0 || strcmp(lane->prim, "i1") == 0);
}

static bool lane_is_float(TypeRec* lane) {
  if (!lane || lane->kind != TYPE_PRIM || !lane->prim) return false;
  return (strcmp(lane->prim, "f32") == 0 || strcmp(lane->prim, "f64") == 0);
}

static LLVMValueRef bool_to_i8(FunctionCtx* f, LLVMValueRef v) {
  if (!f || !v) return NULL;
  LLVMTypeRef vty = LLVMTypeOf(v);
  if (LLVMGetTypeKind(vty) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(vty) == 1) {
    return LLVMBuildZExt(f->builder, v, LLVMInt8TypeInContext(f->ctx), "b.i8");
  }
  if (LLVMGetTypeKind(vty) == LLVMIntegerTypeKind) {
    LLVMValueRef z = LLVMConstInt(vty, 0, 0);
    LLVMValueRef i1 = LLVMBuildICmp(f->builder, LLVMIntNE, v, z, "b.i1");
    return LLVMBuildZExt(f->builder, i1, LLVMInt8TypeInContext(f->ctx), "b.i8");
  }
  // If v isn't an int, let LLVM complain later via verifier; keep this path deterministic.
  return LLVMBuildTruncOrBitCast(f->builder, v, LLVMInt8TypeInContext(f->ctx), "b.i8");
}

static LLVMValueRef i8_to_bool(FunctionCtx* f, LLVMValueRef v) {
  if (!f || !v) return NULL;
  LLVMTypeRef i8 = LLVMInt8TypeInContext(f->ctx);
  if (LLVMTypeOf(v) != i8) v = LLVMBuildTruncOrBitCast(f->builder, v, i8, "b.tr");
  LLVMValueRef z = LLVMConstInt(i8, 0, 0);
  return LLVMBuildICmp(f->builder, LLVMIntNE, v, z, "b");
}

static bool emit_vec_idx_bounds_check(FunctionCtx* f, int64_t node_id, NodeRec* n, LLVMValueRef idx, int64_t lanes) {
  if (!f || !idx) return false;
  if (lanes <= 0 || lanes > INT_MAX) {
    if (n) LOWER_ERR_NODE(f, n, "sircc.vec.lanes.bad", "sircc: %s node %lld has invalid lane count", n->tag, (long long)node_id);
    else err_codef(f->p, "sircc.vec.lanes.bad", "sircc: vec op node %lld has invalid lane count", (long long)node_id);
    return false;
  }

  LLVMTypeRef i32 = LLVMInt32TypeInContext(f->ctx);
  if (LLVMTypeOf(idx) != i32) {
    if (LLVMGetTypeKind(LLVMTypeOf(idx)) != LLVMIntegerTypeKind) {
      if (n) LOWER_ERR_NODE(f, n, "sircc.vec.idx.type_bad", "sircc: %s node %lld idx must be i32", n->tag, (long long)node_id);
      else err_codef(f->p, "sircc.vec.idx.type_bad", "sircc: vec op node %lld idx must be i32", (long long)node_id);
      return false;
    }
    idx = LLVMBuildTruncOrBitCast(f->builder, idx, i32, "idx.i32");
  }

  LLVMValueRef zero = LLVMConstInt(i32, 0, 0);
  LLVMValueRef max = LLVMConstInt(i32, (unsigned long long)lanes, 0);

  LLVMValueRef neg = LLVMBuildICmp(f->builder, LLVMIntSLT, idx, zero, "idx.neg");
  LLVMValueRef oob = LLVMBuildICmp(f->builder, LLVMIntSGE, idx, max, "idx.oob");
  LLVMValueRef bad = LLVMBuildOr(f->builder, neg, oob, "idx.bad");
  return emit_trap_if(f, bad);
}

static LLVMValueRef canonicalize_float_vec(FunctionCtx* f, LLVMValueRef v, TypeRec* vec_ty, TypeRec* lane_ty) {
  if (!f || !v || !vec_ty || !lane_ty) return NULL;
  if (!lane_is_float(lane_ty)) return v;
  if (vec_ty->lanes <= 0 || vec_ty->lanes > INT_MAX) return NULL;

  LLVMTypeRef lane_llvm = lower_type_prim(f->ctx, lane_ty->prim);
  if (!lane_llvm) return NULL;

  // For f32/f64, canonicalize lane-wise by extract/canon/insert.
  LLVMValueRef out = v;
  for (int i = 0; i < (int)vec_ty->lanes; i++) {
    LLVMValueRef idx = LLVMConstInt(LLVMInt32TypeInContext(f->ctx), (unsigned long long)i, 0);
    LLVMValueRef lane = LLVMBuildExtractElement(f->builder, out, idx, "lane");
    if (LLVMTypeOf(lane) != lane_llvm) lane = LLVMBuildBitCast(f->builder, lane, lane_llvm, "lane.cast");
    lane = canonicalize_float(f, lane);
    out = LLVMBuildInsertElement(f->builder, out, lane, idx, "lane.set");
  }
  return out;
}

static LLVMValueRef bool_vec_normalize(FunctionCtx* f, LLVMValueRef v, int lanes) {
  if (!f || !v || lanes <= 0 || lanes > INT_MAX) return NULL;
  LLVMTypeRef i8 = LLVMInt8TypeInContext(f->ctx);
  LLVMTypeRef vec_i8 = LLVMVectorType(i8, (unsigned)lanes);
  if (LLVMTypeOf(v) != vec_i8) v = LLVMBuildTruncOrBitCast(f->builder, v, vec_i8, "bvec.cast");
  LLVMValueRef z = LLVMConstNull(vec_i8);
  LLVMValueRef i1v = LLVMBuildICmp(f->builder, LLVMIntNE, v, z, "bvec.i1");
  return LLVMBuildZExt(f->builder, i1v, vec_i8, "bvec");
}

static LLVMValueRef bool_vec_from_i1(FunctionCtx* f, LLVMValueRef i1v, int lanes) {
  if (!f || !i1v || lanes <= 0 || lanes > INT_MAX) return NULL;
  LLVMTypeRef vec_i8 = LLVMVectorType(LLVMInt8TypeInContext(f->ctx), (unsigned)lanes);
  return LLVMBuildZExt(f->builder, i1v, vec_i8, "bvec");
}

static int64_t find_bool_vec_type_id(SirProgram* p, int64_t lanes) {
  if (!p || lanes <= 0) return 0;
  for (size_t i = 0; i < p->types_cap; i++) {
    TypeRec* t = p->types[i];
    if (!t || t->kind != TYPE_VEC) continue;
    if (t->lanes != lanes) continue;
    TypeRec* lane = get_type(p, t->lane_ty);
    if (!lane || lane->kind != TYPE_PRIM || !lane->prim) continue;
    if (strcmp(lane->prim, "bool") == 0 || strcmp(lane->prim, "i1") == 0) return (int64_t)i;
  }
  return 0;
}

bool lower_expr_simd(FunctionCtx* f, int64_t node_id, NodeRec* n, LLVMValueRef* outp) {
  if (!f || !n || !outp) return false;

  if (strcmp(n->tag, "vec.shuffle") == 0) {
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.vec.shuffle.missing_fields", "sircc: vec.shuffle node %lld missing fields", (long long)node_id);
      return false;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) {
      LOWER_ERR_NODE(f, n, "sircc.vec.shuffle.args.bad", "sircc: vec.shuffle node %lld requires args:[a, b]", (long long)node_id);
      return false;
    }
    JsonValue* flags = json_obj_get(n->fields, "flags");
    if (!flags || flags->type != JSON_OBJECT) {
      LOWER_ERR_NODE(f, n, "sircc.vec.shuffle.flags.bad", "sircc: vec.shuffle node %lld requires fields.flags object", (long long)node_id);
      return false;
    }
    JsonValue* idxs = json_obj_get(flags, "idx");
    if (!idxs || idxs->type != JSON_ARRAY) {
      LOWER_ERR_NODE(f, n, "sircc.vec.shuffle.idx.bad", "sircc: vec.shuffle node %lld requires flags.idx array", (long long)node_id);
      return false;
    }

    int64_t aid = 0, bid = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[0], &aid) || !parse_node_ref_id(f->p, args->v.arr.items[1], &bid)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.shuffle.args.ref_bad", "sircc: vec.shuffle node %lld args must be node refs", (long long)node_id);
      return false;
    }
    NodeRec* an = get_node(f->p, aid);
    NodeRec* bn = get_node(f->p, bid);
    int64_t vec_ty_id = n->type_ref ? n->type_ref : (an ? an->type_ref : 0);
    if (!vec_ty_id || !an || !bn || an->type_ref == 0 || bn->type_ref == 0) {
      LOWER_ERR_NODE(f, n, "sircc.vec.shuffle.type.missing", "sircc: vec.shuffle node %lld requires vec type_ref", (long long)node_id);
      return false;
    }
    if (an->type_ref != vec_ty_id || bn->type_ref != vec_ty_id) {
      LOWER_ERR_NODE(f, n, "sircc.vec.shuffle.type.mismatch", "sircc: vec.shuffle node %lld requires a,b to have the same vec type", (long long)node_id);
      return false;
    }

    TypeRec* vec = NULL;
    TypeRec* lane = NULL;
    if (!is_vec_type(f->p, vec_ty_id, &vec, &lane)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.shuffle.type.bad", "sircc: vec.shuffle node %lld type_ref must be a vec type", (long long)node_id);
      return false;
    }
    if (idxs->v.arr.len != (size_t)vec->lanes) {
      LOWER_ERR_NODE(f, n, "sircc.vec.shuffle.idx.len_bad", "sircc: vec.shuffle node %lld flags.idx length must equal lanes", (long long)node_id);
      return false;
    }

    // Validate indices; out-of-range is a deterministic trap.
    int64_t max = vec->lanes * 2;
    bool any_oob = false;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(f->ctx);
    LLVMValueRef* mask_elts = (LLVMValueRef*)malloc((size_t)vec->lanes * sizeof(LLVMValueRef));
    if (!mask_elts) return false;
    for (int i = 0; i < (int)vec->lanes; i++) {
      int64_t idx = 0;
      if (!json_get_i64(idxs->v.arr.items[i], &idx)) {
        free(mask_elts);
        LOWER_ERR_NODE(f, n, "sircc.vec.shuffle.idx.elem_bad", "sircc: vec.shuffle node %lld flags.idx[%d] must be an integer", (long long)node_id, i);
        return false;
      }
      if (idx < 0 || idx >= max) any_oob = true;
      if (idx < 0) idx = 0;
      if (idx >= max) idx = max - 1;
      mask_elts[i] = LLVMConstInt(i32, (unsigned long long)idx, 0);
    }
    LLVMValueRef mask = LLVMConstVector(mask_elts, (unsigned)vec->lanes);
    free(mask_elts);

    if (any_oob) {
      LLVMValueRef one = LLVMConstInt(LLVMInt1TypeInContext(f->ctx), 1, 0);
      if (!emit_trap_if(f, one)) return false;
    }

    LLVMValueRef a = lower_expr(f, aid);
    LLVMValueRef b = lower_expr(f, bid);
    if (!a || !b) return false;

    LLVMTypeRef vec_llvm = lower_type(f->p, f->ctx, vec_ty_id);
    if (!vec_llvm) return false;
    if (LLVMTypeOf(a) != vec_llvm) a = LLVMBuildBitCast(f->builder, a, vec_llvm, "a.cast");
    if (LLVMTypeOf(b) != vec_llvm) b = LLVMBuildBitCast(f->builder, b, vec_llvm, "b.cast");

    LLVMValueRef out = LLVMBuildShuffleVector(f->builder, a, b, mask, "shuf");
    out = canonicalize_float_vec(f, out, vec, lane);
    *outp = out;
    return true;
  }

  if (strcmp(n->tag, "vec.splat") == 0) {
    if (n->type_ref == 0) {
      LOWER_ERR_NODE(f, n, "sircc.vec.splat.missing_type", "sircc: vec.splat node %lld missing type_ref (vec type)", (long long)node_id);
      return false;
    }
    TypeRec* vec = NULL;
    TypeRec* lane = NULL;
    if (!is_vec_type(f->p, n->type_ref, &vec, &lane)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.splat.type.bad", "sircc: vec.splat node %lld type_ref must be a vec type", (long long)node_id);
      return false;
    }
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.vec.splat.missing_fields", "sircc: vec.splat node %lld missing fields", (long long)node_id);
      return false;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
      LOWER_ERR_NODE(f, n, "sircc.vec.splat.args.bad", "sircc: vec.splat node %lld requires args:[x]", (long long)node_id);
      return false;
    }
    int64_t xid = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[0], &xid)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.splat.args.ref_bad", "sircc: vec.splat node %lld args[0] must be a node ref", (long long)node_id);
      return false;
    }
    LLVMValueRef x = lower_expr(f, xid);
    if (!x) return false;

    LLVMTypeRef vec_llvm = lower_type(f->p, f->ctx, n->type_ref);
    if (!vec_llvm || LLVMGetTypeKind(vec_llvm) != LLVMVectorTypeKind) {
      LOWER_ERR_NODE(f, n, "sircc.vec.splat.llvm_type.bad", "sircc: vec.splat node %lld has non-vector LLVM type", (long long)node_id);
      return false;
    }

    LLVMTypeRef lane_llvm = lower_type_prim(f->ctx, lane->prim);
    if (!lane_llvm) {
      LOWER_ERR_NODE(f, n, "sircc.vec.lane.unsupported", "sircc: vec.splat lane type unsupported");
      return false;
    }

    LLVMValueRef lane_v = x;
    if (lane_is_bool(lane)) {
      lane_v = bool_to_i8(f, x);
      lane_llvm = LLVMInt8TypeInContext(f->ctx);
    } else {
      if (LLVMTypeOf(lane_v) != lane_llvm) lane_v = LLVMBuildTruncOrBitCast(f->builder, lane_v, lane_llvm, "lane.cast");
      if (LLVMGetTypeKind(lane_llvm) == LLVMFloatTypeKind || LLVMGetTypeKind(lane_llvm) == LLVMDoubleTypeKind) {
        lane_v = canonicalize_float(f, lane_v);
      }
    }

    LLVMValueRef out = LLVMGetUndef(vec_llvm);
    for (int i = 0; i < (int)vec->lanes; i++) {
      LLVMValueRef idx = LLVMConstInt(LLVMInt32TypeInContext(f->ctx), (unsigned long long)i, 0);
      out = LLVMBuildInsertElement(f->builder, out, lane_v, idx, "splat");
    }
    *outp = out;
    return true;
  }

  if (strcmp(n->tag, "vec.add") == 0 || strcmp(n->tag, "vec.sub") == 0 || strcmp(n->tag, "vec.mul") == 0 ||
      strcmp(n->tag, "vec.and") == 0 || strcmp(n->tag, "vec.or") == 0 || strcmp(n->tag, "vec.xor") == 0 ||
      strcmp(n->tag, "vec.not") == 0 || strncmp(n->tag, "vec.cmp.", 8) == 0 || strcmp(n->tag, "vec.select") == 0) {
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.vec.op.missing_fields", "sircc: %s node %lld missing fields", n->tag, (long long)node_id);
      return false;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY) {
      LOWER_ERR_NODE(f, n, "sircc.vec.op.args.bad", "sircc: %s node %lld requires fields.args array", n->tag, (long long)node_id);
      return false;
    }

    int64_t vec_ty_id = n->type_ref;
    TypeRec* vec = NULL;
    TypeRec* lane = NULL;

    // For cmp/select we can infer dst types from operands when type_ref is omitted.
    if (strncmp(n->tag, "vec.cmp.", 8) == 0) {
      if (args->v.arr.len != 2) {
        LOWER_ERR_NODE(f, n, "sircc.vec.cmp.args.bad", "sircc: %s node %lld requires args:[a,b]", n->tag, (long long)node_id);
        return false;
      }
      int64_t aid = 0, bid = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &aid) || !parse_node_ref_id(f->p, args->v.arr.items[1], &bid)) {
        LOWER_ERR_NODE(f, n, "sircc.vec.cmp.args.ref_bad", "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
        return false;
      }
      NodeRec* an = get_node(f->p, aid);
      NodeRec* bn = get_node(f->p, bid);
      if (!an || !bn || an->type_ref == 0 || bn->type_ref == 0 || an->type_ref != bn->type_ref) {
        LOWER_ERR_NODE(f, n, "sircc.vec.cmp.type.bad", "sircc: %s node %lld requires a,b with same vec type_ref", n->tag, (long long)node_id);
        return false;
      }
      TypeRec* src_vec = NULL;
      TypeRec* src_lane = NULL;
      if (!is_vec_type(f->p, an->type_ref, &src_vec, &src_lane)) {
        LOWER_ERR_NODE(f, n, "sircc.vec.cmp.src.bad", "sircc: %s node %lld requires vec operands", n->tag, (long long)node_id);
        return false;
      }
      if (vec_ty_id == 0) {
        vec_ty_id = find_bool_vec_type_id(f->p, src_vec->lanes);
        if (vec_ty_id == 0) {
          LOWER_ERR_NODE(f, n, "sircc.vec.cmp.bool_ty_missing",
                         "sircc: %s node %lld requires a vec(bool,%lld) type definition to exist in the stream", n->tag, (long long)node_id,
                         (long long)src_vec->lanes);
          return false;
        }
      }
      if (!is_vec_type(f->p, vec_ty_id, &vec, &lane) || !lane_is_bool(lane)) {
        LOWER_ERR_NODE(f, n, "sircc.vec.cmp.dst.bad", "sircc: %s node %lld type_ref must be vec(bool,lanes)", n->tag, (long long)node_id);
        return false;
      }

      LLVMValueRef a = lower_expr(f, aid);
      LLVMValueRef b = lower_expr(f, bid);
      if (!a || !b) return false;
      LLVMTypeRef src_llvm = lower_type(f->p, f->ctx, an->type_ref);
      if (!src_llvm) return false;
      if (LLVMTypeOf(a) != src_llvm) a = LLVMBuildBitCast(f->builder, a, src_llvm, "a.cast");
      if (LLVMTypeOf(b) != src_llvm) b = LLVMBuildBitCast(f->builder, b, src_llvm, "b.cast");

      LLVMValueRef cmp = NULL;
      const char* cc = n->tag + 8;
      if (lane_is_float(src_lane)) {
        LLVMRealPredicate pred = LLVMRealOEQ;
        if (strcmp(cc, "eq") == 0) pred = LLVMRealOEQ;
        else if (strcmp(cc, "ne") == 0) pred = LLVMRealONE;
        else if (strcmp(cc, "lt") == 0) pred = LLVMRealOLT;
        else if (strcmp(cc, "le") == 0) pred = LLVMRealOLE;
        else if (strcmp(cc, "gt") == 0) pred = LLVMRealOGT;
        else if (strcmp(cc, "ge") == 0) pred = LLVMRealOGE;
        else {
          LOWER_ERR_NODE(f, n, "sircc.vec.cmp.cc.bad", "sircc: unsupported vec.cmp predicate '%s'", cc);
          return false;
        }
        cmp = LLVMBuildFCmp(f->builder, pred, a, b, "vcmp");
      } else if (lane_is_bool(src_lane)) {
        if (strcmp(cc, "eq") != 0 && strcmp(cc, "ne") != 0) {
          LOWER_ERR_NODE(f, n, "sircc.vec.cmp.bool.cc.bad", "sircc: vec.cmp.%s not supported for bool lanes (only eq/ne)", cc);
          return false;
        }
        LLVMValueRef na = bool_vec_normalize(f, a, (int)src_vec->lanes);
        LLVMValueRef nb = bool_vec_normalize(f, b, (int)src_vec->lanes);
        if (!na || !nb) return false;
        cmp = LLVMBuildICmp(f->builder, (strcmp(cc, "eq") == 0) ? LLVMIntEQ : LLVMIntNE, na, nb, "vcmp");
      } else {
        LLVMIntPredicate pred = LLVMIntEQ;
        if (strcmp(cc, "eq") == 0) pred = LLVMIntEQ;
        else if (strcmp(cc, "ne") == 0) pred = LLVMIntNE;
        else if (strcmp(cc, "lt") == 0) pred = LLVMIntSLT;
        else if (strcmp(cc, "le") == 0) pred = LLVMIntSLE;
        else if (strcmp(cc, "gt") == 0) pred = LLVMIntSGT;
        else if (strcmp(cc, "ge") == 0) pred = LLVMIntSGE;
        else {
          LOWER_ERR_NODE(f, n, "sircc.vec.cmp.cc.bad", "sircc: unsupported vec.cmp predicate '%s'", cc);
          return false;
        }
        cmp = LLVMBuildICmp(f->builder, pred, a, b, "vcmp");
      }

      LLVMValueRef out = bool_vec_from_i1(f, cmp, (int)vec->lanes);
      if (!out) return false;
      *outp = out;
      return true;
    }

    if (strcmp(n->tag, "vec.select") == 0) {
      if (args->v.arr.len != 3) {
        LOWER_ERR_NODE(f, n, "sircc.vec.select.args.bad", "sircc: vec.select node %lld requires args:[mask,a,b]", (long long)node_id);
        return false;
      }
      int64_t mid = 0, aid = 0, bid = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &mid) || !parse_node_ref_id(f->p, args->v.arr.items[1], &aid) ||
          !parse_node_ref_id(f->p, args->v.arr.items[2], &bid)) {
        LOWER_ERR_NODE(f, n, "sircc.vec.select.args.ref_bad", "sircc: vec.select node %lld args must be node refs", (long long)node_id);
        return false;
      }
      NodeRec* mn = get_node(f->p, mid);
      NodeRec* an = get_node(f->p, aid);
      NodeRec* bn = get_node(f->p, bid);
      if (!mn || !an || !bn || mn->type_ref == 0 || an->type_ref == 0 || bn->type_ref == 0) {
        LOWER_ERR_NODE(f, n, "sircc.vec.select.type.missing", "sircc: vec.select node %lld requires operand type_refs", (long long)node_id);
        return false;
      }
      if (an->type_ref != bn->type_ref) {
        LOWER_ERR_NODE(f, n, "sircc.vec.select.type.mismatch", "sircc: vec.select node %lld requires a and b to share type_ref", (long long)node_id);
        return false;
      }
      TypeRec* src_vec = NULL;
      TypeRec* src_lane = NULL;
      if (!is_vec_type(f->p, an->type_ref, &src_vec, &src_lane)) {
        LOWER_ERR_NODE(f, n, "sircc.vec.select.ab.bad", "sircc: vec.select node %lld requires vec a/b operands", (long long)node_id);
        return false;
      }
      TypeRec* mask_vec = NULL;
      TypeRec* mask_lane = NULL;
      if (!is_vec_type(f->p, mn->type_ref, &mask_vec, &mask_lane) || !lane_is_bool(mask_lane) || mask_vec->lanes != src_vec->lanes) {
        LOWER_ERR_NODE(f, n, "sircc.vec.select.mask.bad", "sircc: vec.select node %lld mask must be vec(bool,lanes)", (long long)node_id);
        return false;
      }

      int64_t dst_ty_id = vec_ty_id ? vec_ty_id : an->type_ref;
      if (dst_ty_id != an->type_ref) {
        LOWER_ERR_NODE(f, n, "sircc.vec.select.dst.bad", "sircc: vec.select node %lld type_ref must match a/b vec type", (long long)node_id);
        return false;
      }
      if (!is_vec_type(f->p, dst_ty_id, &vec, &lane)) return false;

      LLVMValueRef m = lower_expr(f, mid);
      LLVMValueRef a = lower_expr(f, aid);
      LLVMValueRef b = lower_expr(f, bid);
      if (!m || !a || !b) return false;

      LLVMTypeRef mask_llvm = lower_type(f->p, f->ctx, mn->type_ref);
      LLVMTypeRef src_llvm = lower_type(f->p, f->ctx, dst_ty_id);
      if (!mask_llvm || !src_llvm) return false;
      if (LLVMTypeOf(m) != mask_llvm) m = LLVMBuildBitCast(f->builder, m, mask_llvm, "m.cast");
      if (LLVMTypeOf(a) != src_llvm) a = LLVMBuildBitCast(f->builder, a, src_llvm, "a.cast");
      if (LLVMTypeOf(b) != src_llvm) b = LLVMBuildBitCast(f->builder, b, src_llvm, "b.cast");

      LLVMValueRef mnz = LLVMBuildICmp(f->builder, LLVMIntNE, m, LLVMConstNull(mask_llvm), "m.nz");
      LLVMValueRef out = LLVMBuildSelect(f->builder, mnz, a, b, "vsel");
      out = canonicalize_float_vec(f, out, vec, lane);
      if (lane_is_bool(lane)) out = bool_vec_normalize(f, out, (int)vec->lanes);
      *outp = out;
      return true;
    }

    // Unary op: vec.not
    if (strcmp(n->tag, "vec.not") == 0) {
      if (args->v.arr.len != 1) {
        LOWER_ERR_NODE(f, n, "sircc.vec.not.args.bad", "sircc: vec.not node %lld requires args:[v]", (long long)node_id);
        return false;
      }
      int64_t vid = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &vid)) {
        LOWER_ERR_NODE(f, n, "sircc.vec.not.args.ref_bad", "sircc: vec.not node %lld args must be node refs", (long long)node_id);
        return false;
      }
      NodeRec* vn = get_node(f->p, vid);
      int64_t src_ty_id = vn ? vn->type_ref : 0;
      if (vec_ty_id == 0) vec_ty_id = src_ty_id;
      if (!src_ty_id || vec_ty_id != src_ty_id) {
        LOWER_ERR_NODE(f, n, "sircc.vec.not.type.bad", "sircc: vec.not node %lld requires type_ref matching operand vec type", (long long)node_id);
        return false;
      }
      if (!is_vec_type(f->p, vec_ty_id, &vec, &lane)) return false;
      if (lane_is_float(lane)) {
        LOWER_ERR_NODE(f, n, "sircc.vec.not.lane.bad", "sircc: vec.not lane type must be integer or bool");
        return false;
      }

      LLVMValueRef v = lower_expr(f, vid);
      if (!v) return false;
      LLVMTypeRef vec_llvm = lower_type(f->p, f->ctx, vec_ty_id);
      if (!vec_llvm) return false;
      if (LLVMTypeOf(v) != vec_llvm) v = LLVMBuildBitCast(f->builder, v, vec_llvm, "v.cast");

      LLVMValueRef out = NULL;
      if (lane_is_bool(lane)) {
        LLVMValueRef nz = LLVMBuildICmp(f->builder, LLVMIntNE, v, LLVMConstNull(vec_llvm), "b.nz");
        out = bool_vec_from_i1(f, LLVMBuildNot(f->builder, nz, "b.not"), (int)vec->lanes);
      } else {
        out = LLVMBuildNot(f->builder, v, "vnot");
      }
      if (lane_is_bool(lane)) out = bool_vec_normalize(f, out, (int)vec->lanes);
      *outp = out;
      return true;
    }

    // Binary arithmetic/logic ops.
    bool is_arith = (strcmp(n->tag, "vec.add") == 0 || strcmp(n->tag, "vec.sub") == 0 || strcmp(n->tag, "vec.mul") == 0);
    bool is_logic = (strcmp(n->tag, "vec.and") == 0 || strcmp(n->tag, "vec.or") == 0 || strcmp(n->tag, "vec.xor") == 0);
    if (args->v.arr.len != 2) {
      LOWER_ERR_NODE(f, n, "sircc.vec.bin.args.bad", "sircc: %s node %lld requires args:[a,b]", n->tag, (long long)node_id);
      return false;
    }
    int64_t aid = 0, bid = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[0], &aid) || !parse_node_ref_id(f->p, args->v.arr.items[1], &bid)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.bin.args.ref_bad", "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
      return false;
    }
    NodeRec* an = get_node(f->p, aid);
    NodeRec* bn = get_node(f->p, bid);
    int64_t src_ty_id = (an && bn && an->type_ref == bn->type_ref) ? an->type_ref : 0;
    if (vec_ty_id == 0) vec_ty_id = src_ty_id;
    if (!vec_ty_id || vec_ty_id != src_ty_id) {
      LOWER_ERR_NODE(f, n, "sircc.vec.bin.type.bad", "sircc: %s node %lld requires type_ref matching operand vec types", n->tag, (long long)node_id);
      return false;
    }
    if (!is_vec_type(f->p, vec_ty_id, &vec, &lane)) return false;

    if (is_arith && lane_is_bool(lane)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.arith.lane.bad", "sircc: %s lane type must be integer or float (not bool)", n->tag);
      return false;
    }
    if (is_logic && lane_is_float(lane)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.logic.lane.bad", "sircc: %s lane type must be integer or bool (not float)", n->tag);
      return false;
    }

    LLVMValueRef a = lower_expr(f, aid);
    LLVMValueRef b = lower_expr(f, bid);
    if (!a || !b) return false;
    LLVMTypeRef vec_llvm = lower_type(f->p, f->ctx, vec_ty_id);
    if (!vec_llvm) return false;
    if (LLVMTypeOf(a) != vec_llvm) a = LLVMBuildBitCast(f->builder, a, vec_llvm, "a.cast");
    if (LLVMTypeOf(b) != vec_llvm) b = LLVMBuildBitCast(f->builder, b, vec_llvm, "b.cast");

    LLVMValueRef out = NULL;
    if (lane_is_float(lane)) {
      if (strcmp(n->tag, "vec.add") == 0) out = LLVMBuildFAdd(f->builder, a, b, "vadd");
      else if (strcmp(n->tag, "vec.sub") == 0) out = LLVMBuildFSub(f->builder, a, b, "vsub");
      else if (strcmp(n->tag, "vec.mul") == 0) out = LLVMBuildFMul(f->builder, a, b, "vmul");
      else {
        LOWER_ERR_NODE(f, n, "sircc.vec.op.bad", "sircc: unsupported float vec op '%s'", n->tag);
        return false;
      }
      out = canonicalize_float_vec(f, out, vec, lane);
    } else {
      if (strcmp(n->tag, "vec.add") == 0) out = LLVMBuildAdd(f->builder, a, b, "vadd");
      else if (strcmp(n->tag, "vec.sub") == 0) out = LLVMBuildSub(f->builder, a, b, "vsub");
      else if (strcmp(n->tag, "vec.mul") == 0) out = LLVMBuildMul(f->builder, a, b, "vmul");
      else if (strcmp(n->tag, "vec.and") == 0) out = LLVMBuildAnd(f->builder, a, b, "vand");
      else if (strcmp(n->tag, "vec.or") == 0) out = LLVMBuildOr(f->builder, a, b, "vor");
      else if (strcmp(n->tag, "vec.xor") == 0) out = LLVMBuildXor(f->builder, a, b, "vxor");
      else {
        LOWER_ERR_NODE(f, n, "sircc.vec.op.bad", "sircc: unsupported int/bool vec op '%s'", n->tag);
        return false;
      }
      if (lane_is_bool(lane)) out = bool_vec_normalize(f, out, (int)vec->lanes);
    }

    *outp = out;
    return true;
  }

  if (strcmp(n->tag, "vec.extract") == 0) {
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.vec.extract.missing_fields", "sircc: vec.extract node %lld missing fields", (long long)node_id);
      return false;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) {
      LOWER_ERR_NODE(f, n, "sircc.vec.extract.args.bad", "sircc: vec.extract node %lld requires args:[v, idx]", (long long)node_id);
      return false;
    }
    int64_t vid = 0, idxid = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[0], &vid) || !parse_node_ref_id(f->p, args->v.arr.items[1], &idxid)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.extract.args.ref_bad", "sircc: vec.extract node %lld args must be node refs", (long long)node_id);
      return false;
    }
    NodeRec* vn = get_node(f->p, vid);
    if (!vn || vn->type_ref == 0) {
      LOWER_ERR_NODE(f, n, "sircc.vec.extract.v.missing_type", "sircc: vec.extract node %lld v must have a vec type_ref", (long long)node_id);
      return false;
    }
    TypeRec* vec = NULL;
    TypeRec* lane = NULL;
    if (!is_vec_type(f->p, vn->type_ref, &vec, &lane)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.extract.v.type.bad", "sircc: vec.extract node %lld v must be a vec", (long long)node_id);
      return false;
    }
    LLVMValueRef v = lower_expr(f, vid);
    LLVMValueRef idx = lower_expr(f, idxid);
    if (!v || !idx) return false;

    if (!emit_vec_idx_bounds_check(f, node_id, n, idx, vec->lanes)) return false;

    LLVMValueRef lane_idx = idx;
    if (LLVMTypeOf(lane_idx) != LLVMInt32TypeInContext(f->ctx)) {
      lane_idx = LLVMBuildTruncOrBitCast(f->builder, lane_idx, LLVMInt32TypeInContext(f->ctx), "idx.i32");
    }
    LLVMValueRef el = LLVMBuildExtractElement(f->builder, v, lane_idx, "extract");
    if (lane_is_bool(lane)) {
      *outp = i8_to_bool(f, el);
    } else {
      LLVMTypeRef want = lower_type_prim(f->ctx, lane->prim);
      if (!want) {
        LOWER_ERR_NODE(f, n, "sircc.vec.lane.unsupported", "sircc: vec.extract lane type unsupported");
        return false;
      }
      if (LLVMTypeOf(el) != want) el = LLVMBuildBitCast(f->builder, el, want, "lane.cast");
      if (LLVMGetTypeKind(want) == LLVMFloatTypeKind || LLVMGetTypeKind(want) == LLVMDoubleTypeKind) el = canonicalize_float(f, el);
      *outp = el;
    }
    return true;
  }

  if (strcmp(n->tag, "vec.replace") == 0) {
    if (n->type_ref == 0) {
      LOWER_ERR_NODE(f, n, "sircc.vec.replace.missing_type", "sircc: vec.replace node %lld missing type_ref (vec type)", (long long)node_id);
      return false;
    }
    TypeRec* vec = NULL;
    TypeRec* lane = NULL;
    if (!is_vec_type(f->p, n->type_ref, &vec, &lane)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.replace.type.bad", "sircc: vec.replace node %lld type_ref must be a vec type", (long long)node_id);
      return false;
    }
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.vec.replace.missing_fields", "sircc: vec.replace node %lld missing fields", (long long)node_id);
      return false;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) {
      LOWER_ERR_NODE(f, n, "sircc.vec.replace.args.bad", "sircc: vec.replace node %lld requires args:[v, idx, x]", (long long)node_id);
      return false;
    }
    int64_t vid = 0, idxid = 0, xid = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[0], &vid) || !parse_node_ref_id(f->p, args->v.arr.items[1], &idxid) ||
        !parse_node_ref_id(f->p, args->v.arr.items[2], &xid)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.replace.args.ref_bad", "sircc: vec.replace node %lld args must be node refs", (long long)node_id);
      return false;
    }
    LLVMValueRef v = lower_expr(f, vid);
    LLVMValueRef idx = lower_expr(f, idxid);
    LLVMValueRef x = lower_expr(f, xid);
    if (!v || !idx || !x) return false;

    if (!emit_vec_idx_bounds_check(f, node_id, n, idx, vec->lanes)) return false;

    LLVMValueRef lane_idx = idx;
    if (LLVMTypeOf(lane_idx) != LLVMInt32TypeInContext(f->ctx)) {
      lane_idx = LLVMBuildTruncOrBitCast(f->builder, lane_idx, LLVMInt32TypeInContext(f->ctx), "idx.i32");
    }

    LLVMTypeRef want_lane = lower_type_prim(f->ctx, lane->prim);
    if (!want_lane) {
      LOWER_ERR_NODE(f, n, "sircc.vec.lane.unsupported", "sircc: vec.replace lane type unsupported");
      return false;
    }

    LLVMValueRef lane_x = x;
    if (lane_is_bool(lane)) {
      lane_x = bool_to_i8(f, x);
    } else {
      if (LLVMTypeOf(lane_x) != want_lane) lane_x = LLVMBuildTruncOrBitCast(f->builder, lane_x, want_lane, "lane.cast");
      if (LLVMGetTypeKind(want_lane) == LLVMFloatTypeKind || LLVMGetTypeKind(want_lane) == LLVMDoubleTypeKind) lane_x = canonicalize_float(f, lane_x);
    }

    LLVMValueRef out = LLVMBuildInsertElement(f->builder, v, lane_x, lane_idx, "replace");
    if (lane_is_float(lane)) out = canonicalize_float_vec(f, out, vec, lane);
    *outp = out;
    return true;
  }

  if (strcmp(n->tag, "load.vec") == 0) {
    if (n->type_ref == 0) {
      LOWER_ERR_NODE(f, n, "sircc.load.vec.missing_type", "sircc: load.vec node %lld missing type_ref (vec type)", (long long)node_id);
      return false;
    }
    TypeRec* vec = NULL;
    TypeRec* lane = NULL;
    if (!is_vec_type(f->p, n->type_ref, &vec, &lane)) {
      LOWER_ERR_NODE(f, n, "sircc.load.vec.type.bad", "sircc: load.vec node %lld type_ref must be a vec type", (long long)node_id);
      return false;
    }
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.load.vec.missing_fields", "sircc: load.vec node %lld missing fields", (long long)node_id);
      return false;
    }
    int64_t aid = 0;
    if (!parse_node_ref_id(f->p, json_obj_get(n->fields, "addr"), &aid)) {
      LOWER_ERR_NODE(f, n, "sircc.load.vec.addr.ref_bad", "sircc: load.vec node %lld missing fields.addr ref", (long long)node_id);
      return false;
    }
    LLVMValueRef pval = lower_expr(f, aid);
    if (!pval) return false;
    LLVMTypeRef pty = LLVMTypeOf(pval);
    if (LLVMGetTypeKind(pty) != LLVMPointerTypeKind) {
      LOWER_ERR_NODE(f, n, "sircc.load.vec.addr.not_ptr", "sircc: load.vec requires pointer addr");
      return false;
    }

    LLVMTypeRef vec_llvm = lower_type(f->p, f->ctx, n->type_ref);
    if (!vec_llvm) return false;
    LLVMTypeRef want_ptr = LLVMPointerType(vec_llvm, 0);
    if (want_ptr != pty) pval = LLVMBuildBitCast(f->builder, pval, want_ptr, "ldv.cast");

    unsigned align = 1;
    JsonValue* alignv = json_obj_get(n->fields, "align");
    if (alignv) {
      int64_t a = 0;
      if (!json_get_i64(alignv, &a) || a <= 0 || a > (int64_t)UINT_MAX) {
        LOWER_ERR_NODE(f, n, "sircc.load.vec.align.bad", "sircc: load.vec node %lld align must be a positive integer", (long long)node_id);
        return false;
      }
      align = (unsigned)a;
    }
    if ((align & (align - 1u)) != 0u) {
      LOWER_ERR_NODE(f, n, "sircc.load.vec.align.not_pow2", "sircc: load.vec node %lld align must be a power of two", (long long)node_id);
      return false;
    }
    if (!emit_trap_if_misaligned(f, pval, align)) return false;

    LLVMValueRef out = LLVMBuildLoad2(f->builder, vec_llvm, pval, "loadv");
    LLVMSetAlignment(out, align);
    JsonValue* volv = json_obj_get(n->fields, "vol");
    if (volv && volv->type == JSON_BOOL) LLVMSetVolatile(out, volv->v.b ? 1 : 0);
    out = canonicalize_float_vec(f, out, vec, lane);
    *outp = out;
    return true;
  }

  if (strcmp(n->tag, "vec.bitcast") == 0) {
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.vec.bitcast.missing_fields", "sircc: vec.bitcast node %lld missing fields", (long long)node_id);
      return false;
    }
    int64_t from_id = 0;
    int64_t to_id = 0;
    if (!parse_type_ref_id(f->p, json_obj_get(n->fields, "from"), &from_id) || !parse_type_ref_id(f->p, json_obj_get(n->fields, "to"), &to_id)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.bitcast.from_to.bad", "sircc: vec.bitcast node %lld requires fields.from and fields.to type refs",
                     (long long)node_id);
      return false;
    }
    TypeRec* from_vec = NULL;
    TypeRec* from_lane = NULL;
    TypeRec* to_vec = NULL;
    TypeRec* to_lane = NULL;
    if (!is_vec_type(f->p, from_id, &from_vec, &from_lane) || !is_vec_type(f->p, to_id, &to_vec, &to_lane)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.bitcast.type.bad", "sircc: vec.bitcast node %lld from/to must be vec types", (long long)node_id);
      return false;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
      LOWER_ERR_NODE(f, n, "sircc.vec.bitcast.args.bad", "sircc: vec.bitcast node %lld requires args:[v]", (long long)node_id);
      return false;
    }
    int64_t vid = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[0], &vid)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.bitcast.args.ref_bad", "sircc: vec.bitcast node %lld args[0] must be a node ref", (long long)node_id);
      return false;
    }
    LLVMValueRef v = lower_expr(f, vid);
    if (!v) return false;

    int64_t from_sz = 0, from_al = 0;
    int64_t to_sz = 0, to_al = 0;
    if (!type_size_align(f->p, from_id, &from_sz, &from_al) || !type_size_align(f->p, to_id, &to_sz, &to_al) || from_sz != to_sz) {
      LOWER_ERR_NODE(f, n, "sircc.vec.bitcast.size_mismatch",
                     "sircc: vec.bitcast node %lld requires sizeof(from)==sizeof(to) (from=%lld, to=%lld)", (long long)node_id,
                     (long long)from_sz, (long long)to_sz);
      return false;
    }

    LLVMTypeRef to_llvm = lower_type(f->p, f->ctx, to_id);
    if (!to_llvm) return false;
    LLVMValueRef out = LLVMBuildBitCast(f->builder, v, to_llvm, "vcast");
    out = canonicalize_float_vec(f, out, to_vec, to_lane);
    *outp = out;
    return true;
  }

  LOWER_ERR_NODE(f, n, "sircc.vec.mnemonic.unhandled", "sircc: unhandled simd mnemonic '%s'", n->tag);
  return false;
}

bool lower_stmt_simd(FunctionCtx* f, int64_t node_id, NodeRec* n) {
  if (!f || !n) return false;

  if (strcmp(n->tag, "store.vec") == 0) {
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.store.vec.missing_fields", "sircc: store.vec node %lld missing fields", (long long)node_id);
      return false;
    }
    int64_t aid = 0, vid = 0;
    if (!parse_node_ref_id(f->p, json_obj_get(n->fields, "addr"), &aid) || !parse_node_ref_id(f->p, json_obj_get(n->fields, "value"), &vid)) {
      LOWER_ERR_NODE(f, n, "sircc.store.vec.addr_value.ref_bad", "sircc: store.vec node %lld requires fields.addr and fields.value refs",
                     (long long)node_id);
      return false;
    }

    int64_t vec_ty_id = 0;
    NodeRec* vn = get_node(f->p, vid);
    if (vn && vn->type_ref) vec_ty_id = vn->type_ref;
    if (vec_ty_id == 0) {
      parse_type_ref_id(f->p, json_obj_get(n->fields, "ty"), &vec_ty_id);
    }
    if (vec_ty_id == 0) {
      LOWER_ERR_NODE(f, n, "sircc.store.vec.missing_type", "sircc: store.vec node %lld requires a vec type (value.type_ref or fields.ty)",
                     (long long)node_id);
      return false;
    }

    TypeRec* vec = NULL;
    TypeRec* lane = NULL;
    if (!is_vec_type(f->p, vec_ty_id, &vec, &lane)) {
      LOWER_ERR_NODE(f, n, "sircc.store.vec.type.bad", "sircc: store.vec node %lld vec type must be kind:'vec'", (long long)node_id);
      return false;
    }

    LLVMValueRef pval = lower_expr(f, aid);
    LLVMValueRef vval = lower_expr(f, vid);
    if (!pval || !vval) return false;
    LLVMTypeRef pty = LLVMTypeOf(pval);
    if (LLVMGetTypeKind(pty) != LLVMPointerTypeKind) {
      LOWER_ERR_NODE(f, n, "sircc.store.vec.addr.not_ptr", "sircc: store.vec requires pointer addr");
      return false;
    }

    LLVMTypeRef vec_llvm = lower_type(f->p, f->ctx, vec_ty_id);
    if (!vec_llvm) return false;
    LLVMTypeRef want_ptr = LLVMPointerType(vec_llvm, 0);
    if (want_ptr != pty) pval = LLVMBuildBitCast(f->builder, pval, want_ptr, "stv.cast");

    unsigned align = 1;
    JsonValue* alignv = json_obj_get(n->fields, "align");
    if (alignv) {
      int64_t a = 0;
      if (!json_get_i64(alignv, &a) || a <= 0 || a > (int64_t)UINT_MAX) {
        LOWER_ERR_NODE(f, n, "sircc.store.vec.align.bad", "sircc: store.vec node %lld align must be a positive integer", (long long)node_id);
        return false;
      }
      align = (unsigned)a;
    }
    if ((align & (align - 1u)) != 0u) {
      LOWER_ERR_NODE(f, n, "sircc.store.vec.align.not_pow2", "sircc: store.vec node %lld align must be a power of two", (long long)node_id);
      return false;
    }
    if (!emit_trap_if_misaligned(f, pval, align)) return false;

    vval = canonicalize_float_vec(f, vval, vec, lane);

    LLVMValueRef st = LLVMBuildStore(f->builder, vval, pval);
    LLVMSetAlignment(st, align);
    JsonValue* volv = json_obj_get(n->fields, "vol");
    if (volv && volv->type == JSON_BOOL) LLVMSetVolatile(st, volv->v.b ? 1 : 0);
    return true;
  }

  LOWER_ERR_NODE(f, n, "sircc.simd.stmt.unhandled", "sircc: unhandled simd stmt '%s'", n->tag);
  return false;
}
