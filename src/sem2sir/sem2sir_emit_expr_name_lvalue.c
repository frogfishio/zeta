#include "sem2sir_emit_internal.h"

bool parse_expr_name(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
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

  sem2sir_type_id t = SEM2SIR_TYPE_INVALID;
  sem2sir_type_id ptr_of = SEM2SIR_TYPE_INVALID;
  const char *tid = NULL;
  bool is_slot = false;
  if (!locals_lookup(ctx, name_text, &t, &ptr_of, &tid, &is_slot)) {
    err(ctx->in_path, "Name refers to unknown local (no implicit globals)");
    free(name_text);
    return false;
  }
  if (expected != SEM2SIR_TYPE_INVALID && expected != t) {
    err(ctx->in_path, "Name type mismatch against expected type");
    free(name_text);
    return false;
  }

  if (!tid)
    tid = sir_type_id_for(t);
  SemTypeInfo ti = {.base = t, .ptr_of = ptr_of, .sir_id = tid};
  if (!emit_typeinfo_if_needed(ctx, &ti)) {
    free(name_text);
    return false;
  }

  if (!tid) {
    err(ctx->in_path, "unsupported name type");
    free(name_text);
    return false;
  }

  if (!is_slot) {
    char *nid = new_node_id(ctx);
    fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
    emit_json_string(ctx->out, nid);
    fprintf(ctx->out, ",\"tag\":\"name\",\"type_ref\":");
    emit_json_string(ctx->out, tid);
    fprintf(ctx->out, ",\"fields\":{\"name\":");
    emit_json_string(ctx->out, name_text);
    fprintf(ctx->out, "}}\n");

    free(name_text);
    out->id = nid;
    out->type = t;
    out->ptr_of = ptr_of;
    out->sir_type_id = tid;
    return true;
  }

  // Slot-backed locals: the binding is a ptr; Name() loads the value.
  const char *addr_tid = "t:ptr";
  SemTypeInfo addr_ti = {.base = SEM2SIR_TYPE_PTR, .ptr_of = SEM2SIR_TYPE_INVALID, .sir_id = addr_tid};
  if (t != SEM2SIR_TYPE_PTR && t != SEM2SIR_TYPE_SLICE && type_store_tag(t) && type_load_tag(t) && type_align_bytes(t) != 0) {
    const char *did = get_derived_ptr_type_id(ctx, t);
    if (did) {
      addr_tid = did;
      addr_ti.ptr_of = t;
      addr_ti.sir_id = did;
    }
  }
  if (!emit_typeinfo_if_needed(ctx, &addr_ti)) {
    free(name_text);
    return false;
  }

  const char *load_tag = type_load_tag(t);
  int align = type_align_bytes(t);
  if (!load_tag || align == 0) {
    err(ctx->in_path, "Name slot type not supported for load");
    free(name_text);
    return false;
  }

  char *addr_id = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, addr_id);
  fprintf(ctx->out, ",\"tag\":\"name\",\"type_ref\":");
  emit_json_string(ctx->out, addr_tid);
  fprintf(ctx->out, ",\"fields\":{\"name\":");
  emit_json_string(ctx->out, name_text);
  fprintf(ctx->out, "}}\n");

  char *nid = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, nid);
  fprintf(ctx->out, ",\"tag\":");
  emit_json_string(ctx->out, load_tag);
  fprintf(ctx->out, ",\"type_ref\":");
  emit_json_string(ctx->out, tid);
  fprintf(ctx->out, ",\"fields\":{\"addr\":{\"t\":\"ref\",\"id\":");
  emit_json_string(ctx->out, addr_id);
  fprintf(ctx->out, "},\"align\":%d}}\n", align);

  free(addr_id);
  free(name_text);
  out->id = nid;
  out->type = t;
  out->ptr_of = ptr_of;
  out->sir_type_id = tid;
  return true;
}

bool parse_name_id_only(GritJsonCursor *c, EmitCtx *ctx, char **out_name_text) {
  *out_name_text = NULL;
  char *k = NULL;
  if (!parse_node_k_string(c, ctx, &k))
    return false;
  if (strcmp(k, "Name") != 0) {
    err(ctx->in_path, "expected Name node");
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
  *out_name_text = name_text;
  return true;
}

bool parse_lvalue_addr(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id store_ty, SirExpr *out_addr) {
  out_addr->id = NULL;
  out_addr->type = SEM2SIR_TYPE_INVALID;
  out_addr->ptr_of = SEM2SIR_TYPE_INVALID;
  out_addr->sir_type_id = NULL;

  if (store_ty == SEM2SIR_TYPE_INVALID) {
    err(ctx->in_path, "assignment requires an explicit store type (no implicit pointee typing)");
    return false;
  }

  char *k = NULL;
  if (!parse_node_k_string(c, ctx, &k))
    return false;

  if (strcmp(k, "Name") == 0) {
    free(k);
    k = NULL;

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

    sem2sir_type_id local_ty = SEM2SIR_TYPE_INVALID;
    bool is_slot = false;
    if (!locals_lookup(ctx, name_text, &local_ty, NULL, NULL, &is_slot)) {
      err(ctx->in_path, "assignment lhs refers to unknown local");
      free(name_text);
      return false;
    }
    if (!is_slot) {
      err(ctx->in_path, "assignment lhs must be a slot-backed local in emitter MVP");
      free(name_text);
      return false;
    }
    if (local_ty != store_ty) {
      err(ctx->in_path, "assignment lhs type mismatch against committed store type");
      free(name_text);
      return false;
    }
    if (!type_supports_slot_storage(local_ty)) {
      err(ctx->in_path, "assignment lhs type not supported for store in emitter MVP");
      free(name_text);
      return false;
    }

    const char *addr_tid = "t:ptr";
    SemTypeInfo addr_ti = {.base = SEM2SIR_TYPE_PTR, .ptr_of = SEM2SIR_TYPE_INVALID, .sir_id = addr_tid};
    if (store_ty != SEM2SIR_TYPE_PTR && store_ty != SEM2SIR_TYPE_SLICE && type_store_tag(store_ty) && type_load_tag(store_ty) &&
        type_align_bytes(store_ty) != 0) {
      const char *did = get_derived_ptr_type_id(ctx, store_ty);
      if (did) {
        addr_tid = did;
        addr_ti.ptr_of = store_ty;
        addr_ti.sir_id = did;
      }
    }
    if (!emit_typeinfo_if_needed(ctx, &addr_ti)) {
      free(name_text);
      return false;
    }

    char *addr_id = new_node_id(ctx);
    fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
    emit_json_string(ctx->out, addr_id);
    fprintf(ctx->out, ",\"tag\":\"name\",\"type_ref\":");
    emit_json_string(ctx->out, addr_tid);
    fprintf(ctx->out, ",\"fields\":{\"name\":");
    emit_json_string(ctx->out, name_text);
    fprintf(ctx->out, "}}\n");

    free(name_text);
    out_addr->id = addr_id;
    out_addr->type = SEM2SIR_TYPE_PTR;
    out_addr->ptr_of = addr_ti.ptr_of;
    out_addr->sir_type_id = addr_tid;
    return true;
  }

  if (strcmp(k, "Deref") == 0) {
    free(k);
    k = NULL;

    if (store_ty == SEM2SIR_TYPE_PTR) {
      err(ctx->in_path, "assignment through Deref of ptr would require ptr-to-ptr which sem2sir MVP does not model");
      return false;
    }

    bool seen_expr = false;
    SirExpr p = {0};
    char ch = 0;
    for (;;) {
      if (!json_peek_non_ws(c, &ch)) {
        err(ctx->in_path, "unexpected EOF in Deref");
        free(p.id);
        return false;
      }
      if (ch == '}') {
        c->p++;
        break;
      }
      if (ch != ',') {
        err(ctx->in_path, "expected ',' or '}' in Deref");
        free(p.id);
        return false;
      }
      c->p++;
      char *key = NULL;
      if (!json_expect_key(c, &key)) {
        err(ctx->in_path, "invalid Deref key");
        free(p.id);
        return false;
      }
      if (strcmp(key, "expr") == 0) {
        seen_expr = true;
        if (!parse_expr(c, ctx, SEM2SIR_TYPE_PTR, &p)) {
          free(key);
          return false;
        }
      } else {
        if (!grit_json_skip_value(c)) {
          err(ctx->in_path, "invalid Deref field");
          free(key);
          free(p.id);
          return false;
        }
      }
      free(key);
    }

    if (!seen_expr || !p.id) {
      err(ctx->in_path, "Deref requires field: expr");
      free(p.id);
      return false;
    }
    if (p.type != SEM2SIR_TYPE_PTR) {
      err(ctx->in_path, "Deref expr must be ptr");
      free(p.id);
      return false;
    }

    if (p.ptr_of != SEM2SIR_TYPE_INVALID) {
      if (p.ptr_of == SEM2SIR_TYPE_VOID) {
        err(ctx->in_path, "cannot assign through ptr(void) (opaque pointer)");
        free(p.id);
        return false;
      }
      if (store_ty != p.ptr_of) {
        err(ctx->in_path, "assignment store type does not match pointer pointee type");
        free(p.id);
        return false;
      }
    }

    // LValue address for Deref is the pointer expression itself.
    out_addr->id = p.id;
    out_addr->type = SEM2SIR_TYPE_PTR;
    out_addr->ptr_of = p.ptr_of;
    out_addr->sir_type_id = p.sir_type_id;
    p.id = NULL;
    return true;
  }

  err(ctx->in_path, "assignment lhs must be Name(id) or Deref(expr) in emitter MVP");
  free(k);
  return false;
}

