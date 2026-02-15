#include "sem2sir_emit_internal.h"

bool parse_expr_call(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  bool seen_callee = false;
  bool seen_args = false;
  char *callee_name = NULL;
  char *args_json = NULL;
  size_t args_len = 0;

  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in Call");
      free(callee_name);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in Call");
      free(callee_name);
      return false;
    }
    c->p++;
    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid Call key");
      free(callee_name);
      return false;
    }
    if (strcmp(key, "callee") == 0) {
      seen_callee = true;
      if (!parse_name_id_alloc(c, ctx, &callee_name)) {
        free(key);
        free(callee_name);
        free(args_json);
        return false;
      }
    } else if (strcmp(key, "args") == 0) {
      seen_args = true;
      free(args_json);
      args_json = NULL;
      args_len = 0;
      if (!capture_json_value_alloc(c, &args_json, &args_len)) {
        err(ctx->in_path, "invalid Call.args");
        free(key);
        free(callee_name);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid Call field");
        free(key);
        free(callee_name);
        free(args_json);
        return false;
      }
    }
    free(key);
  }

  if (!seen_callee) {
    err(ctx->in_path, "Call missing required field callee");
    free(callee_name);
    free(args_json);
    return false;
  }
  if (!seen_args) {
    err(ctx->in_path, "Call missing required field args (no implicit empty args)");
    free(callee_name);
    free(args_json);
    return false;
  }
  if (!args_json) {
    err(ctx->in_path, "internal: Call.args capture failed");
    free(callee_name);
    return false;
  }

  ProcInfo *p = proc_table_find(ctx, callee_name);
  if (!p) {
    err(ctx->in_path, "Call.callee refers to unknown Proc (no implicit externs/globals)");
    free(callee_name);
    free(args_json);
    return false;
  }
  if (expected != SEM2SIR_TYPE_INVALID && p->ret != expected) {
    err(ctx->in_path, "Call return type mismatch against expected type");
    free(callee_name);
    free(args_json);
    return false;
  }
  if (!emit_typeinfo_if_needed(ctx, &p->ret_ti)) {
    free(callee_name);
    return false;
  }
  const char *tid = p->ret_ti.sir_id ? p->ret_ti.sir_id : sir_type_id_for(p->ret);
  if (!tid) {
    err(ctx->in_path, "unsupported Call return type");
    free(callee_name);
    free(args_json);
    return false;
  }

  // Parse args in a second pass so we can type-check against the callee signature.
  char **arg_ids = NULL;
  size_t arg_count = 0;
  if (p->param_count) {
    arg_ids = (char **)calloc(p->param_count, sizeof(char *));
    if (!arg_ids) {
      err(ctx->in_path, "OOM allocating call args");
      free(callee_name);
      free(args_json);
      return false;
    }
  }
  {
    GritJsonCursor ac = grit_json_cursor(args_json, args_len);
    if (!grit_json_skip_ws(&ac)) {
      err(ctx->in_path, "invalid Call.args JSON");
      free(arg_ids);
      free(callee_name);
      free(args_json);
      return false;
    }
    char ach = 0;
    if (!json_peek_non_ws(&ac, &ach)) {
      err(ctx->in_path, "unexpected EOF in Call.args");
      free(arg_ids);
      free(callee_name);
      free(args_json);
      return false;
    }
    if (ach == 'n') {
      if (!grit_json_skip_value(&ac)) {
        err(ctx->in_path, "invalid Call.args");
        free(arg_ids);
        free(callee_name);
        free(args_json);
        return false;
      }
      arg_count = 0;
    } else if (ach == '{') {
      char *ak = NULL;
      if (!parse_node_k_string(&ac, ctx, &ak)) {
        free(arg_ids);
        free(callee_name);
        free(args_json);
        return false;
      }
      if (strcmp(ak, "Args") != 0) {
        err(ctx->in_path, "Call.args must be null or Args");
        free(ak);
        free(arg_ids);
        free(callee_name);
        free(args_json);
        return false;
      }
      free(ak);

      bool seen_items = false;
      for (;;) {
        if (!json_peek_non_ws(&ac, &ach)) {
          err(ctx->in_path, "unexpected EOF in Args");
          free(arg_ids);
          free(callee_name);
          free(args_json);
          return false;
        }
        if (ach == '}') {
          ac.p++;
          break;
        }
        if (ach != ',') {
          err(ctx->in_path, "expected ',' or '}' in Args");
          free(arg_ids);
          free(callee_name);
          free(args_json);
          return false;
        }
        ac.p++;
        char *akey = NULL;
        if (!json_expect_key(&ac, &akey)) {
          err(ctx->in_path, "invalid Args key");
          free(arg_ids);
          free(callee_name);
          free(args_json);
          return false;
        }
        if (strcmp(akey, "items") == 0) {
          seen_items = true;
          if (!grit_json_consume_char(&ac, '[')) {
            err(ctx->in_path, "Args.items must be array");
            free(akey);
            free(arg_ids);
            free(callee_name);
            free(args_json);
            return false;
          }
          if (!json_peek_non_ws(&ac, &ach)) {
            err(ctx->in_path, "unexpected EOF in Args.items");
            free(akey);
            free(arg_ids);
            free(callee_name);
            free(args_json);
            return false;
          }
          if (ach != ']') {
            for (;;) {
              if (arg_count >= p->param_count) {
                err(ctx->in_path, "Call args exceed Proc param arity");
                free(akey);
                free(arg_ids);
                free(callee_name);
                free(args_json);
                return false;
              }
              sem2sir_type_id expected_ty = p->params[arg_count].base;
              SirExpr a = {0};
              if (!parse_expr(&ac, ctx, expected_ty, &a)) {
                free(akey);
                free(arg_ids);
                free(callee_name);
                free(args_json);
                return false;
              }
              if (p->params[arg_count].base == SEM2SIR_TYPE_PTR && p->params[arg_count].ptr_of != SEM2SIR_TYPE_INVALID) {
                if (a.ptr_of != p->params[arg_count].ptr_of) {
                  err(ctx->in_path, "Call arg ptr pointee type does not match Proc param type");
                  free(akey);
                  free(arg_ids);
                  free(callee_name);
                  free(args_json);
                  return false;
                }
              }
              arg_ids[arg_count] = a.id;
              arg_count++;

              if (!json_peek_non_ws(&ac, &ach)) {
                err(ctx->in_path, "unexpected EOF in Args.items");
                free(akey);
                free(arg_ids);
                free(callee_name);
                free(args_json);
                return false;
              }
              if (ach == ',') {
                ac.p++;
                continue;
              }
              if (ach == ']') break;
              err(ctx->in_path, "expected ',' or ']' in Args.items");
              free(akey);
              free(arg_ids);
              free(callee_name);
              free(args_json);
              return false;
            }
          }
          ac.p++; // consume ']'
        } else {
          if (!grit_json_skip_value(&ac)) {
            err(ctx->in_path, "invalid Args field");
            free(akey);
            free(arg_ids);
            free(callee_name);
            free(args_json);
            return false;
          }
        }
        free(akey);
      }
      if (!seen_items) {
        err(ctx->in_path, "Args missing required field items (no implicit empty list)");
        free(arg_ids);
        free(callee_name);
        free(args_json);
        return false;
      }
    } else {
      err(ctx->in_path, "Call.args must be null or Args");
      free(arg_ids);
      free(callee_name);
      free(args_json);
      return false;
    }
  }
  if (arg_count != p->param_count) {
    err(ctx->in_path, "Call args arity does not match Proc param arity");
    free(arg_ids);
    free(callee_name);
    free(args_json);
    return false;
  }

  char *nid = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, nid);
  if (p->is_extern) {
    // Extern calls use decl.fn + call.indirect (sircc producer rule).
    fprintf(ctx->out, ",\"tag\":\"call.indirect\",\"type_ref\":");
    emit_json_string(ctx->out, tid);
    fprintf(ctx->out, ",\"fields\":{\"sig\":");
    emit_json_string(ctx->out, p->fn_type_id);
    fprintf(ctx->out, ",\"args\":[{\"t\":\"ref\",\"id\":");
    emit_json_string(ctx->out, p->fn_id);
    fprintf(ctx->out, "}");
    for (size_t i = 0; i < arg_count; i++) {
      fprintf(ctx->out, ",{\"t\":\"ref\",\"id\":");
      emit_json_string(ctx->out, arg_ids[i]);
      fprintf(ctx->out, "}");
    }
    fprintf(ctx->out, "]}}\n");
  } else {
    fprintf(ctx->out, ",\"tag\":\"call\",\"type_ref\":");
    emit_json_string(ctx->out, tid);
    fprintf(ctx->out, ",\"fields\":{\"callee\":{\"t\":\"ref\",\"id\":");
    emit_json_string(ctx->out, p->fn_id);
    fprintf(ctx->out, "},\"args\":[");
    for (size_t i = 0; i < arg_count; i++) {
      if (i) fprintf(ctx->out, ",");
      fprintf(ctx->out, "{\"t\":\"ref\",\"id\":");
      emit_json_string(ctx->out, arg_ids[i]);
      fprintf(ctx->out, "}");
    }
    fprintf(ctx->out, "]}}\n");
  }

  free(callee_name);
  free(args_json);
  free(arg_ids);
  out->id = nid;
  out->type = p->ret;
  out->ptr_of = p->ret_ti.ptr_of;
  out->sir_type_id = tid;
  return true;
}

bool parse_expr_int(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  // We are inside the Int object (k already parsed), positioned after k's value.
  if (expected == SEM2SIR_TYPE_INVALID) {
    if (ctx->default_int != SEM2SIR_TYPE_INVALID) {
      expected = ctx->default_int;
    } else {
      err(ctx->in_path,
          "Int literal requires an expected type unless meta.types['@default.int'/'__default_int'] is set (no defaults)");
      return false;
    }
  }
  if (expected != SEM2SIR_TYPE_I32 && expected != SEM2SIR_TYPE_I64 && expected != SEM2SIR_TYPE_U8 &&
      expected != SEM2SIR_TYPE_U32 && expected != SEM2SIR_TYPE_U64) {
    err(ctx->in_path, "Int literal type not supported in sem2sir MVP");
    return false;
  }

  bool seen_lit = false;
  char *lit_text = NULL;

  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in Int");
      free(lit_text);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in Int");
      free(lit_text);
      return false;
    }
    c->p++;
    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid Int key");
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
        err(ctx->in_path, "invalid Int field");
        free(key);
        free(lit_text);
        return false;
      }
    }
    free(key);
  }

  if (!seen_lit || !lit_text) {
    err(ctx->in_path, "Int missing required field lit");
    free(lit_text);
    return false;
  }

  const char *tag = NULL;
  const char *type_ref = sir_type_id_for(expected);
  if (!type_ref) {
    err(ctx->in_path, "Int literal expected type is unsupported");
    free(lit_text);
    return false;
  }

  bool is_unsigned = (expected == SEM2SIR_TYPE_U8 || expected == SEM2SIR_TYPE_U32 || expected == SEM2SIR_TYPE_U64);
  if (is_unsigned) {
    errno = 0;
    char *endp = NULL;
    unsigned long long uv = strtoull(lit_text, &endp, 10);
    if (errno != 0 || !endp || *endp != '\0') {
      err(ctx->in_path, "Int literal token is not a valid base-10 unsigned integer");
      free(lit_text);
      return false;
    }
    if (expected == SEM2SIR_TYPE_U8 && uv > UINT8_MAX) {
      err(ctx->in_path, "Int literal does not fit u8");
      free(lit_text);
      return false;
    }
    if (expected == SEM2SIR_TYPE_U32 && uv > UINT32_MAX) {
      err(ctx->in_path, "Int literal does not fit u32");
      free(lit_text);
      return false;
    }
    // u64: any strtoull-valid value fits by construction.
    free(lit_text);

    if (!emit_type_if_needed(ctx, expected))
      return false;
    tag = (expected == SEM2SIR_TYPE_U8)    ? "const.u8"
          : (expected == SEM2SIR_TYPE_U32) ? "const.u32"
                                           : "const.u64";

    char *nid = new_node_id(ctx);
    fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
    emit_json_string(ctx->out, nid);
    fprintf(ctx->out, ",\"tag\":");
    emit_json_string(ctx->out, tag);
    fprintf(ctx->out, ",\"type_ref\":");
    emit_json_string(ctx->out, type_ref);
    fprintf(ctx->out, ",\"fields\":{\"value\":%llu}}\n", uv);

    out->id = nid;
    out->type = expected;
    return true;
  }

  errno = 0;
  char *endp = NULL;
  long long v = strtoll(lit_text, &endp, 10);
  if (errno != 0 || !endp || *endp != '\0') {
    err(ctx->in_path, "Int literal token is not a valid base-10 integer");
    free(lit_text);
    return false;
  }
  if (expected == SEM2SIR_TYPE_I32 && (v < INT32_MIN || v > INT32_MAX)) {
    err(ctx->in_path, "Int literal does not fit i32");
    free(lit_text);
    return false;
  }
  free(lit_text);

  if (!emit_type_if_needed(ctx, expected))
    return false;

  tag = (expected == SEM2SIR_TYPE_I32) ? "const.i32" : "const.i64";

  char *nid = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, nid);
  fprintf(ctx->out, ",\"tag\":");
  emit_json_string(ctx->out, tag);
  fprintf(ctx->out, ",\"type_ref\":");
  emit_json_string(ctx->out, type_ref);
  fprintf(ctx->out, ",\"fields\":{\"value\":%lld}}\n", v);

  out->id = nid;
  out->type = expected;
  return true;
}

