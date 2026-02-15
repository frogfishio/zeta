#include "sem2sir_emit_internal.h"

bool parse_stmt_bin_assign_emit_store(GritJsonCursor *c, EmitCtx *ctx, char **out_store_id) {
  *out_store_id = NULL;

  bool seen_op = false;
  bool seen_lhs = false;
  bool seen_rhs = false;
  sem2sir_op_id opid = SEM2SIR_OP_INVALID;
  char *lhs_json = NULL;
  size_t lhs_len = 0;
  char *rhs_json = NULL;
  size_t rhs_len = 0;
  sem2sir_type_id store_ty = SEM2SIR_TYPE_INVALID;
  SirExpr addr = {0};
  SirExpr rhs = {0};

  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in Bin");
      free(lhs_json);
      free(rhs_json);
      free(addr.id);
      free(rhs.id);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in Bin");
      free(lhs_json);
      free(rhs_json);
      free(addr.id);
      free(rhs.id);
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid Bin key");
      free(lhs_json);
      free(rhs_json);
      free(addr.id);
      free(rhs.id);
      return false;
    }

    if (strcmp(key, "op") == 0) {
      seen_op = true;
      char *op_str = NULL;
      if (!grit_json_parse_string_alloc(c, &op_str)) {
        err(ctx->in_path, "Bin.op must be string");
        free(key);
        free(lhs_json);
        free(rhs_json);
        free(addr.id);
        free(rhs.id);
        return false;
      }
      opid = sem2sir_op_parse(op_str, strlen(op_str));
      free(op_str);
      if (opid != SEM2SIR_OP_CORE_ASSIGN) {
        err(ctx->in_path, "only Bin(op=core.assign) is supported as a statement in emitter MVP");
        free(key);
        free(lhs_json);
        free(rhs_json);
        free(addr.id);
        free(rhs.id);
        return false;
      }
    } else if (strcmp(key, "lhs") == 0) {
      if (!seen_op) {
        err(ctx->in_path, "Bin.op must appear before lhs (no implicit context)");
        free(key);
        free(lhs_json);
        free(rhs_json);
        free(addr.id);
        free(rhs.id);
        return false;
      }
      seen_lhs = true;
      free(lhs_json);
      lhs_json = NULL;
      if (!capture_json_value_alloc(c, &lhs_json, &lhs_len)) {
        err(ctx->in_path, "invalid Bin.lhs");
        free(key);
        free(rhs.id);
        return false;
      }
    } else if (strcmp(key, "rhs") == 0) {
      if (!seen_op || opid != SEM2SIR_OP_CORE_ASSIGN) {
        err(ctx->in_path, "Bin.op must appear before rhs (no implicit context)");
        free(key);
        free(lhs_json);
        free(rhs_json);
        free(addr.id);
        free(rhs.id);
        return false;
      }
      seen_rhs = true;
      free(rhs_json);
      rhs_json = NULL;
      if (!capture_json_value_alloc(c, &rhs_json, &rhs_len)) {
        err(ctx->in_path, "invalid Bin.rhs");
        free(key);
        free(lhs_json);
        free(rhs.id);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid Bin field");
        free(key);
        free(lhs_json);
        free(rhs_json);
        free(addr.id);
        free(rhs.id);
        return false;
      }
    }

    free(key);
  }

  if (!seen_op || !seen_lhs || !seen_rhs || opid != SEM2SIR_OP_CORE_ASSIGN) {
    err(ctx->in_path, "assignment statement requires fields: op, lhs, rhs");
    free(lhs_json);
    free(rhs_json);
    free(addr.id);
    free(rhs.id);
    return false;
  }

  if (!lhs_json || !rhs_json) {
    err(ctx->in_path, "assignment statement requires fields: lhs, rhs");
    free(lhs_json);
    free(rhs_json);
    free(addr.id);
    free(rhs.id);
    return false;
  }

  // Commit store type.
  GritJsonCursor lc0 = grit_json_cursor(lhs_json, lhs_len);
  if (!grit_json_skip_ws(&lc0) || !grit_json_consume_char(&lc0, '{')) {
    err(ctx->in_path, "invalid Bin.lhs");
    free(lhs_json);
    free(rhs_json);
    free(addr.id);
    free(rhs.id);
    return false;
  }
  char *k0_key = NULL;
  if (!json_expect_key(&lc0, &k0_key) || strcmp(k0_key, "k") != 0) {
    free(k0_key);
    err(ctx->in_path, "invalid Bin.lhs");
    free(lhs_json);
    free(rhs_json);
    free(addr.id);
    free(rhs.id);
    return false;
  }
  free(k0_key);
  char *k0 = NULL;
  if (!grit_json_parse_string_alloc(&lc0, &k0)) {
    err(ctx->in_path, "invalid Bin.lhs");
    free(lhs_json);
    free(rhs_json);
    free(addr.id);
    free(rhs.id);
    return false;
  }
  bool lhs_is_name = (strcmp(k0, "Name") == 0);
  bool lhs_is_deref = (strcmp(k0, "Deref") == 0);
  free(k0);

  sem2sir_type_id lhs_ptr_of = SEM2SIR_TYPE_INVALID;
  if (lhs_is_name) {
    char *lhs_name = NULL;
    GritJsonCursor lcn = grit_json_cursor(lhs_json, lhs_len);
    if (!parse_name_id_only(&lcn, ctx, &lhs_name)) {
      free(lhs_json);
      free(rhs_json);
      free(addr.id);
      free(rhs.id);
      return false;
    }
    sem2sir_type_id lhs_ty = SEM2SIR_TYPE_INVALID;
    bool lhs_is_slot = false;
    if (!locals_lookup(ctx, lhs_name, &lhs_ty, &lhs_ptr_of, NULL, &lhs_is_slot)) {
      err(ctx->in_path, "assignment lhs refers to unknown local");
      free(lhs_name);
      free(lhs_json);
      free(rhs_json);
      free(addr.id);
      free(rhs.id);
      return false;
    }
    if (!lhs_is_slot) {
      err(ctx->in_path, "assignment lhs must be a slot-backed local in emitter MVP");
      free(lhs_name);
      free(lhs_json);
      free(rhs_json);
      free(addr.id);
      free(rhs.id);
      return false;
    }
    store_ty = lhs_ty;
    free(lhs_name);
  } else if (lhs_is_deref) {
    // Prefer explicitly committed ptr(T) pointee typing when the lvalue is Deref(expr).
    // This avoids requiring @default.ptr.pointee when expr is already typed as ptr(T).
    sem2sir_type_id probed = probe_deref_expr_pointee_no_expected(lhs_json, lhs_len, ctx);
    if (probed != SEM2SIR_TYPE_INVALID) {
      if (probed == SEM2SIR_TYPE_VOID) {
        err(ctx->in_path, "cannot assign through ptr(void) (opaque pointer)");
        free(lhs_json);
        free(rhs_json);
        free(addr.id);
        free(rhs.id);
        return false;
      }
      store_ty = probed;
    } else {
      store_ty = ctx->default_ptr_pointee;
      if (store_ty == SEM2SIR_TYPE_INVALID) {
        err(ctx->in_path,
            "assignment to Deref(lhs) requires meta.types['@default.ptr.pointee'/'__default_ptr_pointee'] unless the pointer is explicitly typed ptr(T)");
        free(lhs_json);
        free(rhs_json);
        free(addr.id);
        free(rhs.id);
        return false;
      }
    }
  } else {
    err(ctx->in_path, "assignment lhs must be Name(id) or Deref(expr) in emitter MVP");
    free(lhs_json);
    free(rhs_json);
    free(addr.id);
    free(rhs.id);
    return false;
  }

  if (!type_supports_slot_storage(store_ty)) {
    err(ctx->in_path, "assignment type not supported for store in emitter MVP");
    free(lhs_json);
    free(rhs_json);
    free(addr.id);
    free(rhs.id);
    return false;
  }

  // Parse lvalue address first.
  GritJsonCursor lvc = grit_json_cursor(lhs_json, lhs_len);
  if (!parse_lvalue_addr(&lvc, ctx, store_ty, &addr)) {
    free(lhs_json);
    free(rhs_json);
    free(rhs.id);
    return false;
  }
  if (addr.type != SEM2SIR_TYPE_PTR) {
    err(ctx->in_path, "assignment lhs did not produce an address");
    free(lhs_json);
    free(rhs_json);
    free(addr.id);
    free(rhs.id);
    return false;
  }

  // Then parse rhs value.
  GritJsonCursor rc = grit_json_cursor(rhs_json, rhs_len);
  if (!parse_expr(&rc, ctx, store_ty, &rhs)) {
    free(lhs_json);
    free(rhs_json);
    free(addr.id);
    return false;
  }
  if (rhs.type != store_ty) {
    err(ctx->in_path, "assignment rhs type mismatch");
    free(lhs_json);
    free(rhs_json);
    free(addr.id);
    free(rhs.id);
    return false;
  }

  // If storing into an explicitly-typed derived pointer local, enforce pointee agreement.
  if (lhs_is_name && store_ty == SEM2SIR_TYPE_PTR && lhs_ptr_of != SEM2SIR_TYPE_INVALID && lhs_ptr_of != SEM2SIR_TYPE_VOID) {
    if (rhs.ptr_of != lhs_ptr_of) {
      err(ctx->in_path, "assignment rhs pointer pointee does not match destination ptr(T)");
      free(lhs_json);
      free(rhs_json);
      free(addr.id);
      free(rhs.id);
      return false;
    }
  }

  if (!emit_type_if_needed(ctx, SEM2SIR_TYPE_PTR) || !emit_type_if_needed(ctx, store_ty)) {
    free(lhs_json);
    free(rhs_json);
    free(addr.id);
    free(rhs.id);
    return false;
  }

  const char *store_tag = type_store_tag(store_ty);
  int align = type_align_bytes(store_ty);
  if (!store_tag || align == 0) {
    err(ctx->in_path, "assignment lhs type not supported for store");
    free(lhs_json);
    free(rhs_json);
    free(addr.id);
    free(rhs.id);
    return false;
  }

  char *st_id = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, st_id);
  fprintf(ctx->out, ",\"tag\":");
  emit_json_string(ctx->out, store_tag);
  fprintf(ctx->out, ",\"fields\":{\"addr\":{\"t\":\"ref\",\"id\":");
  emit_json_string(ctx->out, addr.id);
  fprintf(ctx->out, "},\"value\":{\"t\":\"ref\",\"id\":");
  emit_json_string(ctx->out, rhs.id);
  fprintf(ctx->out, "},\"align\":%d}}\n", align);

  free(lhs_json);
  free(rhs_json);
  free(addr.id);
  free(rhs.id);
  *out_store_id = st_id;
  return true;
}

