#include "sem2sir_emit_internal.h"

bool stmtlist_push(StmtList *sl, char *id) {
  if (!sl)
    return false;
  char **next = (char **)realloc(sl->ids, (sl->count + 1) * sizeof(char *));
  if (!next)
    return false;
  sl->ids = next;
  sl->ids[sl->count++] = id;
  return true;
}

bool parse_expr(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out);

bool capture_json_value_alloc(GritJsonCursor *c, char **out_buf, size_t *out_len) {
  *out_buf = NULL;
  if (out_len)
    *out_len = 0;
  const char *start = c->p;
  if (!grit_json_skip_value(c)) {
    return false;
  }
  const char *end = c->p;
  if (end < start)
    return false;
  size_t n = (size_t)(end - start);
  char *buf = (char *)malloc(n + 1);
  if (!buf)
    return false;
  memcpy(buf, start, n);
  buf[n] = '\0';
  *out_buf = buf;
  if (out_len)
    *out_len = n;
  return true;
}

bool probe_tok_text_alloc(GritJsonCursor *c, char **out_text) {
  *out_text = NULL;
  if (!grit_json_consume_char(c, '{'))
    return false;

  // Expect key "k" first.
  char *key = NULL;
  if (!json_expect_key(c, &key))
    return false;
  bool ok = (strcmp(key, "k") == 0);
  free(key);
  if (!ok)
    return false;

  char *k_str = NULL;
  if (!grit_json_parse_string_alloc(c, &k_str))
    return false;
  ok = (strcmp(k_str, "tok") == 0);
  free(k_str);
  if (!ok)
    return false;

  bool seen_text = false;
  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch))
      return false;
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',')
      return false;
    c->p++;
    char *tkey = NULL;
    if (!json_expect_key(c, &tkey))
      return false;
    if (strcmp(tkey, "text") == 0) {
      seen_text = true;
      if (!grit_json_parse_string_alloc(c, out_text)) {
        free(tkey);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        free(tkey);
        return false;
      }
    }
    free(tkey);
  }

  if (!seen_text || !*out_text)
    return false;
  return true;
}

sem2sir_type_id probe_expr_type_no_expected(const char *expr_json, size_t expr_len, EmitCtx *ctx) {
  if (!expr_json || expr_len == 0)
    return SEM2SIR_TYPE_INVALID;

  GritJsonCursor c = grit_json_cursor(expr_json, expr_len);
  if (!grit_json_skip_ws(&c))
    return SEM2SIR_TYPE_INVALID;
  if (!grit_json_consume_char(&c, '{'))
    return SEM2SIR_TYPE_INVALID;

  char *key = NULL;
  if (!json_expect_key(&c, &key))
    return SEM2SIR_TYPE_INVALID;
  bool ok = (strcmp(key, "k") == 0);
  free(key);
  if (!ok)
    return SEM2SIR_TYPE_INVALID;

  char *k_str = NULL;
  if (!grit_json_parse_string_alloc(&c, &k_str))
    return SEM2SIR_TYPE_INVALID;

  sem2sir_type_id result = SEM2SIR_TYPE_INVALID;
  if (strcmp(k_str, "Name") == 0) {
    bool seen_id = false;
    char *name_text = NULL;
    char ch = 0;
    for (;;) {
      if (!json_peek_non_ws(&c, &ch))
        break;
      if (ch == '}') {
        c.p++;
        break;
      }
      if (ch != ',')
        break;
      c.p++;
      char *nkey = NULL;
      if (!json_expect_key(&c, &nkey))
        break;
      if (strcmp(nkey, "id") == 0) {
        seen_id = true;
        if (!probe_tok_text_alloc(&c, &name_text)) {
          free(nkey);
          break;
        }
      } else {
        if (!grit_json_skip_value(&c)) {
          free(nkey);
          break;
        }
      }
      free(nkey);
    }
    if (seen_id && name_text) {
      sem2sir_type_id t = SEM2SIR_TYPE_INVALID;
      if (locals_lookup(ctx, name_text, &t, NULL, NULL, NULL)) {
        result = t;
      }
    }
    free(name_text);
  } else if (strcmp(k_str, "Int") == 0) {
    // If the language commits a default int literal width via metadata,
    // then Int literals are not ambiguous even without surrounding context.
    // Otherwise, we do not guess a width here.
    result = ctx->default_int;
  } else if (strcmp(k_str, "Paren") == 0) {
    bool seen_expr = false;
    char *inner_json = NULL;
    size_t inner_len = 0;
    char ch = 0;
    for (;;) {
      if (!json_peek_non_ws(&c, &ch))
        break;
      if (ch == '}') {
        c.p++;
        break;
      }
      if (ch != ',')
        break;
      c.p++;
      char *pkey = NULL;
      if (!json_expect_key(&c, &pkey))
        break;
      if (strcmp(pkey, "expr") == 0) {
        seen_expr = true;
        if (!capture_json_value_alloc(&c, &inner_json, &inner_len)) {
          free(pkey);
          break;
        }
      } else {
        if (!grit_json_skip_value(&c)) {
          free(pkey);
          break;
        }
      }
      free(pkey);
    }
    if (seen_expr && inner_json) {
      result = probe_expr_type_no_expected(inner_json, inner_len, ctx);
    }
    free(inner_json);
  }

  free(k_str);
  return result;
}

// Best-effort probe: if expr is Name/Paren(Name) referring to a local with an explicit derived ptr(T)
// type, return T (including void). Otherwise return SEM2SIR_TYPE_INVALID.
// This performs no SIR emission and is used to avoid requiring @default.ptr.pointee when the pointee
// is already explicitly committed by ptr(T).
sem2sir_type_id probe_ptr_pointee_no_expected(const char *expr_json, size_t expr_len, EmitCtx *ctx) {
  if (!expr_json || expr_len == 0)
    return SEM2SIR_TYPE_INVALID;

  GritJsonCursor c = grit_json_cursor(expr_json, expr_len);
  if (!grit_json_skip_ws(&c))
    return SEM2SIR_TYPE_INVALID;
  if (!grit_json_consume_char(&c, '{'))
    return SEM2SIR_TYPE_INVALID;

  char *key = NULL;
  if (!json_expect_key(&c, &key))
    return SEM2SIR_TYPE_INVALID;
  bool ok = (strcmp(key, "k") == 0);
  free(key);
  if (!ok)
    return SEM2SIR_TYPE_INVALID;

  char *k_str = NULL;
  if (!grit_json_parse_string_alloc(&c, &k_str))
    return SEM2SIR_TYPE_INVALID;

  sem2sir_type_id result = SEM2SIR_TYPE_INVALID;
  if (strcmp(k_str, "Name") == 0) {
    bool seen_id = false;
    char *name_text = NULL;
    char ch = 0;
    for (;;) {
      if (!json_peek_non_ws(&c, &ch))
        break;
      if (ch == '}') {
        c.p++;
        break;
      }
      if (ch != ',')
        break;
      c.p++;
      char *nkey = NULL;
      if (!json_expect_key(&c, &nkey))
        break;
      if (strcmp(nkey, "id") == 0) {
        seen_id = true;
        if (!probe_tok_text_alloc(&c, &name_text)) {
          free(nkey);
          break;
        }
      } else {
        if (!grit_json_skip_value(&c)) {
          free(nkey);
          break;
        }
      }
      free(nkey);
    }
    if (seen_id && name_text) {
      sem2sir_type_id t = SEM2SIR_TYPE_INVALID;
      sem2sir_type_id ptr_of = SEM2SIR_TYPE_INVALID;
      if (locals_lookup(ctx, name_text, &t, &ptr_of, NULL, NULL)) {
        if (t == SEM2SIR_TYPE_PTR && ptr_of != SEM2SIR_TYPE_INVALID) {
          result = ptr_of;
        }
      }
    }
    free(name_text);
  } else if (strcmp(k_str, "Paren") == 0) {
    bool seen_expr = false;
    char *inner_json = NULL;
    size_t inner_len = 0;
    char ch = 0;
    for (;;) {
      if (!json_peek_non_ws(&c, &ch))
        break;
      if (ch == '}') {
        c.p++;
        break;
      }
      if (ch != ',')
        break;
      c.p++;
      char *pkey = NULL;
      if (!json_expect_key(&c, &pkey))
        break;
      if (strcmp(pkey, "expr") == 0) {
        seen_expr = true;
        if (!capture_json_value_alloc(&c, &inner_json, &inner_len)) {
          free(pkey);
          break;
        }
      } else {
        if (!grit_json_skip_value(&c)) {
          free(pkey);
          break;
        }
      }
      free(pkey);
    }
    if (seen_expr && inner_json) {
      result = probe_ptr_pointee_no_expected(inner_json, inner_len, ctx);
    }
    free(inner_json);
  }

  free(k_str);
  return result;
}

// If deref_json is a Deref node whose expr is a Name/Paren(Name) referring to a local typed as ptr(T),
// return T (including void). Otherwise return SEM2SIR_TYPE_INVALID.
sem2sir_type_id probe_deref_expr_pointee_no_expected(const char *deref_json, size_t deref_len, EmitCtx *ctx) {
  if (!deref_json || deref_len == 0)
    return SEM2SIR_TYPE_INVALID;

  GritJsonCursor c = grit_json_cursor(deref_json, deref_len);
  if (!grit_json_skip_ws(&c))
    return SEM2SIR_TYPE_INVALID;
  if (!grit_json_consume_char(&c, '{'))
    return SEM2SIR_TYPE_INVALID;

  char *key = NULL;
  if (!json_expect_key(&c, &key))
    return SEM2SIR_TYPE_INVALID;
  bool ok = (strcmp(key, "k") == 0);
  free(key);
  if (!ok)
    return SEM2SIR_TYPE_INVALID;

  char *k_str = NULL;
  if (!grit_json_parse_string_alloc(&c, &k_str))
    return SEM2SIR_TYPE_INVALID;
  ok = (strcmp(k_str, "Deref") == 0);
  free(k_str);
  if (!ok)
    return SEM2SIR_TYPE_INVALID;

  char *expr_json = NULL;
  size_t expr_len = 0;
  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(&c, &ch))
      break;
    if (ch == '}') {
      c.p++;
      break;
    }
    if (ch != ',')
      break;
    c.p++;
    char *dkey = NULL;
    if (!json_expect_key(&c, &dkey))
      break;
    if (strcmp(dkey, "expr") == 0) {
      free(expr_json);
      expr_json = NULL;
      expr_len = 0;
      if (!capture_json_value_alloc(&c, &expr_json, &expr_len)) {
        free(dkey);
        break;
      }
    } else {
      if (!grit_json_skip_value(&c)) {
        free(dkey);
        break;
      }
    }
    free(dkey);
  }

  sem2sir_type_id result = SEM2SIR_TYPE_INVALID;
  if (expr_json) {
    result = probe_ptr_pointee_no_expected(expr_json, expr_len, ctx);
  }
  free(expr_json);
  return result;
}

bool parse_node_k_string(GritJsonCursor *c, EmitCtx *ctx, char **out_k) {
  *out_k = NULL;
  if (!grit_json_skip_ws(c) || c->p >= c->end) {
    err(ctx->in_path, "unexpected EOF (expected node object)");
    return false;
  }
  if (*c->p != '{') {
    char got = *c->p;
    char msg[96];
    if (got >= 32 && got <= 126) {
      snprintf(msg, sizeof(msg), "expected node object, got '%c'", got);
    } else {
      snprintf(msg, sizeof(msg), "expected node object, got byte %u", (unsigned char)got);
    }
    err(ctx->in_path, msg);
    return false;
  }
  c->p++;
  char *key = NULL;
  if (!json_expect_key(c, &key)) {
    err(ctx->in_path, "invalid node key");
    return false;
  }
  if (strcmp(key, "k") != 0) {
    err(ctx->in_path, "node must start with key 'k'");
    free(key);
    return false;
  }
  free(key);
  if (!grit_json_parse_string_alloc(c, out_k)) {
    err(ctx->in_path, "node field k must be string");
    return false;
  }
  return true;
}

bool parse_name_id_alloc(GritJsonCursor *c, EmitCtx *ctx, char **out_name) {
  *out_name = NULL;
  char *k = NULL;
  if (!parse_node_k_string(c, ctx, &k))
    return false;
  if (strcmp(k, "Name") != 0) {
    err(ctx->in_path, "Call.callee must be Name (direct calls only)");
    free(k);
    return false;
  }
  free(k);

  bool seen_id = false;
  char *name_text = NULL;

  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in Name");
      free(name_text);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in Name");
      free(name_text);
      return false;
    }
    c->p++;
    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid Name key");
      free(name_text);
      return false;
    }
    if (strcmp(key, "id") == 0) {
      seen_id = true;
      if (!parse_tok_text_alloc_strict(c, ctx->in_path, &name_text)) {
        free(key);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid Name field");
        free(key);
        free(name_text);
        return false;
      }
    }
    free(key);
  }

  if (!seen_id || !name_text) {
    err(ctx->in_path, "Name missing required field id");
    free(name_text);
    return false;
  }
  *out_name = name_text;
  return true;
}


