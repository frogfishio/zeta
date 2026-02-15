#include "sem2sir_emit.h"

#include "sem2sir_emit_internal.h"

static void sem2sir_emit_ctx_free(EmitCtx *ctx) {
  if (!ctx)
    return;
  locals_free(ctx);
  proc_table_free(ctx);
}

int sem2sir_emit_sir_file(const char *in_stage4_jsonl_path, const char *out_sir_jsonl_path) {
  // First: enforce the strict Stage 4 boundary checker.
  if (sem2sir_check_stage4_file(in_stage4_jsonl_path) != 0) {
    return 1;
  }

  size_t len = 0;
  char *buf = read_file(in_stage4_jsonl_path, &len);
  if (!buf) {
    fprintf(stderr, "sem2sir: %s: failed to read file\n", in_stage4_jsonl_path ? in_stage4_jsonl_path : "<input>");
    return 1;
  }

  // Open output early; on any error we will unlink it (no partial success).
  FILE *out = fopen(out_sir_jsonl_path, "wb");
  if (!out) {
    fprintf(stderr, "sem2sir: %s: failed to open output\n", out_sir_jsonl_path ? out_sir_jsonl_path : "<out>");
    free(buf);
    return 1;
  }

  EmitCtx ctx = {0};
  ctx.in_path = in_stage4_jsonl_path;
  ctx.out_path = out_sir_jsonl_path;
  ctx.out = out;
  ctx.next_node = 1;
  ctx.next_sym = 1;
  ctx.next_anon_type = 1;
  ctx.fn_ret = SEM2SIR_TYPE_INVALID;
  ctx.default_int = SEM2SIR_TYPE_INVALID;
  ctx.default_ptr_pointee = SEM2SIR_TYPE_INVALID;

  // SIR is the lowering target. Emit data:v1 canonical types and literals.
  ctx.meta_data_v1 = true;

  bool ok = true;

  // We must emit meta first and cannot retroactively add features.
  // Pre-scan for sem:v1-only constructs.
  // - short-circuit boolean operators (lower to sem.and_sc / sem.or_sc)
  // - Match expressions (lower to sem.switch)
  if (strstr(buf, "core.bool.and_sc") != NULL || strstr(buf, "core.bool.or_sc") != NULL ||
      strstr(buf, "\"k\":\"Match\"") != NULL || strstr(buf, "\"k\": \"Match\"") != NULL) {
    ctx.meta_sem_v1 = true;
  }

  // Pre-scan metadata for explicit default rules (order-independent).
  if (!prescan_root_for_meta_defaults(buf, len, &ctx)) {
    err(in_stage4_jsonl_path, "failed to parse root/meta for defaults");
    ok = false;
    goto done;
  }

  // Pre-scan AST for Proc headers so we can assign stable fn IDs
  // and resolve direct calls without relying on item ordering.
  if (!prescan_ast_for_procs(buf, len, &ctx)) {
    err(in_stage4_jsonl_path, "failed to pre-scan AST for procs");
    ok = false;
    goto done;
  }

  // Emit meta first.
  fprintf(out, "{\"ir\":\"sir-v1.0\",\"k\":\"meta\",\"producer\":\"sem2sir\",\"unit\":");
  emit_json_string(out, "main");
  if (ctx.meta_sem_v1 || ctx.meta_data_v1) {
    fprintf(out, ",\"ext\":{\"features\":[");
    bool first = true;
    if (ctx.meta_sem_v1) {
      fprintf(out, "%s\"sem:v1\"", first ? "" : ",");
      first = false;
    }
    if (ctx.meta_data_v1) {
      fprintf(out, "%s\"data:v1\"", first ? "" : ",");
      first = false;
    }
    fprintf(out, "]}");
  }
  fprintf(out, "}\n\n");

  // data:v1 pack validation requires these canonical named types to exist.
  if (ctx.meta_data_v1) {
    if (!emit_type_if_needed(&ctx, SEM2SIR_TYPE_BYTES) || !emit_type_if_needed(&ctx, SEM2SIR_TYPE_STRING_UTF8) ||
        !emit_type_if_needed(&ctx, SEM2SIR_TYPE_CSTR)) {
      err(in_stage4_jsonl_path, "failed to emit data:v1 canonical types");
      ok = false;
      goto done;
    }
    fprintf(out, "\n");
  }

  GritJsonCursor c = grit_json_cursor(buf, len);

  if (!grit_json_consume_char(&c, '{')) {
    err(in_stage4_jsonl_path, "expected root object");
    ok = false;
    goto done;
  }

  char ch = 0;
  if (!json_peek_non_ws(&c, &ch)) {
    err(in_stage4_jsonl_path, "unexpected EOF in root");
    ok = false;
    goto done;
  }

  bool seen_ast = false;

  for (;;) {
    if (!json_peek_non_ws(&c, &ch)) {
      err(in_stage4_jsonl_path, "unexpected EOF in root");
      ok = false;
      goto done;
    }
    if (ch == '}') {
      c.p++;
      break;
    }

    char *key = NULL;
    if (!json_expect_key(&c, &key)) {
      err(in_stage4_jsonl_path, "invalid root key");
      ok = false;
      goto done;
    }

    if (strcmp(key, "ast") == 0) {
      seen_ast = true;

      // ast must be Unit.
      char *k = NULL;
      if (!parse_node_k_string(&c, &ctx, &k)) {
        free(key);
        ok = false;
        goto done;
      }
      if (strcmp(k, "Unit") != 0) {
        err(in_stage4_jsonl_path, "ast must be Unit");
        free(k);
        free(key);
        ok = false;
        goto done;
      }
      free(k);

      // Parse Unit fields; require items with exactly one Proc.
      bool seen_items = false;
      bool saw_main = false;

      for (;;) {
        if (!json_peek_non_ws(&c, &ch)) {
          err(in_stage4_jsonl_path, "unexpected EOF in Unit");
          free(key);
          ok = false;
          goto done;
        }
        if (ch == '}') {
          c.p++;
          break;
        }
        if (ch != ',') {
          err(in_stage4_jsonl_path, "expected ',' or '}' in Unit");
          free(key);
          ok = false;
          goto done;
        }
        c.p++;

        char *ukey = NULL;
        if (!json_expect_key(&c, &ukey)) {
          err(in_stage4_jsonl_path, "invalid Unit key");
          free(key);
          ok = false;
          goto done;
        }

        if (strcmp(ukey, "items") == 0) {
          seen_items = true;
          if (!grit_json_consume_char(&c, '[')) {
            err(in_stage4_jsonl_path, "Unit.items must be array");
            free(ukey);
            free(key);
            ok = false;
            goto done;
          }
          if (!json_peek_non_ws(&c, &ch)) {
            err(in_stage4_jsonl_path, "unexpected EOF in Unit.items");
            free(ukey);
            free(key);
            ok = false;
            goto done;
          }
          if (ch != ']') {
            for (;;) {
              if (!parse_unit_item_and_maybe_emit(&c, &ctx)) {
                free(ukey);
                free(key);
                ok = false;
                goto done;
              }

              if (!json_peek_non_ws(&c, &ch)) {
                err(in_stage4_jsonl_path, "unexpected EOF in Unit.items");
                free(ukey);
                free(key);
                ok = false;
                goto done;
              }
              if (ch == ',') {
                c.p++;
                continue;
              }
              if (ch == ']')
                break;
              err(in_stage4_jsonl_path, "expected ',' or ']' in Unit.items");
              free(ukey);
              free(key);
              ok = false;
              goto done;
            }
          }
          if (!grit_json_consume_char(&c, ']')) {
            err(in_stage4_jsonl_path, "expected ']' to close Unit.items");
            free(ukey);
            free(key);
            ok = false;
            goto done;
          }
        } else {
          if (!grit_json_skip_value(&c)) {
            err(in_stage4_jsonl_path, "invalid Unit field");
            free(ukey);
            free(key);
            ok = false;
            goto done;
          }
        }
        free(ukey);
      }

      (void)saw_main;
      if (!seen_items) {
        err(in_stage4_jsonl_path, "Unit requires items");
        free(key);
        ok = false;
        goto done;
      }
    } else {
      // Everything else was validated by sem2sir_check_stage4_file; skip.
      if (!grit_json_skip_value(&c)) {
        err(in_stage4_jsonl_path, "invalid root value");
        free(key);
        ok = false;
        goto done;
      }
    }

    free(key);
    if (!json_peek_non_ws(&c, &ch)) {
      err(in_stage4_jsonl_path, "unexpected EOF in root");
      ok = false;
      goto done;
    }
    if (ch == ',') {
      c.p++;
      continue;
    }
    if (ch == '}') {
      c.p++;
      break;
    }
    err(in_stage4_jsonl_path, "expected ',' or '}' in root");
    ok = false;
    goto done;
  }

  if (!seen_ast) {
    err(in_stage4_jsonl_path, "missing required field ast");
    ok = false;
    goto done;
  }

done:
  sem2sir_emit_ctx_free(&ctx);
  free(buf);
  if (!ok) {
    fclose(out);
    (void)unlink(out_sir_jsonl_path);
    return 1;
  }
  fclose(out);
  return 0;
}
