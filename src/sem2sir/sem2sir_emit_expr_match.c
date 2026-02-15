#include "sem2sir_emit_internal.h"

typedef struct {
  char *lit_id;
  char *body_id;
} SwitchCase;

static bool try_consume_char(GritJsonCursor *c, char expected) {
  char ch = 0;
  if (!json_peek_non_ws(c, &ch))
    return false;
  if (ch != expected)
    return false;
  c->p++;
  return true;
}

static bool json_value_is_null(const char *json, size_t json_len) {
  if (!json)
    return false;
  GritJsonCursor t = grit_json_cursor((char *)json, json_len);
  if (!grit_json_skip_ws(&t) || t.p + 4 > t.end)
    return false;
  if (memcmp(t.p, "null", 4) != 0)
    return false;
  t.p += 4;
  if (!grit_json_skip_ws(&t))
    return false;
  return t.p == t.end;
}

static bool emit_int_const_from_text(EmitCtx *ctx, sem2sir_type_id t, const char *lit_text, char **out_node_id) {
  if (!ctx || !lit_text || !out_node_id)
    return false;
  *out_node_id = NULL;

  if (t != SEM2SIR_TYPE_I32 && t != SEM2SIR_TYPE_I64) {
    err(ctx->in_path, "Match switch scrutinee type must be i32 or i64 (MVP)");
    return false;
  }

  errno = 0;
  char *endp = NULL;
  long long v = strtoll(lit_text, &endp, 0);
  if (errno != 0 || !endp || *endp != '\0') {
    err(ctx->in_path, "PatInt literal token is not a valid integer (base10/0x supported)");
    return false;
  }
  if (t == SEM2SIR_TYPE_I32 && (v < INT32_MIN || v > INT32_MAX)) {
    err(ctx->in_path, "PatInt literal does not fit i32");
    return false;
  }

  if (!emit_type_if_needed(ctx, t))
    return false;

  const char *type_ref = sir_type_id_for(t);
  if (!type_ref) {
    err(ctx->in_path, "unsupported PatInt literal type");
    return false;
  }

  const char *tag = (t == SEM2SIR_TYPE_I32) ? "const.i32" : "const.i64";

  char *nid = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, nid);
  fprintf(ctx->out, ",\"tag\":");
  emit_json_string(ctx->out, tag);
  fprintf(ctx->out, ",\"type_ref\":");
  emit_json_string(ctx->out, type_ref);
  fprintf(ctx->out, ",\"fields\":{\"value\":%lld}}\n", v);

  *out_node_id = nid;
  return true;
}

static bool parse_pat_kind_and_lit(GritJsonCursor *c, EmitCtx *ctx, bool *out_is_wild, char **out_lit_text) {
  *out_is_wild = false;
  *out_lit_text = NULL;

  char *k = NULL;
  if (!parse_node_k_string(c, ctx, &k))
    return false;

  if (strcmp(k, "PatWild") == 0) {
    free(k);
    // consume remaining fields, if any
    if (!skip_remaining_object_fields(c, ctx, "PatWild"))
      return false;
    *out_is_wild = true;
    return true;
  }

  if (strcmp(k, "PatInt") != 0) {
    err(ctx->in_path, "MatchArm.pat must be PatInt or PatWild (MVP)");
    free(k);
    return false;
  }
  free(k);

  bool seen_lit = false;
  char *lit_text = NULL;

  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in PatInt");
      free(lit_text);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in PatInt");
      free(lit_text);
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid PatInt key");
      free(lit_text);
      return false;
    }

    if (strcmp(key, "lit") == 0) {
      seen_lit = true;
      if (!parse_tok_text_alloc_strict(c, ctx->in_path, &lit_text)) {
        free(key);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid PatInt field");
        free(key);
        free(lit_text);
        return false;
      }
    }
    free(key);
  }

  if (!seen_lit || !lit_text) {
    err(ctx->in_path, "PatInt missing required field lit");
    free(lit_text);
    return false;
  }

  *out_lit_text = lit_text;
  return true;
}

bool parse_expr_match(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  // We are inside the Match object (k already parsed), positioned after k's value.
  if (expected == SEM2SIR_TYPE_INVALID) {
    err(ctx->in_path, "Match expression requires an expected result type (no inference)");
    return false;
  }

  bool seen_cond = false;
  bool seen_arms = false;
  char *cond_json = NULL;
  size_t cond_len = 0;
  char *arms_json = NULL;
  size_t arms_len = 0;

  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in Match");
      free(cond_json);
      free(arms_json);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in Match");
      free(cond_json);
      free(arms_json);
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid Match key");
      free(cond_json);
      free(arms_json);
      return false;
    }

    if (strcmp(key, "cond") == 0) {
      seen_cond = true;
      if (!capture_json_value_alloc(c, &cond_json, &cond_len)) {
        free(key);
        free(arms_json);
        return false;
      }
    } else if (strcmp(key, "arms") == 0) {
      seen_arms = true;
      if (!capture_json_value_alloc(c, &arms_json, &arms_len)) {
        free(key);
        free(cond_json);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid Match field");
        free(key);
        free(cond_json);
        free(arms_json);
        return false;
      }
    }

    free(key);
  }

  if (!seen_cond || !cond_json) {
    err(ctx->in_path, "Match missing required field cond");
    free(cond_json);
    free(arms_json);
    return false;
  }
  if (!seen_arms || !arms_json) {
    err(ctx->in_path, "Match missing required field arms");
    free(cond_json);
    free(arms_json);
    return false;
  }

  // Determine scrutinee type without emitting.
  sem2sir_type_id scrut_t = probe_expr_type_no_expected(cond_json, cond_len, ctx);
  if (scrut_t != SEM2SIR_TYPE_I32 && scrut_t != SEM2SIR_TYPE_I64) {
    err(ctx->in_path, "Match cond must be Name/Paren(Name) of type i32/i64 (MVP)");
    free(cond_json);
    free(arms_json);
    return false;
  }

  // Emit scrutinee value.
  SirExpr scrut = {0};
  {
    GritJsonCursor c2 = grit_json_cursor(cond_json, cond_len);
    if (!parse_expr(&c2, ctx, scrut_t, &scrut)) {
      free(cond_json);
      free(arms_json);
      return false;
    }
  }

  // Parse arms.
  SwitchCase *cases = NULL;
  size_t case_count = 0;
  size_t case_cap = 0;
  char *default_body_id = NULL;

  {
    GritJsonCursor a = grit_json_cursor(arms_json, arms_len);
    if (!grit_json_skip_ws(&a) || !grit_json_consume_char(&a, '[')) {
      err(ctx->in_path, "Match.arms must be an array");
      free(scrut.id);
      free(cond_json);
      free(arms_json);
      return false;
    }

    if (!grit_json_skip_ws(&a)) {
      err(ctx->in_path, "invalid Match.arms");
      free(scrut.id);
      free(cond_json);
      free(arms_json);
      return false;
    }

    if (!try_consume_char(&a, ']')) {
      for (;;) {
        if (!grit_json_skip_ws(&a) || a.p >= a.end || *a.p != '{') {
          err(ctx->in_path, "Match.arms must contain objects");
          goto fail_arms;
        }

        char *arm_k = NULL;
        if (!parse_node_k_string(&a, ctx, &arm_k))
          goto fail_arms;
        if (strcmp(arm_k, "MatchArm") != 0) {
          err(ctx->in_path, "Match.arms must contain MatchArm nodes");
          free(arm_k);
          goto fail_arms;
        }
        free(arm_k);

        bool seen_pat = false;
        bool seen_guard = false;
        bool seen_body = false;
        char *pat_json = NULL;
        size_t pat_len = 0;
        char *guard_json = NULL;
        size_t guard_len = 0;
        char *body_json = NULL;
        size_t body_len = 0;

        char ch2 = 0;
        for (;;) {
          if (!json_peek_non_ws(&a, &ch2)) {
            err(ctx->in_path, "unexpected EOF in MatchArm");
            goto fail_arm_fields;
          }
          if (ch2 == '}') {
            a.p++;
            break;
          }
          if (ch2 != ',') {
            err(ctx->in_path, "expected ',' or '}' in MatchArm");
            goto fail_arm_fields;
          }
          a.p++;

          char *k2 = NULL;
          if (!json_expect_key(&a, &k2)) {
            err(ctx->in_path, "invalid MatchArm key");
            goto fail_arm_fields;
          }

          if (strcmp(k2, "pat") == 0) {
            seen_pat = true;
            if (!capture_json_value_alloc(&a, &pat_json, &pat_len)) {
              free(k2);
              goto fail_arm_fields;
            }
          } else if (strcmp(k2, "guard") == 0) {
            seen_guard = true;
            if (!capture_json_value_alloc(&a, &guard_json, &guard_len)) {
              free(k2);
              goto fail_arm_fields;
            }
          } else if (strcmp(k2, "body") == 0) {
            seen_body = true;
            if (!capture_json_value_alloc(&a, &body_json, &body_len)) {
              free(k2);
              goto fail_arm_fields;
            }
          } else {
            if (!grit_json_skip_value(&a)) {
              err(ctx->in_path, "invalid MatchArm field");
              free(k2);
              goto fail_arm_fields;
            }
          }
          free(k2);
        }

        if (!seen_pat || !pat_json) {
          err(ctx->in_path, "MatchArm missing required field pat");
          goto fail_arm_fields;
        }
        if (!seen_body || !body_json) {
          err(ctx->in_path, "MatchArm missing required field body");
          goto fail_arm_fields;
        }
        if (seen_guard && guard_json && !json_value_is_null(guard_json, guard_len)) {
          err(ctx->in_path, "MatchArm.guard is not supported in sem2sir Match MVP");
          goto fail_arm_fields;
        }

        // Parse pat.
        bool is_wild = false;
        char *lit_text = NULL;
        {
          GritJsonCursor pc = grit_json_cursor(pat_json, pat_len);
          if (!parse_pat_kind_and_lit(&pc, ctx, &is_wild, &lit_text))
            goto fail_arm_fields;
        }

        // Emit body expr.
        SirExpr body = {0};
        {
          if (json_value_is_null(body_json, body_len)) {
            err(ctx->in_path, "MatchArm.body must not be null (MVP)");
            free(lit_text);
            goto fail_arm_fields;
          }
          GritJsonCursor bc = grit_json_cursor(body_json, body_len);
          if (!parse_expr(&bc, ctx, expected, &body)) {
            free(lit_text);
            goto fail_arm_fields;
          }
        }

        if (is_wild) {
          if (default_body_id) {
            err(ctx->in_path, "Match must have at most one PatWild default arm");
            free(body.id);
            goto fail_arm_fields;
          }
          default_body_id = body.id;
        } else {
          char *lit_id = NULL;
          if (!emit_int_const_from_text(ctx, scrut_t, lit_text, &lit_id)) {
            free(lit_text);
            free(body.id);
            goto fail_arm_fields;
          }

          if (case_count == case_cap) {
            size_t new_cap = case_cap ? (case_cap * 2) : 8;
            SwitchCase *new_cases = (SwitchCase *)realloc(cases, new_cap * sizeof(SwitchCase));
            if (!new_cases) {
              err(ctx->in_path, "OOM building sem.switch cases");
              free(lit_text);
              free(lit_id);
              free(body.id);
              goto fail_arm_fields;
            }
            cases = new_cases;
            case_cap = new_cap;
          }
          cases[case_count].lit_id = lit_id;
          cases[case_count].body_id = body.id;
          case_count++;
        }

        free(lit_text);

        free(pat_json);
        free(guard_json);
        free(body_json);

        if (!grit_json_skip_ws(&a)) {
          err(ctx->in_path, "invalid Match.arms array");
          goto fail_arms;
        }
        if (try_consume_char(&a, ','))
          continue;
        if (try_consume_char(&a, ']'))
          break;

        err(ctx->in_path, "expected ',' or ']' in Match.arms");
        goto fail_arms;

      fail_arm_fields:
        free(pat_json);
        free(guard_json);
        free(body_json);
        goto fail_arms;
      }
    }

    // ok
    goto ok_arms;

  fail_arms:
    if (cases) {
      for (size_t i = 0; i < case_count; i++) {
        free(cases[i].lit_id);
        free(cases[i].body_id);
      }
    }
    free(cases);
    free(default_body_id);
    free(scrut.id);
    free(cond_json);
    free(arms_json);
    return false;
  }

ok_arms:
  if (!default_body_id) {
    err(ctx->in_path, "Match requires a PatWild default arm (MVP)");
    for (size_t i = 0; i < case_count; i++) {
      free(cases[i].lit_id);
      free(cases[i].body_id);
    }
    free(cases);
    free(scrut.id);
    free(cond_json);
    free(arms_json);
    return false;
  }

  if (!emit_type_if_needed(ctx, expected)) {
    for (size_t i = 0; i < case_count; i++) {
      free(cases[i].lit_id);
      free(cases[i].body_id);
    }
    free(cases);
    free(default_body_id);
    free(scrut.id);
    free(cond_json);
    free(arms_json);
    return false;
  }

  const char *tid = sir_type_id_for(expected);
  if (!tid) {
    err(ctx->in_path, "unsupported Match result type");
    for (size_t i = 0; i < case_count; i++) {
      free(cases[i].lit_id);
      free(cases[i].body_id);
    }
    free(cases);
    free(default_body_id);
    free(scrut.id);
    free(cond_json);
    free(arms_json);
    return false;
  }

  // We must feature-gate sem:v1 if we emit sem.* nodes.
  ctx->meta_sem_v1 = true;

  char *nid = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, nid);
  fprintf(ctx->out, ",\"tag\":\"sem.switch\",\"type_ref\":");
  emit_json_string(ctx->out, tid);
  fprintf(ctx->out, ",\"fields\":{\"args\":[{\"t\":\"ref\",\"id\":");
  emit_json_string(ctx->out, scrut.id);
  fprintf(ctx->out, "}],\"cases\":[");

  for (size_t i = 0; i < case_count; i++) {
    if (i)
      fprintf(ctx->out, ",");
    fprintf(ctx->out, "{\"lit\":{\"t\":\"ref\",\"id\":");
    emit_json_string(ctx->out, cases[i].lit_id);
    fprintf(ctx->out, "},\"body\":{\"kind\":\"val\",\"v\":{\"t\":\"ref\",\"id\":");
    emit_json_string(ctx->out, cases[i].body_id);
    fprintf(ctx->out, "}}}");
  }

  fprintf(ctx->out, "],\"default\":{\"kind\":\"val\",\"v\":{\"t\":\"ref\",\"id\":");
  emit_json_string(ctx->out, default_body_id);
  fprintf(ctx->out, "}}}}\n");

  for (size_t i = 0; i < case_count; i++) {
    free(cases[i].lit_id);
    free(cases[i].body_id);
  }
  free(cases);

  free(default_body_id);
  free(scrut.id);
  free(cond_json);
  free(arms_json);

  out->id = nid;
  out->type = expected;
  out->ptr_of = SEM2SIR_TYPE_INVALID;
  out->sir_type_id = tid;
  return true;
}
