#include "sem2sir_emit_internal.h"

static bool parse_pat_bind_name_alloc_strict(GritJsonCursor *c, EmitCtx *ctx, char **out_name) {
  *out_name = NULL;
  char *k = NULL;
  if (!parse_node_k_string(c, ctx, &k)) return false;
  if (strcmp(k, "PatBind") != 0) {
    err(ctx->in_path, "expected PatBind pattern");
    free(k);
    return false;
  }
  free(k);

  bool seen_name = false;
  char *name_text = NULL;
  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in PatBind");
      free(name_text);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in PatBind");
      free(name_text);
      return false;
    }
    c->p++;
    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid PatBind key");
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
        err(ctx->in_path, "invalid PatBind field");
        free(key);
        free(name_text);
        return false;
      }
    }
    free(key);
  }

  if (!seen_name || !name_text) {
    err(ctx->in_path, "PatBind missing required field name");
    free(name_text);
    return false;
  }
  *out_name = name_text;
  return true;
}

static bool forint_parse_var_name_alloc_strict(const char *var_json, size_t var_len, EmitCtx *ctx, char **out_name) {
  *out_name = NULL;
  if (!var_json || var_len == 0) {
    err(ctx->in_path, "ForInt.var missing JSON");
    return false;
  }

  GritJsonCursor vc = grit_json_cursor(var_json, var_len);
  char *k = NULL;
  if (!parse_node_k_string(&vc, ctx, &k))
    return false;

  bool is_var = (strcmp(k, "Var") == 0);
  bool is_var_pat = (strcmp(k, "VarPat") == 0);
  if (!is_var && !is_var_pat) {
    err(ctx->in_path, "ForInt.var must be Var or VarPat");
    free(k);
    return false;
  }
  free(k);

  bool seen_name = false;
  bool seen_pat = false;
  char *name_text = NULL;
  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(&vc, &ch)) {
      err(ctx->in_path, "unexpected EOF in ForInt.var");
      free(name_text);
      return false;
    }
    if (ch == '}') {
      vc.p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in ForInt.var");
      free(name_text);
      return false;
    }
    vc.p++;
    char *key = NULL;
    if (!json_expect_key(&vc, &key)) {
      err(ctx->in_path, "invalid ForInt.var key");
      free(name_text);
      return false;
    }

    if (is_var && strcmp(key, "name") == 0) {
      seen_name = true;
      if (!parse_tok_text_alloc_strict(&vc, ctx->in_path, &name_text)) {
        free(key);
        return false;
      }
    } else if (is_var_pat && strcmp(key, "pat") == 0) {
      seen_pat = true;
      if (!parse_pat_bind_name_alloc_strict(&vc, ctx, &name_text)) {
        free(key);
        return false;
      }
    } else {
      if (!grit_json_skip_value(&vc)) {
        err(ctx->in_path, "invalid ForInt.var field");
        free(key);
        free(name_text);
        return false;
      }
    }

    free(key);
  }

  if ((is_var && (!seen_name || !name_text)) || (is_var_pat && (!seen_pat || !name_text))) {
    err(ctx->in_path, "ForInt.var must bind a name (Var.name or VarPat.pat=PatBind)");
    free(name_text);
    return false;
  }

  *out_name = name_text;
  return true;
}

bool parse_block(GritJsonCursor *c, EmitCtx *ctx, SirFnBuild *fn, bool require_return, const LoopTargets *loop);

bool skip_remaining_object_fields(GritJsonCursor *c, EmitCtx *ctx, const char *what) {
  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF");
      return false;
    }
    if (ch == '}') {
      c->p++;
      return true;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}'");
      return false;
    }
    c->p++;
    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid key");
      return false;
    }
    if (!grit_json_skip_value(c)) {
      err(ctx->in_path, "invalid field");
      free(key);
      return false;
    }
    free(key);
  }

  (void)what;
  return false;
}

bool parse_stmt_if(GritJsonCursor *c, EmitCtx *ctx, SirFnBuild *fn, const LoopTargets *loop) {
  bool seen_cond = false;
  bool seen_then = false;
  bool seen_else = false;

  SirExpr cond = {0};
  StmtList cond_effects = {0};

  // We'll parse blocks into newly created SIR blocks.
  size_t then_idx = 0;
  size_t else_idx = 0;
  size_t join_idx = 0;
  bool join_created = false;

  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in If");
      free(cond.id);
      free(cond_effects.ids);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in If");
      free(cond.id);
      free(cond_effects.ids);
      return false;
    }
    c->p++;
    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid If key");
      free(cond.id);
      free(cond_effects.ids);
      return false;
    }

    if (strcmp(key, "cond") == 0) {
      seen_cond = true;
      StmtList *saved = ctx->effects;
      ctx->effects = &cond_effects;
      bool ok = parse_expr(c, ctx, SEM2SIR_TYPE_BOOL, &cond);
      ctx->effects = saved;
      free(key);
      if (!ok)
        return false;
      continue;
    }
    if (strcmp(key, "then") == 0) {
      seen_then = true;

      // Control-flow split: allocate then/else blocks once we see then.
      if (!fn_build_new_block(fn, ctx, &then_idx) || !fn_build_new_block(fn, ctx, &else_idx)) {
        err(ctx->in_path, "OOM creating If blocks");
        free(key);
        free(cond.id);
        free(cond_effects.ids);
        return false;
      }

      if (!fn_build_append_effects(fn, ctx, &cond_effects)) {
        free(key);
        free(cond.id);
        return false;
      }
      if (cond.type != SEM2SIR_TYPE_BOOL) {
        err(ctx->in_path, "If.cond must be bool");
        free(key);
        free(cond.id);
        return false;
      }
      char *term_id = NULL;
      if (!emit_term_condbr(ctx, cond.id, fn->blocks[then_idx].id, fn->blocks[else_idx].id, &term_id)) {
        err(ctx->in_path, "OOM emitting term.condbr");
        free(key);
        free(cond.id);
        return false;
      }
      if (!fn_build_append_stmt(fn, ctx, term_id, true)) {
        free(key);
        free(cond.id);
        return false;
      }

      // Parse then block.
      fn->cur_block = then_idx;
      if (!parse_block(c, ctx, fn, false, loop)) {
        free(key);
        free(cond.id);
        return false;
      }
      if (!fn->blocks[fn->cur_block].terminated) {
        if (!join_created) {
          if (!fn_build_new_block(fn, ctx, &join_idx)) {
            err(ctx->in_path, "OOM creating If join block");
            free(key);
            free(cond.id);
            return false;
          }
          join_created = true;
        }
        char *br_id = NULL;
        if (!emit_term_br(ctx, fn->blocks[join_idx].id, &br_id)) {
          free(key);
          free(cond.id);
          return false;
        }
        if (!fn_build_append_stmt(fn, ctx, br_id, true)) {
          free(key);
          free(cond.id);
          return false;
        }
      }

      // We'll parse else later if present; otherwise we'll fill it with a branch.
      free(key);
      continue;
    }
    if (strcmp(key, "else") == 0) {
      seen_else = true;
      // else may be null (no else branch)
      if (!seen_then) {
        err(ctx->in_path, "If.then must appear before If.else (no implicit context)");
        free(key);
        free(cond.id);
        return false;
      }
      if (!json_peek_non_ws(c, &ch)) {
        err(ctx->in_path, "unexpected EOF in If.else");
        free(key);
        free(cond.id);
        return false;
      }
      if (ch == 'n') {
        // null: no else branch.
        if (!grit_json_skip_value(c)) {
          free(key);
          free(cond.id);
          return false;
        }
        if (!join_created) {
          if (!fn_build_new_block(fn, ctx, &join_idx)) {
            err(ctx->in_path, "OOM creating If join block");
            free(key);
            free(cond.id);
            return false;
          }
          join_created = true;
        }
        free(key);
        continue;
      }

      fn->cur_block = else_idx;
      if (!parse_block(c, ctx, fn, false, loop)) {
        free(key);
        free(cond.id);
        return false;
      }
      if (!fn->blocks[fn->cur_block].terminated) {
        if (!join_created) {
          if (!fn_build_new_block(fn, ctx, &join_idx)) {
            err(ctx->in_path, "OOM creating If join block");
            free(key);
            free(cond.id);
            return false;
          }
          join_created = true;
        }
        char *br_id = NULL;
        if (!emit_term_br(ctx, fn->blocks[join_idx].id, &br_id)) {
          free(key);
          free(cond.id);
          return false;
        }
        if (!fn_build_append_stmt(fn, ctx, br_id, true)) {
          free(key);
          free(cond.id);
          return false;
        }
      }
      free(key);
      continue;
    }

    if (!grit_json_skip_value(c)) {
      err(ctx->in_path, "invalid If field");
      free(key);
      free(cond.id);
      free(cond_effects.ids);
      return false;
    }
    free(key);
  }

  if (!seen_cond || !seen_then) {
    err(ctx->in_path, "If requires fields: cond, then");
    free(cond.id);
    free(cond_effects.ids);
    return false;
  }

  // If no else body was parsed (or else=null), ensure else block falls through to join.
  if (!seen_else) {
    // Absent else is treated as empty else -> join.
    if (!join_created) {
      if (!fn_build_new_block(fn, ctx, &join_idx)) {
        err(ctx->in_path, "OOM creating If join block");
        free(cond.id);
        return false;
      }
      join_created = true;
    }
    fn->cur_block = else_idx;
    if (!fn->blocks[fn->cur_block].terminated) {
      char *br_id = NULL;
      if (!emit_term_br(ctx, fn->blocks[join_idx].id, &br_id)) {
        free(cond.id);
        return false;
      }
      if (!fn_build_append_stmt(fn, ctx, br_id, true)) {
        free(cond.id);
        return false;
      }
    }
  }

  if (join_created) {
    // Ensure else block ends in a branch if it's still empty+unterminated.
    fn->cur_block = else_idx;
    if (!fn->blocks[fn->cur_block].terminated) {
      char *br_id = NULL;
      if (!emit_term_br(ctx, fn->blocks[join_idx].id, &br_id)) {
        free(cond.id);
        return false;
      }
      if (!fn_build_append_stmt(fn, ctx, br_id, true)) {
        free(cond.id);
        return false;
      }
    }

    // Continue in join.
    fn->cur_block = join_idx;
  } else {
    // Both branches terminated; treat following statements as invalid/unreachable.
    fn->cur_block = else_idx;
  }
  free(cond.id);
  return true;
}

bool parse_stmt_while(GritJsonCursor *c, EmitCtx *ctx, SirFnBuild *fn) {
  bool seen_cond = false;
  bool seen_body = false;

  // Allocate blocks upfront: header (cond), body, exit.
  size_t header_idx = 0;
  size_t body_idx = 0;
  size_t exit_idx = 0;
  if (!fn_build_new_block(fn, ctx, &header_idx) || !fn_build_new_block(fn, ctx, &body_idx) ||
      !fn_build_new_block(fn, ctx, &exit_idx)) {
    err(ctx->in_path, "OOM creating While blocks");
    return false;
  }

  // Jump from current block to header.
  char *br_to_header = NULL;
  if (!emit_term_br(ctx, fn->blocks[header_idx].id, &br_to_header)) {
    err(ctx->in_path, "OOM emitting term.br");
    return false;
  }
  if (!fn_build_append_stmt(fn, ctx, br_to_header, true)) {
    return false;
  }

  SirExpr cond = {0};
  StmtList cond_effects = {0};

  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in While");
      free(cond.id);
      free(cond_effects.ids);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in While");
      free(cond.id);
      free(cond_effects.ids);
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid While key");
      free(cond.id);
      free(cond_effects.ids);
      return false;
    }

    if (strcmp(key, "cond") == 0) {
      seen_cond = true;
      StmtList *saved = ctx->effects;
      ctx->effects = &cond_effects;
      bool ok = parse_expr(c, ctx, SEM2SIR_TYPE_BOOL, &cond);
      ctx->effects = saved;
      free(key);
      if (!ok)
        return false;
      continue;
    }
    if (strcmp(key, "body") == 0) {
      seen_body = true;

      // Finish header once cond is known.
      if (!seen_cond) {
        err(ctx->in_path, "While.cond must appear before While.body (no implicit context)");
        free(key);
        free(cond.id);
        free(cond_effects.ids);
        return false;
      }
      fn->cur_block = header_idx;
      if (!fn_build_append_effects(fn, ctx, &cond_effects)) {
        free(key);
        free(cond.id);
        return false;
      }
      if (cond.type != SEM2SIR_TYPE_BOOL) {
        err(ctx->in_path, "While.cond must be bool");
        free(key);
        free(cond.id);
        return false;
      }
      char *t_id = NULL;
      if (!emit_term_condbr(ctx, cond.id, fn->blocks[body_idx].id, fn->blocks[exit_idx].id, &t_id)) {
        free(key);
        free(cond.id);
        return false;
      }
      if (!fn_build_append_stmt(fn, ctx, t_id, true)) {
        free(key);
        free(cond.id);
        return false;
      }

      // Parse body block in body_idx.
      LoopTargets targets = {.break_to = exit_idx, .continue_to = header_idx};
      fn->cur_block = body_idx;
      if (!parse_block(c, ctx, fn, false, &targets)) {
        free(key);
        free(cond.id);
        return false;
      }
      if (!fn->blocks[fn->cur_block].terminated) {
        char *back_id = NULL;
        if (!emit_term_br(ctx, fn->blocks[header_idx].id, &back_id)) {
          free(key);
          free(cond.id);
          return false;
        }
        if (!fn_build_append_stmt(fn, ctx, back_id, true)) {
          free(key);
          free(cond.id);
          return false;
        }
      }

      free(key);
      continue;
    }

    if (!grit_json_skip_value(c)) {
      err(ctx->in_path, "invalid While field");
      free(key);
      free(cond.id);
      free(cond_effects.ids);
      return false;
    }
    free(key);
  }

  if (!seen_cond || !seen_body) {
    err(ctx->in_path, "While requires fields: cond, body");
    free(cond.id);
    free(cond_effects.ids);
    return false;
  }

  // Continue after the loop.
  fn->cur_block = exit_idx;
  free(cond.id);
  return true;
}

bool parse_stmt_loop(GritJsonCursor *c, EmitCtx *ctx, SirFnBuild *fn) {
  bool seen_body = false;

  // Allocate blocks upfront: body, exit.
  size_t body_idx = 0;
  size_t exit_idx = 0;
  if (!fn_build_new_block(fn, ctx, &body_idx) || !fn_build_new_block(fn, ctx, &exit_idx)) {
    err(ctx->in_path, "OOM creating Loop blocks");
    return false;
  }

  // Jump from current block to body.
  char *br_to_body = NULL;
  if (!emit_term_br(ctx, fn->blocks[body_idx].id, &br_to_body)) {
    err(ctx->in_path, "OOM emitting term.br");
    return false;
  }
  if (!fn_build_append_stmt(fn, ctx, br_to_body, true)) {
    return false;
  }

  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in Loop");
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in Loop");
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid Loop key");
      return false;
    }

    if (strcmp(key, "body") == 0) {
      seen_body = true;

      LoopTargets targets = {.break_to = exit_idx, .continue_to = body_idx};
      fn->cur_block = body_idx;
      if (!parse_block(c, ctx, fn, false, &targets)) {
        free(key);
        return false;
      }

      // If body falls through, loop back to the top of the body.
      if (!fn->blocks[fn->cur_block].terminated) {
        char *back_id = NULL;
        if (!emit_term_br(ctx, fn->blocks[body_idx].id, &back_id)) {
          free(key);
          return false;
        }
        if (!fn_build_append_stmt(fn, ctx, back_id, true)) {
          free(key);
          return false;
        }
      }

      free(key);
      continue;
    }

    if (!grit_json_skip_value(c)) {
      err(ctx->in_path, "invalid Loop field");
      free(key);
      return false;
    }
    free(key);
  }

  if (!seen_body) {
    err(ctx->in_path, "Loop requires field: body");
    return false;
  }

  // Continue after the loop.
  fn->cur_block = exit_idx;
  return true;
}

bool parse_stmt_do_while(GritJsonCursor *c, EmitCtx *ctx, SirFnBuild *fn) {
  bool seen_body = false;
  bool seen_cond = false;

  char *body_json = NULL;
  size_t body_len = 0;
  char *cond_json = NULL;
  size_t cond_len = 0;

  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in DoWhile");
      free(body_json);
      free(cond_json);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in DoWhile");
      free(body_json);
      free(cond_json);
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid DoWhile key");
      free(body_json);
      free(cond_json);
      return false;
    }

    if (strcmp(key, "body") == 0) {
      seen_body = true;
      free(body_json);
      body_json = NULL;
      if (!capture_json_value_alloc(c, &body_json, &body_len)) {
        err(ctx->in_path, "invalid DoWhile.body");
        free(key);
        free(cond_json);
        return false;
      }
      free(key);
      continue;
    }

    if (strcmp(key, "cond") == 0) {
      seen_cond = true;
      free(cond_json);
      cond_json = NULL;
      if (!capture_json_value_alloc(c, &cond_json, &cond_len)) {
        err(ctx->in_path, "invalid DoWhile.cond");
        free(key);
        free(body_json);
        return false;
      }
      free(key);
      continue;
    }

    if (!grit_json_skip_value(c)) {
      err(ctx->in_path, "invalid DoWhile field");
      free(key);
      free(body_json);
      free(cond_json);
      return false;
    }
    free(key);
  }

  if (!seen_body || !seen_cond || !body_json || !cond_json) {
    err(ctx->in_path, "DoWhile requires fields: body, cond");
    free(body_json);
    free(cond_json);
    return false;
  }

  // Allocate blocks: body, cond-check, exit.
  size_t body_idx = 0;
  size_t cond_idx = 0;
  size_t exit_idx = 0;
  if (!fn_build_new_block(fn, ctx, &body_idx) || !fn_build_new_block(fn, ctx, &cond_idx) ||
      !fn_build_new_block(fn, ctx, &exit_idx)) {
    err(ctx->in_path, "OOM creating DoWhile blocks");
    free(body_json);
    free(cond_json);
    return false;
  }

  // Jump to body.
  char *br_to_body = NULL;
  if (!emit_term_br(ctx, fn->blocks[body_idx].id, &br_to_body)) {
    err(ctx->in_path, "OOM emitting term.br");
    free(body_json);
    free(cond_json);
    return false;
  }
  if (!fn_build_append_stmt(fn, ctx, br_to_body, true)) {
    free(body_json);
    free(cond_json);
    return false;
  }

  // Parse body.
  LoopTargets targets = {.break_to = exit_idx, .continue_to = cond_idx};
  fn->cur_block = body_idx;
  GritJsonCursor bc = grit_json_cursor(body_json, body_len);
  if (!parse_block(&bc, ctx, fn, false, &targets)) {
    free(body_json);
    free(cond_json);
    return false;
  }
  if (!fn->blocks[fn->cur_block].terminated) {
    char *to_cond = NULL;
    if (!emit_term_br(ctx, fn->blocks[cond_idx].id, &to_cond)) {
      free(body_json);
      free(cond_json);
      return false;
    }
    if (!fn_build_append_stmt(fn, ctx, to_cond, true)) {
      free(body_json);
      free(cond_json);
      return false;
    }
  }

  // Parse cond-check.
  fn->cur_block = cond_idx;
  SirExpr cond = {0};
  StmtList cond_effects = {0};
  {
    GritJsonCursor cc = grit_json_cursor(cond_json, cond_len);
    StmtList *saved = ctx->effects;
    ctx->effects = &cond_effects;
    bool ok = parse_expr(&cc, ctx, SEM2SIR_TYPE_BOOL, &cond);
    ctx->effects = saved;
    if (!ok) {
      free(body_json);
      free(cond_json);
      free(cond_effects.ids);
      return false;
    }
  }
  if (!fn_build_append_effects(fn, ctx, &cond_effects)) {
    free(body_json);
    free(cond_json);
    free(cond.id);
    return false;
  }
  if (cond.type != SEM2SIR_TYPE_BOOL) {
    err(ctx->in_path, "DoWhile.cond must be bool");
    free(body_json);
    free(cond_json);
    free(cond.id);
    return false;
  }
  char *t_id = NULL;
  if (!emit_term_condbr(ctx, cond.id, fn->blocks[body_idx].id, fn->blocks[exit_idx].id, &t_id)) {
    free(body_json);
    free(cond_json);
    free(cond.id);
    return false;
  }
  if (!fn_build_append_stmt(fn, ctx, t_id, true)) {
    free(body_json);
    free(cond_json);
    free(cond.id);
    return false;
  }

  fn->cur_block = exit_idx;
  free(body_json);
  free(cond_json);
  free(cond.id);
  return true;
}

bool parse_stmt_for(GritJsonCursor *c, EmitCtx *ctx, SirFnBuild *fn) {
  bool seen_body = false;
  bool init_is_null = true;
  bool cond_is_null = true;
  bool step_is_null = true;

  char *init_json = NULL;
  size_t init_len = 0;
  char *cond_json = NULL;
  size_t cond_len = 0;
  char *step_json = NULL;
  size_t step_len = 0;
  char *body_json = NULL;
  size_t body_len = 0;

  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in For");
      free(init_json);
      free(cond_json);
      free(step_json);
      free(body_json);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in For");
      free(init_json);
      free(cond_json);
      free(step_json);
      free(body_json);
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid For key");
      free(init_json);
      free(cond_json);
      free(step_json);
      free(body_json);
      return false;
    }

    if (strcmp(key, "init") == 0) {
      free(init_json);
      init_json = NULL;
      init_len = 0;
      if (!json_peek_non_ws(c, &ch)) {
        err(ctx->in_path, "unexpected EOF in For.init");
        free(key);
        free(cond_json);
        free(step_json);
        free(body_json);
        return false;
      }
      if (ch == 'n') {
        init_is_null = true;
        if (!grit_json_skip_value(c)) {
          err(ctx->in_path, "invalid For.init");
          free(key);
          free(cond_json);
          free(step_json);
          free(body_json);
          return false;
        }
      } else {
        init_is_null = false;
        if (!capture_json_value_alloc(c, &init_json, &init_len)) {
          err(ctx->in_path, "invalid For.init");
          free(key);
          free(cond_json);
          free(step_json);
          free(body_json);
          return false;
        }
      }
      free(key);
      continue;
    }

    if (strcmp(key, "cond") == 0) {
      free(cond_json);
      cond_json = NULL;
      cond_len = 0;
      if (!json_peek_non_ws(c, &ch)) {
        err(ctx->in_path, "unexpected EOF in For.cond");
        free(key);
        free(init_json);
        free(step_json);
        free(body_json);
        return false;
      }
      if (ch == 'n') {
        cond_is_null = true;
        if (!grit_json_skip_value(c)) {
          err(ctx->in_path, "invalid For.cond");
          free(key);
          free(init_json);
          free(step_json);
          free(body_json);
          return false;
        }
      } else {
        cond_is_null = false;
        if (!capture_json_value_alloc(c, &cond_json, &cond_len)) {
          err(ctx->in_path, "invalid For.cond");
          free(key);
          free(init_json);
          free(step_json);
          free(body_json);
          return false;
        }
      }
      free(key);
      continue;
    }

    if (strcmp(key, "step") == 0) {
      free(step_json);
      step_json = NULL;
      step_len = 0;
      if (!json_peek_non_ws(c, &ch)) {
        err(ctx->in_path, "unexpected EOF in For.step");
        free(key);
        free(init_json);
        free(cond_json);
        free(body_json);
        return false;
      }
      if (ch == 'n') {
        step_is_null = true;
        if (!grit_json_skip_value(c)) {
          err(ctx->in_path, "invalid For.step");
          free(key);
          free(init_json);
          free(cond_json);
          free(body_json);
          return false;
        }
      } else {
        step_is_null = false;
        if (!capture_json_value_alloc(c, &step_json, &step_len)) {
          err(ctx->in_path, "invalid For.step");
          free(key);
          free(init_json);
          free(cond_json);
          free(body_json);
          return false;
        }
      }
      free(key);
      continue;
    }

    if (strcmp(key, "body") == 0) {
      seen_body = true;
      free(body_json);
      body_json = NULL;
      if (!capture_json_value_alloc(c, &body_json, &body_len)) {
        err(ctx->in_path, "invalid For.body");
        free(key);
        free(init_json);
        free(cond_json);
        free(step_json);
        return false;
      }
      free(key);
      continue;
    }

    if (!grit_json_skip_value(c)) {
      err(ctx->in_path, "invalid For field");
      free(key);
      free(init_json);
      free(cond_json);
      free(step_json);
      free(body_json);
      return false;
    }
    free(key);
  }

  if (!seen_body || !body_json) {
    err(ctx->in_path, "For requires field: body");
    free(init_json);
    free(cond_json);
    free(step_json);
    free(body_json);
    return false;
  }

  // Emit init (if present) into the current block.
  if (!init_is_null && init_json) {
    GritJsonCursor ic = grit_json_cursor(init_json, init_len);
    if (!parse_block(&ic, ctx, fn, false, NULL)) {
      free(init_json);
      free(cond_json);
      free(step_json);
      free(body_json);
      return false;
    }
  }

  // Allocate CFG blocks.
  size_t header_idx = 0;
  size_t body_idx = 0;
  size_t step_idx = 0;
  size_t exit_idx = 0;
  bool has_step = (!step_is_null && step_json);

  if (!fn_build_new_block(fn, ctx, &header_idx) || !fn_build_new_block(fn, ctx, &body_idx) ||
      (has_step && !fn_build_new_block(fn, ctx, &step_idx)) || !fn_build_new_block(fn, ctx, &exit_idx)) {
    err(ctx->in_path, "OOM creating For blocks");
    free(init_json);
    free(cond_json);
    free(step_json);
    free(body_json);
    return false;
  }

  // Jump to header.
  char *br_to_header = NULL;
  if (!emit_term_br(ctx, fn->blocks[header_idx].id, &br_to_header)) {
    err(ctx->in_path, "OOM emitting term.br");
    free(init_json);
    free(cond_json);
    free(step_json);
    free(body_json);
    return false;
  }
  if (!fn_build_append_stmt(fn, ctx, br_to_header, true)) {
    free(init_json);
    free(cond_json);
    free(step_json);
    free(body_json);
    return false;
  }

  // Header: evaluate cond if present; else unconditional branch to body.
  fn->cur_block = header_idx;
  if (!cond_is_null && cond_json) {
    SirExpr cond = {0};
    StmtList cond_effects = {0};
    {
      GritJsonCursor cc = grit_json_cursor(cond_json, cond_len);
      StmtList *saved = ctx->effects;
      ctx->effects = &cond_effects;
      bool ok = parse_expr(&cc, ctx, SEM2SIR_TYPE_BOOL, &cond);
      ctx->effects = saved;
      if (!ok) {
        free(init_json);
        free(cond_json);
        free(step_json);
        free(body_json);
        free(cond_effects.ids);
        return false;
      }
    }
    if (!fn_build_append_effects(fn, ctx, &cond_effects)) {
      free(init_json);
      free(cond_json);
      free(step_json);
      free(body_json);
      free(cond.id);
      return false;
    }
    if (cond.type != SEM2SIR_TYPE_BOOL) {
      err(ctx->in_path, "For.cond must be bool");
      free(init_json);
      free(cond_json);
      free(step_json);
      free(body_json);
      free(cond.id);
      return false;
    }
    char *t_id = NULL;
    if (!emit_term_condbr(ctx, cond.id, fn->blocks[body_idx].id, fn->blocks[exit_idx].id, &t_id)) {
      free(init_json);
      free(cond_json);
      free(step_json);
      free(body_json);
      free(cond.id);
      return false;
    }
    if (!fn_build_append_stmt(fn, ctx, t_id, true)) {
      free(init_json);
      free(cond_json);
      free(step_json);
      free(body_json);
      free(cond.id);
      return false;
    }
    free(cond.id);
  } else {
    char *t_id = NULL;
    if (!emit_term_br(ctx, fn->blocks[body_idx].id, &t_id)) {
      free(init_json);
      free(cond_json);
      free(step_json);
      free(body_json);
      return false;
    }
    if (!fn_build_append_stmt(fn, ctx, t_id, true)) {
      free(init_json);
      free(cond_json);
      free(step_json);
      free(body_json);
      return false;
    }
  }

  // Body.
  LoopTargets targets = {.break_to = exit_idx, .continue_to = has_step ? step_idx : header_idx};
  fn->cur_block = body_idx;
  {
    GritJsonCursor bc = grit_json_cursor(body_json, body_len);
    if (!parse_block(&bc, ctx, fn, false, &targets)) {
      free(init_json);
      free(cond_json);
      free(step_json);
      free(body_json);
      return false;
    }
  }
  if (!fn->blocks[fn->cur_block].terminated) {
    char *to_next = NULL;
    const char *next_id = fn->blocks[has_step ? step_idx : header_idx].id;
    if (!emit_term_br(ctx, next_id, &to_next)) {
      free(init_json);
      free(cond_json);
      free(step_json);
      free(body_json);
      return false;
    }
    if (!fn_build_append_stmt(fn, ctx, to_next, true)) {
      free(init_json);
      free(cond_json);
      free(step_json);
      free(body_json);
      return false;
    }
  }

  // Step.
  if (has_step) {
    fn->cur_block = step_idx;
    GritJsonCursor sc = grit_json_cursor(step_json, step_len);
    if (!parse_block(&sc, ctx, fn, false, NULL)) {
      free(init_json);
      free(cond_json);
      free(step_json);
      free(body_json);
      return false;
    }
    if (!fn->blocks[fn->cur_block].terminated) {
      char *back = NULL;
      if (!emit_term_br(ctx, fn->blocks[header_idx].id, &back)) {
        free(init_json);
        free(cond_json);
        free(step_json);
        free(body_json);
        return false;
      }
      if (!fn_build_append_stmt(fn, ctx, back, true)) {
        free(init_json);
        free(cond_json);
        free(step_json);
        free(body_json);
        return false;
      }
    }
  }

  fn->cur_block = exit_idx;
  free(init_json);
  free(cond_json);
  free(step_json);
  free(body_json);
  return true;
}

bool parse_stmt_for_int(GritJsonCursor *c, EmitCtx *ctx, SirFnBuild *fn) {
  bool seen_var = false;
  bool seen_end = false;
  bool seen_body = false;
  bool step_is_null = true;

  char *var_json = NULL;
  size_t var_len = 0;
  char *end_json = NULL;
  size_t end_len = 0;
  char *step_json = NULL;
  size_t step_len = 0;
  char *body_json = NULL;
  size_t body_len = 0;

  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in ForInt");
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in ForInt");
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid ForInt key");
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }

    if (strcmp(key, "var") == 0) {
      seen_var = true;
      free(var_json);
      var_json = NULL;
      if (!capture_json_value_alloc(c, &var_json, &var_len)) {
        err(ctx->in_path, "invalid ForInt.var");
        free(key);
        free(end_json);
        free(step_json);
        free(body_json);
        return false;
      }
      free(key);
      continue;
    }

    if (strcmp(key, "end") == 0) {
      seen_end = true;
      free(end_json);
      end_json = NULL;
      if (!capture_json_value_alloc(c, &end_json, &end_len)) {
        err(ctx->in_path, "invalid ForInt.end");
        free(key);
        free(var_json);
        free(step_json);
        free(body_json);
        return false;
      }
      free(key);
      continue;
    }

    if (strcmp(key, "step") == 0) {
      free(step_json);
      step_json = NULL;
      step_len = 0;
      if (!json_peek_non_ws(c, &ch)) {
        err(ctx->in_path, "unexpected EOF in ForInt.step");
        free(key);
        free(var_json);
        free(end_json);
        free(body_json);
        return false;
      }
      if (ch == 'n') {
        step_is_null = true;
        if (!grit_json_skip_value(c)) {
          err(ctx->in_path, "invalid ForInt.step");
          free(key);
          free(var_json);
          free(end_json);
          free(body_json);
          return false;
        }
      } else {
        step_is_null = false;
        if (!capture_json_value_alloc(c, &step_json, &step_len)) {
          err(ctx->in_path, "invalid ForInt.step");
          free(key);
          free(var_json);
          free(end_json);
          free(body_json);
          return false;
        }
      }
      free(key);
      continue;
    }

    if (strcmp(key, "body") == 0) {
      seen_body = true;
      free(body_json);
      body_json = NULL;
      if (!capture_json_value_alloc(c, &body_json, &body_len)) {
        err(ctx->in_path, "invalid ForInt.body");
        free(key);
        free(var_json);
        free(end_json);
        free(step_json);
        return false;
      }
      free(key);
      continue;
    }

    if (!grit_json_skip_value(c)) {
      err(ctx->in_path, "invalid ForInt field");
      free(key);
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
    free(key);
  }

  if (!seen_var || !seen_end || !seen_body || !var_json || !end_json || !body_json) {
    err(ctx->in_path, "ForInt requires fields: var, end, body");
    free(var_json);
    free(end_json);
    free(step_json);
    free(body_json);
    return false;
  }

  // Determine induction variable name.
  char *iv_name = NULL;
  if (!forint_parse_var_name_alloc_strict(var_json, var_len, ctx, &iv_name)) {
    free(var_json);
    free(end_json);
    free(step_json);
    free(body_json);
    return false;
  }

  // Emit var declaration by parsing a synthetic one-item Block.
  {
    const char *pre = "{\"k\":\"Block\",\"items\":[";
    const char *post = "]}";
    size_t syn_len = strlen(pre) + var_len + strlen(post);
    char *syn = (char *)malloc(syn_len + 1);
    if (!syn) {
      err(ctx->in_path, "OOM building ForInt synthetic Block");
      free(iv_name);
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
    memcpy(syn, pre, strlen(pre));
    memcpy(syn + strlen(pre), var_json, var_len);
    memcpy(syn + strlen(pre) + var_len, post, strlen(post));
    syn[syn_len] = '\0';

    GritJsonCursor sc = grit_json_cursor(syn, syn_len);
    bool ok = parse_block(&sc, ctx, fn, false, NULL);
    free(syn);
    if (!ok) {
      free(iv_name);
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
  }

  // Lookup iv type and ensure it's assignable + supported.
  sem2sir_type_id iv_ty = SEM2SIR_TYPE_INVALID;
  sem2sir_type_id iv_ptr_of = SEM2SIR_TYPE_INVALID;
  const char *iv_sir_tyid = NULL;
  bool iv_is_slot = false;
  if (!locals_lookup(ctx, iv_name, &iv_ty, &iv_ptr_of, &iv_sir_tyid, &iv_is_slot)) {
    err(ctx->in_path, "ForInt.var did not bind a local");
    free(iv_name);
    free(var_json);
    free(end_json);
    free(step_json);
    free(body_json);
    return false;
  }
  if (!iv_is_slot) {
    err(ctx->in_path, "ForInt induction var must be addressable (slot-backed local)");
    free(iv_name);
    free(var_json);
    free(end_json);
    free(step_json);
    free(body_json);
    return false;
  }
  if (iv_ty != SEM2SIR_TYPE_I32 && iv_ty != SEM2SIR_TYPE_I64) {
    err(ctx->in_path, "ForInt induction var type must be i32 or i64 in emitter MVP");
    free(iv_name);
    free(var_json);
    free(end_json);
    free(step_json);
    free(body_json);
    return false;
  }

  // Allocate CFG blocks: header, body, step, exit.
  size_t header_idx = 0;
  size_t body_idx = 0;
  size_t step_idx = 0;
  size_t exit_idx = 0;
  if (!fn_build_new_block(fn, ctx, &header_idx) || !fn_build_new_block(fn, ctx, &body_idx) ||
      !fn_build_new_block(fn, ctx, &step_idx) || !fn_build_new_block(fn, ctx, &exit_idx)) {
    err(ctx->in_path, "OOM creating ForInt blocks");
    free(iv_name);
    free(var_json);
    free(end_json);
    free(step_json);
    free(body_json);
    return false;
  }

  // Jump to header.
  char *br_to_header = NULL;
  if (!emit_term_br(ctx, fn->blocks[header_idx].id, &br_to_header)) {
    err(ctx->in_path, "OOM emitting term.br");
    free(iv_name);
    free(var_json);
    free(end_json);
    free(step_json);
    free(body_json);
    return false;
  }
  if (!fn_build_append_stmt(fn, ctx, br_to_header, true)) {
    free(iv_name);
    free(var_json);
    free(end_json);
    free(step_json);
    free(body_json);
    return false;
  }

  // Build JSON snippets for Name(iv) and step default.
  const char *step_default = "{\"k\":\"Int\",\"lit\":{\"k\":\"tok\",\"text\":\"1\"}}";
  const char *step_expr = (step_is_null || !step_json) ? step_default : step_json;
  size_t step_expr_len = (step_is_null || !step_json) ? strlen(step_default) : step_len;

  // name_json
  const char *name_pre = "{\"k\":\"Name\",\"id\":{\"k\":\"tok\",\"text\":";
  const char *name_post = "}}";
  size_t iv_esc_extra = 0;
  (void)iv_esc_extra;
  // Note: iv_name comes from tok.text, so it should not contain quotes in well-formed input.
  size_t name_json_len = strlen(name_pre) + 2 + strlen(iv_name) + strlen(name_post);
  char *name_json = (char *)malloc(name_json_len + 1);
  if (!name_json) {
    err(ctx->in_path, "OOM building ForInt Name JSON");
    free(iv_name);
    free(var_json);
    free(end_json);
    free(step_json);
    free(body_json);
    return false;
  }
  snprintf(name_json, name_json_len + 1, "%s\"%s\"%s", name_pre, iv_name, name_post);

  // Header: cond is (iv < end) end-exclusive.
  fn->cur_block = header_idx;
  {
    const char *cmp_pre = "{\"k\":\"Bin\",\"op\":\"core.lt\",\"lhs\":";
    const char *mid = ",\"rhs\":";
    const char *cmp_post = "}";
    size_t cmp_len = strlen(cmp_pre) + strlen(name_json) + strlen(mid) + end_len + strlen(cmp_post);
    char *cmp_json = (char *)malloc(cmp_len + 1);
    if (!cmp_json) {
      err(ctx->in_path, "OOM building ForInt cond JSON");
      free(name_json);
      free(iv_name);
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
    memcpy(cmp_json, cmp_pre, strlen(cmp_pre));
    memcpy(cmp_json + strlen(cmp_pre), name_json, strlen(name_json));
    memcpy(cmp_json + strlen(cmp_pre) + strlen(name_json), mid, strlen(mid));
    memcpy(cmp_json + strlen(cmp_pre) + strlen(name_json) + strlen(mid), end_json, end_len);
    memcpy(cmp_json + strlen(cmp_pre) + strlen(name_json) + strlen(mid) + end_len, cmp_post, strlen(cmp_post));
    cmp_json[cmp_len] = '\0';

    SirExpr cond = {0};
    StmtList cond_effects = {0};
    GritJsonCursor cc = grit_json_cursor(cmp_json, cmp_len);
    StmtList *saved = ctx->effects;
    ctx->effects = &cond_effects;
    bool ok = parse_expr(&cc, ctx, SEM2SIR_TYPE_BOOL, &cond);
    ctx->effects = saved;
    free(cmp_json);
    if (!ok) {
      free(cond_effects.ids);
      free(name_json);
      free(iv_name);
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
    if (!fn_build_append_effects(fn, ctx, &cond_effects)) {
      free(cond.id);
      free(name_json);
      free(iv_name);
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
    char *t_id = NULL;
    if (!emit_term_condbr(ctx, cond.id, fn->blocks[body_idx].id, fn->blocks[exit_idx].id, &t_id)) {
      free(cond.id);
      free(name_json);
      free(iv_name);
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
    if (!fn_build_append_stmt(fn, ctx, t_id, true)) {
      free(cond.id);
      free(name_json);
      free(iv_name);
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
    free(cond.id);
  }

  // Body.
  LoopTargets targets = {.break_to = exit_idx, .continue_to = step_idx};
  fn->cur_block = body_idx;
  {
    GritJsonCursor bc = grit_json_cursor(body_json, body_len);
    if (!parse_block(&bc, ctx, fn, false, &targets)) {
      free(name_json);
      free(iv_name);
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
  }
  if (!fn->blocks[fn->cur_block].terminated) {
    char *to_step = NULL;
    if (!emit_term_br(ctx, fn->blocks[step_idx].id, &to_step)) {
      free(name_json);
      free(iv_name);
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
    if (!fn_build_append_stmt(fn, ctx, to_step, true)) {
      free(name_json);
      free(iv_name);
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
  }

  // Step: iv = iv + step (default step=1).
  fn->cur_block = step_idx;
  {
    const char *add_pre = "{\"k\":\"Bin\",\"op\":\"core.add\",\"lhs\":";
    const char *mid = ",\"rhs\":";
    const char *add_post = "}";
    size_t add_len = strlen(add_pre) + strlen(name_json) + strlen(mid) + step_expr_len + strlen(add_post);
    char *add_json = (char *)malloc(add_len + 1);
    if (!add_json) {
      err(ctx->in_path, "OOM building ForInt step add JSON");
      free(name_json);
      free(iv_name);
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
    memcpy(add_json, add_pre, strlen(add_pre));
    memcpy(add_json + strlen(add_pre), name_json, strlen(name_json));
    memcpy(add_json + strlen(add_pre) + strlen(name_json), mid, strlen(mid));
    memcpy(add_json + strlen(add_pre) + strlen(name_json) + strlen(mid), step_expr, step_expr_len);
    memcpy(add_json + strlen(add_pre) + strlen(name_json) + strlen(mid) + step_expr_len, add_post, strlen(add_post));
    add_json[add_len] = '\0';

    const char *as_pre = "{\"k\":\"Bin\",\"op\":\"core.assign\",\"lhs\":";
    const char *as_mid = ",\"rhs\":";
    const char *as_post = "}";
    size_t as_len = strlen(as_pre) + strlen(name_json) + strlen(as_mid) + add_len + strlen(as_post);
    char *as_json = (char *)malloc(as_len + 1);
    if (!as_json) {
      err(ctx->in_path, "OOM building ForInt step assign JSON");
      free(add_json);
      free(name_json);
      free(iv_name);
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
    memcpy(as_json, as_pre, strlen(as_pre));
    memcpy(as_json + strlen(as_pre), name_json, strlen(name_json));
    memcpy(as_json + strlen(as_pre) + strlen(name_json), as_mid, strlen(as_mid));
    memcpy(as_json + strlen(as_pre) + strlen(name_json) + strlen(as_mid), add_json, add_len);
    memcpy(as_json + strlen(as_pre) + strlen(name_json) + strlen(as_mid) + add_len, as_post, strlen(as_post));
    as_json[as_len] = '\0';
    free(add_json);

    GritJsonCursor ac = grit_json_cursor(as_json, as_len);
    char *k = NULL;
    if (!parse_node_k_string(&ac, ctx, &k)) {
      free(as_json);
      free(name_json);
      free(iv_name);
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
    free(k);

    StmtList step_effects = {0};
    StmtList *saved = ctx->effects;
    ctx->effects = &step_effects;
    char *st_id = NULL;
    bool ok = parse_stmt_bin_assign_emit_store(&ac, ctx, &st_id);
    ctx->effects = saved;
    free(as_json);
    if (!ok) {
      free(step_effects.ids);
      free(name_json);
      free(iv_name);
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
    if (!fn_build_append_effects(fn, ctx, &step_effects)) {
      free(st_id);
      free(name_json);
      free(iv_name);
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
    if (!fn_build_append_stmt(fn, ctx, st_id, false)) {
      free(name_json);
      free(iv_name);
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }

    char *back = NULL;
    if (!emit_term_br(ctx, fn->blocks[header_idx].id, &back)) {
      free(name_json);
      free(iv_name);
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
    if (!fn_build_append_stmt(fn, ctx, back, true)) {
      free(name_json);
      free(iv_name);
      free(var_json);
      free(end_json);
      free(step_json);
      free(body_json);
      return false;
    }
  }

  fn->cur_block = exit_idx;
  free(name_json);
  free(iv_name);
  free(var_json);
  free(end_json);
  free(step_json);
  free(body_json);
  return true;
}

bool parse_block(GritJsonCursor *c, EmitCtx *ctx, SirFnBuild *fn, bool require_return, const LoopTargets *loop) {

  char *k = NULL;
  if (!parse_node_k_string(c, ctx, &k))
    return false;
  if (strcmp(k, "Block") != 0) {
    err(ctx->in_path, "expected Block node");
    free(k);
    return false;
  }
  free(k);

  bool seen_items = false;
  bool saw_return = false;
  char ch = 0;

  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in Block");
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in Block");
      return false;
    }
    c->p++;
    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid Block key");
      return false;
    }

    if (strcmp(key, "items") == 0) {
      seen_items = true;
      if (!grit_json_consume_char(c, '[')) {
        err(ctx->in_path, "Block.items must be array");
        free(key);
        return false;
      }
      if (!json_peek_non_ws(c, &ch)) {
        err(ctx->in_path, "unexpected EOF in Block.items");
        free(key);
        return false;
      }
      if (ch != ']') {
        for (;;) {
          if (fn->blocks[fn->cur_block].terminated) {
            err(ctx->in_path, "Block has statements after terminator (no implicit control flow)");
            free(key);
            return false;
          }

          // Parse one statement node.
          char *sk = NULL;
          if (!parse_node_k_string(c, ctx, &sk)) {
            free(key);
            return false;
          }

          bool is_var_pat = (strcmp(sk, "VarPat") == 0);
          if (strcmp(sk, "Var") == 0 || is_var_pat) {
            free(sk);
            bool seen_name = false;
            bool seen_pat = false;
            bool seen_type = false;
            bool seen_init = false;
            char *var_name = NULL;
            SemTypeInfo var_ti = {0};
            SirExpr init = {0};
            StmtList init_effects = {0};

            for (;;) {
              if (!json_peek_non_ws(c, &ch)) {
                err(ctx->in_path, "unexpected EOF in Var");
                free(var_name);
                return false;
              }
              if (ch == '}') {
                c->p++;
                break;
              }
              if (ch != ',') {
                err(ctx->in_path, "expected ',' or '}' in Var");
                free(var_name);
                return false;
              }
              c->p++;
              char *vkey = NULL;
              if (!json_expect_key(c, &vkey)) {
                err(ctx->in_path, "invalid Var key");
                free(var_name);
                return false;
              }
              if (!is_var_pat && strcmp(vkey, "name") == 0) {
                seen_name = true;
                if (!parse_tok_text_alloc_strict(c, ctx->in_path, &var_name)) {
                  free(vkey);
                  return false;
                }
              } else if (is_var_pat && strcmp(vkey, "pat") == 0) {
                seen_pat = true;
                if (!parse_pat_bind_name_alloc_strict(c, ctx, &var_name)) {
                  free(vkey);
                  return false;
                }
              } else if (strcmp(vkey, "type") == 0) {
                seen_type = true;
                if (!json_peek_non_ws(c, &ch)) {
                  err(ctx->in_path, "unexpected EOF in Var.type");
                  free(vkey);
                  free(var_name);
                  return false;
                }
                if (ch == 'n') {
                  err(ctx->in_path, "Var.type must be explicit (no defaults)");
                  free(vkey);
                  free(var_name);
                  return false;
                }
                if (!parse_type_typeinfo(c, ctx, &var_ti)) {
                  free(vkey);
                  free(var_name);
                  return false;
                }
              } else if (strcmp(vkey, "init") == 0) {
                seen_init = true;
                if (!json_peek_non_ws(c, &ch)) {
                  err(ctx->in_path, "unexpected EOF in Var.init");
                  free(vkey);
                  free(var_name);
                  return false;
                }
                if (ch == 'n') {
                  err(ctx->in_path, "Var.init must be explicit (no defaults)");
                  free(vkey);
                  free(var_name);
                  return false;
                }
                // init expression: expected type will be checked after we parse type.
                // For strictness, require type is present before init.
                if (!seen_type || var_ti.base == SEM2SIR_TYPE_INVALID) {
                  err(ctx->in_path, "Var.type must appear before Var.init (no implicit context)");
                  free(vkey);
                  free(var_name);
                  return false;
                }
                StmtList *saved = ctx->effects;
                ctx->effects = &init_effects;
                bool ok = parse_expr(c, ctx, var_ti.base, &init);
                ctx->effects = saved;
                if (!ok) {
                  free(vkey);
                  free(var_name);
                  free(init_effects.ids);
                  return false;
                }
              } else {
                if (!grit_json_skip_value(c)) {
                  err(ctx->in_path, "invalid Var field");
                  free(vkey);
                  free(var_name);
                  return false;
                }
              }
              free(vkey);
            }

            if (!seen_type || !seen_init || (!seen_name && !seen_pat)) {
              err(ctx->in_path, "Var requires fields: name/pat, type, init (no implicitness)");
              free(var_name);
              free(init.id);
              return false;
            }
            if (init.type != var_ti.base) {
              err(ctx->in_path, "Var.init type does not match Var.type");
              free(var_name);
              free(init.id);
              free(init_effects.ids);
              return false;
            }

            if (var_ti.base == SEM2SIR_TYPE_PTR && var_ti.ptr_of != SEM2SIR_TYPE_INVALID && var_ti.ptr_of != SEM2SIR_TYPE_VOID) {
              if (init.ptr_of != var_ti.ptr_of) {
                err(ctx->in_path, "Var.init pointer pointee does not match declared ptr(T)");
                free(var_name);
                free(init.id);
                free(init_effects.ids);
                return false;
              }
            }

            if (!fn_build_append_effects(fn, ctx, &init_effects)) {
              err(ctx->in_path, "OOM building block stmt list");
              free(var_name);
              free(init.id);
              return false;
            }

              bool use_slot = type_supports_slot_storage(var_ti.base);

              if (use_slot) {
                // Slot-backed local: alloca + store init + let binds the slot pointer.
                SemTypeInfo slot_ptr_ti = {.base = SEM2SIR_TYPE_PTR, .ptr_of = SEM2SIR_TYPE_INVALID, .sir_id = "t:ptr"};
                if (var_ti.base != SEM2SIR_TYPE_PTR && var_ti.base != SEM2SIR_TYPE_SLICE && type_store_tag(var_ti.base) &&
                    type_load_tag(var_ti.base) && type_align_bytes(var_ti.base) != 0) {
                  const char *did = get_derived_ptr_type_id(ctx, var_ti.base);
                  if (did) {
                    slot_ptr_ti.ptr_of = var_ti.base;
                    slot_ptr_ti.sir_id = did;
                  }
                }
                if (!emit_typeinfo_if_needed(ctx, &slot_ptr_ti)) {
                  free(var_name);
                  free(init.id);
                  return false;
                }
                if (!emit_typeinfo_if_needed(ctx, &var_ti)) {
                  free(var_name);
                  free(init.id);
                  return false;
                }
                const char *tyid = var_ti.sir_id ? var_ti.sir_id : sir_type_id_for(var_ti.base);
                if (!tyid) {
                  err(ctx->in_path, "unsupported Var.type for slot allocation");
                  free(var_name);
                  free(init.id);
                  return false;
                }
                const char *store_tag = type_store_tag(var_ti.base);
                int align = type_align_bytes(var_ti.base);
                if (!store_tag || align == 0) {
                  err(ctx->in_path, "unsupported Var.type for store");
                  free(var_name);
                  free(init.id);
                  return false;
                }

                char *slot_id = new_node_id(ctx);
                fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
                emit_json_string(ctx->out, slot_id);
                fprintf(ctx->out, ",\"tag\":\"alloca\",\"fields\":{\"ty\":{\"t\":\"ref\",\"k\":\"type\",\"id\":");
                emit_json_string(ctx->out, tyid);
                fprintf(ctx->out, "},\"flags\":{\"count\":1,\"align\":%d,\"zero\":true}}}\n", align);

                char *st_id = new_node_id(ctx);
                fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
                emit_json_string(ctx->out, st_id);
                fprintf(ctx->out, ",\"tag\":");
                emit_json_string(ctx->out, store_tag);
                fprintf(ctx->out, ",\"fields\":{\"addr\":{\"t\":\"ref\",\"id\":");
                emit_json_string(ctx->out, slot_id);
                fprintf(ctx->out, "},\"value\":{\"t\":\"ref\",\"id\":");
                emit_json_string(ctx->out, init.id);
                fprintf(ctx->out, "},\"align\":%d}}\n", align);

                char *let_id = new_node_id(ctx);
                fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
                emit_json_string(ctx->out, let_id);
                fprintf(ctx->out, ",\"tag\":\"let\",\"fields\":{\"name\":");
                emit_json_string(ctx->out, var_name);
                fprintf(ctx->out, ",\"value\":{\"t\":\"ref\",\"id\":");
                emit_json_string(ctx->out, slot_id);
                fprintf(ctx->out, "}}}\n");

                if (!locals_push_binding(ctx, var_name, var_ti, true)) {
                  err(ctx->in_path, "OOM recording local");
                  free(slot_id);
                  free(st_id);
                  free(let_id);
                  free(var_name);
                  free(init.id);
                  return false;
                }

                // Append statements in order: alloca, store, let.
                if (!fn_build_append_stmt(fn, ctx, slot_id, false) || !fn_build_append_stmt(fn, ctx, st_id, false) ||
                    !fn_build_append_stmt(fn, ctx, let_id, false)) {
                  err(ctx->in_path, "OOM building block stmt list");
                  free(var_name);
                  free(init.id);
                  return false;
                }

                free(var_name);
                free(init.id);
              } else {
                // Direct-binding local (no addressability): let binds the value.
                char *let_id = new_node_id(ctx);
                fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
                emit_json_string(ctx->out, let_id);
                fprintf(ctx->out, ",\"tag\":\"let\",\"fields\":{\"name\":");
                emit_json_string(ctx->out, var_name);
                fprintf(ctx->out, ",\"value\":{\"t\":\"ref\",\"id\":");
                emit_json_string(ctx->out, init.id);
                fprintf(ctx->out, "}}}\n");

                if (!locals_push_binding(ctx, var_name, var_ti, false)) {
                  err(ctx->in_path, "OOM recording local");
                  free(let_id);
                  free(var_name);
                  free(init.id);
                  return false;
                }

                if (!fn_build_append_stmt(fn, ctx, let_id, false)) {
                  err(ctx->in_path, "OOM building block stmt list");
                  free(var_name);
                  free(init.id);
                  return false;
                }

                free(var_name);
                free(init.id);
              }
          } else if (strcmp(sk, "Return") == 0) {
            free(sk);
            saw_return = true;
            bool seen_value = false;
            bool value_is_null = false;
            SirExpr v = {0};
            StmtList ret_effects = {0};

            for (;;) {
              if (!json_peek_non_ws(c, &ch)) {
                err(ctx->in_path, "unexpected EOF in Return");
                return false;
              }
              if (ch == '}') {
                c->p++;
                break;
              }
              if (ch != ',') {
                err(ctx->in_path, "expected ',' or '}' in Return");
                return false;
              }
              c->p++;
              char *rkey = NULL;
              if (!json_expect_key(c, &rkey)) {
                err(ctx->in_path, "invalid Return key");
                return false;
              }
              if (strcmp(rkey, "value") == 0) {
                seen_value = true;
                if (!json_peek_non_ws(c, &ch)) {
                  err(ctx->in_path, "unexpected EOF in Return.value");
                  free(rkey);
                  free(ret_effects.ids);
                  return false;
                }
                if (ch == 'n') {
                  value_is_null = true;
                  if (!grit_json_skip_value(c)) {
                    err(ctx->in_path, "invalid Return.value (expected null)");
                    free(rkey);
                    free(ret_effects.ids);
                    return false;
                  }
                } else {
                  StmtList *saved = ctx->effects;
                  ctx->effects = &ret_effects;
                  bool ok = parse_expr(c, ctx, ctx->fn_ret, &v);
                  ctx->effects = saved;
                  if (!ok) {
                    free(rkey);
                    free(ret_effects.ids);
                    return false;
                  }
                }
              } else {
                if (!grit_json_skip_value(c)) {
                  err(ctx->in_path, "invalid Return field");
                  free(rkey);
                  return false;
                }
              }
              free(rkey);
            }

            if (ctx->fn_ret != SEM2SIR_TYPE_VOID) {
              if (!seen_value || value_is_null || !v.id) {
                err(ctx->in_path, "Return.value required for non-void function (no implicit return value)");
                free(v.id);
                free(ret_effects.ids);
                return false;
              }
              if (v.type != ctx->fn_ret) {
                err(ctx->in_path, "Return.value type mismatch");
                free(v.id);
                free(ret_effects.ids);
                return false;
              }
            } else {
              if (seen_value && !value_is_null && v.id) {
                err(ctx->in_path, "Return.value must be null for void function");
                free(v.id);
                free(ret_effects.ids);
                return false;
              }
            }

            if (!fn_build_append_effects(fn, ctx, &ret_effects)) {
              err(ctx->in_path, "OOM building block stmt list");
              free(v.id);
              return false;
            }

            char *ret_id = NULL;
            if (!emit_term_ret(ctx, ctx->fn_ret, v.id, &ret_id)) {
              free(v.id);
              return false;
            }

            if (!fn_build_append_stmt(fn, ctx, ret_id, true)) {
              err(ctx->in_path, "OOM building block stmt list");
              free(v.id);
              return false;
            }

            free(v.id);
          } else if (strcmp(sk, "Bin") == 0) {
            free(sk);

            StmtList bin_effects = {0};
            StmtList *saved = ctx->effects;
            ctx->effects = &bin_effects;
            char *st_id = NULL;
            bool ok = parse_stmt_bin_assign_emit_store(c, ctx, &st_id);
            ctx->effects = saved;
            if (!ok) {
              free(bin_effects.ids);
              return false;
            }

            if (!fn_build_append_effects(fn, ctx, &bin_effects)) {
              err(ctx->in_path, "OOM building block stmt list");
              free(st_id);
              return false;
            }

            if (!fn_build_append_stmt(fn, ctx, st_id, false)) {
              err(ctx->in_path, "OOM building block stmt list");
              return false;
            }
          } else if (strcmp(sk, "If") == 0) {
            free(sk);
            if (!parse_stmt_if(c, ctx, fn, loop)) {
              free(key);
              return false;
            }
          } else if (strcmp(sk, "While") == 0) {
            free(sk);
            if (!parse_stmt_while(c, ctx, fn)) {
              free(key);
              return false;
            }
          } else if (strcmp(sk, "Loop") == 0) {
            free(sk);
            if (!parse_stmt_loop(c, ctx, fn)) {
              free(key);
              return false;
            }
          } else if (strcmp(sk, "DoWhile") == 0) {
            free(sk);
            if (!parse_stmt_do_while(c, ctx, fn)) {
              free(key);
              return false;
            }
          } else if (strcmp(sk, "For") == 0) {
            free(sk);
            if (!parse_stmt_for(c, ctx, fn)) {
              free(key);
              return false;
            }
          } else if (strcmp(sk, "ForInt") == 0) {
            free(sk);
            if (!parse_stmt_for_int(c, ctx, fn)) {
              free(key);
              return false;
            }
          } else if (strcmp(sk, "Break") == 0) {
            free(sk);
            if (!loop) {
              err(ctx->in_path, "Break outside of loop is not supported");
              free(key);
              return false;
            }
            if (!skip_remaining_object_fields(c, ctx, "Break")) {
              err(ctx->in_path, "invalid Break object");
              free(key);
              return false;
            }
            // Emit branch to loop exit.
            char *t_id = NULL;
            if (!emit_term_br(ctx, fn->blocks[loop->break_to].id, &t_id)) {
              free(key);
              return false;
            }
            if (!fn_build_append_stmt(fn, ctx, t_id, true)) {
              free(key);
              return false;
            }
          } else if (strcmp(sk, "Continue") == 0) {
            free(sk);
            if (!loop) {
              err(ctx->in_path, "Continue outside of loop is not supported");
              free(key);
              return false;
            }
            if (!skip_remaining_object_fields(c, ctx, "Continue")) {
              err(ctx->in_path, "invalid Continue object");
              free(key);
              return false;
            }
            char *t_id = NULL;
            if (!emit_term_br(ctx, fn->blocks[loop->continue_to].id, &t_id)) {
              free(key);
              return false;
            }
            if (!fn_build_append_stmt(fn, ctx, t_id, true)) {
              free(key);
              return false;
            }
          } else if (strcmp(sk, "Call") == 0) {
            free(sk);
            // Expression statement: lower call for its effects and discard the value.
            StmtList call_effects = {0};
            StmtList *saved = ctx->effects;
            ctx->effects = &call_effects;
            SirExpr call = {0};
            bool ok = parse_expr_call(c, ctx, SEM2SIR_TYPE_INVALID, &call);
            ctx->effects = saved;
            if (!ok) {
              free(call_effects.ids);
              free(key);
              return false;
            }
            if (!fn_build_append_effects(fn, ctx, &call_effects)) {
              err(ctx->in_path, "OOM building block stmt list");
              free(key);
              return false;
            }
            if (!fn_build_append_stmt(fn, ctx, call.id, false)) {
              err(ctx->in_path, "OOM building block stmt list");
              free(key);
              return false;
            }
          } else if (strcmp(sk, "ExprStmt") == 0) {
            free(sk);
            bool seen_expr = false;
            bool expr_is_null = false;
            char *ek = NULL;

            for (;;) {
              if (!json_peek_non_ws(c, &ch)) {
                err(ctx->in_path, "unexpected EOF in ExprStmt");
                free(ek);
                free(key);
                return false;
              }
              if (ch == '}') {
                c->p++;
                break;
              }
              if (ch != ',') {
                err(ctx->in_path, "expected ',' or '}' in ExprStmt");
                free(ek);
                free(key);
                return false;
              }
              c->p++;
              char *ekey = NULL;
              if (!json_expect_key(c, &ekey)) {
                err(ctx->in_path, "invalid ExprStmt key");
                free(ek);
                free(key);
                return false;
              }

              if (strcmp(ekey, "expr") == 0) {
                seen_expr = true;
                if (!json_peek_non_ws(c, &ch)) {
                  err(ctx->in_path, "unexpected EOF in ExprStmt.expr");
                  free(ekey);
                  free(ek);
                  free(key);
                  return false;
                }
                if (ch == 'n') {
                  expr_is_null = true;
                  if (!grit_json_skip_value(c)) {
                    err(ctx->in_path, "invalid ExprStmt.expr (expected null)");
                    free(ekey);
                    free(ek);
                    free(key);
                    return false;
                  }
                } else {
                  // ExprStmt has no expected type. To avoid implicitness, only allow UnitVal (void) here.
                  if (!parse_node_k_string(c, ctx, &ek)) {
                    free(ekey);
                    free(ek);
                    free(key);
                    return false;
                  }
                  if (strcmp(ek, "UnitVal") != 0) {
                    err(ctx->in_path, "ExprStmt only supports UnitVal in sem2sir MVP (no untyped expression statements)");
                    free(ekey);
                    free(ek);
                    free(key);
                    return false;
                  }
                  free(ek);
                  ek = NULL;
                  SirExpr uv = {0};
                  if (!parse_expr_unitval(c, ctx, SEM2SIR_TYPE_VOID, &uv)) {
                    free(ekey);
                    free(key);
                    return false;
                  }
                  // UnitVal emits no node; no stmt appended.
                }
              } else {
                if (!grit_json_skip_value(c)) {
                  err(ctx->in_path, "invalid ExprStmt field");
                  free(ekey);
                  free(ek);
                  free(key);
                  return false;
                }
              }

              free(ekey);
            }

            (void)seen_expr;
            (void)expr_is_null;
          } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "unsupported Block item kind: %s", sk ? sk : "<null>");
            err(ctx->in_path, msg);
            free(sk);
            return false;
          }

          if (!json_peek_non_ws(c, &ch)) {
            err(ctx->in_path, "unexpected EOF in Block.items");
            free(key);
            return false;
          }
          if (ch == ',') {
            c->p++;
            continue;
          }
          if (ch == ']')
            break;
          err(ctx->in_path, "expected ',' or ']' in Block.items");
          free(key);
          return false;
        }
      }

      if (!grit_json_consume_char(c, ']')) {
        err(ctx->in_path, "expected ']' to close Block.items");
        free(key);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid Block field");
        free(key);
        return false;
      }
    }

    free(key);
  }

  if (!seen_items) {
    err(ctx->in_path, "Block requires field: items");
    return false;
  }
  if (require_return && !saw_return) {
    err(ctx->in_path, "Block must contain a Return (no implicit fallthrough)");
    return false;
  }
  return true;
}

