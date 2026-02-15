#include "sem2sir_emit_internal.h"

static bool is_hex_bits_strict(const char *s, size_t hex_digits) {
  if (!s)
    return false;
  if (s[0] != '0' || (s[1] != 'x' && s[1] != 'X'))
    return false;
  size_t n = strlen(s);
  if (n != 2 + hex_digits)
    return false;
  for (size_t i = 2; i < n; i++) {
    char ch = s[i];
    bool ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    if (!ok)
      return false;
  }
  return true;
}

static bool emit_const_fbits(EmitCtx *ctx, const char *tag, sem2sir_type_id ty, const char *bits, char **out_id) {
  *out_id = NULL;
  if (!emit_type_if_needed(ctx, ty))
    return false;

  const char *tyid = sir_type_id_for(ty);
  if (!tyid) {
    err(ctx->in_path, "float const type unsupported");
    return false;
  }

  char *id = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, id);
  fprintf(ctx->out, ",\"tag\":");
  emit_json_string(ctx->out, tag);
  fprintf(ctx->out, ",\"type_ref\":");
  emit_json_string(ctx->out, tyid);
  fprintf(ctx->out, ",\"fields\":{\"bits\":");
  emit_json_string(ctx->out, bits);
  fprintf(ctx->out, "}}\n");

  *out_id = id;
  return true;
}

static bool emit_conv_unary(EmitCtx *ctx, const char *tag, sem2sir_type_id dst_ty, const char *arg_id, char **out_id) {
  *out_id = NULL;
  if (!emit_type_if_needed(ctx, dst_ty))
    return false;
  const char *dst_type_ref = sir_type_id_for(dst_ty);
  if (!dst_type_ref) {
    err(ctx->in_path, "conversion dst type unsupported");
    return false;
  }

  char *id = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, id);
  fprintf(ctx->out, ",\"tag\":");
  emit_json_string(ctx->out, tag);
  fprintf(ctx->out, ",\"type_ref\":");
  emit_json_string(ctx->out, dst_type_ref);
  fprintf(ctx->out, ",\"fields\":{\"args\":[{\"t\":\"ref\",\"id\":");
  emit_json_string(ctx->out, arg_id);
  fprintf(ctx->out, "}]}}\n");

  *out_id = id;
  return true;
}

bool parse_expr_f64(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  if (expected != SEM2SIR_TYPE_F64) {
    err(ctx->in_path, "F64 literal must be used in f64 context (no implicit casts)");
    return false;
  }

  bool seen_bits = false;
  char *bits = NULL;
  char ch = 0;

  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in F64");
      free(bits);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in F64");
      free(bits);
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid F64 key");
      free(bits);
      return false;
    }

    if (strcmp(key, "bits") == 0) {
      seen_bits = true;
      if (!parse_tok_text_alloc_strict(c, ctx->in_path, &bits)) {
        free(key);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid F64 field");
        free(key);
        free(bits);
        return false;
      }
    }
    free(key);
  }

  if (!seen_bits || !bits) {
    err(ctx->in_path, "F64 requires field: bits");
    free(bits);
    return false;
  }
  if (!is_hex_bits_strict(bits, 16)) {
    err(ctx->in_path, "F64.bits must be 0x + 16 hex digits (IEEE-754 bits)");
    free(bits);
    return false;
  }

  char *id = NULL;
  bool ok = emit_const_fbits(ctx, "const.f64", SEM2SIR_TYPE_F64, bits, &id);
  free(bits);
  if (!ok)
    return false;

  out->id = id;
  out->type = SEM2SIR_TYPE_F64;
  out->ptr_of = SEM2SIR_TYPE_INVALID;
  out->sir_type_id = sir_type_id_for(SEM2SIR_TYPE_F64);
  return true;
}

bool parse_expr_f32(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  if (expected != SEM2SIR_TYPE_F32) {
    err(ctx->in_path, "F32 literal must be used in f32 context (no implicit casts)");
    return false;
  }

  bool seen_bits = false;
  char *bits = NULL;
  char ch = 0;

  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in F32");
      free(bits);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in F32");
      free(bits);
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid F32 key");
      free(bits);
      return false;
    }

    if (strcmp(key, "bits") == 0) {
      seen_bits = true;
      if (!parse_tok_text_alloc_strict(c, ctx->in_path, &bits)) {
        free(key);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid F32 field");
        free(key);
        free(bits);
        return false;
      }
    }
    free(key);
  }

  if (!seen_bits || !bits) {
    err(ctx->in_path, "F32 requires field: bits");
    free(bits);
    return false;
  }
  if (!is_hex_bits_strict(bits, 8)) {
    err(ctx->in_path, "F32.bits must be 0x + 8 hex digits (IEEE-754 bits)");
    free(bits);
    return false;
  }

  char *id = NULL;
  bool ok = emit_const_fbits(ctx, "const.f32", SEM2SIR_TYPE_F32, bits, &id);
  free(bits);
  if (!ok)
    return false;

  out->id = id;
  out->type = SEM2SIR_TYPE_F32;
  out->ptr_of = SEM2SIR_TYPE_INVALID;
  out->sir_type_id = sir_type_id_for(SEM2SIR_TYPE_F32);
  return true;
}

bool parse_expr_unitval(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  if (expected != SEM2SIR_TYPE_VOID && expected != SEM2SIR_TYPE_INVALID) {
    err(ctx->in_path, "UnitVal may only appear in void context");
    return false;
  }
  if (!skip_remaining_object_fields(c, ctx, "UnitVal"))
    return false;

  out->id = NULL;
  out->type = SEM2SIR_TYPE_VOID;
  out->ptr_of = SEM2SIR_TYPE_INVALID;
  out->sir_type_id = sir_type_id_for(SEM2SIR_TYPE_VOID);
  return true;
}

bool parse_expr_zext_i64_from_i32(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  if (expected != SEM2SIR_TYPE_I64) {
    err(ctx->in_path, "ZExtI64FromI32 must be used in i64 context");
    return false;
  }

  bool seen_expr = false;
  SirExpr inner = {0};
  char ch = 0;

  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in ZExtI64FromI32");
      free(inner.id);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in ZExtI64FromI32");
      free(inner.id);
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid ZExtI64FromI32 key");
      free(inner.id);
      return false;
    }

    if (strcmp(key, "expr") == 0) {
      seen_expr = true;
      if (!parse_expr(c, ctx, SEM2SIR_TYPE_I32, &inner)) {
        free(key);
        free(inner.id);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid ZExtI64FromI32 field");
        free(key);
        free(inner.id);
        return false;
      }
    }

    free(key);
  }

  if (!seen_expr || !inner.id) {
    err(ctx->in_path, "ZExtI64FromI32 requires field: expr");
    free(inner.id);
    return false;
  }

  char *id = NULL;
  bool ok = emit_conv_unary(ctx, "i64.zext.i32", SEM2SIR_TYPE_I64, inner.id, &id);
  free(inner.id);
  if (!ok)
    return false;

  out->id = id;
  out->type = SEM2SIR_TYPE_I64;
  out->ptr_of = SEM2SIR_TYPE_INVALID;
  out->sir_type_id = sir_type_id_for(SEM2SIR_TYPE_I64);
  return true;
}

bool parse_expr_sext_i64_from_i32(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  if (expected != SEM2SIR_TYPE_I64) {
    err(ctx->in_path, "SExtI64FromI32 must be used in i64 context");
    return false;
  }

  bool seen_expr = false;
  SirExpr inner = {0};
  char ch = 0;

  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in SExtI64FromI32");
      free(inner.id);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in SExtI64FromI32");
      free(inner.id);
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid SExtI64FromI32 key");
      free(inner.id);
      return false;
    }

    if (strcmp(key, "expr") == 0) {
      seen_expr = true;
      if (!parse_expr(c, ctx, SEM2SIR_TYPE_I32, &inner)) {
        free(key);
        free(inner.id);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid SExtI64FromI32 field");
        free(key);
        free(inner.id);
        return false;
      }
    }

    free(key);
  }

  if (!seen_expr || !inner.id) {
    err(ctx->in_path, "SExtI64FromI32 requires field: expr");
    free(inner.id);
    return false;
  }

  char *id = NULL;
  bool ok = emit_conv_unary(ctx, "i64.sext.i32", SEM2SIR_TYPE_I64, inner.id, &id);
  free(inner.id);
  if (!ok)
    return false;

  out->id = id;
  out->type = SEM2SIR_TYPE_I64;
  out->ptr_of = SEM2SIR_TYPE_INVALID;
  out->sir_type_id = sir_type_id_for(SEM2SIR_TYPE_I64);
  return true;
}

bool parse_expr_trunc_i32_from_i64(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  if (expected != SEM2SIR_TYPE_I32) {
    err(ctx->in_path, "TruncI32FromI64 must be used in i32 context");
    return false;
  }

  bool seen_expr = false;
  SirExpr inner = {0};
  char ch = 0;

  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in TruncI32FromI64");
      free(inner.id);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in TruncI32FromI64");
      free(inner.id);
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid TruncI32FromI64 key");
      free(inner.id);
      return false;
    }

    if (strcmp(key, "expr") == 0) {
      seen_expr = true;
      if (!parse_expr(c, ctx, SEM2SIR_TYPE_I64, &inner)) {
        free(key);
        free(inner.id);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid TruncI32FromI64 field");
        free(key);
        free(inner.id);
        return false;
      }
    }

    free(key);
  }

  if (!seen_expr || !inner.id) {
    err(ctx->in_path, "TruncI32FromI64 requires field: expr");
    free(inner.id);
    return false;
  }

  char *id = NULL;
  bool ok = emit_conv_unary(ctx, "i32.trunc.i64", SEM2SIR_TYPE_I32, inner.id, &id);
  free(inner.id);
  if (!ok)
    return false;

  out->id = id;
  out->type = SEM2SIR_TYPE_I32;
  out->ptr_of = SEM2SIR_TYPE_INVALID;
  out->sir_type_id = sir_type_id_for(SEM2SIR_TYPE_I32);
  return true;
}

bool parse_expr_f64_from_i32_s(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  if (expected != SEM2SIR_TYPE_F64) {
    err(ctx->in_path, "F64FromI32S must be used in f64 context");
    return false;
  }

  bool seen_expr = false;
  SirExpr inner = {0};
  char ch = 0;

  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in F64FromI32S");
      free(inner.id);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in F64FromI32S");
      free(inner.id);
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid F64FromI32S key");
      free(inner.id);
      return false;
    }

    if (strcmp(key, "expr") == 0) {
      seen_expr = true;
      if (!parse_expr(c, ctx, SEM2SIR_TYPE_I32, &inner)) {
        free(key);
        free(inner.id);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid F64FromI32S field");
        free(key);
        free(inner.id);
        return false;
      }
    }

    free(key);
  }

  if (!seen_expr || !inner.id) {
    err(ctx->in_path, "F64FromI32S requires field: expr");
    free(inner.id);
    return false;
  }

  char *id = NULL;
  bool ok = emit_conv_unary(ctx, "f64.from_i32.s", SEM2SIR_TYPE_F64, inner.id, &id);
  free(inner.id);
  if (!ok)
    return false;

  out->id = id;
  out->type = SEM2SIR_TYPE_F64;
  out->ptr_of = SEM2SIR_TYPE_INVALID;
  out->sir_type_id = sir_type_id_for(SEM2SIR_TYPE_F64);
  return true;
}

bool parse_expr_f32_from_i32_s(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  if (expected != SEM2SIR_TYPE_F32) {
    err(ctx->in_path, "F32FromI32S must be used in f32 context");
    return false;
  }

  bool seen_expr = false;
  SirExpr inner = {0};
  char ch = 0;

  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in F32FromI32S");
      free(inner.id);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in F32FromI32S");
      free(inner.id);
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid F32FromI32S key");
      free(inner.id);
      return false;
    }

    if (strcmp(key, "expr") == 0) {
      seen_expr = true;
      if (!parse_expr(c, ctx, SEM2SIR_TYPE_I32, &inner)) {
        free(key);
        free(inner.id);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid F32FromI32S field");
        free(key);
        free(inner.id);
        return false;
      }
    }

    free(key);
  }

  if (!seen_expr || !inner.id) {
    err(ctx->in_path, "F32FromI32S requires field: expr");
    free(inner.id);
    return false;
  }

  char *id = NULL;
  bool ok = emit_conv_unary(ctx, "f32.from_i32.s", SEM2SIR_TYPE_F32, inner.id, &id);
  free(inner.id);
  if (!ok)
    return false;

  out->id = id;
  out->type = SEM2SIR_TYPE_F32;
  out->ptr_of = SEM2SIR_TYPE_INVALID;
  out->sir_type_id = sir_type_id_for(SEM2SIR_TYPE_F32);
  return true;
}

bool parse_expr_trunc_sat_i32_from_f64_s(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  if (expected != SEM2SIR_TYPE_I32) {
    err(ctx->in_path, "TruncSatI32FromF64S must be used in i32 context");
    return false;
  }

  bool seen_expr = false;
  SirExpr inner = {0};
  char ch = 0;

  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in TruncSatI32FromF64S");
      free(inner.id);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in TruncSatI32FromF64S");
      free(inner.id);
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid TruncSatI32FromF64S key");
      free(inner.id);
      return false;
    }

    if (strcmp(key, "expr") == 0) {
      seen_expr = true;
      if (!parse_expr(c, ctx, SEM2SIR_TYPE_F64, &inner)) {
        free(key);
        free(inner.id);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid TruncSatI32FromF64S field");
        free(key);
        free(inner.id);
        return false;
      }
    }

    free(key);
  }

  if (!seen_expr || !inner.id) {
    err(ctx->in_path, "TruncSatI32FromF64S requires field: expr");
    free(inner.id);
    return false;
  }

  char *id = NULL;
  bool ok = emit_conv_unary(ctx, "i32.trunc_sat_f64.s", SEM2SIR_TYPE_I32, inner.id, &id);
  free(inner.id);
  if (!ok)
    return false;

  out->id = id;
  out->type = SEM2SIR_TYPE_I32;
  out->ptr_of = SEM2SIR_TYPE_INVALID;
  out->sir_type_id = sir_type_id_for(SEM2SIR_TYPE_I32);
  return true;
}

bool parse_expr_trunc_sat_i32_from_f32_s(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  if (expected != SEM2SIR_TYPE_I32) {
    err(ctx->in_path, "TruncSatI32FromF32S must be used in i32 context");
    return false;
  }

  bool seen_expr = false;
  SirExpr inner = {0};
  char ch = 0;

  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in TruncSatI32FromF32S");
      free(inner.id);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in TruncSatI32FromF32S");
      free(inner.id);
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid TruncSatI32FromF32S key");
      free(inner.id);
      return false;
    }

    if (strcmp(key, "expr") == 0) {
      seen_expr = true;
      if (!parse_expr(c, ctx, SEM2SIR_TYPE_F32, &inner)) {
        free(key);
        free(inner.id);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid TruncSatI32FromF32S field");
        free(key);
        free(inner.id);
        return false;
      }
    }

    free(key);
  }

  if (!seen_expr || !inner.id) {
    err(ctx->in_path, "TruncSatI32FromF32S requires field: expr");
    free(inner.id);
    return false;
  }

  char *id = NULL;
  bool ok = emit_conv_unary(ctx, "i32.trunc_sat_f32.s", SEM2SIR_TYPE_I32, inner.id, &id);
  free(inner.id);
  if (!ok)
    return false;

  out->id = id;
  out->type = SEM2SIR_TYPE_I32;
  out->ptr_of = SEM2SIR_TYPE_INVALID;
  out->sir_type_id = sir_type_id_for(SEM2SIR_TYPE_I32);
  return true;
}

bool parse_expr_ptr_from_i64(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  if (expected != SEM2SIR_TYPE_PTR) {
    err(ctx->in_path, "PtrFromI64 must be used in ptr context");
    return false;
  }

  bool seen_expr = false;
  SirExpr inner = {0};
  char ch = 0;

  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in PtrFromI64");
      free(inner.id);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in PtrFromI64");
      free(inner.id);
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid PtrFromI64 key");
      free(inner.id);
      return false;
    }

    if (strcmp(key, "expr") == 0) {
      seen_expr = true;
      if (!parse_expr(c, ctx, SEM2SIR_TYPE_I64, &inner)) {
        free(key);
        free(inner.id);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid PtrFromI64 field");
        free(key);
        free(inner.id);
        return false;
      }
    }

    free(key);
  }

  if (!seen_expr || !inner.id) {
    err(ctx->in_path, "PtrFromI64 requires field: expr");
    free(inner.id);
    return false;
  }

  // Under SIR strict verification, ptr.from_i64 must have a ptr-typed type_ref (kind:"ptr").
  // sem2sir's base ptr type (t:ptr) is currently emitted as a prim, so model this as ptr(void).
  if (!emit_type_if_needed(ctx, SEM2SIR_TYPE_VOID)) {
    free(inner.id);
    return false;
  }
  const char *ptr_tid = get_derived_ptr_type_id(ctx, SEM2SIR_TYPE_VOID);
  SemTypeInfo ptr_ti = {.base = SEM2SIR_TYPE_PTR, .ptr_of = SEM2SIR_TYPE_VOID, .sir_id = ptr_tid};
  if (!ptr_tid || !emit_typeinfo_if_needed(ctx, &ptr_ti)) {
    err(ctx->in_path, "failed to materialize ptr(void) type");
    free(inner.id);
    return false;
  }

  char *id = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, id);
  fprintf(ctx->out, ",\"tag\":\"ptr.from_i64\",\"type_ref\":");
  emit_json_string(ctx->out, ptr_tid);
  fprintf(ctx->out, ",\"fields\":{\"args\":[{\"t\":\"ref\",\"id\":");
  emit_json_string(ctx->out, inner.id);
  fprintf(ctx->out, "}]}}\n");

  free(inner.id);

  out->id = id;
  out->type = SEM2SIR_TYPE_PTR;
  out->ptr_of = SEM2SIR_TYPE_VOID;
  out->sir_type_id = ptr_tid;
  return true;
}

bool parse_expr_i64_from_ptr(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  if (expected != SEM2SIR_TYPE_I64) {
    err(ctx->in_path, "I64FromPtr must be used in i64 context");
    return false;
  }

  bool seen_expr = false;
  SirExpr inner = {0};
  char ch = 0;

  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in I64FromPtr");
      free(inner.id);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in I64FromPtr");
      free(inner.id);
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid I64FromPtr key");
      free(inner.id);
      return false;
    }

    if (strcmp(key, "expr") == 0) {
      seen_expr = true;
      if (!parse_expr(c, ctx, SEM2SIR_TYPE_PTR, &inner)) {
        free(key);
        free(inner.id);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid I64FromPtr field");
        free(key);
        free(inner.id);
        return false;
      }
    }

    free(key);
  }

  if (!seen_expr || !inner.id) {
    err(ctx->in_path, "I64FromPtr requires field: expr");
    free(inner.id);
    return false;
  }

  char *id = NULL;
  bool ok = emit_conv_unary(ctx, "ptr.to_i64", SEM2SIR_TYPE_I64, inner.id, &id);
  free(inner.id);
  if (!ok)
    return false;

  out->id = id;
  out->type = SEM2SIR_TYPE_I64;
  out->ptr_of = SEM2SIR_TYPE_INVALID;
  out->sir_type_id = sir_type_id_for(SEM2SIR_TYPE_I64);
  return true;
}
