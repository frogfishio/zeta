#include "sem2sir_emit_internal.h"

bool parse_expr_true_false(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, bool v, SirExpr *out) {
  // Consume remaining fields (strict checker already enforced the allowlist).
  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in bool literal");
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in bool literal");
      return false;
    }
    c->p++;
    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid bool literal key");
      return false;
    }
    free(key);
    if (!grit_json_skip_value(c)) {
      err(ctx->in_path, "invalid bool literal field");
      return false;
    }
  }

  if (expected != SEM2SIR_TYPE_BOOL) {
    err(ctx->in_path, "True/False requires expected type bool (no defaults)");
    return false;
  }
  if (!emit_type_if_needed(ctx, SEM2SIR_TYPE_BOOL))
    return false;

  char *nid = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, nid);
  fprintf(ctx->out, ",\"tag\":\"const.bool\",\"type_ref\":\"t:bool\",\"fields\":{\"value\":%d}}\n", v ? 1 : 0);
  out->id = nid;
  out->type = SEM2SIR_TYPE_BOOL;
  return true;
}

bool parse_expr_paren(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  bool seen_expr = false;
  SirExpr inner = {0};
  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in Paren");
      free(inner.id);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in Paren");
      free(inner.id);
      return false;
    }
    c->p++;
    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid Paren key");
      free(inner.id);
      return false;
    }
    if (strcmp(key, "expr") == 0) {
      seen_expr = true;
      if (!parse_expr(c, ctx, expected, &inner)) {
        free(key);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid Paren field");
        free(key);
        free(inner.id);
        return false;
      }
    }
    free(key);
  }

  if (!seen_expr) {
    err(ctx->in_path, "Paren missing required field expr");
    free(inner.id);
    return false;
  }
  *out = inner;
  return true;
}

bool parse_expr_unary_1(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, const char *tag,
                               sem2sir_type_id operand_expected, sem2sir_type_id result, SirExpr *out) {
  // k: <UnaryKind>, expr: <Expr>
  SirExpr inner = {0};

  bool seen_expr = false;
  char ch = 0;

  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in unary expr");
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in unary expr");
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid unary expr key");
      return false;
    }

    if (strcmp(key, "expr") == 0) {
      seen_expr = true;
      if (!parse_expr(c, ctx, operand_expected, &inner)) {
        free(key);
        return false;
      }
      free(key);
      continue;
    }

    // Everything else was validated by sem2sir_check_stage4_file; skip.
    if (!grit_json_skip_value(c)) {
      free(key);
      err(ctx->in_path, "failed to skip unary expr field");
      return false;
    }
    free(key);
  }

  if (!seen_expr) {
    err(ctx->in_path, "unary expr requires field: expr");
    return false;
  }

  if (expected != result) {
    err(ctx->in_path, "unary expr result type does not match expected type (no implicit coercions)");
    free(inner.id);
    return false;
  }

  if (!emit_type_if_needed(ctx, result)) {
    free(inner.id);
    return false;
  }
  const char *tid = sir_type_id_for(result);
  if (!tid) {
    err(ctx->in_path, "unsupported unary result type");
    free(inner.id);
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
  emit_json_string(ctx->out, inner.id);
  fprintf(ctx->out, "}]}}\n");

  free(inner.id);
  out->id = nid;
  out->type = result;
  return true;
}

bool parse_expr_not(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  // Not(expr) : bool -> bool
  return parse_expr_unary_1(c, ctx, expected, "bool.not", SEM2SIR_TYPE_BOOL, SEM2SIR_TYPE_BOOL, out);
}

bool parse_expr_neg(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  if (expected == SEM2SIR_TYPE_I32) {
    return parse_expr_unary_1(c, ctx, expected, "i32.neg", SEM2SIR_TYPE_I32, SEM2SIR_TYPE_I32, out);
  }
  if (expected == SEM2SIR_TYPE_I64) {
    return parse_expr_unary_1(c, ctx, expected, "i64.neg", SEM2SIR_TYPE_I64, SEM2SIR_TYPE_I64, out);
  }
  err(ctx->in_path, "Neg expected type must be i32 or i64 in MVP");
  return false;
}

bool parse_expr_bitnot(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  if (expected == SEM2SIR_TYPE_I32) {
    return parse_expr_unary_1(c, ctx, expected, "i32.not", SEM2SIR_TYPE_I32, SEM2SIR_TYPE_I32, out);
  }
  if (expected == SEM2SIR_TYPE_I64) {
    return parse_expr_unary_1(c, ctx, expected, "i64.not", SEM2SIR_TYPE_I64, SEM2SIR_TYPE_I64, out);
  }
  err(ctx->in_path, "BitNot expected type must be i32 or i64 in MVP");
  return false;
}

