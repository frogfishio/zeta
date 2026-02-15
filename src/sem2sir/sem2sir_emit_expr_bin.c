#include "sem2sir_emit_internal.h"

bool parse_expr_bin(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  bool seen_op = false;
  bool seen_lhs = false;
  bool seen_rhs = false;
  sem2sir_op_id opid = SEM2SIR_OP_INVALID;
  SirExpr lhs = {0};
  SirExpr rhs = {0};

  // Assignment as expression: capture JSON so lhs/rhs order is irrelevant.
  char *assign_lhs_json = NULL;
  size_t assign_lhs_len = 0;
  char *assign_rhs_json = NULL;
  size_t assign_rhs_len = 0;

  // For comparisons, we need an explicitly-committed operand type (i32/i64)
  // but Bin's JSON field order is not guaranteed (rhs may appear before lhs).
  // We therefore capture operand JSON and parse after committing a type.
  char *cmp_lhs_json = NULL;
  size_t cmp_lhs_len = 0;
  char *cmp_rhs_json = NULL;
  size_t cmp_rhs_len = 0;
  sem2sir_type_id cmp_operand_ty = SEM2SIR_TYPE_INVALID;

  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in Bin");
      free(lhs.id);
      free(rhs.id);
      free(assign_lhs_json);
      free(assign_rhs_json);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in Bin");
      free(lhs.id);
      free(rhs.id);
      free(assign_lhs_json);
      free(assign_rhs_json);
      return false;
    }
    c->p++;
    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid Bin key");
      free(lhs.id);
      free(rhs.id);
      free(assign_lhs_json);
      free(assign_rhs_json);
      return false;
    }

    if (strcmp(key, "op") == 0) {
      seen_op = true;
      char *op_str = NULL;
      if (!grit_json_parse_string_alloc(c, &op_str)) {
        err(ctx->in_path, "Bin.op must be string");
        free(key);
        free(lhs.id);
        free(rhs.id);
        free(assign_lhs_json);
        free(assign_rhs_json);
        return false;
      }
      opid = sem2sir_op_parse(op_str, strlen(op_str));
      free(op_str);
      if (opid == SEM2SIR_OP_INVALID) {
        err(ctx->in_path, "Bin.op is unknown or not normalized");
        free(key);
        free(lhs.id);
        free(rhs.id);
        free(assign_lhs_json);
        free(assign_rhs_json);
        return false;
      }
    } else if (strcmp(key, "lhs") == 0) {
      if (!seen_op) {
        err(ctx->in_path, "Bin.op must appear before lhs (no implicit context)");
        free(key);
        free(lhs.id);
        free(rhs.id);
        free(cmp_lhs_json);
        free(cmp_rhs_json);
        free(assign_lhs_json);
        free(assign_rhs_json);
        return false;
      }
      seen_lhs = true;

      if (opid == SEM2SIR_OP_CORE_ASSIGN) {
        free(assign_lhs_json);
        assign_lhs_json = NULL;
        if (!capture_json_value_alloc(c, &assign_lhs_json, &assign_lhs_len)) {
          err(ctx->in_path, "invalid Bin.lhs");
          free(key);
          free(lhs.id);
          free(rhs.id);
          free(assign_rhs_json);
          return false;
        }
        free(key);
        continue;
      }

      if (sem2sir_op_is_cmp(opid)) {
        if (!capture_json_value_alloc(c, &cmp_lhs_json, &cmp_lhs_len)) {
          err(ctx->in_path, "invalid Bin.lhs");
          free(key);
          free(lhs.id);
          free(rhs.id);
          free(cmp_lhs_json);
          free(cmp_rhs_json);
          return false;
        }
        free(key);
        continue;
      }

      sem2sir_type_id lhs_expected = SEM2SIR_TYPE_INVALID;
      switch (opid) {
      case SEM2SIR_OP_CORE_ADD:
      case SEM2SIR_OP_CORE_SUB:
      case SEM2SIR_OP_CORE_MUL:
      case SEM2SIR_OP_CORE_DIV:
      case SEM2SIR_OP_CORE_REM:
      case SEM2SIR_OP_CORE_SHL:
      case SEM2SIR_OP_CORE_SHR:
      case SEM2SIR_OP_CORE_BITAND:
      case SEM2SIR_OP_CORE_BITOR:
      case SEM2SIR_OP_CORE_BITXOR:
        // Numeric width is committed by the expected result type.
        if (expected != SEM2SIR_TYPE_I32 && expected != SEM2SIR_TYPE_I64) {
          err(ctx->in_path,
              "core.(add|sub|mul|div|rem|shl|shr|bitand|bitor|bitxor) requires expected type i32 or i64 (no inference)");
          free(key);
          free(lhs.id);
          free(rhs.id);
          return false;
        }
        lhs_expected = expected;
        break;
      case SEM2SIR_OP_CORE_EQ:
      case SEM2SIR_OP_CORE_NE:
      case SEM2SIR_OP_CORE_BOOL_AND_SC:
      case SEM2SIR_OP_CORE_BOOL_OR_SC:
        lhs_expected = SEM2SIR_TYPE_BOOL;
        break;
      default:
        err(ctx->in_path, "Bin op not supported in emitter MVP");
        free(key);
        free(lhs.id);
        free(rhs.id);
        return false;
      }
      if (!parse_expr(c, ctx, lhs_expected, &lhs)) {
        free(key);
        free(rhs.id);
        return false;
      }
    } else if (strcmp(key, "rhs") == 0) {
      if (!seen_op) {
        err(ctx->in_path, "Bin.op must appear before rhs (no implicit context)");
        free(key);
        free(lhs.id);
        free(rhs.id);
        free(cmp_lhs_json);
        free(cmp_rhs_json);
        return false;
      }
      seen_rhs = true;

      if (opid == SEM2SIR_OP_CORE_ASSIGN) {
        free(assign_rhs_json);
        assign_rhs_json = NULL;
        if (!capture_json_value_alloc(c, &assign_rhs_json, &assign_rhs_len)) {
          err(ctx->in_path, "invalid Bin.rhs");
          free(key);
          free(lhs.id);
          free(rhs.id);
          free(assign_lhs_json);
          return false;
        }
        free(key);
        continue;
      }

      if (sem2sir_op_is_cmp(opid)) {
        if (!capture_json_value_alloc(c, &cmp_rhs_json, &cmp_rhs_len)) {
          err(ctx->in_path, "invalid Bin.rhs");
          free(key);
          free(lhs.id);
          free(rhs.id);
          free(cmp_lhs_json);
          free(cmp_rhs_json);
          return false;
        }
        free(key);
        continue;
      }
      sem2sir_type_id rhs_expected = SEM2SIR_TYPE_INVALID;
      switch (opid) {
      case SEM2SIR_OP_CORE_ADD:
      case SEM2SIR_OP_CORE_SUB:
      case SEM2SIR_OP_CORE_MUL:
      case SEM2SIR_OP_CORE_DIV:
      case SEM2SIR_OP_CORE_REM:
      case SEM2SIR_OP_CORE_SHL:
      case SEM2SIR_OP_CORE_SHR:
      case SEM2SIR_OP_CORE_BITAND:
      case SEM2SIR_OP_CORE_BITOR:
      case SEM2SIR_OP_CORE_BITXOR:
        // Numeric width is committed by the expected result type.
        if (expected != SEM2SIR_TYPE_I32 && expected != SEM2SIR_TYPE_I64) {
          err(ctx->in_path,
              "core.(add|sub|mul|div|rem|shl|shr|bitand|bitor|bitxor) requires expected type i32 or i64 (no inference)");
          free(key);
          free(lhs.id);
          free(rhs.id);
          return false;
        }
        rhs_expected = expected;
        break;
      case SEM2SIR_OP_CORE_EQ:
      case SEM2SIR_OP_CORE_NE:
      case SEM2SIR_OP_CORE_BOOL_AND_SC:
      case SEM2SIR_OP_CORE_BOOL_OR_SC:
        rhs_expected = SEM2SIR_TYPE_BOOL;
        break;
      default:
        err(ctx->in_path, "Bin op not supported in emitter MVP");
        free(key);
        free(lhs.id);
        free(rhs.id);
        return false;
      }
      if (!parse_expr(c, ctx, rhs_expected, &rhs)) {
        free(key);
        free(lhs.id);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid Bin field");
        free(key);
        free(lhs.id);
        free(rhs.id);
        free(cmp_lhs_json);
        free(cmp_rhs_json);
        return false;
      }
    }

    free(key);
  }

  if (!seen_op || !seen_lhs || !seen_rhs) {
    err(ctx->in_path, "Bin requires fields: op, lhs, rhs");
    free(lhs.id);
    free(rhs.id);
    free(assign_lhs_json);
    free(assign_rhs_json);
    free(cmp_lhs_json);
    free(cmp_rhs_json);
    return false;
  }

  if (opid == SEM2SIR_OP_CORE_ASSIGN) {
    // No implicit typing: the surrounding context must commit the result/store type.
    if (expected == SEM2SIR_TYPE_INVALID) {
      err(ctx->in_path, "core.assign requires an expected type (no inference)");
      free(lhs.id);
      free(rhs.id);
      free(assign_lhs_json);
      free(assign_rhs_json);
      return false;
    }
    if (!assign_lhs_json || !assign_rhs_json) {
      err(ctx->in_path, "core.assign requires fields: lhs, rhs");
      free(lhs.id);
      free(rhs.id);
      free(assign_lhs_json);
      free(assign_rhs_json);
      return false;
    }
    if (!ctx->effects) {
      err(ctx->in_path, "core.assign used in expression position requires an effect context");
      free(lhs.id);
      free(rhs.id);
      free(assign_lhs_json);
      free(assign_rhs_json);
      return false;
    }

    // Commit store type from the lvalue (not from the RHS, and not from ambient context).
    sem2sir_type_id assign_store_ty = SEM2SIR_TYPE_INVALID;
    sem2sir_type_id lhs_ptr_of = SEM2SIR_TYPE_INVALID;
    {
      GritJsonCursor lc0 = grit_json_cursor(assign_lhs_json, assign_lhs_len);
      if (!grit_json_skip_ws(&lc0) || !grit_json_consume_char(&lc0, '{')) {
        err(ctx->in_path, "invalid Bin.lhs");
        free(lhs.id);
        free(rhs.id);
        free(assign_lhs_json);
        free(assign_rhs_json);
        return false;
      }
      char *k0_key = NULL;
      if (!json_expect_key(&lc0, &k0_key) || strcmp(k0_key, "k") != 0) {
        free(k0_key);
        err(ctx->in_path, "invalid Bin.lhs");
        free(lhs.id);
        free(rhs.id);
        free(assign_lhs_json);
        free(assign_rhs_json);
        return false;
      }
      free(k0_key);
      char *k0 = NULL;
      if (!grit_json_parse_string_alloc(&lc0, &k0)) {
        err(ctx->in_path, "invalid Bin.lhs");
        free(lhs.id);
        free(rhs.id);
        free(assign_lhs_json);
        free(assign_rhs_json);
        return false;
      }
      bool lhs_is_name = (strcmp(k0, "Name") == 0);
      bool lhs_is_deref = (strcmp(k0, "Deref") == 0);
      free(k0);

      if (lhs_is_name) {
        char *lhs_name = NULL;
        GritJsonCursor lcn = grit_json_cursor(assign_lhs_json, assign_lhs_len);
        if (!parse_name_id_only(&lcn, ctx, &lhs_name)) {
          free(lhs.id);
          free(rhs.id);
          free(assign_lhs_json);
          free(assign_rhs_json);
          return false;
        }
        sem2sir_type_id lhs_ty = SEM2SIR_TYPE_INVALID;
        bool lhs_is_slot = false;
        if (!locals_lookup(ctx, lhs_name, &lhs_ty, &lhs_ptr_of, NULL, &lhs_is_slot)) {
          err(ctx->in_path, "assignment lhs refers to unknown local");
          free(lhs_name);
          free(lhs.id);
          free(rhs.id);
          free(assign_lhs_json);
          free(assign_rhs_json);
          return false;
        }
        if (!lhs_is_slot) {
          err(ctx->in_path, "assignment lhs must be a slot-backed local in emitter MVP");
          free(lhs_name);
          free(lhs.id);
          free(rhs.id);
          free(assign_lhs_json);
          free(assign_rhs_json);
          return false;
        }
        assign_store_ty = lhs_ty;
        free(lhs_name);
      } else if (lhs_is_deref) {
        sem2sir_type_id probed = probe_deref_expr_pointee_no_expected(assign_lhs_json, assign_lhs_len, ctx);
        if (probed != SEM2SIR_TYPE_INVALID) {
          if (probed == SEM2SIR_TYPE_VOID) {
            err(ctx->in_path, "cannot assign through ptr(void) (opaque pointer)");
            free(lhs.id);
            free(rhs.id);
            free(assign_lhs_json);
            free(assign_rhs_json);
            return false;
          }
          assign_store_ty = probed;
        } else {
          assign_store_ty = ctx->default_ptr_pointee;
          if (assign_store_ty == SEM2SIR_TYPE_INVALID) {
            err(ctx->in_path,
                "assignment to Deref(lhs) requires meta.types['@default.ptr.pointee'/'__default_ptr_pointee'] unless the pointer is explicitly typed ptr(T)");
            free(lhs.id);
            free(rhs.id);
            free(assign_lhs_json);
            free(assign_rhs_json);
            return false;
          }
        }
      } else {
        err(ctx->in_path, "assignment lhs must be Name(id) or Deref(expr) in emitter MVP");
        free(lhs.id);
        free(rhs.id);
        free(assign_lhs_json);
        free(assign_rhs_json);
        return false;
      }
    }

    if (expected != assign_store_ty) {
      err(ctx->in_path, "core.assign expected type must match committed lhs store type");
      free(lhs.id);
      free(rhs.id);
      free(assign_lhs_json);
      free(assign_rhs_json);
      return false;
    }

    if (!type_supports_slot_storage(assign_store_ty)) {
      err(ctx->in_path, "assignment type not supported for store in emitter MVP");
      free(lhs.id);
      free(rhs.id);
      free(assign_lhs_json);
      free(assign_rhs_json);
      return false;
    }

    SirExpr assign_addr = {0};
    GritJsonCursor lvc = grit_json_cursor(assign_lhs_json, assign_lhs_len);
    if (!parse_lvalue_addr(&lvc, ctx, assign_store_ty, &assign_addr)) {
      free(lhs.id);
      free(rhs.id);
      free(assign_lhs_json);
      free(assign_rhs_json);
      return false;
    }

    GritJsonCursor rvc = grit_json_cursor(assign_rhs_json, assign_rhs_len);
    if (!parse_expr(&rvc, ctx, assign_store_ty, &rhs)) {
      free(lhs.id);
      free(assign_addr.id);
      free(assign_lhs_json);
      free(assign_rhs_json);
      return false;
    }
    if (rhs.type != assign_store_ty) {
      err(ctx->in_path, "assignment rhs type mismatch");
      free(lhs.id);
      free(rhs.id);
      free(assign_addr.id);
      free(assign_lhs_json);
      free(assign_rhs_json);
      return false;
    }

    if (assign_store_ty == SEM2SIR_TYPE_PTR && lhs_ptr_of != SEM2SIR_TYPE_INVALID && lhs_ptr_of != SEM2SIR_TYPE_VOID) {
      if (rhs.ptr_of != lhs_ptr_of) {
        err(ctx->in_path, "assignment rhs pointer pointee does not match destination ptr(T)");
        free(lhs.id);
        free(rhs.id);
        free(assign_addr.id);
        return false;
      }
    }

    free(assign_lhs_json);
    free(assign_rhs_json);
    assign_lhs_json = NULL;
    assign_rhs_json = NULL;

    if (!emit_type_if_needed(ctx, SEM2SIR_TYPE_PTR) || !emit_type_if_needed(ctx, assign_store_ty)) {
      free(lhs.id);
      free(rhs.id);
      free(assign_addr.id);
      return false;
    }

    const char *store_tag = type_store_tag(assign_store_ty);
    int align = type_align_bytes(assign_store_ty);
    if (!store_tag || align == 0) {
      err(ctx->in_path, "assignment type not supported for store");
      free(lhs.id);
      free(rhs.id);
      free(assign_addr.id);
      return false;
    }

    char *st_id = new_node_id(ctx);
    fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
    emit_json_string(ctx->out, st_id);
    fprintf(ctx->out, ",\"tag\":");
    emit_json_string(ctx->out, store_tag);
    fprintf(ctx->out, ",\"fields\":{\"addr\":{\"t\":\"ref\",\"id\":");
    emit_json_string(ctx->out, assign_addr.id);
    fprintf(ctx->out, "},\"value\":{\"t\":\"ref\",\"id\":");
    emit_json_string(ctx->out, rhs.id);
    fprintf(ctx->out, "},\"align\":%d}}\n", align);

    free(assign_addr.id);
    assign_addr.id = NULL;

    if (!stmtlist_push(ctx->effects, st_id)) {
      err(ctx->in_path, "OOM recording assignment effect");
      free(st_id);
      free(lhs.id);
      free(rhs.id);
      return false;
    }

    // Expression result is the RHS value.
    free(lhs.id);
    out->id = rhs.id;
    out->type = assign_store_ty;
    out->ptr_of = rhs.ptr_of;
    out->sir_type_id = rhs.sir_type_id;
    rhs.id = NULL;
    return true;
  }

  if (sem2sir_op_is_cmp(opid)) {
    if (!cmp_lhs_json || !cmp_rhs_json) {
      err(ctx->in_path, "comparison Bin requires both lhs and rhs");
      free(cmp_lhs_json);
      free(cmp_rhs_json);
      return false;
    }

    sem2sir_type_id lhs_probe = probe_expr_type_no_expected(cmp_lhs_json, cmp_lhs_len, ctx);
    sem2sir_type_id rhs_probe = probe_expr_type_no_expected(cmp_rhs_json, cmp_rhs_len, ctx);

    if (lhs_probe != SEM2SIR_TYPE_INVALID && rhs_probe != SEM2SIR_TYPE_INVALID) {
      if (lhs_probe != rhs_probe) {
        err(ctx->in_path, "comparison operands have mismatched types (no implicit coercions)");
        free(cmp_lhs_json);
        free(cmp_rhs_json);
        return false;
      }
      cmp_operand_ty = lhs_probe;
    } else if (lhs_probe != SEM2SIR_TYPE_INVALID) {
      cmp_operand_ty = lhs_probe;
    } else if (rhs_probe != SEM2SIR_TYPE_INVALID) {
      cmp_operand_ty = rhs_probe;
    } else {
      err(ctx->in_path,
          "comparison requires at least one operand with an explicit type (e.g. Name of typed local); no inference for literals");
      free(cmp_lhs_json);
      free(cmp_rhs_json);
      return false;
    }

    if (cmp_operand_ty != SEM2SIR_TYPE_I32 && cmp_operand_ty != SEM2SIR_TYPE_I64) {
      err(ctx->in_path, "comparison operands must be i32 or i64 in emitter MVP");
      free(cmp_lhs_json);
      free(cmp_rhs_json);
      return false;
    }

    GritJsonCursor lc = grit_json_cursor(cmp_lhs_json, cmp_lhs_len);
    if (!parse_expr(&lc, ctx, cmp_operand_ty, &lhs)) {
      free(cmp_lhs_json);
      free(cmp_rhs_json);
      return false;
    }
    GritJsonCursor rc = grit_json_cursor(cmp_rhs_json, cmp_rhs_len);
    if (!parse_expr(&rc, ctx, cmp_operand_ty, &rhs)) {
      free(lhs.id);
      free(cmp_lhs_json);
      free(cmp_rhs_json);
      return false;
    }
    free(cmp_lhs_json);
    free(cmp_rhs_json);
    cmp_lhs_json = NULL;
    cmp_rhs_json = NULL;

    if (lhs.type != cmp_operand_ty || rhs.type != cmp_operand_ty) {
      err(ctx->in_path, "comparison operands must match committed operand type");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
  }

  const char *tag = NULL;
  sem2sir_type_id result = SEM2SIR_TYPE_INVALID;
  bool rhs_is_sem_branch_val = false;
  switch (opid) {
  case SEM2SIR_OP_CORE_ADD:
    if (expected != SEM2SIR_TYPE_I32 && expected != SEM2SIR_TYPE_I64) {
      err(ctx->in_path, "core.add requires expected type i32 or i64 (no inference)");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    if (lhs.type != expected || rhs.type != expected) {
      err(ctx->in_path, "Bin operands must match expected type for core.add");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    tag = (expected == SEM2SIR_TYPE_I32) ? "i32.add" : "i64.add";
    result = expected;
    break;
  case SEM2SIR_OP_CORE_SUB:
    if (expected != SEM2SIR_TYPE_I32 && expected != SEM2SIR_TYPE_I64) {
      err(ctx->in_path, "core.sub requires expected type i32 or i64 (no inference)");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    if (lhs.type != expected || rhs.type != expected) {
      err(ctx->in_path, "Bin operands must match expected type for core.sub");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    tag = (expected == SEM2SIR_TYPE_I32) ? "i32.sub" : "i64.sub";
    result = expected;
    break;
  case SEM2SIR_OP_CORE_MUL:
    if (expected != SEM2SIR_TYPE_I32 && expected != SEM2SIR_TYPE_I64) {
      err(ctx->in_path, "core.mul requires expected type i32 or i64 (no inference)");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    if (lhs.type != expected || rhs.type != expected) {
      err(ctx->in_path, "Bin operands must match expected type for core.mul");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    tag = (expected == SEM2SIR_TYPE_I32) ? "i32.mul" : "i64.mul";
    result = expected;
    break;
  case SEM2SIR_OP_CORE_DIV:
    if (expected != SEM2SIR_TYPE_I32 && expected != SEM2SIR_TYPE_I64) {
      err(ctx->in_path, "core.div requires expected type i32 or i64 (no inference)");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    if (lhs.type != expected || rhs.type != expected) {
      err(ctx->in_path, "Bin operands must match expected type for core.div");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    // MVP choice: signed, trapping division.
    tag = (expected == SEM2SIR_TYPE_I32) ? "i32.div.s.trap" : "i64.div.s.trap";
    result = expected;
    break;
  case SEM2SIR_OP_CORE_REM:
    if (expected != SEM2SIR_TYPE_I32 && expected != SEM2SIR_TYPE_I64) {
      err(ctx->in_path, "core.rem requires expected type i32 or i64 (no inference)");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    if (lhs.type != expected || rhs.type != expected) {
      err(ctx->in_path, "Bin operands must match expected type for core.rem");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    // MVP choice: unsigned remainder, trapping on divisor=0.
    tag = (expected == SEM2SIR_TYPE_I32) ? "i32.rem.u.trap" : "i64.rem.u.trap";
    result = expected;
    break;
  case SEM2SIR_OP_CORE_SHL:
    if (expected != SEM2SIR_TYPE_I32 && expected != SEM2SIR_TYPE_I64) {
      err(ctx->in_path, "core.shl requires expected type i32 or i64 (no inference)");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    if (lhs.type != expected || rhs.type != expected) {
      err(ctx->in_path, "Bin operands must match expected type for core.shl");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    tag = (expected == SEM2SIR_TYPE_I32) ? "i32.shl" : "i64.shl";
    result = expected;
    break;
  case SEM2SIR_OP_CORE_SHR:
    if (expected != SEM2SIR_TYPE_I32 && expected != SEM2SIR_TYPE_I64) {
      err(ctx->in_path, "core.shr requires expected type i32 or i64 (no inference)");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    if (lhs.type != expected || rhs.type != expected) {
      err(ctx->in_path, "Bin operands must match expected type for core.shr");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    tag = (expected == SEM2SIR_TYPE_I32) ? "i32.shr.u" : "i64.shr.u";
    result = expected;
    break;
  case SEM2SIR_OP_CORE_BITAND:
    if (expected != SEM2SIR_TYPE_I32 && expected != SEM2SIR_TYPE_I64) {
      err(ctx->in_path, "core.bitand requires expected type i32 or i64 (no inference)");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    if (lhs.type != expected || rhs.type != expected) {
      err(ctx->in_path, "Bin operands must match expected type for core.bitand");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    tag = (expected == SEM2SIR_TYPE_I32) ? "i32.and" : "i64.and";
    result = expected;
    break;
  case SEM2SIR_OP_CORE_BITOR:
    if (expected != SEM2SIR_TYPE_I32 && expected != SEM2SIR_TYPE_I64) {
      err(ctx->in_path, "core.bitor requires expected type i32 or i64 (no inference)");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    if (lhs.type != expected || rhs.type != expected) {
      err(ctx->in_path, "Bin operands must match expected type for core.bitor");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    tag = (expected == SEM2SIR_TYPE_I32) ? "i32.or" : "i64.or";
    result = expected;
    break;
  case SEM2SIR_OP_CORE_BITXOR:
    if (expected != SEM2SIR_TYPE_I32 && expected != SEM2SIR_TYPE_I64) {
      err(ctx->in_path, "core.bitxor requires expected type i32 or i64 (no inference)");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    if (lhs.type != expected || rhs.type != expected) {
      err(ctx->in_path, "Bin operands must match expected type for core.bitxor");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    tag = (expected == SEM2SIR_TYPE_I32) ? "i32.xor" : "i64.xor";
    result = expected;
    break;
  case SEM2SIR_OP_CORE_EQ:
    if (lhs.type != rhs.type || (lhs.type != SEM2SIR_TYPE_I32 && lhs.type != SEM2SIR_TYPE_I64)) {
      err(ctx->in_path, "Bin operands must match and be i32/i64 for core.eq (no inference)");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    tag = (lhs.type == SEM2SIR_TYPE_I32) ? "i32.cmp.eq" : "i64.cmp.eq";
    result = SEM2SIR_TYPE_BOOL;
    break;
  case SEM2SIR_OP_CORE_NE:
    if (lhs.type != rhs.type || (lhs.type != SEM2SIR_TYPE_I32 && lhs.type != SEM2SIR_TYPE_I64)) {
      err(ctx->in_path, "Bin operands must match and be i32/i64 for core.ne (no inference)");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    tag = (lhs.type == SEM2SIR_TYPE_I32) ? "i32.cmp.ne" : "i64.cmp.ne";
    result = SEM2SIR_TYPE_BOOL;
    break;
  case SEM2SIR_OP_CORE_LT:
    if (lhs.type != rhs.type || (lhs.type != SEM2SIR_TYPE_I32 && lhs.type != SEM2SIR_TYPE_I64)) {
      err(ctx->in_path, "Bin operands must match and be i32/i64 for core.lt (no inference)");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    tag = (lhs.type == SEM2SIR_TYPE_I32) ? "i32.cmp.slt" : "i64.cmp.slt";
    result = SEM2SIR_TYPE_BOOL;
    break;
  case SEM2SIR_OP_CORE_LTE:
    if (lhs.type != rhs.type || (lhs.type != SEM2SIR_TYPE_I32 && lhs.type != SEM2SIR_TYPE_I64)) {
      err(ctx->in_path, "Bin operands must match and be i32/i64 for core.lte (no inference)");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    tag = (lhs.type == SEM2SIR_TYPE_I32) ? "i32.cmp.sle" : "i64.cmp.sle";
    result = SEM2SIR_TYPE_BOOL;
    break;
  case SEM2SIR_OP_CORE_GT:
    if (lhs.type != rhs.type || (lhs.type != SEM2SIR_TYPE_I32 && lhs.type != SEM2SIR_TYPE_I64)) {
      err(ctx->in_path, "Bin operands must match and be i32/i64 for core.gt (no inference)");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    tag = (lhs.type == SEM2SIR_TYPE_I32) ? "i32.cmp.sgt" : "i64.cmp.sgt";
    result = SEM2SIR_TYPE_BOOL;
    break;
  case SEM2SIR_OP_CORE_GTE:
    if (lhs.type != rhs.type || (lhs.type != SEM2SIR_TYPE_I32 && lhs.type != SEM2SIR_TYPE_I64)) {
      err(ctx->in_path, "Bin operands must match and be i32/i64 for core.gte (no inference)");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    tag = (lhs.type == SEM2SIR_TYPE_I32) ? "i32.cmp.sge" : "i64.cmp.sge";
    result = SEM2SIR_TYPE_BOOL;
    break;
  case SEM2SIR_OP_CORE_BOOL_AND_SC:
    if (lhs.type != SEM2SIR_TYPE_BOOL || rhs.type != SEM2SIR_TYPE_BOOL) {
      err(ctx->in_path, "Bin operands must be bool for core.bool.and_sc");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    tag = "sem.and_sc";
    result = SEM2SIR_TYPE_BOOL;
    rhs_is_sem_branch_val = true;
    break;
  case SEM2SIR_OP_CORE_BOOL_OR_SC:
    if (lhs.type != SEM2SIR_TYPE_BOOL || rhs.type != SEM2SIR_TYPE_BOOL) {
      err(ctx->in_path, "Bin operands must be bool for core.bool.or_sc");
      free(lhs.id);
      free(rhs.id);
      return false;
    }
    tag = "sem.or_sc";
    result = SEM2SIR_TYPE_BOOL;
    rhs_is_sem_branch_val = true;
    break;
  default:
    err(ctx->in_path, "Bin op not supported in emitter MVP");
    free(lhs.id);
    free(rhs.id);
    return false;
  }

  if (rhs_is_sem_branch_val) {
    // We must feature-gate sem:v1 if we emit sem.* nodes.
    ctx->meta_sem_v1 = true;
  }

  if (expected != result) {
    err(ctx->in_path, "Bin result type does not match expected type (no implicit coercions)");
    free(lhs.id);
    free(rhs.id);
    return false;
  }

  if (!emit_type_if_needed(ctx, result)) {
    free(lhs.id);
    free(rhs.id);
    return false;
  }

  const char *tid = sir_type_id_for(result);
  if (!tid) {
    err(ctx->in_path, "unsupported result type");
    free(lhs.id);
    free(rhs.id);
    return false;
  }

  char *nid = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, nid);
  fprintf(ctx->out, ",\"tag\":");
  emit_json_string(ctx->out, tag);
  fprintf(ctx->out, ",\"type_ref\":");
  emit_json_string(ctx->out, tid);
  fprintf(ctx->out, ",\"fields\":{\"args\":[{\"t\":\"ref\",\"id\":");
  emit_json_string(ctx->out, lhs.id);
  fprintf(ctx->out, "},");
  if (rhs_is_sem_branch_val) {
    // sem:v1 branch operand encoding: {kind:"val", v:VALUE}
    fprintf(ctx->out, "{\"kind\":\"val\",\"v\":{\"t\":\"ref\",\"id\":");
    emit_json_string(ctx->out, rhs.id);
    fprintf(ctx->out, "}}");
  } else {
    fprintf(ctx->out, "{\"t\":\"ref\",\"id\":");
    emit_json_string(ctx->out, rhs.id);
    fprintf(ctx->out, "}");
  }
  fprintf(ctx->out, "]}}\n");

  free(lhs.id);
  free(rhs.id);

  out->id = nid;
  out->type = result;
  return true;
}

