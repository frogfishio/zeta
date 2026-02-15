#include "sem2sir_emit_internal.h"

bool parse_expr_addrof(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
  // &x is only supported for addressable locals; since sem2sir has an unparameterized ptr type,
  // we treat AddrOf as producing a raw ptr.
  if (expected != SEM2SIR_TYPE_PTR) {
    err(ctx->in_path, "AddrOf requires expected type ptr (no implicit pointer typing)");
    return false;
  }

  bool seen_expr = false;
  char *name_text = NULL;
  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in AddrOf");
      free(name_text);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in AddrOf");
      free(name_text);
      return false;
    }
    c->p++;
    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid AddrOf key");
      free(name_text);
      return false;
    }
    if (strcmp(key, "expr") == 0) {
      seen_expr = true;
      if (!parse_name_id_only(c, ctx, &name_text)) {
        free(key);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid AddrOf field");
        free(key);
        free(name_text);
        return false;
      }
    }
    free(key);
  }

  if (!seen_expr || !name_text) {
    err(ctx->in_path, "AddrOf requires field: expr");
    free(name_text);
    return false;
  }

  sem2sir_type_id t = SEM2SIR_TYPE_INVALID;
  bool is_slot = false;
  if (!locals_lookup(ctx, name_text, &t, NULL, NULL, &is_slot)) {
    err(ctx->in_path, "AddrOf refers to unknown local");
    free(name_text);
    return false;
  }
  if (!is_slot) {
    err(ctx->in_path, "AddrOf requires an addressable local (slot-backed)");
    free(name_text);
    return false;
  }
  if (t == SEM2SIR_TYPE_PTR) {
    err(ctx->in_path, "AddrOf(ptr) would require ptr-to-ptr which sem2sir MVP does not model");
    free(name_text);
    return false;
  }

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

  char *nid = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, nid);
  fprintf(ctx->out, ",\"tag\":\"name\",\"type_ref\":");
  emit_json_string(ctx->out, addr_tid);
  fprintf(ctx->out, ",\"fields\":{\"name\":");
  emit_json_string(ctx->out, name_text);
  fprintf(ctx->out, "}}\n");

  free(name_text);
  out->id = nid;
  out->type = SEM2SIR_TYPE_PTR;
  out->ptr_of = addr_ti.ptr_of;
  out->sir_type_id = addr_tid;
  return true;
}

bool parse_expr_deref(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out) {
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

  if (!seen_expr) {
    err(ctx->in_path, "Deref requires field: expr");
    free(p.id);
    return false;
  }
  if (p.type != SEM2SIR_TYPE_PTR) {
    err(ctx->in_path, "Deref expr must be ptr");
    free(p.id);
    return false;
  }

  sem2sir_type_id result_ty = expected;
  if (result_ty == SEM2SIR_TYPE_INVALID) {
    if (p.ptr_of != SEM2SIR_TYPE_INVALID && p.ptr_of != SEM2SIR_TYPE_VOID) {
      // Explicit pointee typing from ptr(T) commits the deref result type.
      result_ty = p.ptr_of;
    } else if (ctx->default_ptr_pointee != SEM2SIR_TYPE_INVALID) {
      result_ty = ctx->default_ptr_pointee;
    } else {
      err(ctx->in_path,
          "Deref requires an expected type unless meta.types['@default.ptr.pointee'/'__default_ptr_pointee'] is set (no implicit pointee typing)");
      free(p.id);
      return false;
    }
  }
  if (result_ty == SEM2SIR_TYPE_PTR) {
    err(ctx->in_path, "Deref result ptr would require ptr-to-ptr which sem2sir MVP does not model");
    free(p.id);
    return false;
  }
  if (p.ptr_of != SEM2SIR_TYPE_INVALID) {
    if (p.ptr_of == SEM2SIR_TYPE_VOID) {
      err(ctx->in_path, "cannot Deref a ptr(void) (opaque pointer)");
      free(p.id);
      return false;
    }
    if (result_ty != p.ptr_of) {
      err(ctx->in_path, "Deref result type does not match pointer pointee type");
      free(p.id);
      return false;
    }
  }

  const char *load_tag = type_load_tag(result_ty);
  int align = type_align_bytes(result_ty);
  if (!load_tag || align == 0) {
    err(ctx->in_path, "Deref result type not supported for load");
    free(p.id);
    return false;
  }

  if (!emit_type_if_needed(ctx, result_ty)) {
    free(p.id);
    return false;
  }
  const char *tid = sir_type_id_for(result_ty);
  if (!tid) {
    err(ctx->in_path, "unsupported Deref result type");
    free(p.id);
    return false;
  }

  char *nid = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, nid);
  fprintf(ctx->out, ",\"tag\":");
  emit_json_string(ctx->out, load_tag);
  fprintf(ctx->out, ",\"type_ref\":");
  emit_json_string(ctx->out, tid);
  fprintf(ctx->out, ",\"fields\":{\"addr\":{\"t\":\"ref\",\"id\":");
  emit_json_string(ctx->out, p.id);
  fprintf(ctx->out, "},\"align\":%d}}\n", align);

  free(p.id);
  out->id = nid;
  out->type = result_ty;
  out->ptr_of = SEM2SIR_TYPE_INVALID;
  out->sir_type_id = tid;
  return true;
}

