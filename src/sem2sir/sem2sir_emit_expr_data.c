#include "sem2sir_emit_internal.h"

static bool parse_required_lit_tok(GritJsonCursor *c, EmitCtx *ctx, const char *what, char **out_text) {
  *out_text = NULL;
  bool seen_lit = false;
  char *lit_text = NULL;

  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF");
      free(lit_text);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}'");
      free(lit_text);
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid key");
      free(lit_text);
      return false;
    }

    if (strcmp(key, "lit") == 0) {
      seen_lit = true;
      if (!parse_tok_text_alloc_strict(c, ctx->in_path, &lit_text)) {
        free(key);
        free(lit_text);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid field");
        free(key);
        free(lit_text);
        return false;
      }
    }

    free(key);
  }

  if (!seen_lit || !lit_text) {
    err(ctx->in_path, what ? what : "missing required field lit");
    free(lit_text);
    return false;
  }

  *out_text = lit_text;
  return true;
}

static bool emit_const_i64(EmitCtx *ctx, int64_t v, char **out_id) {
  *out_id = NULL;
  if (!emit_type_if_needed(ctx, SEM2SIR_TYPE_I64))
    return false;

  char *nid = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, nid);
  fprintf(ctx->out, ",\"tag\":\"const.i64\",\"type_ref\":\"t:i64\",\"fields\":{\"value\":%lld}}\n", (long long)v);
  *out_id = nid;
  return true;
}

static bool emit_cstr_node(EmitCtx *ctx, const char *s, char **out_id) {
  *out_id = NULL;
  if (!s) {
    err(ctx->in_path, "cstr literal missing text");
    return false;
  }

  // Note: embedded NUL bytes cannot be represented via tok.text (C string).

  if (!emit_type_if_needed(ctx, SEM2SIR_TYPE_CSTR))
    return false;

  char *nid = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, nid);
  fprintf(ctx->out, ",\"tag\":\"cstr\",\"type_ref\":\"t:cstr\",\"fields\":{\"value\":");
  emit_json_string(ctx->out, s);
  fprintf(ctx->out, "}}\n");
  *out_id = nid;
  return true;
}

static bool emit_const_struct_2(EmitCtx *ctx, const char *type_ref, const char *field0_id, const char *field1_id,
                                char **out_id) {
  *out_id = NULL;
  if (!type_ref || !field0_id || !field1_id)
    return false;

  char *nid = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, nid);
  fprintf(ctx->out, ",\"tag\":\"const.struct\",\"type_ref\":");
  emit_json_string(ctx->out, type_ref);
  fprintf(ctx->out, ",\"fields\":{\"fields\":[{\"i\":0,\"v\":{\"t\":\"ref\",\"id\":");
  emit_json_string(ctx->out, field0_id);
  fprintf(ctx->out, "}},{\"i\":1,\"v\":{\"t\":\"ref\",\"id\":");
  emit_json_string(ctx->out, field1_id);
  fprintf(ctx->out, "}}]}}\n");

  *out_id = nid;
  return true;
}

bool parse_expr_cstr(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  if (expected == SEM2SIR_TYPE_INVALID)
    expected = SEM2SIR_TYPE_CSTR;
  if (expected != SEM2SIR_TYPE_CSTR) {
    err(ctx->in_path, "CStr literal has mismatched expected type");
    return false;
  }

  char *lit_text = NULL;
  if (!parse_required_lit_tok(c, ctx, "CStr missing required field lit", &lit_text))
    return false;

  char *nid = NULL;
  bool ok = emit_cstr_node(ctx, lit_text, &nid);
  free(lit_text);
  if (!ok)
    return false;

  out->id = nid;
  out->type = SEM2SIR_TYPE_CSTR;
  out->ptr_of = SEM2SIR_TYPE_I8;
  out->sir_type_id = "t:cstr";
  return true;
}

bool parse_expr_string_utf8(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  if (expected == SEM2SIR_TYPE_INVALID)
    expected = SEM2SIR_TYPE_STRING_UTF8;
  if (expected != SEM2SIR_TYPE_STRING_UTF8) {
    err(ctx->in_path, "StringUtf8 literal has mismatched expected type");
    return false;
  }

  char *lit_text = NULL;
  if (!parse_required_lit_tok(c, ctx, "StringUtf8 missing required field lit", &lit_text))
    return false;

  size_t nbytes = strlen(lit_text);

  char *data_id = NULL;
  char *len_id = NULL;
  char *sid = NULL;

  bool ok = emit_cstr_node(ctx, lit_text, &data_id);
  if (ok)
    ok = emit_const_i64(ctx, (int64_t)nbytes, &len_id);
  if (ok)
    ok = emit_type_if_needed(ctx, SEM2SIR_TYPE_STRING_UTF8);
  if (ok)
    ok = emit_const_struct_2(ctx, "t:string.utf8", data_id, len_id, &sid);

  free(lit_text);
  free(data_id);
  free(len_id);

  if (!ok) {
    free(sid);
    return false;
  }

  out->id = sid;
  out->type = SEM2SIR_TYPE_STRING_UTF8;
  out->ptr_of = SEM2SIR_TYPE_INVALID;
  out->sir_type_id = "t:string.utf8";
  return true;
}

bool parse_expr_bytes(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  if (expected == SEM2SIR_TYPE_INVALID)
    expected = SEM2SIR_TYPE_BYTES;
  if (expected != SEM2SIR_TYPE_BYTES) {
    err(ctx->in_path, "Bytes literal has mismatched expected type");
    return false;
  }

  char *lit_text = NULL;
  if (!parse_required_lit_tok(c, ctx, "Bytes missing required field lit", &lit_text))
    return false;

  // MVP encoding: treat the token text as the bytes payload. This cannot represent embedded NUL.
  size_t nbytes = strlen(lit_text);

  char *data_id = NULL;
  char *len_id = NULL;
  char *bid = NULL;

  bool ok = emit_cstr_node(ctx, lit_text, &data_id);
  if (ok)
    ok = emit_const_i64(ctx, (int64_t)nbytes, &len_id);
  if (ok)
    ok = emit_type_if_needed(ctx, SEM2SIR_TYPE_BYTES);
  if (ok)
    ok = emit_const_struct_2(ctx, "t:bytes", data_id, len_id, &bid);

  free(lit_text);
  free(data_id);
  free(len_id);

  if (!ok) {
    free(bid);
    return false;
  }

  out->id = bid;
  out->type = SEM2SIR_TYPE_BYTES;
  out->ptr_of = SEM2SIR_TYPE_INVALID;
  out->sir_type_id = "t:bytes";
  return true;
}

bool parse_expr_char(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  // Char is modeled as a Unicode scalar value (u32).
  if (expected == SEM2SIR_TYPE_INVALID)
    expected = SEM2SIR_TYPE_U32;
  if (expected != SEM2SIR_TYPE_U32) {
    err(ctx->in_path, "Char literal has mismatched expected type (must be u32)");
    return false;
  }

  char *lit_text = NULL;
  if (!parse_required_lit_tok(c, ctx, "Char missing required field lit", &lit_text))
    return false;

  int base = 10;
  if (strncmp(lit_text, "0x", 2) == 0 || strncmp(lit_text, "0X", 2) == 0)
    base = 16;

  errno = 0;
  char *endp = NULL;
  unsigned long long uv = strtoull(lit_text, &endp, base);
  if (errno != 0 || !endp || *endp != '\0') {
    err(ctx->in_path, "Char literal token is not a valid integer (decimal or 0xHEX)");
    free(lit_text);
    return false;
  }
  free(lit_text);

  if (uv > 0x10FFFFULL) {
    err(ctx->in_path, "Char literal out of Unicode range (max 0x10FFFF)");
    return false;
  }
  if (uv >= 0xD800ULL && uv <= 0xDFFFULL) {
    err(ctx->in_path, "Char literal is a surrogate code point (invalid Unicode scalar value)");
    return false;
  }

  if (!emit_type_if_needed(ctx, SEM2SIR_TYPE_U32))
    return false;

  char *nid = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, nid);
  fprintf(ctx->out, ",\"tag\":\"const.u32\",\"type_ref\":\"t:u32\",\"fields\":{\"value\":%llu}}\n", uv);

  out->id = nid;
  out->type = SEM2SIR_TYPE_U32;
  out->ptr_of = SEM2SIR_TYPE_INVALID;
  out->sir_type_id = "t:u32";
  return true;
}
