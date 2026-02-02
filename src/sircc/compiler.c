// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler.h"
#include "json.h"
#include "sircc.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum TypeKind {
  TYPE_INVALID = 0,
  TYPE_PRIM,
  TYPE_PTR,
  TYPE_FN,
} TypeKind;

typedef struct SrcRec {
  int64_t id;
  const char* file;
  int64_t line;
  int64_t col;
  int64_t end_line;
  int64_t end_col;
  const char* text;
} SrcRec;

typedef struct LocRec {
  const char* unit;
  int64_t line;
  int64_t col;
} LocRec;

typedef struct SymRec {
  int64_t id;
  const char* name;
  const char* kind;
  const char* linkage;
} SymRec;

typedef struct TypeRec {
  int64_t id;
  TypeKind kind;

  const char* prim;
  int64_t of;

  int64_t* params;
  size_t param_len;
  int64_t ret;
  bool varargs;

  LLVMTypeRef llvm;
  bool resolving;
} TypeRec;

typedef struct NodeRec {
  int64_t id;
  const char* tag;
  int64_t type_ref;  // 0 means absent
  JsonValue* fields; // JSON object (or NULL)

  LLVMValueRef llvm_value; // cached when lowered (expressions); for fn nodes this is the LLVM function
  bool resolving;
} NodeRec;

typedef struct SirProgram {
  Arena arena;

  const char* cur_path;
  size_t cur_line;
  const char* cur_kind;
  int64_t cur_src_ref;
  LocRec cur_loc;

  const char* unit_name;
  const char* target_triple;

  bool feat_atomics_v1;
  bool feat_simd_v1;
  bool feat_adt_v1;
  bool feat_fun_v1;
  bool feat_closure_v1;
  bool feat_coro_v1;
  bool feat_eh_v1;
  bool feat_gc_v1;
  bool feat_sem_v1;

  SrcRec** srcs;
  size_t srcs_cap;

  SymRec** syms;
  size_t syms_cap;

  TypeRec** types;
  size_t types_cap;

  NodeRec** nodes;
  size_t nodes_cap;
} SirProgram;

static void errf(SirProgram* p, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (p) {
    const char* file = NULL;
    int64_t line = 0;
    int64_t col = 0;

    if (p->cur_loc.line > 0) {
      file = p->cur_loc.unit ? p->cur_loc.unit : p->cur_path;
      line = p->cur_loc.line;
      col = p->cur_loc.col;
    } else if (p->cur_src_ref >= 0 && (size_t)p->cur_src_ref < p->srcs_cap) {
      SrcRec* sr = p->srcs[p->cur_src_ref];
      if (sr) {
        file = sr->file ? sr->file : p->cur_path;
        line = sr->line;
        col = sr->col;
      }
    } else if (p->cur_path && p->cur_line) {
      file = p->cur_path;
      line = (int64_t)p->cur_line;
    }

    if (file && line > 0) {
      if (col > 0) fprintf(stderr, "%s:%lld:%lld: ", file, (long long)line, (long long)col);
      else fprintf(stderr, "%s:%lld: ", file, (long long)line);
    }
  }
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

static bool ensure_src_slot(SirProgram* p, int64_t id) {
  if (id < 0) return false;
  size_t want = (size_t)id + 1;
  if (want <= p->srcs_cap) return true;
  size_t new_cap = p->srcs_cap ? p->srcs_cap * 2 : 64;
  while (new_cap < want) new_cap *= 2;
  SrcRec** next = (SrcRec**)realloc(p->srcs, new_cap * sizeof(SrcRec*));
  if (!next) return false;
  memset(next + p->srcs_cap, 0, (new_cap - p->srcs_cap) * sizeof(SrcRec*));
  p->srcs = next;
  p->srcs_cap = new_cap;
  return true;
}

static bool ensure_sym_slot(SirProgram* p, int64_t id) {
  if (id < 0) return false;
  size_t want = (size_t)id + 1;
  if (want <= p->syms_cap) return true;
  size_t new_cap = p->syms_cap ? p->syms_cap * 2 : 64;
  while (new_cap < want) new_cap *= 2;
  SymRec** next = (SymRec**)realloc(p->syms, new_cap * sizeof(SymRec*));
  if (!next) return false;
  memset(next + p->syms_cap, 0, (new_cap - p->syms_cap) * sizeof(SymRec*));
  p->syms = next;
  p->syms_cap = new_cap;
  return true;
}

static bool ensure_type_slot(SirProgram* p, int64_t id) {
  if (id < 0) return false;
  size_t want = (size_t)id + 1;
  if (want <= p->types_cap) return true;
  size_t new_cap = p->types_cap ? p->types_cap * 2 : 64;
  while (new_cap < want) new_cap *= 2;
  TypeRec** next = (TypeRec**)realloc(p->types, new_cap * sizeof(TypeRec*));
  if (!next) return false;
  memset(next + p->types_cap, 0, (new_cap - p->types_cap) * sizeof(TypeRec*));
  p->types = next;
  p->types_cap = new_cap;
  return true;
}

static bool ensure_node_slot(SirProgram* p, int64_t id) {
  if (id < 0) return false;
  size_t want = (size_t)id + 1;
  if (want <= p->nodes_cap) return true;
  size_t new_cap = p->nodes_cap ? p->nodes_cap * 2 : 128;
  while (new_cap < want) new_cap *= 2;
  NodeRec** next = (NodeRec**)realloc(p->nodes, new_cap * sizeof(NodeRec*));
  if (!next) return false;
  memset(next + p->nodes_cap, 0, (new_cap - p->nodes_cap) * sizeof(NodeRec*));
  p->nodes = next;
  p->nodes_cap = new_cap;
  return true;
}

static JsonValue* must_obj(SirProgram* p, JsonValue* v, const char* ctx) {
  if (!v || v->type != JSON_OBJECT) {
    errf(p, "sircc: expected object for %s", ctx);
    return NULL;
  }
  return v;
}

static const char* must_string(SirProgram* p, JsonValue* v, const char* ctx) {
  const char* s = json_get_string(v);
  if (!s) errf(p, "sircc: expected string for %s", ctx);
  return s;
}

static bool must_i64(SirProgram* p, JsonValue* v, int64_t* out, const char* ctx) {
  if (!json_get_i64(v, out)) {
    errf(p, "sircc: expected integer for %s", ctx);
    return false;
  }
  return true;
}

static bool parse_node_ref_id(const JsonValue* v, int64_t* out_id) {
  if (!v || v->type != JSON_OBJECT) return false;
  JsonValue* t = json_obj_get(v, "t");
  const char* ts = json_get_string(t);
  if (!ts || strcmp(ts, "ref") != 0) return false;
  JsonValue* idv = json_obj_get(v, "id");
  return json_get_i64(idv, out_id);
}

static bool is_ident(const char* s) {
  if (!s || !*s) return false;
  unsigned char c0 = (unsigned char)s[0];
  bool ok0 = (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_' || c0 == '.' || c0 == '$';
  if (!ok0) return false;
  for (size_t i = 1; s[i]; i++) {
    unsigned char c = (unsigned char)s[i];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
              c == '$';
    if (!ok) return false;
  }
  return true;
}

static bool validate_value(SirProgram* p, const JsonValue* v, const char* what) {
  if (!v || v->type != JSON_OBJECT) {
    errf(p, "sircc: %s must be an object value", what);
    return false;
  }
  const char* t = json_get_string(json_obj_get(v, "t"));
  if (!t) {
    errf(p, "sircc: %s missing string field 't'", what);
    return false;
  }

  if (strcmp(t, "sym") == 0 || strcmp(t, "lbl") == 0 || strcmp(t, "reg") == 0) {
    const char* name = json_get_string(json_obj_get(v, "v"));
    if (!name || !is_ident(name)) {
      errf(p, "sircc: %s %s.v must be an Ident", what, t);
      return false;
    }
    return true;
  }
  if (strcmp(t, "num") == 0) {
    int64_t n = 0;
    if (!json_get_i64(json_obj_get(v, "v"), &n)) {
      errf(p, "sircc: %s num.v must be an integer", what);
      return false;
    }
    return true;
  }
  if (strcmp(t, "str") == 0) {
    const char* s = json_get_string(json_obj_get(v, "v"));
    if (!s) {
      errf(p, "sircc: %s str.v must be a string", what);
      return false;
    }
    return true;
  }
  if (strcmp(t, "mem") == 0) {
    const JsonValue* base = json_obj_get(v, "base");
    if (!base || base->type != JSON_OBJECT) {
      errf(p, "sircc: %s mem.base must be an object", what);
      return false;
    }
    const char* bt = json_get_string(json_obj_get(base, "t"));
    if (!bt || (strcmp(bt, "reg") != 0 && strcmp(bt, "sym") != 0)) {
      errf(p, "sircc: %s mem.base must be reg or sym", what);
      return false;
    }
    if (!validate_value(p, base, what)) return false;

    JsonValue* disp = json_obj_get(v, "disp");
    if (disp) {
      int64_t d = 0;
      if (!json_get_i64(disp, &d)) {
        errf(p, "sircc: %s mem.disp must be an integer", what);
        return false;
      }
    }
    JsonValue* size = json_obj_get(v, "size");
    if (size) {
      int64_t s = 0;
      if (!json_get_i64(size, &s) || !(s == 1 || s == 2 || s == 4 || s == 8 || s == 16)) {
        errf(p, "sircc: %s mem.size must be one of 1,2,4,8,16", what);
        return false;
      }
    }
    return true;
  }
  if (strcmp(t, "ref") == 0) {
    int64_t id = 0;
    if (!json_get_i64(json_obj_get(v, "id"), &id)) {
      errf(p, "sircc: %s ref.id must be an integer", what);
      return false;
    }
    JsonValue* k = json_obj_get(v, "k");
    if (k) {
      const char* ks = json_get_string(k);
      if (!ks || (strcmp(ks, "sym") != 0 && strcmp(ks, "type") != 0 && strcmp(ks, "node") != 0)) {
        errf(p, "sircc: %s ref.k must be one of sym/type/node", what);
        return false;
      }
    }
    return true;
  }

  errf(p, "sircc: %s has unknown value tag t='%s'", what, t);
  return false;
}

static void enable_feature(SirProgram* p, const char* name) {
  if (!p || !name) return;
  if (strcmp(name, "atomics:v1") == 0) p->feat_atomics_v1 = true;
  else if (strcmp(name, "simd:v1") == 0) p->feat_simd_v1 = true;
  else if (strcmp(name, "adt:v1") == 0) p->feat_adt_v1 = true;
  else if (strcmp(name, "fun:v1") == 0) p->feat_fun_v1 = true;
  else if (strcmp(name, "closure:v1") == 0) p->feat_closure_v1 = true;
  else if (strcmp(name, "coro:v1") == 0) p->feat_coro_v1 = true;
  else if (strcmp(name, "eh:v1") == 0) p->feat_eh_v1 = true;
  else if (strcmp(name, "gc:v1") == 0) p->feat_gc_v1 = true;
  else if (strcmp(name, "sem:v1") == 0) p->feat_sem_v1 = true;
}

static const char* required_feature_for_mnemonic(const char* m) {
  if (!m) return NULL;

  if (strncmp(m, "atomic.", 7) == 0) return "atomics:v1";
  if (strncmp(m, "vec.", 4) == 0) return "simd:v1";

  if (strncmp(m, "adt.", 4) == 0) return "adt:v1";

  if (strncmp(m, "fun.", 4) == 0) return "fun:v1";
  if (strcmp(m, "call.fun") == 0) return "fun:v1";

  if (strncmp(m, "closure.", 8) == 0) return "closure:v1";
  if (strcmp(m, "call.closure") == 0) return "closure:v1";

  if (strncmp(m, "coro.", 5) == 0) return "coro:v1";
  if (strcmp(m, "term.resume") == 0) return "coro:v1";

  if (strcmp(m, "term.invoke") == 0 || strcmp(m, "term.throw") == 0) return "eh:v1";

  if (strncmp(m, "gc.", 3) == 0) return "gc:v1";

  if (strncmp(m, "sem.", 4) == 0) return "sem:v1";

  return NULL;
}

static bool has_feature(SirProgram* p, const char* name) {
  if (!name) return true;
  if (strcmp(name, "atomics:v1") == 0) return p->feat_atomics_v1;
  if (strcmp(name, "simd:v1") == 0) return p->feat_simd_v1;
  if (strcmp(name, "adt:v1") == 0) return p->feat_adt_v1;
  if (strcmp(name, "fun:v1") == 0) return p->feat_fun_v1;
  if (strcmp(name, "closure:v1") == 0) return p->feat_closure_v1;
  if (strcmp(name, "coro:v1") == 0) return p->feat_coro_v1;
  if (strcmp(name, "eh:v1") == 0) return p->feat_eh_v1;
  if (strcmp(name, "gc:v1") == 0) return p->feat_gc_v1;
  if (strcmp(name, "sem:v1") == 0) return p->feat_sem_v1;
  return false;
}

static bool read_line(FILE* f, char** buf, size_t* cap, size_t* out_len) {
  if (!*buf || *cap == 0) {
    *cap = 4096;
    *buf = (char*)malloc(*cap);
    if (!*buf) return false;
  }
  (*buf)[0] = 0;

  size_t len = 0;
  while (fgets(*buf + len, (int)(*cap - len), f)) {
    len += strlen(*buf + len);
    if (len && (*buf)[len - 1] == '\n') break;
    if (*cap - len < 2) {
      size_t next = (*cap) * 2;
      char* bigger = (char*)realloc(*buf, next);
      if (!bigger) return false;
      *buf = bigger;
      *cap = next;
    }
  }

  if (len == 0 && feof(f)) return false;
  while (len && ((*buf)[len - 1] == '\n' || (*buf)[len - 1] == '\r')) {
    (*buf)[--len] = 0;
  }
  *out_len = len;
  return true;
}

static bool is_blank_line(const char* s) {
  for (; *s; s++) {
    if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') return false;
  }
  return true;
}

static bool require_only_keys(SirProgram* p, const JsonValue* obj, const char* const* keys, size_t key_count,
                              const char* what) {
  const char* bad = NULL;
  if (json_obj_has_only_keys(obj, keys, key_count, &bad)) return true;
  if (!bad) bad = "(unknown)";
  errf(p, "sircc: invalid %s: unknown field '%s'", what, bad);
  return false;
}

static bool parse_meta_record(SirProgram* p, const SirccOptions* opt, JsonValue* obj) {
  static const char* const keys[] = {"ir", "k", "producer", "ts", "unit", "id", "ext"};
  if (!require_only_keys(p, obj, keys, sizeof(keys) / sizeof(keys[0]), "meta record")) return false;

  JsonValue* unit = json_obj_get(obj, "unit");
  if (unit) p->unit_name = json_get_string(unit);

  JsonValue* ext = json_obj_get(obj, "ext");
  if (ext && ext->type == JSON_OBJECT) {
    // Convention (sircc-defined): ext.target.triple (string)
    JsonValue* target = json_obj_get(ext, "target");
    if (target && target->type == JSON_OBJECT) {
      const char* triple = json_get_string(json_obj_get(target, "triple"));
      if (triple) p->target_triple = triple;
    }

    // Convention (sircc-defined): ext.features (array of strings)
    JsonValue* features = json_obj_get(ext, "features");
    if (features && features->type == JSON_ARRAY) {
      for (size_t i = 0; i < features->v.arr.len; i++) {
        const char* f = json_get_string(features->v.arr.items[i]);
        if (!f) {
          errf(p, "sircc: meta.ext.features[%zu] must be a string", i);
          return false;
        }
        enable_feature(p, f);
      }
    }
  }

  return true;
}

static bool parse_src_record(SirProgram* p, JsonValue* obj) {
  static const char* const keys[] = {"ir", "k", "id", "file", "line", "col", "end_line", "end_col", "text"};
  if (!require_only_keys(p, obj, keys, sizeof(keys) / sizeof(keys[0]), "src record")) return false;

  int64_t id = 0;
  if (!must_i64(p, json_obj_get(obj, "id"), &id, "src.id")) return false;
  if (!ensure_src_slot(p, id)) return false;
  if (p->srcs[id]) {
    errf(p, "sircc: duplicate src id %lld", (long long)id);
    return false;
  }

  int64_t line = 0;
  if (!must_i64(p, json_obj_get(obj, "line"), &line, "src.line")) return false;

  SrcRec* sr = (SrcRec*)arena_alloc(&p->arena, sizeof(SrcRec));
  if (!sr) return false;
  sr->id = id;
  sr->file = json_get_string(json_obj_get(obj, "file"));
  sr->line = line;
  sr->col = 0;
  sr->end_line = 0;
  sr->end_col = 0;
  sr->text = json_get_string(json_obj_get(obj, "text"));

  JsonValue* col = json_obj_get(obj, "col");
  if (col) (void)must_i64(p, col, &sr->col, "src.col");
  JsonValue* end_line = json_obj_get(obj, "end_line");
  if (end_line) (void)must_i64(p, end_line, &sr->end_line, "src.end_line");
  JsonValue* end_col = json_obj_get(obj, "end_col");
  if (end_col) (void)must_i64(p, end_col, &sr->end_col, "src.end_col");

  if ((sr->end_line && !sr->end_col) || (sr->end_col && !sr->end_line)) {
    errf(p, "sircc: src record %lld must include both end_line and end_col (or neither)", (long long)id);
    return false;
  }

  p->srcs[id] = sr;
  return true;
}

static bool parse_diag_record(SirProgram* p, JsonValue* obj) {
  static const char* const keys[] = {"ir", "k", "level", "msg", "code", "notes", "help", "src_ref", "loc", "id", "about"};
  if (!require_only_keys(p, obj, keys, sizeof(keys) / sizeof(keys[0]), "diag record")) return false;
  // For now, treat producer-emitted diagnostics as informational input; sircc's own diagnostics are separate.
  (void)p;
  (void)obj;
  return true;
}

static bool parse_sym_record(SirProgram* p, JsonValue* obj) {
  static const char* const keys[] = {"ir", "k", "id", "name", "kind", "linkage", "type_ref", "value", "attrs", "src_ref",
                                     "loc"};
  if (!require_only_keys(p, obj, keys, sizeof(keys) / sizeof(keys[0]), "sym record")) return false;

  int64_t id = 0;
  if (!must_i64(p, json_obj_get(obj, "id"), &id, "sym.id")) return false;
  if (!ensure_sym_slot(p, id)) return false;
  if (p->syms[id]) {
    errf(p, "sircc: duplicate sym id %lld", (long long)id);
    return false;
  }

  SymRec* s = (SymRec*)arena_alloc(&p->arena, sizeof(SymRec));
  if (!s) return false;
  s->id = id;
  s->name = must_string(p, json_obj_get(obj, "name"), "sym.name");
  s->kind = must_string(p, json_obj_get(obj, "kind"), "sym.kind");
  s->linkage = json_get_string(json_obj_get(obj, "linkage"));
  if (!s->name || !s->kind) return false;
  if (!is_ident(s->name)) {
    errf(p, "sircc: sym.name must be an Ident");
    return false;
  }
  p->syms[id] = s;
  return true;
}

static bool parse_ext_record(SirProgram* p, JsonValue* obj) {
  static const char* const keys[] = {"ir", "k", "name", "about", "payload", "src_ref", "loc", "id"};
  if (!require_only_keys(p, obj, keys, sizeof(keys) / sizeof(keys[0]), "ext record")) return false;
  // Accept and ignore (ext is explicitly free-form).
  (void)p;
  (void)obj;
  return true;
}

static bool parse_label_record(SirProgram* p, JsonValue* obj) {
  static const char* const keys[] = {"ir", "k", "name", "loc", "id"};
  if (!require_only_keys(p, obj, keys, sizeof(keys) / sizeof(keys[0]), "label record")) return false;
  const char* name = must_string(p, json_obj_get(obj, "name"), "label.name");
  if (!name) return false;
  if (!is_ident(name)) {
    errf(p, "sircc: label.name must be an Ident");
    return false;
  }
  return true;
}

static bool parse_instr_record(SirProgram* p, const SirccOptions* opt, JsonValue* obj) {
  static const char* const keys[] = {"ir", "k", "m", "ops", "src_ref", "loc", "id"};
  if (!require_only_keys(p, obj, keys, sizeof(keys) / sizeof(keys[0]), "instr record")) return false;
  if (!must_string(p, json_obj_get(obj, "m"), "instr.m")) return false;
  JsonValue* ops = json_obj_get(obj, "ops");
  if (!ops || ops->type != JSON_ARRAY) {
    errf(p, "sircc: expected array for instr.ops");
    return false;
  }
  for (size_t i = 0; i < ops->v.arr.len; i++) {
    if (!validate_value(p, ops->v.arr.items[i], "instr operand")) return false;
  }
  const char* m = json_get_string(json_obj_get(obj, "m"));
  const char* need = required_feature_for_mnemonic(m);
  if (need && !has_feature(p, need)) {
    errf(p, "sircc: mnemonic '%s' requires feature %s (enable via meta.ext.features)", m ? m : "(null)", need);
    return false;
  }
  if (opt && opt->dump_records) {
    fprintf(stderr, "%s:%zu: instr %s (%zu ops)\n", p->cur_path, p->cur_line, m ? m : "(null)", ops->v.arr.len);
  }
  return true;
}

static bool parse_dir_record(SirProgram* p, JsonValue* obj) {
  static const char* const keys[] = {"ir", "k", "d", "name", "args", "section", "sig", "src_ref", "loc", "id"};
  if (!require_only_keys(p, obj, keys, sizeof(keys) / sizeof(keys[0]), "dir record")) return false;
  if (!must_string(p, json_obj_get(obj, "d"), "dir.d")) return false;
  JsonValue* args = json_obj_get(obj, "args");
  if (!args || args->type != JSON_ARRAY) {
    errf(p, "sircc: expected array for dir.args");
    return false;
  }
  for (size_t i = 0; i < args->v.arr.len; i++) {
    if (!validate_value(p, args->v.arr.items[i], "dir arg")) return false;
  }
  return true;
}

static bool parse_type_record(SirProgram* p, JsonValue* obj) {
  static const char* const keys[] = {"ir",   "k",      "id",     "kind",   "name",   "prim",  "of",      "len",
                                     "params", "ret",    "varargs", "fields", "variants", "attrs", "src_ref", "loc"};
  if (!require_only_keys(p, obj, keys, sizeof(keys) / sizeof(keys[0]), "type record")) return false;

  int64_t id = 0;
  if (!must_i64(p, json_obj_get(obj, "id"), &id, "type.id")) return false;
  const char* kind = must_string(p, json_obj_get(obj, "kind"), "type.kind");
  if (!kind) return false;
  if (!ensure_type_slot(p, id)) return false;
  if (p->types[id]) {
    errf(p, "sircc: duplicate type id %lld", (long long)id);
    return false;
  }

  TypeRec* tr = (TypeRec*)arena_alloc(&p->arena, sizeof(TypeRec));
  if (!tr) return false;
  tr->id = id;
  tr->kind = TYPE_INVALID;
  tr->of = 0;
  tr->ret = 0;
  tr->params = NULL;
  tr->param_len = 0;
  tr->varargs = false;

  if (strcmp(kind, "prim") == 0) {
    tr->kind = TYPE_PRIM;
    tr->prim = must_string(p, json_obj_get(obj, "prim"), "type.prim");
    if (!tr->prim) return false;
  } else if (strcmp(kind, "ptr") == 0) {
    tr->kind = TYPE_PTR;
    if (!must_i64(p, json_obj_get(obj, "of"), &tr->of, "type.of")) return false;
  } else if (strcmp(kind, "fn") == 0) {
    tr->kind = TYPE_FN;
    JsonValue* params = json_obj_get(obj, "params");
    if (!params || params->type != JSON_ARRAY) {
      fatalf("sircc: expected array for type.params");
      return false;
    }
    tr->param_len = params->v.arr.len;
    tr->params = (int64_t*)arena_alloc(&p->arena, tr->param_len * sizeof(int64_t));
    if (!tr->params) return false;
    for (size_t i = 0; i < tr->param_len; i++) {
      int64_t pid = 0;
      if (!must_i64(p, params->v.arr.items[i], &pid, "type.params[i]")) return false;
      tr->params[i] = pid;
    }
    if (!must_i64(p, json_obj_get(obj, "ret"), &tr->ret, "type.ret")) return false;
    JsonValue* va = json_obj_get(obj, "varargs");
    if (va && va->type == JSON_BOOL) tr->varargs = va->v.b;
  } else {
    errf(p, "sircc: unsupported type kind '%s' (v1 subset)", kind);
    return false;
  }

  p->types[id] = tr;
  return true;
}

static bool parse_node_record(SirProgram* p, JsonValue* obj) {
  static const char* const keys[] = {"ir", "k", "id", "tag", "type_ref", "inputs", "fields", "src_ref", "loc"};
  if (!require_only_keys(p, obj, keys, sizeof(keys) / sizeof(keys[0]), "node record")) return false;

  int64_t id = 0;
  if (!must_i64(p, json_obj_get(obj, "id"), &id, "node.id")) return false;
  const char* tag = must_string(p, json_obj_get(obj, "tag"), "node.tag");
  if (!tag) return false;

  int64_t type_ref = 0;
  JsonValue* tr = json_obj_get(obj, "type_ref");
  if (tr) {
    if (!must_i64(p, tr, &type_ref, "node.type_ref")) return false;
  }

  JsonValue* fields = json_obj_get(obj, "fields");
  if (fields && fields->type != JSON_OBJECT) {
    fatalf("sircc: expected object for node.fields");
    return false;
  }

  if (!ensure_node_slot(p, id)) return false;
  if (p->nodes[id]) {
    errf(p, "sircc: duplicate node id %lld", (long long)id);
    return false;
  }

  NodeRec* nr = (NodeRec*)arena_alloc(&p->arena, sizeof(NodeRec));
  if (!nr) return false;
  nr->id = id;
  nr->tag = tag;
  nr->type_ref = type_ref;
  nr->fields = fields;
  nr->llvm_value = NULL;
  nr->resolving = false;

  p->nodes[id] = nr;
  return true;
}

static bool parse_program(SirProgram* p, const SirccOptions* opt, const char* input_path) {
  FILE* f = fopen(input_path, "rb");
  if (!f) {
    errf(NULL, "sircc: failed to open %s: %s", input_path, strerror(errno));
    return false;
  }

  char* line = NULL;
  size_t cap = 0;
  size_t len = 0;
  size_t line_no = 0;

  while (read_line(f, &line, &cap, &len)) {
    line_no++;
    if (len == 0 || is_blank_line(line)) continue;

    p->cur_path = input_path;
    p->cur_line = line_no;
    p->cur_src_ref = -1;
    p->cur_loc.unit = NULL;
    p->cur_loc.line = 0;
    p->cur_loc.col = 0;

    JsonError jerr = {0};
    JsonValue* root = NULL;
    if (!json_parse(&p->arena, line, &root, &jerr)) {
      errf(p, "sircc: JSON parse error at column %zu: %s", jerr.offset + 1, jerr.msg ? jerr.msg : "unknown");
      free(line);
      fclose(f);
      return false;
    }
    if (!must_obj(p, root, "record")) {
      free(line);
      fclose(f);
      return false;
    }

    const char* ir = must_string(p, json_obj_get(root, "ir"), "record.ir");
    const char* k = must_string(p, json_obj_get(root, "k"), "record.k");
    if (!ir || !k) {
      free(line);
      fclose(f);
      return false;
    }

    JsonValue* src_ref = json_obj_get(root, "src_ref");
    if (src_ref) {
      int64_t sid = -1;
      if (!json_get_i64(src_ref, &sid)) {
        errf(p, "sircc: src_ref must be an integer");
        free(line);
        fclose(f);
        return false;
      }
      p->cur_src_ref = sid;
    }
    JsonValue* loc = json_obj_get(root, "loc");
    if (loc && loc->type == JSON_OBJECT) {
      int64_t l = 0;
      JsonValue* linev = json_obj_get(loc, "line");
      if (linev && json_get_i64(linev, &l) && l > 0) {
        p->cur_loc.line = l;
        int64_t c = 0;
        JsonValue* colv = json_obj_get(loc, "col");
        if (colv && json_get_i64(colv, &c) && c > 0) p->cur_loc.col = c;
        p->cur_loc.unit = json_get_string(json_obj_get(loc, "unit"));
      }
    }

    if (strcmp(ir, "sir-v1.0") != 0) {
      errf(p, "sircc: unsupported ir '%s' (expected sir-v1.0)", ir);
      free(line);
      fclose(f);
      return false;
    }

    if (strcmp(k, "meta") == 0) {
      if (!parse_meta_record(p, opt, root)) {
        free(line);
        fclose(f);
        return false;
      }
      if (opt && opt->dump_records) fprintf(stderr, "%s:%zu: meta\n", p->cur_path, p->cur_line);
      continue;
    }
    if (strcmp(k, "src") == 0) {
      if (!parse_src_record(p, root)) {
        free(line);
        fclose(f);
        return false;
      }
      if (opt && opt->dump_records) fprintf(stderr, "%s:%zu: src\n", p->cur_path, p->cur_line);
      continue;
    }
    if (strcmp(k, "diag") == 0) {
      if (!parse_diag_record(p, root)) {
        free(line);
        fclose(f);
        return false;
      }
      if (opt && opt->dump_records) fprintf(stderr, "%s:%zu: diag\n", p->cur_path, p->cur_line);
      continue;
    }
    if (strcmp(k, "sym") == 0) {
      if (!parse_sym_record(p, root)) {
        free(line);
        fclose(f);
        return false;
      }
      if (opt && opt->dump_records) fprintf(stderr, "%s:%zu: sym\n", p->cur_path, p->cur_line);
      continue;
    }
    if (strcmp(k, "type") == 0) {
      if (!parse_type_record(p, root)) {
        free(line);
        fclose(f);
        return false;
      }
      if (opt && opt->dump_records) fprintf(stderr, "%s:%zu: type\n", p->cur_path, p->cur_line);
      continue;
    }
    if (strcmp(k, "node") == 0) {
      if (!parse_node_record(p, root)) {
        free(line);
        fclose(f);
        return false;
      }
      if (opt && opt->dump_records) fprintf(stderr, "%s:%zu: node\n", p->cur_path, p->cur_line);
      continue;
    }
    if (strcmp(k, "ext") == 0) {
      if (!parse_ext_record(p, root)) {
        free(line);
        fclose(f);
        return false;
      }
      if (opt && opt->dump_records) fprintf(stderr, "%s:%zu: ext\n", p->cur_path, p->cur_line);
      continue;
    }
    if (strcmp(k, "label") == 0) {
      if (!parse_label_record(p, root)) {
        free(line);
        fclose(f);
        return false;
      }
      if (opt && opt->dump_records) fprintf(stderr, "%s:%zu: label\n", p->cur_path, p->cur_line);
      continue;
    }
    if (strcmp(k, "instr") == 0) {
      if (!parse_instr_record(p, opt, root)) {
        free(line);
        fclose(f);
        return false;
      }
      continue;
    }
    if (strcmp(k, "dir") == 0) {
      if (!parse_dir_record(p, root)) {
        free(line);
        fclose(f);
        return false;
      }
      if (opt && opt->dump_records) fprintf(stderr, "%s:%zu: dir\n", p->cur_path, p->cur_line);
      continue;
    }

    errf(p, "sircc: unknown record kind '%s'", k);
    free(line);
    fclose(f);
    return false;
  }

  free(line);
  fclose(f);
  return true;
}

static TypeRec* get_type(SirProgram* p, int64_t id) {
  if (id < 0 || (size_t)id >= p->types_cap) return NULL;
  return p->types[id];
}

static NodeRec* get_node(SirProgram* p, int64_t id) {
  if (id < 0 || (size_t)id >= p->nodes_cap) return NULL;
  return p->nodes[id];
}

static LLVMTypeRef lower_type(SirProgram* p, LLVMContextRef ctx, int64_t id);

static LLVMTypeRef lower_type_prim(LLVMContextRef ctx, const char* prim) {
  if (strcmp(prim, "i1") == 0 || strcmp(prim, "bool") == 0) return LLVMInt1TypeInContext(ctx);
  if (strcmp(prim, "i8") == 0) return LLVMInt8TypeInContext(ctx);
  if (strcmp(prim, "i16") == 0) return LLVMInt16TypeInContext(ctx);
  if (strcmp(prim, "i32") == 0) return LLVMInt32TypeInContext(ctx);
  if (strcmp(prim, "i64") == 0) return LLVMInt64TypeInContext(ctx);
  if (strcmp(prim, "f32") == 0) return LLVMFloatTypeInContext(ctx);
  if (strcmp(prim, "f64") == 0) return LLVMDoubleTypeInContext(ctx);
  if (strcmp(prim, "void") == 0) return LLVMVoidTypeInContext(ctx);
  return NULL;
}

static LLVMValueRef get_or_declare_intrinsic(LLVMModuleRef mod, const char* name, LLVMTypeRef ret,
                                             LLVMTypeRef* params, unsigned param_count) {
  LLVMValueRef fn = LLVMGetNamedFunction(mod, name);
  if (fn) return fn;
  LLVMTypeRef fnty = LLVMFunctionType(ret, params, param_count, 0);
  fn = LLVMAddFunction(mod, name, fnty);
  LLVMSetLinkage(fn, LLVMExternalLinkage);
  return fn;
}

static LLVMTypeRef lower_type(SirProgram* p, LLVMContextRef ctx, int64_t id) {
  TypeRec* tr = get_type(p, id);
  if (!tr) return NULL;
  if (tr->llvm) return tr->llvm;
  if (tr->resolving) return NULL;
  tr->resolving = true;

  LLVMTypeRef out = NULL;
  switch (tr->kind) {
    case TYPE_PRIM:
      out = lower_type_prim(ctx, tr->prim);
      break;
    case TYPE_PTR: {
      LLVMTypeRef of = lower_type(p, ctx, tr->of);
      if (of) out = LLVMPointerType(of, 0);
      break;
    }
    case TYPE_FN: {
      LLVMTypeRef ret = lower_type(p, ctx, tr->ret);
      if (!ret) break;
      LLVMTypeRef* params = NULL;
      if (tr->param_len) {
        params = (LLVMTypeRef*)malloc(tr->param_len * sizeof(LLVMTypeRef));
        if (!params) break;
        for (size_t i = 0; i < tr->param_len; i++) {
          params[i] = lower_type(p, ctx, tr->params[i]);
          if (!params[i]) {
            free(params);
            params = NULL;
            break;
          }
        }
      }
      if (tr->param_len == 0 || params) {
        out = LLVMFunctionType(ret, params, (unsigned)tr->param_len, tr->varargs ? 1 : 0);
      }
      free(params);
      break;
    }
    default:
      break;
  }

  tr->llvm = out;
  tr->resolving = false;
  return out;
}

typedef struct Binding {
  const char* name;
  LLVMValueRef value;
} Binding;

typedef struct FunctionCtx {
  SirProgram* p;
  LLVMContextRef ctx;
  LLVMModuleRef mod;
  LLVMBuilderRef builder;
  LLVMValueRef fn;

  Binding* binds;
  size_t bind_len;
  size_t bind_cap;
} FunctionCtx;

static bool bind_add(FunctionCtx* f, const char* name, LLVMValueRef v) {
  if (!name) return false;
  for (size_t i = 0; i < f->bind_len; i++) {
    if (strcmp(f->binds[i].name, name) == 0) return false;
  }
  if (f->bind_len == f->bind_cap) {
    size_t next = f->bind_cap ? f->bind_cap * 2 : 16;
    Binding* bigger = (Binding*)realloc(f->binds, next * sizeof(Binding));
    if (!bigger) return false;
    f->binds = bigger;
    f->bind_cap = next;
  }
  f->binds[f->bind_len++] = (Binding){.name = name, .value = v};
  return true;
}

static LLVMValueRef bind_get(FunctionCtx* f, const char* name) {
  for (size_t i = 0; i < f->bind_len; i++) {
    if (strcmp(f->binds[i].name, name) == 0) return f->binds[i].value;
  }
  return NULL;
}

static LLVMValueRef lower_expr(FunctionCtx* f, int64_t node_id);
static bool lower_stmt(FunctionCtx* f, int64_t node_id);

static LLVMValueRef lower_expr(FunctionCtx* f, int64_t node_id) {
  NodeRec* n = get_node(f->p, node_id);
  if (!n) {
    errf(f->p, "sircc: unknown node id %lld", (long long)node_id);
    return NULL;
  }
  if (n->llvm_value) return n->llvm_value;
  if (n->resolving) {
    errf(f->p, "sircc: cyclic node reference at %lld", (long long)node_id);
    return NULL;
  }
  n->resolving = true;

  LLVMValueRef out = NULL;

  if (strcmp(n->tag, "name") == 0) {
    const char* name = NULL;
    if (n->fields) name = json_get_string(json_obj_get(n->fields, "name"));
    if (!name) {
      errf(f->p, "sircc: name node %lld missing fields.name", (long long)node_id);
      goto done;
    }
    out = bind_get(f, name);
    if (!out) errf(f->p, "sircc: unknown name '%s' in node %lld", name, (long long)node_id);
    goto done;
  }

  if (strcmp(n->tag, "binop.add") == 0) {
    JsonValue* lhs = n->fields ? json_obj_get(n->fields, "lhs") : NULL;
    JsonValue* rhs = n->fields ? json_obj_get(n->fields, "rhs") : NULL;
    int64_t lhs_id = 0, rhs_id = 0;
    if (!parse_node_ref_id(lhs, &lhs_id) || !parse_node_ref_id(rhs, &rhs_id)) {
      errf(f->p, "sircc: binop.add node %lld missing lhs/rhs refs", (long long)node_id);
      goto done;
    }
    LLVMValueRef a = lower_expr(f, lhs_id);
    LLVMValueRef b = lower_expr(f, rhs_id);
    if (!a || !b) goto done;
    LLVMTypeRef ty = LLVMTypeOf(a);
    if (LLVMGetTypeKind(ty) == LLVMIntegerTypeKind) {
      out = LLVMBuildAdd(f->builder, a, b, "add");
    } else {
      out = LLVMBuildFAdd(f->builder, a, b, "addf");
    }
    goto done;
  }

  if (strncmp(n->tag, "i", 1) == 0) {
    // Mnemonic-style integer ops: i8.add, i16.sub, i32.mul, etc.
    const char* dot = strchr(n->tag, '.');
    if (dot) {
      char wbuf[8];
      size_t wlen = (size_t)(dot - n->tag);
      if (wlen < sizeof(wbuf)) {
        memcpy(wbuf, n->tag, wlen);
        wbuf[wlen] = 0;
        int width = 0;
          if (sscanf(wbuf, "i%d", &width) == 1 && (width == 8 || width == 16 || width == 32 || width == 64)) {
            const char* op = dot + 1;
          JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
          int64_t a_id = 0, b_id = 0;
          // Extract operands.
          LLVMValueRef a = NULL;
          LLVMValueRef b = NULL;

          if (args && args->type == JSON_ARRAY) {
            if (args->v.arr.len == 1) {
              if (!parse_node_ref_id(args->v.arr.items[0], &a_id)) {
                errf(f->p, "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
                goto done;
              }
              a = lower_expr(f, a_id);
              if (!a) goto done;
            } else if (args->v.arr.len == 2) {
              if (!parse_node_ref_id(args->v.arr.items[0], &a_id) || !parse_node_ref_id(args->v.arr.items[1], &b_id)) {
                errf(f->p, "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
                goto done;
              }
              a = lower_expr(f, a_id);
              b = lower_expr(f, b_id);
              if (!a || !b) goto done;
            } else {
              errf(f->p, "sircc: %s node %lld args must have arity 1 or 2", n->tag, (long long)node_id);
              goto done;
            }
          } else {
            // Back-compat: allow lhs/rhs form for binary operators.
            JsonValue* lhs = n->fields ? json_obj_get(n->fields, "lhs") : NULL;
            JsonValue* rhs = n->fields ? json_obj_get(n->fields, "rhs") : NULL;
            if (parse_node_ref_id(lhs, &a_id) && parse_node_ref_id(rhs, &b_id)) {
              a = lower_expr(f, a_id);
              b = lower_expr(f, b_id);
              if (!a || !b) goto done;
            } else {
              errf(f->p, "sircc: %s node %lld missing args", n->tag, (long long)node_id);
              goto done;
            }
          }

          // Lower ops.
          if (strcmp(op, "add") == 0) {
            out = LLVMBuildAdd(f->builder, a, b, "iadd");
            goto done;
          }
          if (strcmp(op, "sub") == 0) {
            out = LLVMBuildSub(f->builder, a, b, "isub");
            goto done;
          }
          if (strcmp(op, "mul") == 0) {
            out = LLVMBuildMul(f->builder, a, b, "imul");
            goto done;
          }
          if (strcmp(op, "and") == 0) {
            out = LLVMBuildAnd(f->builder, a, b, "iand");
            goto done;
          }
          if (strcmp(op, "or") == 0) {
            out = LLVMBuildOr(f->builder, a, b, "ior");
            goto done;
          }
          if (strcmp(op, "xor") == 0) {
            out = LLVMBuildXor(f->builder, a, b, "ixor");
            goto done;
          }
          if (strcmp(op, "not") == 0) {
            out = LLVMBuildNot(f->builder, a, "inot");
            goto done;
          }
          if (strcmp(op, "neg") == 0) {
            out = LLVMBuildNeg(f->builder, a, "ineg");
            goto done;
          }
          if (strcmp(op, "shl") == 0 || strcmp(op, "shr.s") == 0 || strcmp(op, "shr.u") == 0) {
            if (!b) {
              errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
              goto done;
            }
            LLVMTypeRef xty = LLVMTypeOf(a);
            if (LLVMGetTypeKind(xty) != LLVMIntegerTypeKind) {
              errf(f->p, "sircc: %s node %lld requires integer lhs", n->tag, (long long)node_id);
              goto done;
            }

            LLVMTypeRef sty = LLVMTypeOf(b);
            if (LLVMGetTypeKind(sty) != LLVMIntegerTypeKind) {
              errf(f->p, "sircc: %s node %lld requires integer shift amount", n->tag, (long long)node_id);
              goto done;
            }

            LLVMValueRef shift = b;
            if (LLVMGetIntTypeWidth(sty) != LLVMGetIntTypeWidth(xty)) {
              shift = LLVMBuildZExtOrTrunc(f->builder, b, xty, "shift.cast");
            }
            unsigned mask = (unsigned)(width - 1);
            LLVMValueRef maskv = LLVMConstInt(xty, mask, 0);
            shift = LLVMBuildAnd(f->builder, shift, maskv, "shift.mask");

            if (strcmp(op, "shl") == 0) {
              out = LLVMBuildShl(f->builder, a, shift, "shl");
              goto done;
            }
            if (strcmp(op, "shr.s") == 0) {
              out = LLVMBuildAShr(f->builder, a, shift, "ashr");
              goto done;
            }
            out = LLVMBuildLShr(f->builder, a, shift, "lshr");
            goto done;
          }

          if (strncmp(op, "cmp.", 4) == 0) {
            if (!b) {
              errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
              goto done;
            }
            const char* cc = op + 4;
            LLVMIntPredicate pred;
            if (strcmp(cc, "eq") == 0) pred = LLVMIntEQ;
            else if (strcmp(cc, "ne") == 0) pred = LLVMIntNE;
            else if (strcmp(cc, "slt") == 0) pred = LLVMIntSLT;
            else if (strcmp(cc, "sle") == 0) pred = LLVMIntSLE;
            else if (strcmp(cc, "sgt") == 0) pred = LLVMIntSGT;
            else if (strcmp(cc, "sge") == 0) pred = LLVMIntSGE;
            else if (strcmp(cc, "ult") == 0) pred = LLVMIntULT;
            else if (strcmp(cc, "ule") == 0) pred = LLVMIntULE;
            else if (strcmp(cc, "ugt") == 0) pred = LLVMIntUGT;
            else if (strcmp(cc, "uge") == 0) pred = LLVMIntUGE;
            else {
              errf(f->p, "sircc: unsupported integer compare '%s' in %s", cc, n->tag);
              goto done;
            }
            out = LLVMBuildICmp(f->builder, pred, a, b, "icmp");
            goto done;
          }

          if (strcmp(op, "clz") == 0 || strcmp(op, "ctz") == 0) {
            const char* iname = (strcmp(op, "clz") == 0) ? "llvm.ctlz" : "llvm.cttz";
            char full[32];
            snprintf(full, sizeof(full), "%s.i%d", iname, width);
            LLVMTypeRef ity = LLVMTypeOf(a);
            LLVMTypeRef i1 = LLVMInt1TypeInContext(f->ctx);
            LLVMTypeRef params[2] = {ity, i1};
            LLVMValueRef fn = get_or_declare_intrinsic(f->mod, full, ity, params, 2);
            LLVMValueRef argsv[2] = {a, LLVMConstInt(i1, 0, 0)};
            out = LLVMBuildCall2(f->builder, LLVMGetElementType(LLVMTypeOf(fn)), fn, argsv, 2, op);
            goto done;
          }

          if (strcmp(op, "popc") == 0) {
            char full[32];
            snprintf(full, sizeof(full), "llvm.ctpop.i%d", width);
            LLVMTypeRef ity = LLVMTypeOf(a);
            LLVMTypeRef params[1] = {ity};
            LLVMValueRef fn = get_or_declare_intrinsic(f->mod, full, ity, params, 1);
            LLVMValueRef argsv[1] = {a};
            out = LLVMBuildCall2(f->builder, LLVMGetElementType(LLVMTypeOf(fn)), fn, argsv, 1, "popc");
            goto done;
          }

          if (strncmp(op, "zext.i", 6) == 0 || strncmp(op, "sext.i", 6) == 0 || strncmp(op, "trunc.i", 7) == 0) {
            int src = 0;
            bool is_zext = strncmp(op, "zext.i", 6) == 0;
            bool is_sext = strncmp(op, "sext.i", 6) == 0;
            bool is_trunc = strncmp(op, "trunc.i", 7) == 0;
            const char* num = is_trunc ? (op + 7) : (op + 6);
            if (sscanf(num, "%d", &src) != 1 || !(src == 8 || src == 16 || src == 32 || src == 64)) {
              errf(f->p, "sircc: invalid cast mnemonic '%s'", n->tag);
              goto done;
            }

            if ((is_zext || is_sext) && width <= src) {
              errf(f->p, "sircc: %s requires dst width > src width", n->tag);
              goto done;
            }
            if (is_trunc && width >= src) {
              errf(f->p, "sircc: %s requires dst width < src width", n->tag);
              goto done;
            }

            LLVMTypeRef ity = LLVMTypeOf(a);
            if (LLVMGetTypeKind(ity) != LLVMIntegerTypeKind || (int)LLVMGetIntTypeWidth(ity) != src) {
              errf(f->p, "sircc: %s requires i%d operand", n->tag, src);
              goto done;
            }
            LLVMTypeRef dst = LLVMIntTypeInContext(f->ctx, (unsigned)width);
            if (is_zext) out = LLVMBuildZExt(f->builder, a, dst, "zext");
            else if (is_sext) out = LLVMBuildSExt(f->builder, a, dst, "sext");
            else out = LLVMBuildTrunc(f->builder, a, dst, "trunc");
            goto done;
          }
        }
      }
    }
  }

  if (strncmp(n->tag, "bool.", 5) == 0) {
    const char* op = n->tag + 5;
    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
    if (!args || args->type != JSON_ARRAY) {
      errf(f->p, "sircc: %s node %lld missing args array", n->tag, (long long)node_id);
      goto done;
    }

    if (strcmp(op, "not") == 0) {
      if (args->v.arr.len != 1) {
        errf(f->p, "sircc: bool.not node %lld requires 1 arg", (long long)node_id);
        goto done;
      }
      int64_t x_id = 0;
      if (!parse_node_ref_id(args->v.arr.items[0], &x_id)) {
        errf(f->p, "sircc: bool.not node %lld arg must be node ref", (long long)node_id);
        goto done;
      }
      LLVMValueRef x = lower_expr(f, x_id);
      if (!x) goto done;
      out = LLVMBuildNot(f->builder, x, "bnot");
      goto done;
    }

    if (strcmp(op, "and") == 0 || strcmp(op, "or") == 0 || strcmp(op, "xor") == 0) {
      if (args->v.arr.len != 2) {
        errf(f->p, "sircc: bool.%s node %lld requires 2 args", op, (long long)node_id);
        goto done;
      }
      int64_t a_id = 0, b_id = 0;
      if (!parse_node_ref_id(args->v.arr.items[0], &a_id) || !parse_node_ref_id(args->v.arr.items[1], &b_id)) {
        errf(f->p, "sircc: bool.%s node %lld args must be node refs", op, (long long)node_id);
        goto done;
      }
      LLVMValueRef a = lower_expr(f, a_id);
      LLVMValueRef b = lower_expr(f, b_id);
      if (!a || !b) goto done;
      if (strcmp(op, "and") == 0) out = LLVMBuildAnd(f->builder, a, b, "band");
      else if (strcmp(op, "or") == 0) out = LLVMBuildOr(f->builder, a, b, "bor");
      else out = LLVMBuildXor(f->builder, a, b, "bxor");
      goto done;
    }
  }

  if (strcmp(n->tag, "select") == 0) {
    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) {
      errf(f->p, "sircc: select node %lld requires args:[cond, then, else]", (long long)node_id);
      goto done;
    }
    int64_t c_id = 0, t_id = 0, e_id = 0;
    if (!parse_node_ref_id(args->v.arr.items[0], &c_id) || !parse_node_ref_id(args->v.arr.items[1], &t_id) ||
        !parse_node_ref_id(args->v.arr.items[2], &e_id)) {
      errf(f->p, "sircc: select node %lld args must be node refs", (long long)node_id);
      goto done;
    }
    LLVMValueRef c = lower_expr(f, c_id);
    LLVMValueRef tv = lower_expr(f, t_id);
    LLVMValueRef ev = lower_expr(f, e_id);
    if (!c || !tv || !ev) goto done;
    out = LLVMBuildSelect(f->builder, c, tv, ev, "select");
    goto done;
  }

  if (strcmp(n->tag, "call") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: call node %lld missing fields", (long long)node_id);
      goto done;
    }
    JsonValue* callee_v = json_obj_get(n->fields, "callee");
    int64_t callee_id = 0;
    if (!parse_node_ref_id(callee_v, &callee_id)) {
      errf(f->p, "sircc: call node %lld missing callee ref", (long long)node_id);
      goto done;
    }
    NodeRec* callee_n = get_node(f->p, callee_id);
    if (!callee_n || strcmp(callee_n->tag, "fn") != 0 || !callee_n->llvm_value) {
      errf(f->p, "sircc: call node %lld callee %lld is not a lowered fn", (long long)node_id, (long long)callee_id);
      goto done;
    }
    LLVMValueRef callee = callee_n->llvm_value;

    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY) {
      errf(f->p, "sircc: call node %lld missing args array", (long long)node_id);
      goto done;
    }
    size_t argc = args->v.arr.len;
    LLVMValueRef* argv = NULL;
    if (argc) {
      argv = (LLVMValueRef*)malloc(argc * sizeof(LLVMValueRef));
      if (!argv) goto done;
      for (size_t i = 0; i < argc; i++) {
        int64_t aid = 0;
        if (!parse_node_ref_id(args->v.arr.items[i], &aid)) {
          errf(f->p, "sircc: call node %lld arg[%zu] must be node ref", (long long)node_id, i);
          free(argv);
          goto done;
        }
        argv[i] = lower_expr(f, aid);
        if (!argv[i]) {
          free(argv);
          goto done;
        }
      }
    }

    LLVMTypeRef callee_fty = LLVMGetElementType(LLVMTypeOf(callee));
    out = LLVMBuildCall2(f->builder, callee_fty, callee, argv, (unsigned)argc, "call");
    free(argv);
    goto done;
  }

  if (strncmp(n->tag, "f32.", 4) == 0 || strncmp(n->tag, "f64.", 4) == 0) {
    int width = (n->tag[1] == '3') ? 32 : 64;
    const char* op = n->tag + 4;

    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
    if (!args || args->type != JSON_ARRAY) {
      errf(f->p, "sircc: %s node %lld missing args array", n->tag, (long long)node_id);
      goto done;
    }

    LLVMValueRef a = NULL;
    LLVMValueRef b = NULL;

    if (args->v.arr.len == 1) {
      int64_t a_id = 0;
      if (!parse_node_ref_id(args->v.arr.items[0], &a_id)) {
        errf(f->p, "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
        goto done;
      }
      a = lower_expr(f, a_id);
      if (!a) goto done;
    } else if (args->v.arr.len == 2) {
      int64_t a_id = 0, b_id = 0;
      if (!parse_node_ref_id(args->v.arr.items[0], &a_id) || !parse_node_ref_id(args->v.arr.items[1], &b_id)) {
        errf(f->p, "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
        goto done;
      }
      a = lower_expr(f, a_id);
      b = lower_expr(f, b_id);
      if (!a || !b) goto done;
    } else {
      errf(f->p, "sircc: %s node %lld args must have arity 1 or 2", n->tag, (long long)node_id);
      goto done;
    }

    LLVMTypeRef fty = LLVMTypeOf(a);
    if (width == 32 && LLVMGetTypeKind(fty) != LLVMFloatTypeKind) {
      errf(f->p, "sircc: %s expects f32 operands", n->tag);
      goto done;
    }
    if (width == 64 && LLVMGetTypeKind(fty) != LLVMDoubleTypeKind) {
      errf(f->p, "sircc: %s expects f64 operands", n->tag);
      goto done;
    }

    if (strcmp(op, "add") == 0) {
      if (!b) {
        errf(f->p, "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      out = LLVMBuildFAdd(f->builder, a, b, "fadd");
      goto done;
    }
    if (strcmp(op, "sub") == 0) {
      if (!b) {
        errf(f->p, "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      out = LLVMBuildFSub(f->builder, a, b, "fsub");
      goto done;
    }
    if (strcmp(op, "mul") == 0) {
      if (!b) {
        errf(f->p, "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      out = LLVMBuildFMul(f->builder, a, b, "fmul");
      goto done;
    }
    if (strcmp(op, "div") == 0) {
      if (!b) {
        errf(f->p, "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      out = LLVMBuildFDiv(f->builder, a, b, "fdiv");
      goto done;
    }
    if (strcmp(op, "neg") == 0) {
      out = LLVMBuildFNeg(f->builder, a, "fneg");
      goto done;
    }
    if (strcmp(op, "abs") == 0) {
      char full[32];
      snprintf(full, sizeof(full), "llvm.fabs.f%d", width);
      LLVMTypeRef params[1] = {fty};
      LLVMValueRef fn = get_or_declare_intrinsic(f->mod, full, fty, params, 1);
      LLVMValueRef argsv[1] = {a};
      out = LLVMBuildCall2(f->builder, LLVMGetElementType(LLVMTypeOf(fn)), fn, argsv, 1, "fabs");
      goto done;
    }
    if (strcmp(op, "sqrt") == 0) {
      char full[32];
      snprintf(full, sizeof(full), "llvm.sqrt.f%d", width);
      LLVMTypeRef params[1] = {fty};
      LLVMValueRef fn = get_or_declare_intrinsic(f->mod, full, fty, params, 1);
      LLVMValueRef argsv[1] = {a};
      out = LLVMBuildCall2(f->builder, LLVMGetElementType(LLVMTypeOf(fn)), fn, argsv, 1, "fsqrt");
      goto done;
    }
  }

  if (strncmp(n->tag, "const.", 6) == 0) {
    const char* tyname = n->tag + 6;
    int64_t value = 0;
    if (!n->fields || !must_i64(f->p, json_obj_get(n->fields, "value"), &value, "const.value")) goto done;
    LLVMTypeRef ty = lower_type_prim(f->ctx, tyname);
    if (!ty) {
      errf(f->p, "sircc: unsupported const type '%s'", tyname);
      goto done;
    }
    out = LLVMConstInt(ty, (unsigned long long)value, 1);
    goto done;
  }

  errf(f->p, "sircc: unsupported expr tag '%s' (node %lld)", n->tag, (long long)node_id);

done:
  n->llvm_value = out;
  n->resolving = false;
  return out;
}

static bool lower_stmt(FunctionCtx* f, int64_t node_id) {
  NodeRec* n = get_node(f->p, node_id);
  if (!n) {
    errf(f->p, "sircc: unknown stmt node %lld", (long long)node_id);
    return false;
  }

  if (strcmp(n->tag, "return") == 0) {
    JsonValue* v = n->fields ? json_obj_get(n->fields, "value") : NULL;
    int64_t vid = 0;
    if (!parse_node_ref_id(v, &vid)) {
      errf(f->p, "sircc: return node %lld missing value ref", (long long)node_id);
      return false;
    }
    LLVMValueRef rv = lower_expr(f, vid);
    if (!rv) return false;
    LLVMBuildRet(f->builder, rv);
    return true;
  }

  if (strcmp(n->tag, "term.ret") == 0) {
    JsonValue* v = n->fields ? json_obj_get(n->fields, "value") : NULL;
    if (!v) {
      LLVMBuildRetVoid(f->builder);
      return true;
    }
    int64_t vid = 0;
    if (!parse_node_ref_id(v, &vid)) {
      errf(f->p, "sircc: term.ret node %lld invalid value ref", (long long)node_id);
      return false;
    }
    LLVMValueRef rv = lower_expr(f, vid);
    if (!rv) return false;
    LLVMBuildRet(f->builder, rv);
    return true;
  }

  if (strcmp(n->tag, "term.unreachable") == 0) {
    LLVMBuildUnreachable(f->builder);
    return true;
  }

  if (strcmp(n->tag, "term.trap") == 0) {
    // Deterministic immediate trap: lower to llvm.trap + unreachable.
    LLVMTypeRef v = LLVMVoidTypeInContext(f->ctx);
    LLVMValueRef fn = get_or_declare_intrinsic(f->mod, "llvm.trap", v, NULL, 0);
    LLVMBuildCall2(f->builder, LLVMGetElementType(LLVMTypeOf(fn)), fn, NULL, 0, "");
    LLVMBuildUnreachable(f->builder);
    return true;
  }

  if (strcmp(n->tag, "block") == 0) {
    JsonValue* stmts = n->fields ? json_obj_get(n->fields, "stmts") : NULL;
    if (!stmts || stmts->type != JSON_ARRAY) {
      errf(f->p, "sircc: block node %lld missing stmts array", (long long)node_id);
      return false;
    }
    for (size_t i = 0; i < stmts->v.arr.len; i++) {
      int64_t sid = 0;
      if (!parse_node_ref_id(stmts->v.arr.items[i], &sid)) {
        errf(f->p, "sircc: block node %lld has non-ref stmt", (long long)node_id);
        return false;
      }
      if (!lower_stmt(f, sid)) return false;
      if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(f->builder))) break;
    }
    return true;
  }

  errf(f->p, "sircc: unsupported stmt tag '%s' (node %lld)", n->tag, (long long)node_id);
  return false;
}

static bool emit_module_ir(LLVMModuleRef mod, const char* out_path) {
  char* err = NULL;
  if (LLVMPrintModuleToFile(mod, out_path, &err) != 0) {
    errf(NULL, "sircc: failed to write LLVM IR: %s", err ? err : "(unknown)");
    LLVMDisposeMessage(err);
    return false;
  }
  return true;
}

static bool emit_module_obj(LLVMModuleRef mod, const char* triple, const char* out_path) {
  if (LLVMInitializeNativeTarget() != 0) {
    errf(NULL, "sircc: LLVMInitializeNativeTarget failed");
    return false;
  }
  LLVMInitializeNativeAsmPrinter();

  char* err = NULL;
  const char* use_triple = triple ? triple : LLVMGetDefaultTargetTriple();
  LLVMTargetRef target = NULL;
  if (LLVMGetTargetFromTriple(use_triple, &target, &err) != 0) {
    errf(NULL, "sircc: target triple '%s' unsupported: %s", use_triple, err ? err : "(unknown)");
    LLVMDisposeMessage(err);
    if (!triple) LLVMDisposeMessage((char*)use_triple);
    return false;
  }

  LLVMTargetMachineRef tm =
      LLVMCreateTargetMachine(target, use_triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocDefault,
                              LLVMCodeModelDefault);
  if (!tm) {
    errf(NULL, "sircc: failed to create target machine");
    if (!triple) LLVMDisposeMessage((char*)use_triple);
    return false;
  }

  LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
  char* dl_str = LLVMCopyStringRepOfTargetData(td);
  LLVMSetTarget(mod, use_triple);
  LLVMSetDataLayout(mod, dl_str);
  LLVMDisposeMessage(dl_str);
  LLVMDisposeTargetData(td);

  if (LLVMTargetMachineEmitToFile(tm, mod, (char*)out_path, LLVMObjectFile, &err) != 0) {
    errf(NULL, "sircc: failed to emit object: %s", err ? err : "(unknown)");
    LLVMDisposeMessage(err);
    LLVMDisposeTargetMachine(tm);
    if (!triple) LLVMDisposeMessage((char*)use_triple);
    return false;
  }

  LLVMDisposeTargetMachine(tm);
  if (!triple) LLVMDisposeMessage((char*)use_triple);
  return true;
}

static bool run_clang_link(const char* clang_path, const char* obj_path, const char* out_path) {
  const char* clang = clang_path ? clang_path : "clang";

  pid_t pid = fork();
  if (pid < 0) {
    errf(NULL, "sircc: fork failed: %s", strerror(errno));
    return false;
  }
  if (pid == 0) {
    execlp(clang, clang, "-o", out_path, obj_path, (char*)NULL);
    _exit(127);
  }
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) {
    errf(NULL, "sircc: waitpid failed: %s", strerror(errno));
    return false;
  }
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    errf(NULL, "sircc: clang failed (exit=%d)", WIFEXITED(st) ? WEXITSTATUS(st) : 1);
    return false;
  }
  return true;
}

static bool make_tmp_obj(char* out, size_t out_cap) {
  const char* dir = getenv("TMPDIR");
  if (!dir) dir = "/tmp";
  if (snprintf(out, out_cap, "%s/sircc-XXXXXX.o", dir) >= (int)out_cap) return false;
  int fd = mkstemps(out, 2);
  if (fd < 0) return false;
  close(fd);
  return true;
}

static bool lower_functions(SirProgram* p, LLVMContextRef ctx, LLVMModuleRef mod) {
  // Pass 1: create prototypes
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if (strcmp(n->tag, "fn") != 0) continue;

    const char* name = n->fields ? json_get_string(json_obj_get(n->fields, "name")) : NULL;
    if (!name) {
      errf(p, "sircc: fn node %lld missing fields.name", (long long)n->id);
      return false;
    }
    if (n->type_ref == 0) {
      errf(p, "sircc: fn node %lld missing type_ref", (long long)n->id);
      return false;
    }
    LLVMTypeRef fnty = lower_type(p, ctx, n->type_ref);
    if (!fnty || LLVMGetTypeKind(fnty) != LLVMFunctionTypeKind) {
      errf(p, "sircc: fn node %lld has invalid function type_ref %lld", (long long)n->id, (long long)n->type_ref);
      return false;
    }
    LLVMValueRef fn = LLVMAddFunction(mod, name, fnty);
    n->llvm_value = fn;
  }

  // Pass 2: lower bodies
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if (strcmp(n->tag, "fn") != 0) continue;
    LLVMValueRef fn = n->llvm_value;
    if (!fn) continue;

    // Expression nodes are currently lowered relative to a specific function's builder. Clear any
    // previous per-node cached values before lowering a new function (constants + fn prototypes are safe).
    for (size_t j = 0; j < p->nodes_cap; j++) {
      NodeRec* x = p->nodes[j];
      if (!x) continue;
      if (strcmp(x->tag, "fn") == 0) continue;
      if (strncmp(x->tag, "const.", 6) == 0) continue;
      x->llvm_value = NULL;
      x->resolving = false;
    }

    JsonValue* paramsv = n->fields ? json_obj_get(n->fields, "params") : NULL;
    JsonValue* bodyv = n->fields ? json_obj_get(n->fields, "body") : NULL;
    if (!paramsv || paramsv->type != JSON_ARRAY) {
      errf(p, "sircc: fn node %lld missing params array", (long long)n->id);
      return false;
    }
    int64_t body_id = 0;
    if (!parse_node_ref_id(bodyv, &body_id)) {
      errf(p, "sircc: fn node %lld missing body ref", (long long)n->id);
      return false;
    }

    // Entry block + builder
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx, fn, "entry");
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(ctx);
    LLVMPositionBuilderAtEnd(builder, entry);

    FunctionCtx f = {.p = p, .ctx = ctx, .mod = mod, .builder = builder, .fn = fn};

    unsigned param_count = LLVMCountParams(fn);
    if (paramsv->v.arr.len != (size_t)param_count) {
      errf(p, "sircc: fn node %lld param count mismatch: node has %zu, type has %u", (long long)n->id,
             paramsv->v.arr.len, param_count);
      LLVMDisposeBuilder(builder);
      free(f.binds);
      return false;
    }

    for (unsigned pi = 0; pi < param_count; pi++) {
      int64_t pid = 0;
      if (!parse_node_ref_id(paramsv->v.arr.items[pi], &pid)) {
        errf(p, "sircc: fn node %lld has non-ref param", (long long)n->id);
        LLVMDisposeBuilder(builder);
        free(f.binds);
        return false;
      }
      NodeRec* pn = get_node(p, pid);
      if (!pn || strcmp(pn->tag, "param") != 0) {
        errf(p, "sircc: fn node %lld param ref %lld is not a param node", (long long)n->id, (long long)pid);
        LLVMDisposeBuilder(builder);
        free(f.binds);
        return false;
      }
      const char* pname = pn->fields ? json_get_string(json_obj_get(pn->fields, "name")) : NULL;
      if (!pname) {
        errf(p, "sircc: param node %lld missing fields.name", (long long)pid);
        LLVMDisposeBuilder(builder);
        free(f.binds);
        return false;
      }
      LLVMValueRef pv = LLVMGetParam(fn, pi);
      LLVMSetValueName2(pv, pname, strlen(pname));
      if (!bind_add(&f, pname, pv)) {
        errf(p, "sircc: duplicate binding for '%s' in fn %lld", pname, (long long)n->id);
        LLVMDisposeBuilder(builder);
        free(f.binds);
        return false;
      }
    }

    if (!lower_stmt(&f, body_id)) {
      LLVMDisposeBuilder(builder);
      free(f.binds);
      return false;
    }

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
      // Conservative default: fallthrough returns 0 for integer returns, otherwise void.
      LLVMTypeRef rty = LLVMGetReturnType(LLVMGetElementType(LLVMTypeOf(fn)));
      if (LLVMGetTypeKind(rty) == LLVMVoidTypeKind) {
        LLVMBuildRetVoid(builder);
      } else if (LLVMGetTypeKind(rty) == LLVMIntegerTypeKind) {
        LLVMBuildRet(builder, LLVMConstInt(rty, 0, 0));
      } else {
        errf(p, "sircc: fn %lld has implicit fallthrough with unsupported return type", (long long)n->id);
        LLVMDisposeBuilder(builder);
        free(f.binds);
        return false;
      }
    }

    LLVMDisposeBuilder(builder);
    free(f.binds);
  }

  return true;
}

bool sircc_compile(const SirccOptions* opt) {
  if (!opt || !opt->input_path) return false;
  if (!opt->verify_only && !opt->output_path) return false;

  SirProgram p = {0};
  arena_init(&p.arena);

  bool ok = parse_program(&p, opt, opt->input_path);
  if (!ok) goto done;

  if (opt->verify_only) {
    ok = true;
    goto done;
  }

  LLVMContextRef ctx = LLVMContextCreate();
  LLVMModuleRef mod = LLVMModuleCreateWithNameInContext("sir", ctx);

  if (!lower_functions(&p, ctx, mod)) {
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    ok = false;
    goto done;
  }

  char* verr = NULL;
  if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &verr) != 0) {
    errf(NULL, "sircc: LLVM verification failed: %s", verr ? verr : "(unknown)");
    LLVMDisposeMessage(verr);
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    ok = false;
    goto done;
  }

  if (opt->emit == SIRCC_EMIT_LLVM_IR) {
    ok = emit_module_ir(mod, opt->output_path);
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    goto done;
  }

  if (opt->emit == SIRCC_EMIT_OBJ) {
    ok = emit_module_obj(mod, opt->target_triple, opt->output_path);
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    goto done;
  }

  char tmp_obj[4096];
  if (!make_tmp_obj(tmp_obj, sizeof(tmp_obj))) {
    errf(NULL, "sircc: failed to create temporary object path");
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    ok = false;
    goto done;
  }

  ok = emit_module_obj(mod, opt->target_triple, tmp_obj);
  LLVMDisposeModule(mod);
  LLVMContextDispose(ctx);
  if (!ok) {
    unlink(tmp_obj);
    goto done;
  }

  ok = run_clang_link(opt->clang_path, tmp_obj, opt->output_path);
  unlink(tmp_obj);

done:
  free(p.srcs);
  free(p.syms);
  free(p.types);
  free(p.nodes);
  arena_free(&p.arena);
  return ok;
}
