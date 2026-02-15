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

