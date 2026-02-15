#include "sem2sir_emit_internal.h"

bool parse_expr(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  out->id = NULL;
  out->type = SEM2SIR_TYPE_INVALID;
  out->ptr_of = SEM2SIR_TYPE_INVALID;
  out->sir_type_id = NULL;

  char *k = NULL;
  if (!parse_node_k_string(c, ctx, &k))
    return false;

  if (strcmp(k, "Int") == 0) {
    free(k);
    return parse_expr_int(c, ctx, expected, out);
  }
  if (strcmp(k, "Name") == 0) {
    free(k);
    return parse_expr_name(c, ctx, expected, out);
  }
  if (strcmp(k, "True") == 0) {
    free(k);
    return parse_expr_true_false(c, ctx, expected, true, out);
  }
  if (strcmp(k, "False") == 0) {
    free(k);
    return parse_expr_true_false(c, ctx, expected, false, out);
  }
  if (strcmp(k, "Paren") == 0) {
    free(k);
    return parse_expr_paren(c, ctx, expected, out);
  }
  if (strcmp(k, "Not") == 0) {
    free(k);
    return parse_expr_not(c, ctx, expected, out);
  }
  if (strcmp(k, "Neg") == 0) {
    free(k);
    return parse_expr_neg(c, ctx, expected, out);
  }
  if (strcmp(k, "BitNot") == 0) {
    free(k);
    return parse_expr_bitnot(c, ctx, expected, out);
  }
  if (strcmp(k, "AddrOf") == 0) {
    free(k);
    return parse_expr_addrof(c, ctx, expected, out);
  }
  if (strcmp(k, "Deref") == 0) {
    free(k);
    return parse_expr_deref(c, ctx, expected, out);
  }
  if (strcmp(k, "Bin") == 0) {
    free(k);
    return parse_expr_bin(c, ctx, expected, out);
  }
  if (strcmp(k, "Call") == 0) {
    free(k);
    return parse_expr_call(c, ctx, expected, out);
  }
  if (strcmp(k, "Match") == 0) {
    free(k);
    return parse_expr_match(c, ctx, expected, out);
  }

  // Any other expression kind is currently unsupported in the MVP emitter.
  err(ctx->in_path, "unsupported expression kind for SIR emission (define it or fail)");
  free(k);
  return false;
}

