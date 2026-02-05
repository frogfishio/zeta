// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_lower_internal.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

bool lower_stmt(FunctionCtx* f, int64_t node_id) {
  NodeRec* n = get_node(f->p, node_id);
  if (!n) {
    err_codef(f->p, "sircc.stmt.unknown", "sircc: unknown stmt node %lld", (long long)node_id);
    return false;
  }

  if (strcmp(n->tag, "let") == 0) {
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.let.missing_fields", "sircc: let node %lld missing fields", (long long)node_id);
      return false;
    }
    const char* name = json_get_string(json_obj_get(n->fields, "name"));
    if (!name) {
      LOWER_ERR_NODE(f, n, "sircc.let.name.missing", "sircc: let node %lld missing fields.name", (long long)node_id);
      return false;
    }
    int64_t vid = 0;
    if (!parse_node_ref_id(f->p, json_obj_get(n->fields, "value"), &vid)) {
      LOWER_ERR_NODE(f, n, "sircc.let.value.ref_bad", "sircc: let node %lld missing fields.value ref", (long long)node_id);
      return false;
    }
    LLVMValueRef v = lower_expr(f, vid);
    if (!v) return false;
    if (!bind_add(f, name, v)) return false;
    return true;
  }

  if (strncmp(n->tag, "store.", 6) == 0) {
    const char* tname = n->tag + 6;
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.store.missing_fields", "sircc: %s node %lld missing fields", n->tag, (long long)node_id);
      return false;
    }
    int64_t aid = 0, vid = 0;
    if (!parse_node_ref_id(f->p, json_obj_get(n->fields, "addr"), &aid) || !parse_node_ref_id(f->p, json_obj_get(n->fields, "value"), &vid)) {
      LOWER_ERR_NODE(f, n, "sircc.store.addr_value.ref_bad", "sircc: %s node %lld requires fields.addr and fields.value refs", n->tag, (long long)node_id);
      return false;
    }
    NodeRec* vn = get_node(f->p, vid);
    if (vn && vn->type_ref) {
      TypeRec* vt = get_type(f->p, vn->type_ref);
      if (vt && (vt->kind == TYPE_FUN || vt->kind == TYPE_CLOSURE)) {
        LOWER_ERR_NODE(f, n, "sircc.store.opaque.disallowed", "sircc: %s cannot store opaque %s values", n->tag,
                       (vt->kind == TYPE_CLOSURE) ? "closure" : "fun");
        return false;
      }
    }
    LLVMValueRef pval = lower_expr(f, aid);
    LLVMValueRef vval = lower_expr(f, vid);
    if (!pval || !vval) return false;
    LLVMTypeRef el = NULL;
    if (strcmp(tname, "ptr") == 0) {
      el = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
    } else {
      el = lower_type_prim(f->ctx, tname);
    }
    if (!el) {
      LOWER_ERR_NODE(f, n, "sircc.store.type_unsupported", "sircc: unsupported store type '%s'", tname);
      return false;
    }
    if (LLVMGetTypeKind(el) == LLVMFloatTypeKind || LLVMGetTypeKind(el) == LLVMDoubleTypeKind) {
      vval = canonicalize_float(f, vval);
    }
    LLVMTypeRef want_ptr = LLVMPointerType(el, 0);
    LLVMTypeRef pty = LLVMTypeOf(pval);
    if (LLVMGetTypeKind(pty) != LLVMPointerTypeKind) {
      LOWER_ERR_NODE(f, n, "sircc.store.addr.not_ptr", "sircc: %s requires pointer addr", n->tag);
      return false;
    }
    if (want_ptr != pty) {
      pval = LLVMBuildBitCast(f->builder, pval, want_ptr, "st.cast");
    }
    JsonValue* alignv = json_obj_get(n->fields, "align");
    unsigned align = 1;
    if (alignv) {
      int64_t a = 0;
      if (!json_get_i64(alignv, &a)) {
        LOWER_ERR_NODE(f, n, "sircc.store.align.not_int", "sircc: %s node %lld align must be an integer", n->tag, (long long)node_id);
        return false;
      }
      if (a <= 0 || a > (int64_t)UINT_MAX) {
        LOWER_ERR_NODE(f, n, "sircc.store.align.range", "sircc: %s node %lld align must be > 0", n->tag, (long long)node_id);
        return false;
      }
      align = (unsigned)a;
    }
    if ((align & (align - 1u)) != 0u) {
      LOWER_ERR_NODE(f, n, "sircc.store.align.not_pow2", "sircc: %s node %lld align must be a power of two", n->tag, (long long)node_id);
      return false;
    }
    if (!emit_trap_if_misaligned(f, pval, align)) return false;
    LLVMValueRef st = LLVMBuildStore(f->builder, vval, pval);
    LLVMSetAlignment(st, align);
    JsonValue* volv = json_obj_get(n->fields, "vol");
    if (volv && volv->type == JSON_BOOL) LLVMSetVolatile(st, volv->v.b ? 1 : 0);
    return true;
  }

  if (strcmp(n->tag, "mem.copy") == 0) {
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.mem.copy.missing_fields", "sircc: mem.copy node %lld missing fields", (long long)node_id);
      return false;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) {
      LOWER_ERR_NODE(f, n, "sircc.mem.copy.args.bad", "sircc: mem.copy node %lld requires args:[dst, src, len]", (long long)node_id);
      return false;
    }
    int64_t did = 0, sid = 0, lid = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[0], &did) || !parse_node_ref_id(f->p, args->v.arr.items[1], &sid) ||
        !parse_node_ref_id(f->p, args->v.arr.items[2], &lid)) {
      LOWER_ERR_NODE(f, n, "sircc.mem.copy.args.ref_bad", "sircc: mem.copy node %lld args must be node refs", (long long)node_id);
      return false;
    }
    LLVMValueRef dst = lower_expr(f, did);
    LLVMValueRef src = lower_expr(f, sid);
    LLVMValueRef len = lower_expr(f, lid);
    if (!dst || !src || !len) return false;

    LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
    dst = LLVMBuildBitCast(f->builder, dst, i8p, "dst.i8p");
    src = LLVMBuildBitCast(f->builder, src, i8p, "src.i8p");

    LLVMTypeRef i64 = LLVMInt64TypeInContext(f->ctx);
    if (LLVMGetTypeKind(LLVMTypeOf(len)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(len)) != 64) {
      len = build_zext_or_trunc(f->builder, len, i64, "len.i64");
    }

    unsigned align_dst = 1;
    unsigned align_src = 1;
    bool use_memmove = false;
    JsonValue* flags = json_obj_get(n->fields, "flags");
    if (flags && flags->type == JSON_OBJECT) {
      JsonValue* adv = json_obj_get(flags, "alignDst");
      if (adv) {
        int64_t a = 0;
        if (!json_get_i64(adv, &a)) {
          LOWER_ERR_NODE(f, n, "sircc.mem.copy.align_dst.not_int", "sircc: mem.copy node %lld flags.alignDst must be an integer", (long long)node_id);
          return false;
        }
        if (a <= 0 || a > (int64_t)UINT_MAX) {
          LOWER_ERR_NODE(f, n, "sircc.mem.copy.align_dst.range", "sircc: mem.copy node %lld flags.alignDst must be > 0", (long long)node_id);
          return false;
        }
        align_dst = (unsigned)a;
      }
      JsonValue* asv = json_obj_get(flags, "alignSrc");
      if (asv) {
        int64_t a = 0;
        if (!json_get_i64(asv, &a)) {
          LOWER_ERR_NODE(f, n, "sircc.mem.copy.align_src.not_int", "sircc: mem.copy node %lld flags.alignSrc must be an integer", (long long)node_id);
          return false;
        }
        if (a <= 0 || a > (int64_t)UINT_MAX) {
          LOWER_ERR_NODE(f, n, "sircc.mem.copy.align_src.range", "sircc: mem.copy node %lld flags.alignSrc must be > 0", (long long)node_id);
          return false;
        }
        align_src = (unsigned)a;
      }
      const char* ov = json_get_string(json_obj_get(flags, "overlap"));
      if (ov) {
        if (strcmp(ov, "allow") == 0) use_memmove = true;
        else if (strcmp(ov, "disallow") == 0) use_memmove = false;
        else {
          LOWER_ERR_NODE(f, n, "sircc.mem.copy.overlap.bad", "sircc: mem.copy node %lld flags.overlap must be 'allow' or 'disallow'", (long long)node_id);
          return false;
        }
      }
    }

    if (use_memmove) {
      if ((align_dst & (align_dst - 1u)) != 0u) {
        LOWER_ERR_NODE(f, n, "sircc.mem.copy.align_dst.not_pow2", "sircc: mem.copy node %lld flags.alignDst must be a power of two", (long long)node_id);
        return false;
      }
      if ((align_src & (align_src - 1u)) != 0u) {
        LOWER_ERR_NODE(f, n, "sircc.mem.copy.align_src.not_pow2", "sircc: mem.copy node %lld flags.alignSrc must be a power of two", (long long)node_id);
        return false;
      }
      if (!emit_trap_if_misaligned(f, dst, align_dst)) return false;
      if (!emit_trap_if_misaligned(f, src, align_src)) return false;
      LLVMBuildMemMove(f->builder, dst, align_dst, src, align_src, len);
    } else {
      // Deterministic trap on overlapping ranges: overlap = len!=0 && (dst < src+len) && (src < dst+len).
      if ((align_dst & (align_dst - 1u)) != 0u) {
        LOWER_ERR_NODE(f, n, "sircc.mem.copy.align_dst.not_pow2", "sircc: mem.copy node %lld flags.alignDst must be a power of two", (long long)node_id);
        return false;
      }
      if ((align_src & (align_src - 1u)) != 0u) {
        LOWER_ERR_NODE(f, n, "sircc.mem.copy.align_src.not_pow2", "sircc: mem.copy node %lld flags.alignSrc must be a power of two", (long long)node_id);
        return false;
      }
      if (!emit_trap_if_misaligned(f, dst, align_dst)) return false;
      if (!emit_trap_if_misaligned(f, src, align_src)) return false;
      unsigned ptr_bits = f->p->ptr_bits ? f->p->ptr_bits : (unsigned)(sizeof(void*) * 8u);
      LLVMTypeRef ip = LLVMIntTypeInContext(f->ctx, ptr_bits);
      LLVMValueRef dst_i = LLVMBuildPtrToInt(f->builder, dst, ip, "dst.i");
      LLVMValueRef src_i = LLVMBuildPtrToInt(f->builder, src, ip, "src.i");
      LLVMValueRef len_i = LLVMBuildTruncOrBitCast(f->builder, len, ip, "len.i");
      LLVMValueRef z = LLVMConstInt(ip, 0, 0);
      LLVMValueRef nz = LLVMBuildICmp(f->builder, LLVMIntNE, len_i, z, "len.nz");
      LLVMValueRef src_end = LLVMBuildAdd(f->builder, src_i, len_i, "src.end");
      LLVMValueRef dst_end = LLVMBuildAdd(f->builder, dst_i, len_i, "dst.end");
      LLVMValueRef c1 = LLVMBuildICmp(f->builder, LLVMIntULT, dst_i, src_end, "ov.c1");
      LLVMValueRef c2 = LLVMBuildICmp(f->builder, LLVMIntULT, src_i, dst_end, "ov.c2");
      LLVMValueRef ov = LLVMBuildAnd(f->builder, c1, c2, "ov");
      LLVMValueRef trap = LLVMBuildAnd(f->builder, nz, ov, "ov.trap");
      if (!emit_trap_if(f, trap)) return false;
      LLVMBuildMemCpy(f->builder, dst, align_dst, src, align_src, len);
    }
    return true;
  }

  if (strcmp(n->tag, "mem.fill") == 0) {
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.mem.fill.missing_fields", "sircc: mem.fill node %lld missing fields", (long long)node_id);
      return false;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) {
      LOWER_ERR_NODE(f, n, "sircc.mem.fill.args.bad", "sircc: mem.fill node %lld requires args:[dst, byte, len]", (long long)node_id);
      return false;
    }
    int64_t did = 0, bid = 0, lid = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[0], &did) || !parse_node_ref_id(f->p, args->v.arr.items[1], &bid) ||
        !parse_node_ref_id(f->p, args->v.arr.items[2], &lid)) {
      LOWER_ERR_NODE(f, n, "sircc.mem.fill.args.ref_bad", "sircc: mem.fill node %lld args must be node refs", (long long)node_id);
      return false;
    }
    LLVMValueRef dst = lower_expr(f, did);
    LLVMValueRef bytev = lower_expr(f, bid);
    LLVMValueRef len = lower_expr(f, lid);
    if (!dst || !bytev || !len) return false;

    LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
    dst = LLVMBuildBitCast(f->builder, dst, i8p, "dst.i8p");

    LLVMTypeRef i8 = LLVMInt8TypeInContext(f->ctx);
    if (LLVMGetTypeKind(LLVMTypeOf(bytev)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(bytev)) != 8) {
      bytev = LLVMBuildTruncOrBitCast(f->builder, bytev, i8, "byte.i8");
    }

    LLVMTypeRef i64 = LLVMInt64TypeInContext(f->ctx);
    if (LLVMGetTypeKind(LLVMTypeOf(len)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(len)) != 64) {
      len = build_zext_or_trunc(f->builder, len, i64, "len.i64");
    }

    unsigned align_dst = 1;
    JsonValue* flags = json_obj_get(n->fields, "flags");
    if (flags && flags->type == JSON_OBJECT) {
      JsonValue* adv = json_obj_get(flags, "alignDst");
      if (adv) {
        int64_t a = 0;
        if (!json_get_i64(adv, &a)) {
          LOWER_ERR_NODE(f, n, "sircc.mem.fill.align_dst.not_int", "sircc: mem.fill node %lld flags.alignDst must be an integer", (long long)node_id);
          return false;
        }
        if (a <= 0 || a > (int64_t)UINT_MAX) {
          LOWER_ERR_NODE(f, n, "sircc.mem.fill.align_dst.range", "sircc: mem.fill node %lld flags.alignDst must be > 0", (long long)node_id);
          return false;
        }
        align_dst = (unsigned)a;
      }
    }

    if ((align_dst & (align_dst - 1u)) != 0u) {
      LOWER_ERR_NODE(f, n, "sircc.mem.fill.align_dst.not_pow2", "sircc: mem.fill node %lld flags.alignDst must be a power of two", (long long)node_id);
      return false;
    }
    if (!emit_trap_if_misaligned(f, dst, align_dst)) return false;
    LLVMBuildMemSet(f->builder, dst, bytev, len, align_dst);
    return true;
  }

  if (strcmp(n->tag, "eff.fence") == 0) {
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.eff.fence.missing_fields", "sircc: eff.fence node %lld missing fields", (long long)node_id);
      return false;
    }
    JsonValue* flags = json_obj_get(n->fields, "flags");
    const char* mode = NULL;
    if (flags && flags->type == JSON_OBJECT) mode = json_get_string(json_obj_get(flags, "mode"));
    if (!mode) mode = json_get_string(json_obj_get(n->fields, "mode"));
    if (!mode) {
      LOWER_ERR_NODE(f, n, "sircc.eff.fence.mode.missing", "sircc: eff.fence node %lld missing flags.mode", (long long)node_id);
      return false;
    }

    if (strcmp(mode, "relaxed") == 0) {
      // Closed set includes relaxed; model it as a no-op fence.
      return true;
    }

    LLVMAtomicOrdering ord;
    if (strcmp(mode, "acquire") == 0) ord = LLVMAtomicOrderingAcquire;
    else if (strcmp(mode, "release") == 0) ord = LLVMAtomicOrderingRelease;
    else if (strcmp(mode, "acqrel") == 0) ord = LLVMAtomicOrderingAcquireRelease;
    else if (strcmp(mode, "seqcst") == 0) ord = LLVMAtomicOrderingSequentiallyConsistent;
    else {
      LOWER_ERR_NODE(f, n, "sircc.eff.fence.mode.bad", "sircc: eff.fence node %lld invalid mode '%s'", (long long)node_id, mode);
      return false;
    }

    (void)LLVMBuildFence(f->builder, ord, 0, "");
    return true;
  }

  if (strcmp(n->tag, "return") == 0) {
    JsonValue* v = n->fields ? json_obj_get(n->fields, "value") : NULL;
    int64_t vid = 0;
    if (!parse_node_ref_id(f->p, v, &vid)) {
      LOWER_ERR_NODE(f, n, "sircc.return.value.ref_bad", "sircc: return node %lld missing value ref", (long long)node_id);
      return false;
    }
    LLVMValueRef rv = lower_expr(f, vid);
    if (!rv) return false;
    LLVMBuildRet(f->builder, rv);
    return true;
  }

  if (strcmp(n->tag, "term.ret") == 0) {
    JsonValue* v = n->fields ? json_obj_get(n->fields, "value") : NULL;
    if (!v) {
      LLVMBuildRetVoid(f->builder);
      return true;
    }
    int64_t vid = 0;
    if (!parse_node_ref_id(f->p, v, &vid)) {
      LOWER_ERR_NODE(f, n, "sircc.term.ret.value.ref_bad", "sircc: term.ret node %lld invalid value ref", (long long)node_id);
      return false;
    }
    LLVMValueRef rv = lower_expr(f, vid);
    if (!rv) return false;
    LLVMBuildRet(f->builder, rv);
    return true;
  }

  if (strcmp(n->tag, "term.unreachable") == 0) {
    LLVMBuildUnreachable(f->builder);
    return true;
  }

  if (strcmp(n->tag, "term.trap") == 0) {
    // Deterministic immediate trap: lower to llvm.trap + unreachable.
    LLVMTypeRef v = LLVMVoidTypeInContext(f->ctx);
    LLVMValueRef fn = get_or_declare_intrinsic(f->mod, "llvm.trap", v, NULL, 0);
    LLVMBuildCall2(f->builder, LLVMGlobalGetValueType(fn), fn, NULL, 0, "");
    LLVMBuildUnreachable(f->builder);
    return true;
  }

  if (strncmp(n->tag, "term.", 5) == 0) {
    return lower_term_cfg(f, node_id);
  }

  if (strcmp(n->tag, "block") == 0) {
    JsonValue* stmts = n->fields ? json_obj_get(n->fields, "stmts") : NULL;
    if (!stmts || stmts->type != JSON_ARRAY) {
      LOWER_ERR_NODE(f, n, "sircc.block.stmts.bad", "sircc: block node %lld missing stmts array", (long long)node_id);
      return false;
    }
    for (size_t i = 0; i < stmts->v.arr.len; i++) {
      int64_t sid = 0;
      if (!parse_node_ref_id(f->p, stmts->v.arr.items[i], &sid)) {
        LOWER_ERR_NODE(f, n, "sircc.block.stmt.ref_bad", "sircc: block node %lld has non-ref stmt", (long long)node_id);
        return false;
      }
      if (!lower_stmt(f, sid)) return false;
      if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(f->builder))) break;
    }
    return true;
  }

  // Expression-as-statement: evaluate for side-effects (e.g. `call`) and discard.
  LLVMValueRef v = lower_expr(f, node_id);
  return v != NULL;
}

static LLVMBasicBlockRef bb_lookup(FunctionCtx* f, int64_t node_id) {
  if (!f->blocks_by_node || node_id < 0) return NULL;
  size_t i = (size_t)node_id;
  return (i < f->p->nodes_cap) ? f->blocks_by_node[i] : NULL;
}

static bool add_block_args(FunctionCtx* f, const NodeRec* origin, LLVMBasicBlockRef from_bb, int64_t to_block_id, JsonValue* args) {
  NodeRec* bn = get_node(f->p, to_block_id);
  if (!bn || strcmp(bn->tag, "block") != 0) {
    LOWER_ERR_NODE(f, origin, "sircc.branch.target.not_block", "sircc: branch targets non-block node %lld", (long long)to_block_id);
    return false;
  }

  JsonValue* params = bn->fields ? json_obj_get(bn->fields, "params") : NULL;
  size_t pcount = 0;
  if (params) {
    if (params->type != JSON_ARRAY) {
      LOWER_ERR_NODE(f, origin, "sircc.branch.params.not_array", "sircc: block %lld params must be an array", (long long)to_block_id);
      return false;
    }
    pcount = params->v.arr.len;
  }

  size_t acount = 0;
  if (args) {
    if (args->type != JSON_ARRAY) {
      LOWER_ERR_NODE(f, origin, "sircc.branch.args.not_array", "sircc: branch args must be an array");
      return false;
    }
    acount = args->v.arr.len;
  }

  if (pcount != acount) {
    LOWER_ERR_NODE(f, origin, "sircc.branch.param_arg.count_mismatch",
                   "sircc: block %lld param/arg count mismatch (params=%zu, args=%zu)", (long long)to_block_id, pcount, acount);
    return false;
  }

  for (size_t i = 0; i < pcount; i++) {
    int64_t pid = 0;
    if (!parse_node_ref_id(f->p, params->v.arr.items[i], &pid)) {
      LOWER_ERR_NODE(f, origin, "sircc.branch.params.ref_bad", "sircc: block %lld params[%zu] must be node refs", (long long)to_block_id, i);
      return false;
    }
    NodeRec* pn = get_node(f->p, pid);
    if (!pn || strcmp(pn->tag, "bparam") != 0 || !pn->llvm_value) {
      LOWER_ERR_NODE(f, origin, "sircc.branch.params.not_lowered_bparam",
                     "sircc: block %lld params[%zu] must reference a lowered bparam node", (long long)to_block_id, i);
      return false;
    }

    int64_t aid = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[i], &aid)) {
      LOWER_ERR_NODE(f, origin, "sircc.branch.args.ref_bad", "sircc: block %lld args[%zu] must be node refs", (long long)to_block_id, i);
      return false;
    }
    LLVMValueRef av = lower_expr(f, aid);
    if (!av) return false;

    LLVMValueRef phi = pn->llvm_value;
    LLVMAddIncoming(phi, &av, &from_bb, 1);
  }

  return true;
}

bool lower_term_cfg(FunctionCtx* f, int64_t node_id) {
  NodeRec* n = get_node(f->p, node_id);
  if (!n) {
    err_codef(f->p, "sircc.term.unknown", "sircc: unknown term node %lld", (long long)node_id);
    return false;
  }

  if (strcmp(n->tag, "term.br") == 0) {
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.term.br.missing_fields", "sircc: term.br node %lld missing fields", (long long)node_id);
      return false;
    }
    JsonValue* to = json_obj_get(n->fields, "to");
    int64_t bid = 0;
    if (!parse_node_ref_id(f->p, to, &bid)) {
      LOWER_ERR_NODE(f, n, "sircc.term.br.to.ref_bad", "sircc: term.br node %lld missing to ref", (long long)node_id);
      return false;
    }
    LLVMBasicBlockRef bb = bb_lookup(f, bid);
    if (!bb) {
      LOWER_ERR_NODE(f, n, "sircc.term.br.target.unknown", "sircc: term.br node %lld targets unknown block %lld", (long long)node_id,
                     (long long)bid);
      return false;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!add_block_args(f, n, LLVMGetInsertBlock(f->builder), bid, args)) return false;
    LLVMBuildBr(f->builder, bb);
    return true;
  }

  if (strcmp(n->tag, "term.cbr") == 0 || strcmp(n->tag, "term.condbr") == 0) {
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.term.condbr.missing_fields", "sircc: %s node %lld missing fields", n->tag, (long long)node_id);
      return false;
    }

    int64_t cond_id = 0;
    if (!parse_node_ref_id(f->p, json_obj_get(n->fields, "cond"), &cond_id)) {
      LOWER_ERR_NODE(f, n, "sircc.term.condbr.cond.ref_bad", "sircc: %s node %lld missing cond ref", n->tag, (long long)node_id);
      return false;
    }
    LLVMValueRef cond = lower_expr(f, cond_id);
    if (!cond) return false;
    if (LLVMGetTypeKind(LLVMTypeOf(cond)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(cond)) != 1) {
      LOWER_ERR_NODE(f, n, "sircc.term.condbr.cond.type_bad", "sircc: %s cond must be bool/i1", n->tag);
      return false;
    }

    JsonValue* thenb = json_obj_get(n->fields, "then");
    JsonValue* elseb = json_obj_get(n->fields, "else");
    if (!thenb || thenb->type != JSON_OBJECT || !elseb || elseb->type != JSON_OBJECT) {
      LOWER_ERR_NODE(f, n, "sircc.term.condbr.branches.bad", "sircc: %s node %lld requires then/else objects", n->tag, (long long)node_id);
      return false;
    }

    int64_t then_id = 0;
    int64_t else_id = 0;
    if (!parse_node_ref_id(f->p, json_obj_get(thenb, "to"), &then_id) || !parse_node_ref_id(f->p, json_obj_get(elseb, "to"), &else_id)) {
      LOWER_ERR_NODE(f, n, "sircc.term.condbr.to.ref_bad", "sircc: %s node %lld then/else missing to ref", n->tag, (long long)node_id);
      return false;
    }
    LLVMBasicBlockRef then_bb = bb_lookup(f, then_id);
    LLVMBasicBlockRef else_bb = bb_lookup(f, else_id);
    if (!then_bb || !else_bb) {
      LOWER_ERR_NODE(f, n, "sircc.term.condbr.target.unknown", "sircc: %s node %lld targets unknown blocks", n->tag, (long long)node_id);
      return false;
    }

    JsonValue* then_args = json_obj_get(thenb, "args");
    JsonValue* else_args = json_obj_get(elseb, "args");
    LLVMBasicBlockRef from_bb = LLVMGetInsertBlock(f->builder);
    if (!add_block_args(f, n, from_bb, then_id, then_args)) return false;
    if (!add_block_args(f, n, from_bb, else_id, else_args)) return false;

    LLVMBuildCondBr(f->builder, cond, then_bb, else_bb);
    return true;
  }

  if (strcmp(n->tag, "term.switch") == 0) {
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.term.switch.missing_fields", "sircc: term.switch node %lld missing fields", (long long)node_id);
      return false;
    }
    int64_t scrut_id = 0;
    if (!parse_node_ref_id(f->p, json_obj_get(n->fields, "scrut"), &scrut_id)) {
      LOWER_ERR_NODE(f, n, "sircc.term.switch.scrut.ref_bad", "sircc: term.switch node %lld missing scrut ref", (long long)node_id);
      return false;
    }
    LLVMValueRef scrut = lower_expr(f, scrut_id);
    if (!scrut) return false;
    LLVMTypeRef sty = LLVMTypeOf(scrut);
    if (LLVMGetTypeKind(sty) == LLVMPointerTypeKind) {
      // Spec allows ptr scrut; lower by casting to target pointer-sized integer.
      unsigned ptr_bits = f->p->ptr_bits ? f->p->ptr_bits : (unsigned)(sizeof(void*) * 8u);
      LLVMTypeRef ity = LLVMIntTypeInContext(f->ctx, ptr_bits);
      scrut = LLVMBuildPtrToInt(f->builder, scrut, ity, "ptr.switch");
      sty = LLVMTypeOf(scrut);
    }
    if (LLVMGetTypeKind(sty) != LLVMIntegerTypeKind) {
      LOWER_ERR_NODE(f, n, "sircc.term.switch.scrut.type_bad", "sircc: term.switch scrut must be iN or ptr");
      return false;
    }

    JsonValue* def = json_obj_get(n->fields, "default");
    if (!def || def->type != JSON_OBJECT) {
      LOWER_ERR_NODE(f, n, "sircc.term.switch.default.missing", "sircc: term.switch node %lld missing default branch", (long long)node_id);
      return false;
    }
    int64_t def_id = 0;
    if (!parse_node_ref_id(f->p, json_obj_get(def, "to"), &def_id)) {
      LOWER_ERR_NODE(f, n, "sircc.term.switch.default.to.ref_bad", "sircc: term.switch default missing to ref");
      return false;
    }
    LLVMBasicBlockRef def_bb = bb_lookup(f, def_id);
    if (!def_bb) {
      LOWER_ERR_NODE(f, n, "sircc.term.switch.default.target.unknown", "sircc: term.switch default targets unknown block %lld", (long long)def_id);
      return false;
    }
    JsonValue* def_args = json_obj_get(def, "args");
    if (!add_block_args(f, n, LLVMGetInsertBlock(f->builder), def_id, def_args)) return false;

    JsonValue* cases = json_obj_get(n->fields, "cases");
    if (!cases || cases->type != JSON_ARRAY) {
      LOWER_ERR_NODE(f, n, "sircc.term.switch.cases.bad", "sircc: term.switch node %lld missing cases array", (long long)node_id);
      return false;
    }
    LLVMValueRef sw = LLVMBuildSwitch(f->builder, scrut, def_bb, (unsigned)cases->v.arr.len);
    for (size_t i = 0; i < cases->v.arr.len; i++) {
      JsonValue* c = cases->v.arr.items[i];
      if (!c || c->type != JSON_OBJECT) {
        LOWER_ERR_NODE(f, n, "sircc.term.switch.case.obj_bad", "sircc: term.switch case[%zu] must be object", i);
        return false;
      }
      int64_t lit_id = 0;
      if (!parse_node_ref_id(f->p, json_obj_get(c, "lit"), &lit_id)) {
        LOWER_ERR_NODE(f, n, "sircc.term.switch.case.lit.ref_bad", "sircc: term.switch case[%zu] missing lit ref", i);
        return false;
      }
      NodeRec* litn = get_node(f->p, lit_id);
      if (!litn || strncmp(litn->tag, "const.", 6) != 0 || !litn->fields) {
        LOWER_ERR_NODE(f, n, "sircc.term.switch.case.lit.type_bad", "sircc: term.switch case[%zu] lit must be const.* node", i);
        return false;
      }
      int64_t litv = 0;
      if (!json_get_i64(json_obj_get(litn->fields, "value"), &litv)) {
        LOWER_ERR_NODE(f, n, "sircc.term.switch.case.lit.value.bad", "sircc: term.switch case[%zu] lit value must be integer", i);
        return false;
      }
      LLVMValueRef lit = LLVMConstInt(sty, (unsigned long long)litv, 1);

      int64_t to_id = 0;
      if (!parse_node_ref_id(f->p, json_obj_get(c, "to"), &to_id)) {
        LOWER_ERR_NODE(f, n, "sircc.term.switch.case.to.ref_bad", "sircc: term.switch case[%zu] missing to ref", i);
        return false;
      }
      LLVMBasicBlockRef to_bb = bb_lookup(f, to_id);
      if (!to_bb) {
        LOWER_ERR_NODE(f, n, "sircc.term.switch.case.target.unknown", "sircc: term.switch case[%zu] targets unknown block %lld", i,
                       (long long)to_id);
        return false;
      }

      JsonValue* args = json_obj_get(c, "args");
      if (!add_block_args(f, n, LLVMGetInsertBlock(f->builder), to_id, args)) return false;

      LLVMAddCase(sw, lit, to_bb);
    }
    return true;
  }

  // fallthrough to existing term.* handled by lower_stmt
  LOWER_ERR_NODE(f, n, "sircc.term.unsupported", "sircc: unsupported terminator '%s' (node %lld)", n->tag, (long long)node_id);
  return false;
}


bool lower_functions(SirProgram* p, LLVMContextRef ctx, LLVMModuleRef mod) {
  // Pass 1: create prototypes
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if (strcmp(n->tag, "fn") != 0) continue;

    const char* name = n->fields ? json_get_string(json_obj_get(n->fields, "name")) : NULL;
    if (!name) {
      SIRCC_ERR_NODE(p, n, "sircc.fn.name.missing", "sircc: fn node %lld missing fields.name", (long long)n->id);
      return false;
    }
    if (n->type_ref == 0) {
      SIRCC_ERR_NODE(p, n, "sircc.fn.type_ref.missing", "sircc: fn node %lld missing type_ref", (long long)n->id);
      return false;
    }
    LLVMTypeRef fnty = lower_type(p, ctx, n->type_ref);
    if (!fnty || LLVMGetTypeKind(fnty) != LLVMFunctionTypeKind) {
      SIRCC_ERR_NODE(p, n, "sircc.fn.type_ref.bad", "sircc: fn node %lld has invalid function type_ref %lld", (long long)n->id,
                     (long long)n->type_ref);
      return false;
    }
    LLVMValueRef fn = LLVMAddFunction(mod, name, fnty);
    const char* linkage = n->fields ? json_get_string(json_obj_get(n->fields, "linkage")) : NULL;
    if (linkage && strcmp(linkage, "local") == 0) {
      LLVMSetLinkage(fn, LLVMInternalLinkage);
    } else if (linkage && strcmp(linkage, "public") == 0) {
      LLVMSetLinkage(fn, LLVMExternalLinkage);
    } else if (linkage && *linkage) {
      SIRCC_ERR_NODE(p, n, "sircc.fn.linkage.bad",
                     "sircc: fn node %lld has unsupported linkage '%s' (use 'local' or 'public')", (long long)n->id, linkage);
      return false;
    }
    n->llvm_value = fn;
  }

  // Pass 2: lower bodies
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if (strcmp(n->tag, "fn") != 0) continue;
    LLVMValueRef fn = n->llvm_value;
    if (!fn) continue;

    // Expression nodes are currently lowered relative to a specific function's builder. Clear any
    // previous per-node cached values before lowering a new function (constants + fn prototypes are safe).
    for (size_t j = 0; j < p->nodes_cap; j++) {
      NodeRec* x = p->nodes[j];
      if (!x) continue;
      if (strcmp(x->tag, "fn") == 0) continue;
      if (strncmp(x->tag, "const.", 6) == 0) continue;
      x->llvm_value = NULL;
      x->resolving = false;
    }

    JsonValue* paramsv = n->fields ? json_obj_get(n->fields, "params") : NULL;
    if (!paramsv || paramsv->type != JSON_ARRAY) {
      SIRCC_ERR_NODE(p, n, "sircc.fn.params.missing", "sircc: fn node %lld missing params array", (long long)n->id);
      return false;
    }

    FunctionCtx f = {.p = p, .ctx = ctx, .mod = mod, .builder = NULL, .fn = fn};

    unsigned param_count = LLVMCountParams(fn);
    if (paramsv->v.arr.len != (size_t)param_count) {
      SIRCC_ERR_NODE(p, n, "sircc.fn.params.count_mismatch",
                     "sircc: fn node %lld param count mismatch: node has %zu, type has %u", (long long)n->id, paramsv->v.arr.len,
                     param_count);
      free(f.binds);
      return false;
    }

	    for (unsigned pi = 0; pi < param_count; pi++) {
		      int64_t pid = 0;
		      if (!parse_node_ref_id(p, paramsv->v.arr.items[pi], &pid)) {
		        SIRCC_ERR_NODE(p, n, "sircc.fn.param.ref_bad", "sircc: fn node %lld has non-ref param", (long long)n->id);
		        free(f.binds);
		        return false;
		      }
      NodeRec* pn = get_node(p, pid);
      if (!pn || strcmp(pn->tag, "param") != 0) {
        SIRCC_ERR_NODE(p, n, "sircc.fn.param.not_param", "sircc: fn node %lld param ref %lld is not a param node", (long long)n->id,
                       (long long)pid);
        free(f.binds);
        return false;
      }
      const char* pname = pn->fields ? json_get_string(json_obj_get(pn->fields, "name")) : NULL;
      if (!pname) {
        SIRCC_ERR_NODE(p, pn, "sircc.param.name.missing", "sircc: param node %lld missing fields.name", (long long)pid);
        free(f.binds);
        return false;
      }
      LLVMValueRef pv = LLVMGetParam(fn, pi);
      LLVMSetValueName2(pv, pname, strlen(pname));
      pn->llvm_value = pv;
      if (!bind_add(&f, pname, pv)) {
        SIRCC_ERR_NODE(p, n, "sircc.fn.bind.duplicate", "sircc: duplicate binding for '%s' in fn %lld", pname, (long long)n->id);
        free(f.binds);
        return false;
      }
    }

    JsonValue* blocks_v = n->fields ? json_obj_get(n->fields, "blocks") : NULL;
    JsonValue* entry_v = n->fields ? json_obj_get(n->fields, "entry") : NULL;
    if (blocks_v && blocks_v->type == JSON_ARRAY && entry_v) {
	      // CFG form: explicit list of basic blocks + entry.
	      int64_t entry_id = 0;
		      if (!parse_node_ref_id(p, entry_v, &entry_id)) {
		        SIRCC_ERR_NODE(p, n, "sircc.fn.entry.ref_bad", "sircc: fn node %lld entry must be a block ref", (long long)n->id);
		        free(f.binds);
		        return false;
		      }

      f.blocks_by_node = (LLVMBasicBlockRef*)calloc(p->nodes_cap, sizeof(LLVMBasicBlockRef));
      if (!f.blocks_by_node) {
        free(f.binds);
        return false;
      }

		      for (size_t bi = 0; bi < blocks_v->v.arr.len; bi++) {
		        int64_t bid = 0;
		        if (!parse_node_ref_id(p, blocks_v->v.arr.items[bi], &bid)) {
		          SIRCC_ERR_NODE(p, n, "sircc.fn.blocks.ref_bad", "sircc: fn node %lld blocks[%zu] must be block refs", (long long)n->id, bi);
		          free(f.blocks_by_node);
		          free(f.binds);
		          return false;
		        }
	        NodeRec* bn = get_node(p, bid);
	        if (!bn || strcmp(bn->tag, "block") != 0) {
	          SIRCC_ERR_NODE(p, n, "sircc.fn.blocks.not_block",
	                         "sircc: fn node %lld blocks[%zu] does not reference a block node", (long long)n->id, bi);
	          free(f.blocks_by_node);
	          free(f.binds);
	          return false;
	        }
        if (bid < 0 || (size_t)bid >= p->nodes_cap) continue;
        if (!f.blocks_by_node[bid]) {
          char namebuf[32];
          snprintf(namebuf, sizeof(namebuf), "B%lld", (long long)bid);
          f.blocks_by_node[bid] = LLVMAppendBasicBlockInContext(ctx, fn, namebuf);
        }
      }

      // Ensure entry exists.
      if (entry_id < 0 || (size_t)entry_id >= p->nodes_cap || !f.blocks_by_node[entry_id]) {
        SIRCC_ERR_NODE(p, n, "sircc.fn.entry.not_in_blocks",
                       "sircc: fn node %lld entry block %lld not in blocks list", (long long)n->id, (long long)entry_id);
        free(f.blocks_by_node);
        free(f.binds);
        return false;
      }

      // Pre-create PHIs for block params so branches can add incoming values regardless of block order.
      // (Otherwise, a forward branch would see pn->llvm_value == NULL.)
	      for (size_t bi = 0; bi < blocks_v->v.arr.len; bi++) {
	        int64_t bid = 0;
	        if (!parse_node_ref_id(p, blocks_v->v.arr.items[bi], &bid)) continue;
	        NodeRec* bn = get_node(p, bid);
	        LLVMBasicBlockRef bb = f.blocks_by_node[bid];
	        if (!bn || !bb || !bn->fields) continue;

        JsonValue* params = json_obj_get(bn->fields, "params");
	        if (!params) continue;
	        if (params->type != JSON_ARRAY) {
	          SIRCC_ERR_NODE(p, bn, "sircc.block.params.not_array", "sircc: block %lld params must be an array", (long long)bid);
	          free(f.blocks_by_node);
	          free(f.binds);
	          return false;
	        }

        LLVMBuilderRef b = LLVMCreateBuilderInContext(ctx);
        LLVMValueRef first = LLVMGetFirstInstruction(bb);
        if (first) LLVMPositionBuilderBefore(b, first);
        else LLVMPositionBuilderAtEnd(b, bb);

		        for (size_t pi = 0; pi < params->v.arr.len; pi++) {
		          int64_t pid = 0;
		          if (!parse_node_ref_id(p, params->v.arr.items[pi], &pid)) {
		            SIRCC_ERR_NODE(p, bn, "sircc.block.params.ref_bad", "sircc: block %lld params[%zu] must be node refs", (long long)bid, pi);
		            LLVMDisposeBuilder(b);
		            free(f.blocks_by_node);
		            free(f.binds);
		            return false;
		          }
	          NodeRec* pn = get_node(p, pid);
	          if (!pn || strcmp(pn->tag, "bparam") != 0) {
	            SIRCC_ERR_NODE(p, bn, "sircc.block.params.not_bparam",
	                           "sircc: block %lld params[%zu] must reference bparam nodes", (long long)bid, pi);
	            LLVMDisposeBuilder(b);
	            free(f.blocks_by_node);
	            free(f.binds);
	            return false;
	          }
	          if (!pn->llvm_value) {
	            if (pn->type_ref == 0) {
	              SIRCC_ERR_NODE(p, pn, "sircc.bparam.type_ref.missing", "sircc: bparam node %lld missing type_ref", (long long)pid);
	              LLVMDisposeBuilder(b);
	              free(f.blocks_by_node);
	              free(f.binds);
	              return false;
	            }
	            LLVMTypeRef pty = lower_type(p, ctx, pn->type_ref);
	            if (!pty) {
	              SIRCC_ERR_NODE(p, pn, "sircc.bparam.type_ref.bad", "sircc: bparam node %lld has invalid type_ref", (long long)pid);
	              LLVMDisposeBuilder(b);
	              free(f.blocks_by_node);
	              free(f.binds);
	              return false;
	            }
            pn->llvm_value = LLVMBuildPhi(b, pty, "bparam");
          }
        }

        LLVMDisposeBuilder(b);
      }

	      // Lower blocks in listed order.
	      for (size_t bi = 0; bi < blocks_v->v.arr.len; bi++) {
	        int64_t bid = 0;
	        (void)parse_node_ref_id(p, blocks_v->v.arr.items[bi], &bid);
	        NodeRec* bn = get_node(p, bid);
	        LLVMBasicBlockRef bb = f.blocks_by_node[bid];
        if (!bn || !bb) continue;

        LLVMBuilderRef builder = LLVMCreateBuilderInContext(ctx);
        f.builder = builder;
        LLVMPositionBuilderAtEnd(builder, bb);

        size_t mark = bind_mark(&f);

        // Block params: lowered as PHIs (to be populated by predecessors via branch args).
        JsonValue* params = bn->fields ? json_obj_get(bn->fields, "params") : NULL;
	        if (params) {
	          if (params->type != JSON_ARRAY) {
	            SIRCC_ERR_NODE(p, bn, "sircc.block.params.not_array", "sircc: block %lld params must be an array", (long long)bid);
	            LLVMDisposeBuilder(builder);
	            free(f.blocks_by_node);
	            free(f.binds);
	            return false;
	          }
	          for (size_t pi = 0; pi < params->v.arr.len; pi++) {
	            int64_t pid = 0;
	            if (!parse_node_ref_id(p, params->v.arr.items[pi], &pid)) {
	              SIRCC_ERR_NODE(p, bn, "sircc.block.params.ref_bad", "sircc: block %lld params[%zu] must be node refs", (long long)bid, pi);
	              LLVMDisposeBuilder(builder);
	              free(f.blocks_by_node);
	              free(f.binds);
	              return false;
	            }
	            NodeRec* pn = get_node(p, pid);
	            if (!pn || strcmp(pn->tag, "bparam") != 0) {
	              SIRCC_ERR_NODE(p, bn, "sircc.block.params.not_bparam",
	                             "sircc: block %lld params[%zu] must reference bparam nodes", (long long)bid, pi);
	              LLVMDisposeBuilder(builder);
	              free(f.blocks_by_node);
	              free(f.binds);
	              return false;
	            }
	            if (!pn->llvm_value) {
	              SIRCC_ERR_NODE(p, pn, "sircc.bparam.phi.missing", "sircc: bparam node %lld missing lowered phi", (long long)pid);
	              LLVMDisposeBuilder(builder);
	              free(f.blocks_by_node);
	              free(f.binds);
	              return false;
	            }
            const char* bname = pn->fields ? json_get_string(json_obj_get(pn->fields, "name")) : NULL;
            if (bname) {
	              LLVMSetValueName2(pn->llvm_value, bname, strlen(bname));
	              if (!bind_add(&f, bname, pn->llvm_value)) {
	                SIRCC_ERR_NODE(p, n, "sircc.fn.block_param.bind.failed", "sircc: failed to bind block param '%s' in fn %lld", bname,
	                               (long long)n->id);
	                LLVMDisposeBuilder(builder);
	                free(f.blocks_by_node);
	                free(f.binds);
	                return false;
	              }
            }
          }
        }

	        JsonValue* stmts = bn->fields ? json_obj_get(bn->fields, "stmts") : NULL;
	        if (!stmts || stmts->type != JSON_ARRAY) {
	          SIRCC_ERR_NODE(p, bn, "sircc.block.stmts.bad", "sircc: block node %lld missing stmts array", (long long)bid);
	          LLVMDisposeBuilder(builder);
	          free(f.blocks_by_node);
	          free(f.binds);
	          return false;
	        }
        for (size_t si = 0; si < stmts->v.arr.len; si++) {
          int64_t sid = 0;
	          if (!parse_node_ref_id(p, stmts->v.arr.items[si], &sid)) {
	            SIRCC_ERR_NODE(p, bn, "sircc.block.stmt.ref_bad", "sircc: block node %lld has non-ref stmt", (long long)bid);
	            LLVMDisposeBuilder(builder);
	            free(f.blocks_by_node);
	            free(f.binds);
	            return false;
	          }
          if (!lower_stmt(&f, sid)) {
            LLVMDisposeBuilder(builder);
            free(f.blocks_by_node);
            free(f.binds);
            return false;
          }
          if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) break;
        }

	        if (!LLVMGetBasicBlockTerminator(bb)) {
	          SIRCC_ERR_NODE(p, bn, "sircc.block.term.missing", "sircc: block %lld missing terminator", (long long)bid);
	          LLVMDisposeBuilder(builder);
	          bind_restore(&f, mark);
	          free(f.blocks_by_node);
	          free(f.binds);
	          return false;
	        }

        LLVMDisposeBuilder(builder);
        bind_restore(&f, mark);
        f.builder = NULL;
      }

      // Ensure entry is first for execution: create a trampoline if needed.
      LLVMBasicBlockRef first = LLVMGetFirstBasicBlock(fn);
      if (first != f.blocks_by_node[entry_id]) {
        LLVMBasicBlockRef tramp = LLVMInsertBasicBlockInContext(ctx, first, "entry");
        LLVMBuilderRef builder = LLVMCreateBuilderInContext(ctx);
        LLVMPositionBuilderAtEnd(builder, tramp);
        LLVMBuildBr(builder, f.blocks_by_node[entry_id]);
        LLVMDisposeBuilder(builder);
      }

      free(f.blocks_by_node);
      free(f.binds);
      continue;
    }

    // Legacy form: single entry block with `body:ref`.
    JsonValue* bodyv = n->fields ? json_obj_get(n->fields, "body") : NULL;
    int64_t body_id = 0;
    if (!parse_node_ref_id(p, bodyv, &body_id)) {
      SIRCC_ERR_NODE(p, n, "sircc.fn.body.ref_bad", "sircc: fn node %lld missing body ref", (long long)n->id);
      free(f.binds);
      return false;
    }

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx, fn, "entry");
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(ctx);
    f.builder = builder;
    LLVMPositionBuilderAtEnd(builder, entry);

    if (!lower_stmt(&f, body_id)) {
      LLVMDisposeBuilder(builder);
      free(f.binds);
      return false;
    }

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
      // Conservative default: fallthrough returns 0 for integer returns, otherwise void.
      LLVMTypeRef rty = LLVMGetReturnType(LLVMGlobalGetValueType(fn));
      if (LLVMGetTypeKind(rty) == LLVMVoidTypeKind) {
        LLVMBuildRetVoid(builder);
      } else if (LLVMGetTypeKind(rty) == LLVMIntegerTypeKind) {
        LLVMBuildRet(builder, LLVMConstInt(rty, 0, 0));
      } else {
        SIRCC_ERR_NODE(p, n, "sircc.fn.fallthrough.ret_unsupported",
                       "sircc: fn %lld has implicit fallthrough with unsupported return type", (long long)n->id);
        LLVMDisposeBuilder(builder);
        free(f.binds);
        return false;
      }
    }

    LLVMDisposeBuilder(builder);
    free(f.binds);
  }

  return true;
}
