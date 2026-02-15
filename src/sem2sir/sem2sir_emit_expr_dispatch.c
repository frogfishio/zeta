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
  if (strcmp(k, "F64") == 0) {
    free(k);
    return parse_expr_f64(c, ctx, expected, out);
  }
  if (strcmp(k, "F32") == 0) {
    free(k);
    return parse_expr_f32(c, ctx, expected, out);
  }
  if (strcmp(k, "UnitVal") == 0) {
    free(k);
    return parse_expr_unitval(c, ctx, expected, out);
  }
  if (strcmp(k, "Bytes") == 0) {
    free(k);
    return parse_expr_bytes(c, ctx, expected, out);
  }
  if (strcmp(k, "StringUtf8") == 0) {
    free(k);
    return parse_expr_string_utf8(c, ctx, expected, out);
  }
  if (strcmp(k, "CStr") == 0) {
    free(k);
    return parse_expr_cstr(c, ctx, expected, out);
  }
  if (strcmp(k, "Char") == 0) {
    free(k);
    return parse_expr_char(c, ctx, expected, out);
  }
  if (strcmp(k, "ZExtI64FromI32") == 0) {
    free(k);
    return parse_expr_zext_i64_from_i32(c, ctx, expected, out);
  }
  if (strcmp(k, "SExtI64FromI32") == 0) {
    free(k);
    return parse_expr_sext_i64_from_i32(c, ctx, expected, out);
  }
  if (strcmp(k, "TruncI32FromI64") == 0) {
    free(k);
    return parse_expr_trunc_i32_from_i64(c, ctx, expected, out);
  }
  if (strcmp(k, "F64FromI32S") == 0) {
    free(k);
    return parse_expr_f64_from_i32_s(c, ctx, expected, out);
  }
  if (strcmp(k, "F32FromI32S") == 0) {
    free(k);
    return parse_expr_f32_from_i32_s(c, ctx, expected, out);
  }
  if (strcmp(k, "F64FromI32U") == 0) {
    free(k);
    return parse_expr_f64_from_i32_u(c, ctx, expected, out);
  }
  if (strcmp(k, "F32FromI32U") == 0) {
    free(k);
    return parse_expr_f32_from_i32_u(c, ctx, expected, out);
  }
  if (strcmp(k, "TruncSatI32FromF64S") == 0) {
    free(k);
    return parse_expr_trunc_sat_i32_from_f64_s(c, ctx, expected, out);
  }
  if (strcmp(k, "TruncSatI32FromF32S") == 0) {
    free(k);
    return parse_expr_trunc_sat_i32_from_f32_s(c, ctx, expected, out);
  }
  if (strcmp(k, "TruncSatI32FromF64U") == 0) {
    free(k);
    return parse_expr_trunc_sat_i32_from_f64_u(c, ctx, expected, out);
  }
  if (strcmp(k, "TruncSatI32FromF32U") == 0) {
    free(k);
    return parse_expr_trunc_sat_i32_from_f32_u(c, ctx, expected, out);
  }
  if (strcmp(k, "F64FromI64S") == 0) {
    free(k);
    return parse_expr_f64_from_i64_s(c, ctx, expected, out);
  }
  if (strcmp(k, "F32FromI64S") == 0) {
    free(k);
    return parse_expr_f32_from_i64_s(c, ctx, expected, out);
  }
  if (strcmp(k, "F64FromI64U") == 0) {
    free(k);
    return parse_expr_f64_from_i64_u(c, ctx, expected, out);
  }
  if (strcmp(k, "F32FromI64U") == 0) {
    free(k);
    return parse_expr_f32_from_i64_u(c, ctx, expected, out);
  }
  if (strcmp(k, "TruncSatI64FromF64S") == 0) {
    free(k);
    return parse_expr_trunc_sat_i64_from_f64_s(c, ctx, expected, out);
  }
  if (strcmp(k, "TruncSatI64FromF32S") == 0) {
    free(k);
    return parse_expr_trunc_sat_i64_from_f32_s(c, ctx, expected, out);
  }
  if (strcmp(k, "TruncSatI64FromF64U") == 0) {
    free(k);
    return parse_expr_trunc_sat_i64_from_f64_u(c, ctx, expected, out);
  }
  if (strcmp(k, "TruncSatI64FromF32U") == 0) {
    free(k);
    return parse_expr_trunc_sat_i64_from_f32_u(c, ctx, expected, out);
  }
  if (strcmp(k, "PtrFromI64") == 0) {
    free(k);
    return parse_expr_ptr_from_i64(c, ctx, expected, out);
  }
  if (strcmp(k, "I64FromPtr") == 0) {
    free(k);
    return parse_expr_i64_from_ptr(c, ctx, expected, out);
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

