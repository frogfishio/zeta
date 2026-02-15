#include "sem2sir_emit_internal.h"

bool parse_meta_for_defaults(GritJsonCursor *c, EmitCtx *ctx) {
  // Parse Stage4 meta object enough to learn any explicit default rules.
  // This affects strict interpretation only; it does not change output metadata.
  if (!grit_json_consume_char(c, '{')) {
    err(ctx->in_path, "meta must be an object");
    return false;
  }

  char ch = 0;
  if (!json_peek_non_ws(c, &ch)) {
    err(ctx->in_path, "unexpected EOF in meta");
    return false;
  }
  if (ch == '}') {
    c->p++;
    return true;
  }

  for (;;) {
    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      err(ctx->in_path, "invalid meta key");
      return false;
    }

    if (strcmp(key, "types") == 0) {
      if (!grit_json_consume_char(c, '{')) {
        err(ctx->in_path, "meta.types must be an object");
        free(key);
        return false;
      }

      if (!json_peek_non_ws(c, &ch)) {
        err(ctx->in_path, "unexpected EOF in meta.types");
        free(key);
        return false;
      }

      if (ch != '}') {
        for (;;) {
          char *tkey = NULL;
          if (!json_expect_key(c, &tkey)) {
            err(ctx->in_path, "invalid meta.types key");
            free(key);
            return false;
          }

          char *tval = NULL;
          if (!grit_json_parse_string_alloc(c, &tval)) {
            err(ctx->in_path, "meta.types values must be strings");
            free(tkey);
            free(key);
            return false;
          }

          if (strcmp(tkey, "@default.int") == 0 || strcmp(tkey, "__default_int") == 0) {
            sem2sir_type_id tid = sem2sir_type_parse(tval, strlen(tval));
            if (tid != SEM2SIR_TYPE_I32 && tid != SEM2SIR_TYPE_I64) {
              err(ctx->in_path, "meta.types['@default.int'] must be 'i32' or 'i64' in sem2sir MVP");
              free(tval);
              free(tkey);
              free(key);
              return false;
            }
            ctx->default_int = tid;
          }

          if (strcmp(tkey, "@default.ptr.pointee") == 0 || strcmp(tkey, "__default_ptr_pointee") == 0) {
            sem2sir_type_id tid = sem2sir_type_parse(tval, strlen(tval));
            if (tid == SEM2SIR_TYPE_INVALID || tid == SEM2SIR_TYPE_PTR) {
              err(ctx->in_path, "meta.types['@default.ptr.pointee'/'__default_ptr_pointee'] must be a non-ptr sem2sir type id");
              free(tval);
              free(tkey);
              free(key);
              return false;
            }
            const char *load_tag = type_load_tag(tid);
            const char *store_tag = type_store_tag(tid);
            int align = type_align_bytes(tid);
            if (!load_tag || !store_tag || align == 0) {
              err(ctx->in_path,
                  "meta.types['@default.ptr.pointee'/'__default_ptr_pointee'] must be a load/store-capable value type in sem2sir MVP");
              free(tval);
              free(tkey);
              free(key);
              return false;
            }
            ctx->default_ptr_pointee = tid;
          }

          free(tval);
          free(tkey);

          if (!json_peek_non_ws(c, &ch)) {
            err(ctx->in_path, "unexpected EOF in meta.types");
            free(key);
            return false;
          }
          if (ch == ',') {
            c->p++;
            continue;
          }
          if (ch == '}')
            break;
          err(ctx->in_path, "expected ',' or '}' in meta.types");
          free(key);
          return false;
        }
      }

      if (!grit_json_consume_char(c, '}')) {
        err(ctx->in_path, "expected '}' to close meta.types");
        free(key);
        return false;
      }
    } else if (strcmp(key, "ops") == 0) {
      // sem2sir does not consume operator aliasing metadata.
      // If present, it must be an empty object.
      if (!grit_json_consume_char(c, '{')) {
        err(ctx->in_path, "meta.ops must be an object");
        free(key);
        return false;
      }
      if (!json_peek_non_ws(c, &ch)) {
        err(ctx->in_path, "unexpected EOF in meta.ops");
        free(key);
        return false;
      }
      if (ch != '}') {
        err(ctx->in_path, "meta.ops must be {} (commit operators upstream)");
        free(key);
        return false;
      }
      c->p++; // consume '}'
    } else {
      if (!grit_json_skip_value(c)) {
        err(ctx->in_path, "invalid meta value");
        free(key);
        return false;
      }
    }

    free(key);
    if (!json_peek_non_ws(c, &ch)) {
      err(ctx->in_path, "unexpected EOF in meta");
      return false;
    }
    if (ch == ',') {
      c->p++;
      continue;
    }
    if (ch == '}') {
      c->p++;
      return true;
    }
    err(ctx->in_path, "expected ',' or '}' in meta");
    return false;
  }
}

bool prescan_root_for_meta_defaults(const char *buf, size_t len, EmitCtx *ctx) {
  GritJsonCursor c = grit_json_cursor(buf, len);
  if (!grit_json_skip_ws(&c))
    return false;
  if (!grit_json_consume_char(&c, '{'))
    return false;

  char ch = 0;
  if (!json_peek_non_ws(&c, &ch))
    return false;
  if (ch == '}') {
    c.p++;
    return true;
  }

  for (;;) {
    if (!json_peek_non_ws(&c, &ch))
      return false;
    if (ch == '}') {
      c.p++;
      return true;
    }

    char *key = NULL;
    if (!json_expect_key(&c, &key))
      return false;

    if (strcmp(key, "meta") == 0) {
      bool ok = parse_meta_for_defaults(&c, ctx);
      free(key);
      if (!ok)
        return false;
    } else {
      if (!grit_json_skip_value(&c)) {
        free(key);
        return false;
      }
      free(key);
    }

    if (!json_peek_non_ws(&c, &ch))
      return false;
    if (ch == ',') {
      c.p++;
      continue;
    }
    if (ch == '}') {
      c.p++;
      return true;
    }
    return false;
  }
}

