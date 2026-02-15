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

bool parse_proc_fields_and_emit_fn(GritJsonCursor *c, EmitCtx *ctx) {

  bool seen_name = false;
  bool seen_params = false;
  bool seen_ret = false;
  bool seen_body = false;
  bool seen_extern = false;
  bool is_extern = false;
  char *proc_name = NULL;
  char *link_name = NULL;
  SemTypeInfo ret_ti = {0};
  SemTypeInfo *param_tis = NULL;
  char **param_names = NULL;
  char **param_node_ids = NULL;
  size_t param_count = 0;
  size_t param_cap = 0;

  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in Proc");
      free(proc_name);
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(ctx->in_path, "expected ',' or '}' in Proc");
      free(proc_name);
      return false;
    }
    c->p++;
    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid Proc key");
      free(proc_name);
      for (size_t i = 0; i < param_count; i++) free(param_names[i]);
      free(param_names);
      free(param_tis);
      free(param_node_ids);
      return false;
    }

    if (strcmp(key, "name") == 0) {
      seen_name = true;
      if (!parse_tok_text_alloc_strict(c, ctx->in_path, &proc_name)) {
        free(key);
        for (size_t i = 0; i < param_count; i++) free(param_names[i]);
        free(param_names);
        free(param_tis);
        free(param_node_ids);
        return false;
      }
    } else if (strcmp(key, "link_name") == 0) {
      if (seen_body) {
        err(ctx->in_path, "Proc.link_name must appear before Proc.body");
        free(key);
        free(proc_name);
        free(link_name);
        for (size_t i = 0; i < param_count; i++) free(param_names[i]);
        free(param_names);
        free(param_tis);
        free(param_node_ids);
        return false;
      }
      free(link_name);
      link_name = NULL;
      if (!parse_tok_text_alloc_strict(c, ctx->in_path, &link_name)) {
        free(key);
        free(proc_name);
        for (size_t i = 0; i < param_count; i++) free(param_names[i]);
        free(param_names);
        free(param_tis);
        free(param_node_ids);
        return false;
      }
    } else if (strcmp(key, "extern") == 0) {
      if (seen_body) {
        err(ctx->in_path, "Proc.extern must appear before Proc.body");
        free(key);
        free(proc_name);
        free(link_name);
        for (size_t i = 0; i < param_count; i++) free(param_names[i]);
        free(param_names);
        free(param_tis);
        free(param_node_ids);
        return false;
      }
      seen_extern = true;
      char ch2 = 0;
      if (!json_peek_non_ws(c, &ch2)) {
        err(ctx->in_path, "unexpected EOF in Proc.extern");
        free(key);
        free(proc_name);
        free(link_name);
        for (size_t i = 0; i < param_count; i++) free(param_names[i]);
        free(param_names);
        free(param_tis);
        free(param_node_ids);
        return false;
      }
      is_extern = (ch2 == 't');
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid Proc.extern");
        free(key);
        free(proc_name);
        free(link_name);
        for (size_t i = 0; i < param_count; i++) free(param_names[i]);
        free(param_names);
        free(param_tis);
        free(param_node_ids);
        return false;
      }
    } else if (strcmp(key, "params") == 0) {
      seen_params = true;
      if (!grit_json_consume_char(c, '[')) {
        err(ctx->in_path, "Proc.params must be array");
        free(key);
        free(proc_name);
        for (size_t i = 0; i < param_count; i++) free(param_names[i]);
        free(param_names);
        free(param_tis);
        free(param_node_ids);
        return false;
      }
      if (!json_peek_non_ws(c, &ch)) {
        err(ctx->in_path, "unexpected EOF in Proc.params");
        free(key);
        free(proc_name);
        for (size_t i = 0; i < param_count; i++) free(param_names[i]);
        free(param_names);
        free(param_tis);
        free(param_node_ids);
        return false;
      }
      if (ch != ']') {
        for (;;) {
          char *pk = NULL;
          if (!parse_node_k_string(c, ctx, &pk)) {
            free(key);
            free(proc_name);
            for (size_t i = 0; i < param_count; i++) free(param_names[i]);
            free(param_names);
            free(param_tis);
            free(param_node_ids);
            return false;
          }
          bool is_param_pat = false;
          if (strcmp(pk, "Param") == 0) {
            is_param_pat = false;
          } else if (strcmp(pk, "ParamPat") == 0) {
            is_param_pat = true;
          } else {
            err(ctx->in_path, "Proc.params items must be Param or ParamPat");
            free(pk);
            free(key);
            free(proc_name);
            for (size_t i = 0; i < param_count; i++) free(param_names[i]);
            free(param_names);
            free(param_tis);
            free(param_node_ids);
            return false;
          }
          free(pk);

          bool seen_pname = false;
          bool seen_ptype = false;
          char *pname = NULL;
          SemTypeInfo pti = {0};
          for (;;) {
            if (!json_peek_non_ws(c, &ch)) {
              err(ctx->in_path, "unexpected EOF in Param");
              free(pname);
              free(key);
              free(proc_name);
              for (size_t i = 0; i < param_count; i++) free(param_names[i]);
              free(param_names);
              free(param_tis);
              free(param_node_ids);
              return false;
            }
            if (ch == '}') {
              c->p++;
              break;
            }
            if (ch != ',') {
              err(ctx->in_path, "expected ',' or '}' in Param");
              free(pname);
              free(key);
              free(proc_name);
              for (size_t i = 0; i < param_count; i++) free(param_names[i]);
              free(param_names);
              free(param_tis);
              free(param_node_ids);
              return false;
            }
            c->p++;
            char *pkey = NULL;
            if (!json_expect_key(c, &pkey)) {
              err(ctx->in_path, "invalid Param key");
              free(pname);
              free(key);
              free(proc_name);
              for (size_t i = 0; i < param_count; i++) free(param_names[i]);
              free(param_names);
              free(param_tis);
              free(param_node_ids);
              return false;
            }
            if (!is_param_pat && strcmp(pkey, "name") == 0) {
              seen_pname = true;
              if (!parse_tok_text_alloc_strict(c, ctx->in_path, &pname)) {
                free(pkey);
                free(key);
                free(proc_name);
                for (size_t i = 0; i < param_count; i++) free(param_names[i]);
                free(param_names);
                free(param_tis);
                free(param_node_ids);
                return false;
              }
            } else if (is_param_pat && strcmp(pkey, "pat") == 0) {
              seen_pname = true;
              if (!parse_pat_bind_name_alloc_strict(c, ctx, &pname)) {
                free(pkey);
                free(key);
                free(proc_name);
                for (size_t i = 0; i < param_count; i++) free(param_names[i]);
                free(param_names);
                free(param_tis);
                free(param_node_ids);
                return false;
              }
            } else if (strcmp(pkey, "type") == 0) {
              seen_ptype = true;
              if (!parse_type_typeinfo(c, ctx, &pti)) {
                free(pkey);
                free(pname);
                free(key);
                free(proc_name);
                for (size_t i = 0; i < param_count; i++) free(param_names[i]);
                free(param_names);
                free(param_tis);
                free(param_node_ids);
                return false;
              }
            } else if (strcmp(pkey, "mode") == 0) {
              if (!grit_json_skip_value(c)) {
                err(ctx->in_path, "invalid Param mode");
                free(pkey);
                free(pname);
                free(key);
                free(proc_name);
                for (size_t i = 0; i < param_count; i++) free(param_names[i]);
                free(param_names);
                free(param_tis);
                free(param_node_ids);
                return false;
              }
            } else {
              if (!grit_json_skip_value(c)) {
                err(ctx->in_path, "invalid Param field");
                free(pkey);
                free(pname);
                free(key);
                free(proc_name);
                for (size_t i = 0; i < param_count; i++) free(param_names[i]);
                free(param_names);
                free(param_tis);
                free(param_node_ids);
                return false;
              }
            }
            free(pkey);
          }

          if (!seen_pname || !seen_ptype || !pname || pti.base == SEM2SIR_TYPE_INVALID) {
            err(ctx->in_path, "Param requires fields: name/pat, type");
            free(pname);
            free(key);
            free(proc_name);
            for (size_t i = 0; i < param_count; i++) free(param_names[i]);
            free(param_names);
            free(param_tis);
            free(param_node_ids);
            return false;
          }

          if (param_count == param_cap) {
            size_t next_cap = param_cap ? param_cap * 2 : 4;
            SemTypeInfo *next_tis = (SemTypeInfo *)calloc(next_cap, sizeof(SemTypeInfo));
            char **next_names = (char **)calloc(next_cap, sizeof(char *));
            char **next_ids = (char **)calloc(next_cap, sizeof(char *));
            if (!next_tis || !next_names || !next_ids) {
              err(ctx->in_path, "OOM allocating Proc.params");
              free(pname);
              free(key);
              free(proc_name);
              for (size_t i = 0; i < param_count; i++) free(param_names[i]);
              free(param_names);
              free(param_tis);
              free(param_node_ids);
              free(next_tis);
              free(next_names);
              free(next_ids);
              return false;
            }
            for (size_t i = 0; i < param_count; i++) {
              next_tis[i] = param_tis[i];
              next_names[i] = param_names[i];
              next_ids[i] = param_node_ids[i];
            }
            free(param_tis);
            free(param_names);
            free(param_node_ids);
            param_tis = next_tis;
            param_names = next_names;
            param_node_ids = next_ids;
            param_cap = next_cap;
          }
          param_tis[param_count] = pti;
          param_names[param_count] = pname;
          param_node_ids[param_count] = NULL;
          param_count++;

          if (!json_peek_non_ws(c, &ch)) {
            err(ctx->in_path, "unexpected EOF in Proc.params");
            free(key);
            free(proc_name);
            for (size_t i = 0; i < param_count; i++) free(param_names[i]);
            free(param_names);
            free(param_tis);
            free(param_node_ids);
            return false;
          }
          if (ch == ',') {
            c->p++;
            continue;
          }
          if (ch == ']') break;
          err(ctx->in_path, "expected ',' or ']' in Proc.params");
          free(key);
          free(proc_name);
          for (size_t i = 0; i < param_count; i++) free(param_names[i]);
          free(param_names);
          free(param_tis);
          free(param_node_ids);
          return false;
        }
      }
      c->p++;
    } else if (strcmp(key, "ret") == 0) {
      seen_ret = true;
      if (!json_peek_non_ws(c, &ch)) {
        err(ctx->in_path, "unexpected EOF in Proc.ret");
        free(key);
        free(proc_name);
        return false;
      }
      if (ch == 'n') {
        err(ctx->in_path, "Proc.ret must be explicit (no defaults)");
        free(key);
        free(proc_name);
        return false;
      }
      if (!parse_type_typeinfo(c, ctx, &ret_ti)) {
        free(key);
        free(proc_name);
        return false;
      }
    } else if (strcmp(key, "body") == 0) {
      seen_body = true;
      if (!seen_params) {
        err(ctx->in_path, "Proc.params must appear before Proc.body");
        free(key);
        free(proc_name);
        free(link_name);
        for (size_t i = 0; i < param_count; i++) free(param_names[i]);
        free(param_names);
        free(param_tis);
        free(param_node_ids);
        return false;
      }
      // Require ret known before body so we can type-check Return.
      if (!seen_ret || ret_ti.base == SEM2SIR_TYPE_INVALID) {
        err(ctx->in_path, "Proc.ret must appear before Proc.body (no implicit context)");
        free(key);
        free(proc_name);
        free(link_name);
        for (size_t i = 0; i < param_count; i++) free(param_names[i]);
        free(param_names);
        free(param_tis);
        free(param_node_ids);
        return false;
      }

      ProcInfo *p = proc_table_find(ctx, proc_name);
      if (!p) {
        err(ctx->in_path, "internal: Proc not found in pre-scan table");
        free(key);
        free(proc_name);
        free(link_name);
        for (size_t i = 0; i < param_count; i++) free(param_names[i]);
        free(param_names);
        free(param_tis);
        free(param_node_ids);
        return false;
      }

      if (is_extern) {
        // Extern declarations must not have a body.
        char ch2 = 0;
        if (!json_peek_non_ws(c, &ch2)) {
          err(ctx->in_path, "unexpected EOF in Proc.body");
          free(key);
          free(proc_name);
          free(link_name);
          for (size_t i = 0; i < param_count; i++) free(param_names[i]);
          free(param_names);
          free(param_tis);
          free(param_node_ids);
          return false;
        }
        if (ch2 != 'n') {
          err(ctx->in_path, "Proc.extern=true requires Proc.body to be null");
          free(key);
          free(proc_name);
          free(link_name);
          for (size_t i = 0; i < param_count; i++) free(param_names[i]);
          free(param_names);
          free(param_tis);
          free(param_node_ids);
          return false;
        }
        if (!grit_json_skip_value(c)) {
          err(ctx->in_path, "invalid Proc.body");
          free(key);
          free(proc_name);
          free(link_name);
          for (size_t i = 0; i < param_count; i++) free(param_names[i]);
          free(param_names);
          free(param_tis);
          free(param_node_ids);
          return false;
        }
        if (strcmp(proc_name, "main") == 0) {
          err(ctx->in_path, "Proc 'main' cannot be extern");
          free(key);
          free(proc_name);
          free(link_name);
          for (size_t i = 0; i < param_count; i++) free(param_names[i]);
          free(param_names);
          free(param_tis);
          free(param_node_ids);
          return false;
        }

        p->is_extern = true;
        if (link_name) {
          free(p->link_name);
          p->link_name = strdup(link_name);
          if (!p->link_name) {
            err(ctx->in_path, "OOM copying Proc.link_name");
            free(key);
            free(proc_name);
            free(link_name);
            for (size_t i = 0; i < param_count; i++) free(param_names[i]);
            free(param_names);
            free(param_tis);
            free(param_node_ids);
            return false;
          }
        }

        if (p->ret_ti.base != ret_ti.base || p->ret_ti.ptr_of != ret_ti.ptr_of) {
          err(ctx->in_path, "Proc.ret does not match prescan signature");
          free(key);
          free(proc_name);
          free(link_name);
          for (size_t i = 0; i < param_count; i++) free(param_names[i]);
          free(param_names);
          free(param_tis);
          free(param_node_ids);
          return false;
        }
        if (p->param_count != param_count) {
          err(ctx->in_path, "Proc.params arity does not match prescan signature");
          free(key);
          free(proc_name);
          free(link_name);
          for (size_t i = 0; i < param_count; i++) free(param_names[i]);
          free(param_names);
          free(param_tis);
          free(param_node_ids);
          return false;
        }
        for (size_t i = 0; i < param_count; i++) {
          if (p->params[i].base != param_tis[i].base || p->params[i].ptr_of != param_tis[i].ptr_of) {
            err(ctx->in_path, "Proc.params do not match prescan signature");
            free(key);
            free(proc_name);
            free(link_name);
            for (size_t j = 0; j < param_count; j++) free(param_names[j]);
            free(param_names);
            free(param_tis);
            free(param_node_ids);
            return false;
          }
        }
        if (!emit_fn_type_if_needed(ctx, p)) {
          free(key);
          free(proc_name);
          free(link_name);
          for (size_t i = 0; i < param_count; i++) free(param_names[i]);
          free(param_names);
          free(param_tis);
          free(param_node_ids);
          return false;
        }

        fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
        emit_json_string(ctx->out, p->fn_id);
        fprintf(ctx->out, ",\"tag\":\"decl.fn\",\"type_ref\":");
        emit_json_string(ctx->out, p->fn_type_id);
        fprintf(ctx->out, ",\"fields\":{\"name\":");
        emit_json_string(ctx->out, p->link_name ? p->link_name : proc_name);
        fprintf(ctx->out, "}}\n");

        free(key);
        continue;
      }

      ctx->fn_ret = ret_ti.base;
      if (!emit_typeinfo_if_needed(ctx, &ret_ti)) {
        free(key);
        free(proc_name);
        free(link_name);
        for (size_t i = 0; i < param_count; i++) free(param_names[i]);
        free(param_names);
        free(param_tis);
        free(param_node_ids);
        return false;
      }
      if (p->ret_ti.base != ret_ti.base || p->ret_ti.ptr_of != ret_ti.ptr_of) {
        err(ctx->in_path, "Proc.ret does not match prescan signature");
        free(key);
        free(proc_name);
        free(link_name);
        for (size_t i = 0; i < param_count; i++) free(param_names[i]);
        free(param_names);
        free(param_tis);
        free(param_node_ids);
        return false;
      }
      if (p->param_count != param_count) {
        err(ctx->in_path, "Proc.params arity does not match prescan signature");
        free(key);
        free(proc_name);
        free(link_name);
        for (size_t i = 0; i < param_count; i++) free(param_names[i]);
        free(param_names);
        free(param_tis);
        free(param_node_ids);
        return false;
      }
      for (size_t i = 0; i < param_count; i++) {
        if (p->params[i].base != param_tis[i].base || p->params[i].ptr_of != param_tis[i].ptr_of) {
          err(ctx->in_path, "Proc.params do not match prescan signature");
          free(key);
          free(proc_name);
          free(link_name);
          for (size_t j = 0; j < param_count; j++) free(param_names[j]);
          free(param_names);
          free(param_tis);
          free(param_node_ids);
          return false;
        }
      }

      if (!emit_fn_type_if_needed(ctx, p)) {
        free(key);
        free(proc_name);
        free(link_name);
        for (size_t i = 0; i < param_count; i++) free(param_names[i]);
        free(param_names);
        free(param_tis);
        free(param_node_ids);
        return false;
      }

      // Parameters + locals are per-proc; clear any previous Proc bindings.
      locals_free(ctx);

      // Emit param nodes and bind names.
      for (size_t i = 0; i < param_count; i++) {
        if (!emit_typeinfo_if_needed(ctx, &param_tis[i])) {
          free(key);
          free(proc_name);
          for (size_t j = 0; j < param_count; j++) free(param_names[j]);
          free(param_names);
          free(param_tis);
          free(param_node_ids);
          return false;
        }
        if (!locals_push_binding(ctx, param_names[i], param_tis[i], false)) {
          err(ctx->in_path, "OOM binding Proc param");
          free(key);
          free(proc_name);
          for (size_t j = 0; j < param_count; j++) free(param_names[j]);
          free(param_names);
          free(param_tis);
          free(param_node_ids);
          return false;
        }
        char *pid = new_node_id(ctx);
        param_node_ids[i] = pid;
        fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
        emit_json_string(ctx->out, pid);
        fprintf(ctx->out, ",\"tag\":\"param\",\"type_ref\":");
        emit_json_string(ctx->out, param_tis[i].sir_id);
        fprintf(ctx->out, ",\"fields\":{\"name\":");
        emit_json_string(ctx->out, param_names[i]);
        fprintf(ctx->out, "}}\n");
      }

      SirFnBuild fn = {0};
      size_t entry_idx = 0;
      if (!fn_build_new_block(&fn, ctx, &entry_idx)) {
        err(ctx->in_path, "OOM creating entry block");
        free(key);
        free(proc_name);
        return false;
      }
      fn.entry_block = entry_idx;
      fn.cur_block = entry_idx;

      if (!parse_block(c, ctx, &fn, false, NULL)) {
        free(key);
        free(proc_name);
        free(link_name);
        return false;
      }

      // For now, enforce: current block must terminate (no implicit fallthrough).
      if (!fn.blocks[fn.cur_block].terminated) {
        err(ctx->in_path, "Proc.body must end in a terminator (Return/branch); no implicit fallthrough");
        free(key);
        free(proc_name);
        free(link_name);
        return false;
      }

      // Also enforce: all blocks are terminated.
      for (size_t bi = 0; bi < fn.block_count; bi++) {
        if (!fn.blocks[bi].terminated) {
          err(ctx->in_path, "unterminated block in CFG (missing Return or branch)");
          free(key);
          free(proc_name);
          free(link_name);
          return false;
        }
      }

      // Emit block nodes.
      for (size_t bi = 0; bi < fn.block_count; bi++) {
        SirBlockBuild *b = &fn.blocks[bi];
        fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
        emit_json_string(ctx->out, b->id);
        fprintf(ctx->out, ",\"tag\":\"block\",\"fields\":{\"stmts\":[");
        for (size_t i = 0; i < b->stmt_count; i++) {
          if (i)
            fprintf(ctx->out, ",");
          fprintf(ctx->out, "{\"t\":\"ref\",\"id\":");
          emit_json_string(ctx->out, b->stmt_ids[i]);
          fprintf(ctx->out, "}");
        }
        fprintf(ctx->out, "]}}\n");
      }

            // Emit fn node in CFG-form.
            fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
            emit_json_string(ctx->out, p->fn_id);
            fprintf(ctx->out, ",\"tag\":\"fn\",\"type_ref\":");
            emit_json_string(ctx->out, p->fn_type_id);
            fprintf(ctx->out, ",\"fields\":{\"name\":");
            emit_json_string(ctx->out, proc_name);
            fprintf(ctx->out, ",\"linkage\":");
            emit_json_string(ctx->out, (strcmp(proc_name, "main") == 0) ? "public" : "local");
            fprintf(ctx->out, ",\"params\":[");
            for (size_t i = 0; i < param_count; i++) {
              if (i) fprintf(ctx->out, ",");
              fprintf(ctx->out, "{\"t\":\"ref\",\"id\":");
              emit_json_string(ctx->out, param_node_ids[i]);
              fprintf(ctx->out, "}");
            }
            fprintf(ctx->out, "],\"entry\":{\"t\":\"ref\",\"id\":");
      emit_json_string(ctx->out, fn.blocks[fn.entry_block].id);
      fprintf(ctx->out, "},\"blocks\":[");
      for (size_t bi = 0; bi < fn.block_count; bi++) {
        if (bi)
          fprintf(ctx->out, ",");
        fprintf(ctx->out, "{\"t\":\"ref\",\"id\":");
        emit_json_string(ctx->out, fn.blocks[bi].id);
        fprintf(ctx->out, "}");
      }
      fprintf(ctx->out, "]}}\n");

      for (size_t i = 0; i < param_count; i++) {
        free(param_node_ids[i]);
        param_node_ids[i] = NULL;
      }

      // Free builder allocations.
      for (size_t bi = 0; bi < fn.block_count; bi++) {
        SirBlockBuild *b = &fn.blocks[bi];
        for (size_t i = 0; i < b->stmt_count; i++)
          free(b->stmt_ids[i]);
        free(b->stmt_ids);
        free(b->id);
      }
      free(fn.blocks);
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid Proc field");
        free(key);
        free(proc_name);
        free(link_name);
        for (size_t i = 0; i < param_count; i++) free(param_names[i]);
        free(param_names);
        free(param_tis);
        free(param_node_ids);
        return false;
      }
    }
    free(key);
  }

  if (link_name && !is_extern) {
    err(ctx->in_path, "Proc.link_name is only allowed when Proc.extern=true");
    free(proc_name);
    free(link_name);
    for (size_t i = 0; i < param_count; i++) free(param_names[i]);
    free(param_names);
    free(param_tis);
    free(param_node_ids);
    return false;
  }

  if (!seen_name || !seen_params || !seen_ret || (!is_extern && !seen_body)) {
    err(ctx->in_path, "Proc requires fields: name, params, ret, body (unless extern=true)");
    free(proc_name);
    free(link_name);
    for (size_t i = 0; i < param_count; i++) free(param_names[i]);
    free(param_names);
    free(param_tis);
    free(param_node_ids);
    return false;
  }

  if (seen_extern && is_extern && !seen_body) {
    if (strcmp(proc_name, "main") == 0) {
      err(ctx->in_path, "Proc 'main' cannot be extern");
      free(proc_name);
      free(link_name);
      for (size_t i = 0; i < param_count; i++) free(param_names[i]);
      free(param_names);
      free(param_tis);
      free(param_node_ids);
      return false;
    }
    ProcInfo *p = proc_table_find(ctx, proc_name);
    if (!p) {
      err(ctx->in_path, "internal: Proc not found in pre-scan table");
      free(proc_name);
      free(link_name);
      for (size_t i = 0; i < param_count; i++) free(param_names[i]);
      free(param_names);
      free(param_tis);
      free(param_node_ids);
      return false;
    }
    p->is_extern = true;
    if (link_name) {
      free(p->link_name);
      p->link_name = strdup(link_name);
      if (!p->link_name) {
        err(ctx->in_path, "OOM copying Proc.link_name");
        free(proc_name);
        free(link_name);
        for (size_t i = 0; i < param_count; i++) free(param_names[i]);
        free(param_names);
        free(param_tis);
        free(param_node_ids);
        return false;
      }
    }
    if (!emit_fn_type_if_needed(ctx, p)) {
      free(proc_name);
      free(link_name);
      for (size_t i = 0; i < param_count; i++) free(param_names[i]);
      free(param_names);
      free(param_tis);
      free(param_node_ids);
      return false;
    }
    fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
    emit_json_string(ctx->out, p->fn_id);
    fprintf(ctx->out, ",\"tag\":\"decl.fn\",\"type_ref\":");
    emit_json_string(ctx->out, p->fn_type_id);
    fprintf(ctx->out, ",\"fields\":{\"name\":");
    emit_json_string(ctx->out, p->link_name ? p->link_name : proc_name);
    fprintf(ctx->out, "}}\n");
  }
  free(proc_name);
  free(link_name);
  for (size_t i = 0; i < param_count; i++) free(param_names[i]);
  free(param_names);
  free(param_tis);
  free(param_node_ids);
  return true;
}

bool parse_unit_item_and_maybe_emit(GritJsonCursor *c, EmitCtx *ctx) {
  char *k = NULL;
  if (!parse_node_k_string(c, ctx, &k)) return false;

  if (strcmp(k, "Proc") != 0) {
    free(k);
    return skip_remaining_object_fields(c, ctx, "Unit.items item");
  }
  free(k);
  return parse_proc_fields_and_emit_fn(c, ctx);
}

bool prescan_ast_for_procs(const char *buf, size_t len, EmitCtx *ctx) {
  GritJsonCursor c = grit_json_cursor((char *)buf, len);
  if (!grit_json_consume_char(&c, '{')) {
    err(ctx->in_path, "expected root object");
    return false;
  }

  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(&c, &ch)) {
      err(ctx->in_path, "unexpected EOF in root");
      return false;
    }
    if (ch == '}') {
      c.p++;
      break;
    }

    char *key = NULL;
    if (!json_expect_key(&c, &key)) {
      err(ctx->in_path, "invalid root key");
      return false;
    }

    if (strcmp(key, "ast") != 0) {
      if (!grit_json_skip_value(&c)) {
        err(ctx->in_path, "invalid root value");
        free(key);
        return false;
      }
      free(key);
    } else {
      free(key);

      char *k = NULL;
      if (!parse_node_k_string(&c, ctx, &k)) return false;
      if (strcmp(k, "Unit") != 0) {
        err(ctx->in_path, "ast must be Unit");
        free(k);
        return false;
      }
      free(k);

      bool seen_items = false;
      bool saw_main = false;

      for (;;) {
        if (!json_peek_non_ws(&c, &ch)) {
          err(ctx->in_path, "unexpected EOF in Unit");
          return false;
        }
        if (ch == '}') {
          c.p++;
          break;
        }
        if (ch != ',') {
          err(ctx->in_path, "expected ',' or '}' in Unit");
          return false;
        }
        c.p++;
        char *ukey = NULL;
        if (!json_expect_key(&c, &ukey)) {
          err(ctx->in_path, "invalid Unit key");
          return false;
        }

        if (strcmp(ukey, "items") != 0) {
          if (!grit_json_skip_value(&c)) {
            err(ctx->in_path, "invalid Unit field");
            free(ukey);
            return false;
          }
          free(ukey);
          continue;
        }
        free(ukey);
        seen_items = true;

        if (!grit_json_consume_char(&c, '[')) {
          err(ctx->in_path, "Unit.items must be array");
          return false;
        }
        if (!json_peek_non_ws(&c, &ch)) {
          err(ctx->in_path, "unexpected EOF in Unit.items");
          return false;
        }
        if (ch == ']') {
          c.p++;
          continue;
        }

        for (;;) {
          // Each item is a node.
          char *ik = NULL;
          if (!parse_node_k_string(&c, ctx, &ik)) return false;
          if (strcmp(ik, "Proc") != 0) {
            free(ik);
            if (!skip_remaining_object_fields(&c, ctx, "Unit.items item")) return false;
          } else {
            free(ik);

            bool seen_name = false;
            bool seen_params = false;
            bool seen_ret = false;
            char *pname = NULL;
            bool is_extern = false;
            char *plink_name = NULL;
            SemTypeInfo pret_ti = {0};
            SemTypeInfo *pparams = NULL;
            size_t pparam_count = 0;
            size_t pparam_cap = 0;

            for (;;) {
              if (!json_peek_non_ws(&c, &ch)) {
                err(ctx->in_path, "unexpected EOF in Proc (prescan)");
                free(pname);
                return false;
              }
              if (ch == '}') {
                c.p++;
                break;
              }
              if (ch != ',') {
                err(ctx->in_path, "expected ',' or '}' in Proc (prescan)");
                free(pname);
                return false;
              }
              c.p++;
              char *pkey = NULL;
              if (!json_expect_key(&c, &pkey)) {
                err(ctx->in_path, "invalid Proc key (prescan)");
                free(pname);
                return false;
              }

              if (strcmp(pkey, "name") == 0) {
                seen_name = true;
                if (!parse_tok_text_alloc_strict(&c, ctx->in_path, &pname)) {
                  free(pkey);
                  free(pparams);
                  free(plink_name);
                  return false;
                }
              } else if (strcmp(pkey, "link_name") == 0) {
                free(plink_name);
                plink_name = NULL;
                if (!parse_tok_text_alloc_strict(&c, ctx->in_path, &plink_name)) {
                  free(pkey);
                  free(pname);
                  free(pparams);
                  return false;
                }
              } else if (strcmp(pkey, "extern") == 0) {
                char ch2 = 0;
                if (!json_peek_non_ws(&c, &ch2)) {
                  err(ctx->in_path, "unexpected EOF in Proc.extern (prescan)");
                  free(pkey);
                  free(pname);
                  free(pparams);
                  free(plink_name);
                  return false;
                }
                is_extern = (ch2 == 't');
                if (!grit_json_skip_value(&c)) {
                  err(ctx->in_path, "invalid Proc.extern (prescan)");
                  free(pkey);
                  free(pname);
                  free(pparams);
                  free(plink_name);
                  return false;
                }
              } else if (strcmp(pkey, "params") == 0) {
                seen_params = true;
                if (!grit_json_consume_char(&c, '[')) {
                  err(ctx->in_path, "Proc.params must be array (prescan)");
                  free(pkey);
                  free(pname);
                  free(pparams);
                  free(plink_name);
                  return false;
                }
                if (!json_peek_non_ws(&c, &ch)) {
                  err(ctx->in_path, "unexpected EOF in Proc.params (prescan)");
                  free(pkey);
                  free(pname);
                  free(pparams);
                  return false;
                }
                if (ch != ']') {
                  for (;;) {
                    char *pk = NULL;
                    if (!parse_node_k_string(&c, ctx, &pk)) {
                      free(pkey);
                      free(pname);
                      free(pparams);
                      return false;
                    }
                    if (strcmp(pk, "Param") != 0 && strcmp(pk, "ParamPat") != 0) {
                      err(ctx->in_path, "Proc.params items must be Param or ParamPat (prescan)");
                      free(pk);
                      free(pkey);
                      free(pname);
                      free(pparams);
                      return false;
                    }
                    free(pk);

                    bool seen_ptype = false;
                    SemTypeInfo pti = {0};
                    char *tmp_name = NULL;
                    for (;;) {
                      if (!json_peek_non_ws(&c, &ch)) {
                        err(ctx->in_path, "unexpected EOF in Param (prescan)");
                        free(tmp_name);
                        free(pkey);
                        free(pname);
                        free(pparams);
                        return false;
                      }
                      if (ch == '}') {
                        c.p++;
                        break;
                      }
                      if (ch != ',') {
                        err(ctx->in_path, "expected ',' or '}' in Param (prescan)");
                        free(tmp_name);
                        free(pkey);
                        free(pname);
                        free(pparams);
                        return false;
                      }
                      c.p++;
                      char *ppkey = NULL;
                      if (!json_expect_key(&c, &ppkey)) {
                        err(ctx->in_path, "invalid Param key (prescan)");
                        free(tmp_name);
                        free(pkey);
                        free(pname);
                        free(pparams);
                        return false;
                      }
                      if (strcmp(ppkey, "type") == 0) {
                        seen_ptype = true;
                        if (!parse_type_typeinfo(&c, ctx, &pti)) {
                          free(ppkey);
                          free(tmp_name);
                          free(pkey);
                          free(pname);
                          free(pparams);
                          free(plink_name);
                          return false;
                        }
                      } else if (strcmp(ppkey, "name") == 0) {
                        // Parse and free; name is irrelevant in signature.
                        if (!parse_tok_text_alloc_strict(&c, ctx->in_path, &tmp_name)) {
                          free(ppkey);
                          free(pkey);
                          free(pname);
                          free(pparams);
                          free(plink_name);
                          return false;
                        }
                        free(tmp_name);
                        tmp_name = NULL;
                      } else {
                        if (!grit_json_skip_value(&c)) {
                          err(ctx->in_path, "invalid Param field (prescan)");
                          free(ppkey);
                          free(tmp_name);
                          free(pkey);
                          free(pname);
                          free(pparams);
                          free(plink_name);
                          return false;
                        }
                      }
                      free(ppkey);
                    }
                    if (!seen_ptype || pti.base == SEM2SIR_TYPE_INVALID) {
                      err(ctx->in_path, "Param.type is required (prescan)");
                      free(pkey);
                      free(pname);
                      free(pparams);
                      free(plink_name);
                      return false;
                    }

                    if (pparam_count == pparam_cap) {
                      size_t next_cap = pparam_cap ? pparam_cap * 2 : 4;
                      SemTypeInfo *next = (SemTypeInfo *)realloc(pparams, next_cap * sizeof(SemTypeInfo));
                      if (!next) {
                        err(ctx->in_path, "OOM allocating Proc.params (prescan)");
                        free(pkey);
                        free(pname);
                        free(pparams);
                        free(plink_name);
                        return false;
                      }
                      pparams = next;
                      pparam_cap = next_cap;
                    }
                    pparams[pparam_count++] = pti;

                    if (!json_peek_non_ws(&c, &ch)) {
                      err(ctx->in_path, "unexpected EOF in Proc.params (prescan)");
                      free(pkey);
                      free(pname);
                      free(pparams);
                      free(plink_name);
                      return false;
                    }
                    if (ch == ',') {
                      c.p++;
                      continue;
                    }
                    if (ch == ']') break;
                    err(ctx->in_path, "expected ',' or ']' in Proc.params (prescan)");
                    free(pkey);
                    free(pname);
                    free(pparams);
                    free(plink_name);
                    return false;
                  }
                }
                c.p++; // consume ']'
              } else if (strcmp(pkey, "ret") == 0) {
                seen_ret = true;
                if (!json_peek_non_ws(&c, &ch)) {
                  err(ctx->in_path, "unexpected EOF in Proc.ret (prescan)");
                  free(pkey);
                  free(pname);
                  free(pparams);
                  free(plink_name);
                  return false;
                }
                if (ch == 'n') {
                  err(ctx->in_path, "Proc.ret must be explicit (no defaults)");
                  free(pkey);
                  free(pname);
                  free(pparams);
                  free(plink_name);
                  return false;
                }
                if (!parse_type_typeinfo(&c, ctx, &pret_ti)) {
                  free(pkey);
                  free(pname);
                  free(pparams);
                  free(plink_name);
                  return false;
                }
              } else {
                if (!grit_json_skip_value(&c)) {
                  err(ctx->in_path, "invalid Proc field (prescan)");
                  free(pkey);
                  free(pname);
                  free(pparams);
                  free(plink_name);
                  return false;
                }
              }

              free(pkey);
            }

            if (!seen_name || !seen_params || !seen_ret || !pname || pret_ti.base == SEM2SIR_TYPE_INVALID) {
              err(ctx->in_path, "Proc requires fields: name, params, ret (prescan)");
              free(pname);
              free(pparams);
              free(plink_name);
              return false;
            }
            if (is_extern && strcmp(pname, "main") == 0) {
              err(ctx->in_path, "Proc 'main' cannot be extern");
              free(pname);
              free(pparams);
              free(plink_name);
              return false;
            }
            if (strcmp(pname, "main") == 0) saw_main = true;
            if (!proc_table_add(ctx, pname, pparams, pparam_count, pret_ti)) {
              free(pname);
              free(pparams);
              free(plink_name);
              return false;
            }
            ProcInfo *p = proc_table_find(ctx, pname);
            if (!p) {
              err(ctx->in_path, "internal: Proc not found after add (prescan)");
              free(pname);
              free(pparams);
              free(plink_name);
              return false;
            }
            p->is_extern = is_extern;
            if (plink_name) {
              p->link_name = strdup(plink_name);
              if (!p->link_name) {
                err(ctx->in_path, "OOM copying Proc.link_name (prescan)");
                free(pname);
                free(pparams);
                free(plink_name);
                return false;
              }
            }
            free(pname);
            free(pparams);
            free(plink_name);
          }

          if (!json_peek_non_ws(&c, &ch)) {
            err(ctx->in_path, "unexpected EOF in Unit.items (prescan)");
            return false;
          }
          if (ch == ',') {
            c.p++;
            continue;
          }
          if (ch == ']') break;
          err(ctx->in_path, "expected ',' or ']' in Unit.items (prescan)");
          return false;
        }
        if (!grit_json_consume_char(&c, ']')) {
          err(ctx->in_path, "expected ']' to close Unit.items (prescan)");
          return false;
        }
      }

      if (!seen_items) {
        err(ctx->in_path, "Unit missing required field items (prescan)");
        return false;
      }
      if (!saw_main) {
        err(ctx->in_path, "Unit must contain a Proc named 'main'");
        return false;
      }

      // Done prescanning ast.
      return true;
    }

    if (!json_peek_non_ws(&c, &ch)) {
      err(ctx->in_path, "unexpected EOF in root");
      return false;
    }
    if (ch == ',') {
      c.p++;
      continue;
    }
    if (ch == '}') {
      c.p++;
      break;
    }
    err(ctx->in_path, "expected ',' or '}' in root");
    return false;
  }

  err(ctx->in_path, "missing required field ast");
  return false;
}

