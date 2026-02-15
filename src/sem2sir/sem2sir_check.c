#include "sem2sir_check.h"

#include "json_min.h"

#include "sem2sir_profile.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_ident_like_type_name(const char *s) {
  if (!s || !s[0])
    return false;
  unsigned char c0 = (unsigned char)s[0];
  if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_'))
    return false;
  for (size_t i = 1; s[i]; i++) {
    unsigned char c = (unsigned char)s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')
      continue;
    return false;
  }
  return true;
}

static char *read_file(const char *path, size_t *len_out) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;

  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }

  long n = ftell(f);
  if (n < 0) {
    fclose(f);
    return NULL;
  }

  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }

  char *buf = (char *)malloc((size_t)n);
  if (!buf) {
    fclose(f);
    return NULL;
  }

  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  if (got != (size_t)n) {
    free(buf);
    return NULL;
  }

  if (len_out) *len_out = (size_t)n;
  return buf;
}

static void print_err_at(const char *path, const GritJsonCursor *c, const char *msg) {
  (void)c;
  fprintf(stderr, "sem2sir: %s: %s\n", path ? path : "<input>", msg ? msg : "error");
}

static bool json_peek_non_ws(GritJsonCursor *c, char *out) {
  if (!grit_json_skip_ws(c)) return false;
  if (c->p >= c->end) return false;
  if (out) *out = *c->p;
  return true;
}

static bool json_expect_key(GritJsonCursor *c, char **out_key) {
  if (!grit_json_parse_string_alloc(c, out_key)) return false;
  if (!grit_json_consume_char(c, ':')) {
    free(*out_key);
    *out_key = NULL;
    return false;
  }
  return true;
}

static bool validate_value(GritJsonCursor *c, const char *path, const char *ctx_msg);
static bool is_allowed_tok_key(const char *key);

static bool parse_tok_text_alloc_strict(GritJsonCursor *c, const char *path, char **out_text) {
  *out_text = NULL;
  if (!grit_json_consume_char(c, '{')) {
    print_err_at(path, c, "expected token object");
    return false;
  }

  // Require k first.
  char *key = NULL;
  if (!json_expect_key(c, &key)) {
    print_err_at(path, c, "invalid token object key");
    return false;
  }
  if (strcmp(key, "k") != 0) {
    print_err_at(path, c, "token object must start with key 'k'");
    free(key);
    return false;
  }
  free(key);

  char *k_str = NULL;
  if (!grit_json_parse_string_alloc(c, &k_str)) {
    print_err_at(path, c, "token field k must be string");
    return false;
  }
  bool ok = true;
  if (strcmp(k_str, "tok") != 0) {
    print_err_at(path, c, "expected k='tok' for token leaf");
    ok = false;
  }
  free(k_str);
  if (!ok) return false;

  bool seen_text = false;

  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      print_err_at(path, c, "unexpected EOF in token object");
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      print_err_at(path, c, "expected ',' or '}' in token object");
      return false;
    }
    c->p++;

    char *tkey = NULL;
    if (!json_expect_key(c, &tkey)) {
      print_err_at(path, c, "invalid token object key");
      return false;
    }

    if (!is_allowed_tok_key(tkey)) {
      char msg[256];
      snprintf(msg, sizeof(msg), "field '%s' is not allowed on k='tok'", tkey);
      print_err_at(path, c, msg);
      free(tkey);
      return false;
    }

    if (strcmp(tkey, "text") == 0) {
      seen_text = true;
      if (!grit_json_parse_string_alloc(c, out_text)) {
        print_err_at(path, c, "tok.text must be a string");
        free(tkey);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        print_err_at(path, c, "invalid token value");
        free(tkey);
        return false;
      }
    }

    free(tkey);
  }

  if (!seen_text) {
    print_err_at(path, c, "tok requires field: text");
    free(*out_text);
    *out_text = NULL;
    return false;
  }
  return true;
}

static bool is_allowed_tok_key(const char *key) {
  return strcmp(key, "k") == 0 || strcmp(key, "nid") == 0 || strcmp(key, "i") == 0 || strcmp(key, "kind") == 0 ||
         strcmp(key, "start_byte") == 0 || strcmp(key, "end_byte") == 0 || strcmp(key, "text") == 0;
}

static bool is_allowed_node_key(sem2sir_intrinsic_id kid, const char *key) {
  if (strcmp(key, "k") == 0 || strcmp(key, "nid") == 0 || strcmp(key, "span") == 0) return true;

  switch (kid) {
  case SEM2SIR_INTRINSIC_Unit:
    return strcmp(key, "name") == 0 || strcmp(key, "items") == 0;
  case SEM2SIR_INTRINSIC_Proc:
    return strcmp(key, "name") == 0 || strcmp(key, "params") == 0 || strcmp(key, "ret") == 0 ||
           strcmp(key, "decls") == 0 || strcmp(key, "body") == 0 || strcmp(key, "extern") == 0 ||
           strcmp(key, "link_name") == 0;
  case SEM2SIR_INTRINSIC_Block:
    return strcmp(key, "items") == 0;
  case SEM2SIR_INTRINSIC_Var:
    return strcmp(key, "name") == 0 || strcmp(key, "type") == 0 || strcmp(key, "init") == 0;
  case SEM2SIR_INTRINSIC_VarPat:
    return strcmp(key, "pat") == 0 || strcmp(key, "type") == 0 || strcmp(key, "init") == 0;
  case SEM2SIR_INTRINSIC_ExprStmt:
    return strcmp(key, "expr") == 0;
  case SEM2SIR_INTRINSIC_Return:
    return strcmp(key, "value") == 0;
  case SEM2SIR_INTRINSIC_If:
    return strcmp(key, "cond") == 0 || strcmp(key, "then") == 0 || strcmp(key, "else") == 0;
  case SEM2SIR_INTRINSIC_While:
    return strcmp(key, "cond") == 0 || strcmp(key, "body") == 0;
  case SEM2SIR_INTRINSIC_Param:
    return strcmp(key, "name") == 0 || strcmp(key, "type") == 0 || strcmp(key, "mode") == 0;
  case SEM2SIR_INTRINSIC_ParamPat:
    return strcmp(key, "pat") == 0 || strcmp(key, "type") == 0 || strcmp(key, "mode") == 0;
  case SEM2SIR_INTRINSIC_Call:
    return strcmp(key, "callee") == 0 || strcmp(key, "args") == 0;
  case SEM2SIR_INTRINSIC_Args:
    return strcmp(key, "items") == 0;
  case SEM2SIR_INTRINSIC_PatBind:
    return strcmp(key, "name") == 0;
  case SEM2SIR_INTRINSIC_PatInt:
    return strcmp(key, "lit") == 0;
  case SEM2SIR_INTRINSIC_PatWild:
    return false; // no additional fields
  case SEM2SIR_INTRINSIC_Name:
    return strcmp(key, "id") == 0;
  case SEM2SIR_INTRINSIC_TypeRef:
    return strcmp(key, "name") == 0;
  case SEM2SIR_INTRINSIC_Int:
    return strcmp(key, "lit") == 0;
  case SEM2SIR_INTRINSIC_True:
  case SEM2SIR_INTRINSIC_False:
  case SEM2SIR_INTRINSIC_Nil:
    return false; // no additional fields
  case SEM2SIR_INTRINSIC_Paren:
    return strcmp(key, "expr") == 0;
  case SEM2SIR_INTRINSIC_Not:
  case SEM2SIR_INTRINSIC_Neg:
  case SEM2SIR_INTRINSIC_BitNot:
  case SEM2SIR_INTRINSIC_AddrOf:
  case SEM2SIR_INTRINSIC_Deref:
    return strcmp(key, "expr") == 0;
  case SEM2SIR_INTRINSIC_Bin:
    return strcmp(key, "op") == 0 || strcmp(key, "op_tok") == 0 || strcmp(key, "lhs") == 0 || strcmp(key, "rhs") == 0;
  case SEM2SIR_INTRINSIC_Match:
    return strcmp(key, "cond") == 0 || strcmp(key, "arms") == 0;
  case SEM2SIR_INTRINSIC_MatchArm:
    return strcmp(key, "pat") == 0 || strcmp(key, "guard") == 0 || strcmp(key, "body") == 0;
  default:
    return false;
  }
}

static bool validate_array(GritJsonCursor *c, const char *path) {
  if (!grit_json_consume_char(c, '[')) {
    print_err_at(path, c, "expected '['");
    return false;
  }

  char ch = 0;
  if (!json_peek_non_ws(c, &ch)) {
    print_err_at(path, c, "unexpected EOF in array");
    return false;
  }
  if (ch == ']') {
    c->p++;
    return true;
  }

  for (;;) {
    if (!validate_value(c, path, "array item")) return false;
    if (!json_peek_non_ws(c, &ch)) {
      print_err_at(path, c, "unexpected EOF in array");
      return false;
    }
    if (ch == ',') {
      c->p++;
      continue;
    }
    if (ch == ']') {
      c->p++;
      return true;
    }
    print_err_at(path, c, "expected ',' or ']' in array");
    return false;
  }
}

static bool validate_object(GritJsonCursor *c, const char *path) {
  if (!grit_json_consume_char(c, '{')) {
    print_err_at(path, c, "expected '{'");
    return false;
  }

  char ch = 0;
  if (!json_peek_non_ws(c, &ch)) {
    print_err_at(path, c, "unexpected EOF in object");
    return false;
  }
  if (ch == '}') {
    c->p++;
    return true;
  }

  // Closed-world rule: if this is an AST node object, it must begin with `k`.
  // Other auxiliary objects (e.g. spans, meta, symbol tables) are validated structurally.
  char *first_key = NULL;
  if (!json_expect_key(c, &first_key)) {
    print_err_at(path, c, "invalid object key");
    return false;
  }

  bool is_ast_node = (strcmp(first_key, "k") == 0);
  if (!is_ast_node) {
    // Generic object: validate first value, then rest as generic values.
    if (!validate_value(c, path, first_key)) {
      free(first_key);
      return false;
    }
    free(first_key);
  } else {
    // AST node object.
    char *k_str = NULL;
    if (!grit_json_parse_string_alloc(c, &k_str)) {
      print_err_at(path, c, "expected string for field 'k'");
      free(first_key);
      return false;
    }
    free(first_key);

    sem2sir_intrinsic_id kid = SEM2SIR_INTRINSIC_INVALID;
    bool is_tok = (strcmp(k_str, "tok") == 0);
    if (!is_tok) {
      kid = sem2sir_intrinsic_parse(k_str, strlen(k_str));
      if (kid == SEM2SIR_INTRINSIC_INVALID) {
        char msg[256];
        snprintf(msg, sizeof(msg), "unknown intrinsic constructor k='%s'", k_str);
        print_err_at(path, c, msg);
        free(k_str);
        return false;
      }
    }

    bool seen_op = false;
    bool seen_lhs = false;
    bool seen_rhs = false;
    bool seen_items = false;
    bool seen_body = false;
    bool seen_cond = false;
    bool seen_then = false;
    bool seen_id = false;
    bool seen_lit = false;
    bool seen_name = false;
    bool seen_expr = false;
    bool seen_callee = false;
    bool seen_args = false;
    bool seen_type = false;
    bool seen_pat = false;
    bool seen_init = false;
    bool seen_arms = false;

    for (;;) {
      if (!json_peek_non_ws(c, &ch)) {
        print_err_at(path, c, "unexpected EOF in object");
        free(k_str);
        return false;
      }
      if (ch == '}') {
        c->p++;
        break;
      }
      if (ch != ',') {
        print_err_at(path, c, "expected ',' or '}' in object");
        free(k_str);
        return false;
      }
      c->p++;

      char *key = NULL;
      if (!json_expect_key(c, &key)) {
        print_err_at(path, c, "invalid object key");
        free(k_str);
        return false;
      }

      bool allowed = is_tok ? is_allowed_tok_key(key) : is_allowed_node_key(kid, key);
      if (!allowed) {
        char msg[256];
        snprintf(msg, sizeof(msg), "field '%s' is not allowed on k='%s'", key, k_str);
        print_err_at(path, c, msg);
        free(key);
        free(k_str);
        return false;
      }

      if (!is_tok && kid == SEM2SIR_INTRINSIC_Bin && strcmp(key, "op") == 0) {
        seen_op = true;
        char *op_str = NULL;
        if (!grit_json_parse_string_alloc(c, &op_str)) {
          print_err_at(path, c, "expected string for field 'op'");
          free(key);
          free(k_str);
          return false;
        }
        sem2sir_op_id opid = sem2sir_op_parse(op_str, strlen(op_str));
        if (opid == SEM2SIR_OP_INVALID) {
          char msg[256];
          snprintf(msg, sizeof(msg), "unknown or non-normalized operator id '%s'", op_str);
          print_err_at(path, c, msg);
          free(op_str);
          free(key);
          free(k_str);
          return false;
        }
        free(op_str);
      } else {
        if (strcmp(key, "lhs") == 0) seen_lhs = true;
        if (strcmp(key, "rhs") == 0) seen_rhs = true;
        if (strcmp(key, "items") == 0) seen_items = true;
        if (strcmp(key, "body") == 0) seen_body = true;
        if (strcmp(key, "cond") == 0) seen_cond = true;
        if (strcmp(key, "then") == 0) seen_then = true;
        if (strcmp(key, "id") == 0) seen_id = true;
        if (strcmp(key, "lit") == 0) seen_lit = true;
        if (strcmp(key, "name") == 0) seen_name = true;
        if (strcmp(key, "expr") == 0) seen_expr = true;
        if (strcmp(key, "callee") == 0) seen_callee = true;
        if (strcmp(key, "args") == 0) seen_args = true;
        if (strcmp(key, "type") == 0) seen_type = true;
        if (strcmp(key, "pat") == 0) seen_pat = true;
        if (strcmp(key, "init") == 0) seen_init = true;
        if (strcmp(key, "arms") == 0) seen_arms = true;

        if (!is_tok && kid == SEM2SIR_INTRINSIC_TypeRef && strcmp(key, "name") == 0) {
          // TypeRef.name must be a tok leaf. If it's a known physical sem2sir type,
          // require it to be normalized; otherwise allow user-defined/nominal names.
          char *type_text = NULL;
          if (!parse_tok_text_alloc_strict(c, path, &type_text)) {
            free(key);
            free(k_str);
            return false;
          }
          sem2sir_type_id tid = sem2sir_type_parse(type_text, strlen(type_text));
          if (tid == SEM2SIR_TYPE_INVALID && !is_ident_like_type_name(type_text)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "TypeRef.name must be identifier-like, got '%s'", type_text);
            print_err_at(path, c, msg);
            free(type_text);
            free(key);
            free(k_str);
            return false;
          }
          free(type_text);
          free(key);
          continue;
        }

        if (!validate_value(c, path, key)) {
          free(key);
          free(k_str);
          return false;
        }
      }

      free(key);
    }

    // Required fields per intrinsic kind.
    if (!is_tok) {
      if (kid == SEM2SIR_INTRINSIC_Call) {
        if (!seen_callee || !seen_args) {
          print_err_at(path, c, "Call requires fields: callee, args");
          free(k_str);
          return false;
        }
      }
      if (kid == SEM2SIR_INTRINSIC_Args) {
        if (!seen_items) {
          print_err_at(path, c, "Args requires field: items");
          free(k_str);
          return false;
        }
      }
      if (kid == SEM2SIR_INTRINSIC_Param) {
        // Param.name is tracked as seen_name, Param.type as seen_type.
        if (!seen_name || !seen_type) {
          print_err_at(path, c, "Param requires fields: name, type");
          free(k_str);
          return false;
        }
      }
      if (kid == SEM2SIR_INTRINSIC_ParamPat) {
        if (!seen_pat || !seen_type) {
          print_err_at(path, c, "ParamPat requires fields: pat, type");
          free(k_str);
          return false;
        }
      }
      if (kid == SEM2SIR_INTRINSIC_VarPat) {
        if (!seen_pat || !seen_init) {
          print_err_at(path, c, "VarPat requires fields: pat, init");
          free(k_str);
          return false;
        }
      }
      if (kid == SEM2SIR_INTRINSIC_PatBind) {
        if (!seen_name) {
          print_err_at(path, c, "PatBind requires field: name");
          free(k_str);
          return false;
        }
      }
      if (kid == SEM2SIR_INTRINSIC_PatInt) {
        if (!seen_lit) {
          print_err_at(path, c, "PatInt requires field: lit");
          free(k_str);
          return false;
        }
      }
      if (kid == SEM2SIR_INTRINSIC_Bin) {
        if (!seen_op || !seen_lhs || !seen_rhs) {
          print_err_at(path, c, "Bin requires fields: op, lhs, rhs");
          free(k_str);
          return false;
        }
      }
      if (kid == SEM2SIR_INTRINSIC_Match) {
        if (!seen_cond || !seen_arms) {
          print_err_at(path, c, "Match requires fields: cond, arms");
          free(k_str);
          return false;
        }
      }
      if (kid == SEM2SIR_INTRINSIC_Block || kid == SEM2SIR_INTRINSIC_Unit) {
        // Unit.items and Block.items are required (may be empty array).
        if (!seen_items) {
          print_err_at(path, c, "node requires field: items");
          free(k_str);
          return false;
        }
      }
      if (kid == SEM2SIR_INTRINSIC_Proc) {
        if (!seen_body) {
          print_err_at(path, c, "Proc requires field: body");
          free(k_str);
          return false;
        }
      }
      if (kid == SEM2SIR_INTRINSIC_If) {
        if (!seen_cond || !seen_then) {
          print_err_at(path, c, "If requires fields: cond, then");
          free(k_str);
          return false;
        }
      }
      if (kid == SEM2SIR_INTRINSIC_While) {
        if (!seen_cond || !seen_body) {
          print_err_at(path, c, "While requires fields: cond, body");
          free(k_str);
          return false;
        }
      }
      if (kid == SEM2SIR_INTRINSIC_Name && !seen_id) {
        print_err_at(path, c, "Name requires field: id");
        free(k_str);
        return false;
      }
      if (kid == SEM2SIR_INTRINSIC_TypeRef && !seen_name) {
        print_err_at(path, c, "TypeRef requires field: name");
        free(k_str);
        return false;
      }
      if (kid == SEM2SIR_INTRINSIC_Int && !seen_lit) {
        print_err_at(path, c, "Int requires field: lit");
        free(k_str);
        return false;
      }
      if ((kid == SEM2SIR_INTRINSIC_Paren || kid == SEM2SIR_INTRINSIC_Not || kid == SEM2SIR_INTRINSIC_Neg ||
           kid == SEM2SIR_INTRINSIC_BitNot || kid == SEM2SIR_INTRINSIC_AddrOf || kid == SEM2SIR_INTRINSIC_Deref) &&
          !seen_expr) {
        print_err_at(path, c, "node requires field: expr");
        free(k_str);
        return false;
      }
    }

    free(k_str);
  }

  // If we got here and haven't consumed the closing '}', consume it.
  // The AST-node path consumes it explicitly; the generic-object path should too.
  if (is_ast_node) return true;

  if (!json_peek_non_ws(c, &ch)) {
    print_err_at(path, c, "unexpected EOF in object");
    return false;
  }
  if (ch == '}') {
    c->p++;
    return true;
  }

  // Continue parsing remaining generic object fields.
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      print_err_at(path, c, "unexpected EOF in object");
      return false;
    }
    if (ch == '}') {
      c->p++;
      return true;
    }
    if (ch != ',') {
      print_err_at(path, c, "expected ',' or '}' in object");
      return false;
    }
    c->p++;

    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      print_err_at(path, c, "invalid object key");
      return false;
    }
    if (!validate_value(c, path, key)) {
      free(key);
      return false;
    }
    free(key);
  }
}

static bool validate_value(GritJsonCursor *c, const char *path, const char *ctx_msg) {
  (void)ctx_msg;
  char ch = 0;
  if (!json_peek_non_ws(c, &ch)) {
    print_err_at(path, c, "unexpected EOF");
    return false;
  }

  if (ch == '{') return validate_object(c, path);
  if (ch == '[') return validate_array(c, path);

  // Scalars: skip (strings/numbers/bool/null). Any semantic meaning must have
  // already been committed into normalized IDs (validated elsewhere).
  if (!grit_json_skip_value(c)) {
    print_err_at(path, c, "invalid JSON value");
    return false;
  }
  return true;
}

static bool check_empty_diagnostics(GritJsonCursor *c, const char *path) {
  if (!grit_json_consume_char(c, '[')) {
    print_err_at(path, c, "expected diagnostics array");
    return false;
  }
  char ch = 0;
  if (!json_peek_non_ws(c, &ch)) {
    print_err_at(path, c, "unexpected EOF in diagnostics");
    return false;
  }
  if (ch != ']') {
    print_err_at(path, c, "diagnostics must be [] (sem2sir is strict)");
    return false;
  }
  c->p++;
  return true;
}

static bool check_meta_types(GritJsonCursor *c, const char *path) {
  // Expect an object; require field "types": { <surface-name>: <normalized.type.id> }.
  // Reserved keys are allowed but must still map to normalized IDs.
  if (!grit_json_consume_char(c, '{')) {
    print_err_at(path, c, "expected meta object");
    return false;
  }

  char ch = 0;
  if (!json_peek_non_ws(c, &ch)) {
    print_err_at(path, c, "unexpected EOF in meta");
    return false;
  }
  if (ch == '}') {
    c->p++;
    return true;
  }

  bool seen_types = false;

  for (;;) {
    char *key = NULL;
    if (!json_expect_key(c, &key)) {
      print_err_at(path, c, "invalid meta key");
      return false;
    }

    if (strcmp(key, "types") == 0) {
      seen_types = true;
      // Parse { "TokKind": "normalized.type.id", ... }
      if (!grit_json_consume_char(c, '{')) {
        print_err_at(path, c, "meta.types must be an object");
        free(key);
        return false;
      }

      if (!json_peek_non_ws(c, &ch)) {
        print_err_at(path, c, "unexpected EOF in meta.types");
        free(key);
        return false;
      }

      if (ch != '}') {
        for (;;) {
          char *tkey = NULL;
          if (!json_expect_key(c, &tkey)) {
            print_err_at(path, c, "invalid meta.types key");
            free(key);
            return false;
          }

          char *tval = NULL;
          if (!grit_json_parse_string_alloc(c, &tval)) {
            print_err_at(path, c, "meta.types values must be strings");
            free(tkey);
            free(key);
            return false;
          }

          sem2sir_type_id tid = sem2sir_type_parse(tval, strlen(tval));
          if (tid == SEM2SIR_TYPE_INVALID) {
            char msg[256];
            snprintf(msg, sizeof(msg), "unknown or non-normalized type id '%s' in meta.types", tval);
            print_err_at(path, c, msg);
            free(tval);
            free(tkey);
            free(key);
            return false;
          }

          // Optional: explicit literal default policy, committed in metadata.
          // This is not inference: it is an explicit language rule.
          if (strcmp(tkey, "@default.int") == 0 || strcmp(tkey, "__default_int") == 0) {
            switch (tid) {
            case SEM2SIR_TYPE_I32:
            case SEM2SIR_TYPE_I64:
              break;
            default:
              print_err_at(path, c,
                           "meta.types['@default.int'/'__default_int'] must be 'i32' or 'i64' in sem2sir MVP");
              free(tval);
              free(tkey);
              free(key);
              return false;
            }
          }

          // Optional: explicit raw-pointer default pointee policy.
          // This is not inference: it is an explicit language rule.
          if (strcmp(tkey, "@default.ptr.pointee") == 0 || strcmp(tkey, "__default_ptr_pointee") == 0) {
            switch (tid) {
            case SEM2SIR_TYPE_I32:
            case SEM2SIR_TYPE_I64:
            case SEM2SIR_TYPE_U8:
            case SEM2SIR_TYPE_F64:
              break;
            default:
              print_err_at(path, c,
                           "meta.types['@default.ptr.pointee'/'__default_ptr_pointee'] must be a load/store-capable non-ptr value type in sem2sir MVP");
              free(tval);
              free(tkey);
              free(key);
              return false;
            }
          }
          free(tval);
          free(tkey);

          if (!json_peek_non_ws(c, &ch)) {
            print_err_at(path, c, "unexpected EOF in meta.types");
            free(key);
            return false;
          }
          if (ch == ',') {
            c->p++;
            continue;
          }
          if (ch == '}') break;
          print_err_at(path, c, "expected ',' or '}' in meta.types");
          free(key);
          return false;
        }
      }

      if (!grit_json_consume_char(c, '}')) {
        print_err_at(path, c, "expected '}' to close meta.types");
        free(key);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        print_err_at(path, c, "invalid meta value");
        free(key);
        return false;
      }
    }

    free(key);
    if (!json_peek_non_ws(c, &ch)) {
      print_err_at(path, c, "unexpected EOF in meta");
      return false;
    }
    if (ch == ',') {
      c->p++;
      continue;
    }
    if (ch == '}') {
      c->p++;
      if (!seen_types) {
        print_err_at(path, c, "meta.types is required (no implicitness)");
        return false;
      }
      return true;
    }
    print_err_at(path, c, "expected ',' or '}' in meta");
    return false;
  }
}

int sem2sir_check_stage4_file(const char *path) {
  size_t len = 0;
  char *buf = read_file(path, &len);
  if (!buf) {
    fprintf(stderr, "sem2sir: %s: failed to read file\n", path ? path : "<input>");
    return 1;
  }

  GritJsonCursor c = grit_json_cursor(buf, len);
  bool ok = true;

  if (!grit_json_consume_char(&c, '{')) {
    print_err_at(path, &c, "expected root object" );
    ok = false;
    goto done;
  }

  char ch = 0;
  if (!json_peek_non_ws(&c, &ch)) {
    print_err_at(path, &c, "unexpected EOF in root" );
    ok = false;
    goto done;
  }
  if (ch == '}') {
    print_err_at(path, &c, "root object missing required fields" );
    ok = false;
    goto done;
  }

  bool seen_ast = false;
  bool seen_diagnostics = false;
  bool seen_meta = false;

  for (;;) {
    char *key = NULL;
    if (!json_expect_key(&c, &key)) {
      print_err_at(path, &c, "invalid root key" );
      ok = false;
      goto done;
    }

    if (strcmp(key, "diagnostics") == 0) {
      seen_diagnostics = true;
      if (!check_empty_diagnostics(&c, path)) {
        free(key);
        ok = false;
        goto done;
      }
    } else if (strcmp(key, "meta") == 0) {
      seen_meta = true;
      if (!check_meta_types(&c, path)) {
        free(key);
        ok = false;
        goto done;
      }
    } else if (strcmp(key, "ast") == 0) {
      seen_ast = true;
      if (!validate_value(&c, path, "ast")) {
        free(key);
        ok = false;
        goto done;
      }
    } else if (strcmp(key, "symbols") == 0 || strcmp(key, "symtab") == 0 || strcmp(key, "sym_by_tok_i") == 0 ||
               strcmp(key, "tokens") == 0) {
      // We don't depend on symbols/tokens yet, but they are part of the Stage 4 boundary.
      if (!validate_value(&c, path, key)) {
        free(key);
        ok = false;
        goto done;
      }
    } else {
      // Closed root: unknown key is an error.
      char msg[256];
      snprintf(msg, sizeof(msg), "unknown root field '%s'", key);
      print_err_at(path, &c, msg);
      free(key);
      ok = false;
      goto done;
    }

    free(key);
    if (!json_peek_non_ws(&c, &ch)) {
      print_err_at(path, &c, "unexpected EOF in root" );
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
    print_err_at(path, &c, "expected ',' or '}' in root" );
    ok = false;
    goto done;
  }

  if (!seen_diagnostics) {
    print_err_at(path, &c, "missing required field diagnostics" );
    ok = false;
    goto done;
  }
  if (!seen_meta) {
    print_err_at(path, &c, "missing required field meta" );
    ok = false;
    goto done;
  }
  if (!seen_ast) {
    print_err_at(path, &c, "missing required field ast" );
    ok = false;
    goto done;
  }

  grit_json_skip_ws(&c);
  if (c.p != c.end) {
    print_err_at(path, &c, "trailing garbage after root JSON" );
    ok = false;
    goto done;
  }

done:
  free(buf);
  return ok ? 0 : 1;
}
