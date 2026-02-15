#include "sem2sir_emit_internal.h"

bool parse_node_k_string(GritJsonCursor *c, EmitCtx *ctx, char **out_k);

bool parse_typeapp_ctor_name_alloc(GritJsonCursor *c, EmitCtx *ctx, char **out_ctor) {
  *out_ctor = NULL;

  // Expect a TypeRef node and return its name token text.
  char *k = NULL;
  if (!parse_node_k_string(c, ctx, &k))
    return false;
  if (strcmp(k, "TypeRef") != 0) {
    err(ctx->in_path, "type application callee must be TypeRef");
    free(k);
    return false;
  }
  free(k);

  bool seen_name = false;
  char *name_text = NULL;
  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in TypeRef (callee)");
      free(name_text);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in TypeRef (callee)");
      free(name_text);
      return false;
    }
    c->p++;
    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid TypeRef key");
      free(name_text);
      return false;
    }
    if (strcmp(key, "name") == 0) {
      seen_name = true;
      if (!parse_tok_text_alloc_strict(c, ctx->in_path, &name_text)) {
        free(key);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid TypeRef field");
        free(key);
        free(name_text);
        return false;
      }
    }
    free(key);
  }

  if (!seen_name || !name_text) {
    err(ctx->in_path, "TypeRef missing required field name");
    free(name_text);
    return false;
  }

  *out_ctor = name_text;
  return true;
}

bool parse_type_typeinfo(GritJsonCursor *c, EmitCtx *ctx, SemTypeInfo *out_ti);

bool parse_args_node_parse_single_typearg(GritJsonCursor *c, EmitCtx *ctx, SemTypeInfo *out_arg, size_t *out_argc) {
  *out_argc = 0;
  if (out_arg)
    *out_arg = (SemTypeInfo){0};

  char *k = NULL;
  if (!parse_node_k_string(c, ctx, &k))
    return false;
  if (strcmp(k, "Args") != 0) {
    err(ctx->in_path, "expected Args node");
    free(k);
    return false;
  }
  free(k);

  bool seen_items = false;
  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in Args");
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in Args");
      return false;
    }
    c->p++;
    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid Args key");
      return false;
    }
    if (strcmp(key, "items") == 0) {
      seen_items = true;
      if (!grit_json_consume_char(c, '[')) {
        err(ctx->in_path, "Args.items must be array");
        free(key);
        return false;
      }
      if (!json_peek_non_ws(c, &ch)) {
        err(ctx->in_path, "unexpected EOF in Args.items");
        free(key);
        return false;
      }
      if (ch != ']') {
        // First item
        SemTypeInfo first = {0};
        if (!parse_type_typeinfo(c, ctx, &first)) {
          free(key);
          return false;
        }
        (*out_argc)++;
        if (out_arg)
          *out_arg = first;

        if (!json_peek_non_ws(c, &ch)) {
          err(ctx->in_path, "unexpected EOF in Args.items");
          free(key);
          return false;
        }
        while (ch == ',') {
          c->p++;
          if (!grit_json_skip_value(c)) {
            err(ctx->in_path, "invalid Args.items entry");
            free(key);
            return false;
          }
          (*out_argc)++;
          if (!json_peek_non_ws(c, &ch)) {
            err(ctx->in_path, "unexpected EOF in Args.items");
            free(key);
            return false;
          }
        }
        if (ch != ']') {
          err(ctx->in_path, "expected ',' or ']' in Args.items");
          free(key);
          return false;
        }
      }
      if (!grit_json_consume_char(c, ']')) {
        err(ctx->in_path, "expected closing ']' in Args.items");
        free(key);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid Args field");
        free(key);
        return false;
      }
    }
    free(key);
  }

  if (!seen_items) {
    err(ctx->in_path, "Args missing required field items");
    return false;
  }
  return true;
}

bool parse_type_typeinfo(GritJsonCursor *c, EmitCtx *ctx, SemTypeInfo *out_ti) {
  *out_ti = (SemTypeInfo){0};
  out_ti->base = SEM2SIR_TYPE_INVALID;
  out_ti->ptr_of = SEM2SIR_TYPE_INVALID;
  out_ti->sir_id = NULL;

  if (!grit_json_consume_char(c, '{')) {
    err(ctx->in_path, "expected type node object");
    return false;
  }

  // Require k first
  char *key = NULL;
  if (!json_expect_key(c, &key)) {
    err(ctx->in_path, "invalid type node key");
    return false;
  }
  if (strcmp(key, "k") != 0) {
    err(ctx->in_path, "type node must start with key 'k'");
    free(key);
    return false;
  }
  free(key);

  char *k_str = NULL;
  if (!grit_json_parse_string_alloc(c, &k_str)) {
    err(ctx->in_path, "type node k must be string");
    return false;
  }
  bool is_typeref = (strcmp(k_str, "TypeRef") == 0);
  bool is_typeapp = (strcmp(k_str, "Call") == 0);
  free(k_str);
  if (!is_typeref && !is_typeapp) {
    err(ctx->in_path, "type must be TypeRef or type application Call");
    return false;
  }

  if (is_typeapp) {
    bool seen_callee = false;
    bool seen_args = false;
    char *ctor = NULL;
    size_t argc = 0;
    SemTypeInfo arg0 = {0};

    char ch = 0;
    for (;;) {
      if (!json_peek_non_ws(c, &ch)) {
        err(ctx->in_path, "unexpected EOF in type application");
        free(ctor);
        return false;
      }
      if (ch == '}') {
        c->p++;
        break;
      }
      if (ch != ',') {
        err(ctx->in_path, "expected ',' or '}' in type application");
        free(ctor);
        return false;
      }
      c->p++;
      char *tkey = NULL;
      if (!json_expect_key(c, &tkey)) {
        err(ctx->in_path, "invalid type application key");
        free(ctor);
        return false;
      }

      if (strcmp(tkey, "callee") == 0) {
        seen_callee = true;
        if (!parse_typeapp_ctor_name_alloc(c, ctx, &ctor)) {
          free(tkey);
          free(ctor);
          return false;
        }
      } else if (strcmp(tkey, "args") == 0) {
        seen_args = true;
        if (!json_peek_non_ws(c, &ch)) {
          err(ctx->in_path, "unexpected EOF in type application args");
          free(tkey);
          free(ctor);
          return false;
        }
        if (ch == 'n') {
          // null args => arity 0
          if (!grit_json_skip_value(c)) {
            err(ctx->in_path, "invalid type application args");
            free(tkey);
            free(ctor);
            return false;
          }
          argc = 0;
        } else if (ch == '{') {
          if (!parse_args_node_parse_single_typearg(c, ctx, &arg0, &argc)) {
            free(tkey);
            free(ctor);
            return false;
          }
        } else {
          err(ctx->in_path, "type application args must be null or Args");
          free(tkey);
          free(ctor);
          return false;
        }
      } else {
        if (!grit_json_skip_value(c)) {
          err(ctx->in_path, "invalid type application field");
          free(tkey);
          free(ctor);
          return false;
        }
      }
      free(tkey);
    }

    if (!seen_callee || !ctor) {
      err(ctx->in_path, "type application missing required field callee");
      free(ctor);
      return false;
    }
    if (!seen_args) {
      err(ctx->in_path, "type application missing required field args (no implicit empty args)");
      free(ctor);
      return false;
    }

    if (strcmp(ctor, "ptr") == 0) {
      if (argc != 1) {
        err(ctx->in_path, "ptr(T) requires exactly 1 type argument");
        free(ctor);
        return false;
      }
      out_ti->base = SEM2SIR_TYPE_PTR;
      out_ti->ptr_of = arg0.base;
      out_ti->sir_id = get_derived_ptr_type_id(ctx, arg0.base);
      if (!out_ti->sir_id) {
        err(ctx->in_path, "ptr(T) pointee type is not supported");
        free(ctor);
        return false;
      }
      free(ctor);
      return true;
    }
    if (strcmp(ctor, "slice") == 0) {
      if (argc != 1) {
        err(ctx->in_path, "slice(T) requires exactly 1 type argument");
        free(ctor);
        return false;
      }
      // SIR does not have a generic derived slice type today; keep the MVP unparameterized slice.
      out_ti->base = SEM2SIR_TYPE_SLICE;
      out_ti->ptr_of = SEM2SIR_TYPE_INVALID;
      out_ti->sir_id = "t:slice";
      free(ctor);
      return true;
    }

    err(ctx->in_path, "unsupported type constructor (only ptr(T)/slice(T) supported)");
    free(ctor);
    return false;
  }

  bool seen_name = false;
  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in TypeRef");
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in TypeRef");
      return false;
    }
    c->p++;

    char *tkey = NULL;
    if (!json_expect_key(c, &tkey)) {
      err(ctx->in_path, "invalid TypeRef key");
      return false;
    }

    if (strcmp(tkey, "name") == 0) {
      seen_name = true;
      char *type_text = NULL;
      if (!parse_tok_text_alloc_strict(c, ctx->in_path, &type_text)) {
        free(tkey);
        return false;
      }
      sem2sir_type_id tid = sem2sir_type_parse(type_text, strlen(type_text));
      free(type_text);
      if (tid == SEM2SIR_TYPE_INVALID) {
        err(ctx->in_path, "TypeRef.name must be a normalized sem2sir type id");
        free(tkey);
        return false;
      }
      out_ti->base = tid;
      out_ti->ptr_of = SEM2SIR_TYPE_INVALID;
      out_ti->sir_id = sir_type_id_for(tid);
      if (!out_ti->sir_id) {
        err(ctx->in_path, "unsupported TypeRef type");
        free(tkey);
        return false;
      }
    } else {
      // Ignore other allowed fields (nid/span) and reject unknown by earlier checker.
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid TypeRef field");
        free(tkey);
        return false;
      }
    }
    free(tkey);
  }

  if (!seen_name) {
    err(ctx->in_path, "TypeRef missing required field name");
    return false;
  }
  return true;
}

