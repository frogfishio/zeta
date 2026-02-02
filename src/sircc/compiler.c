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
#include <limits.h>
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

static void llvm_init_targets_once(void) {
  static int inited = 0;
  if (inited) return;
  LLVMInitializeAllTargetInfos();
  LLVMInitializeAllTargets();
  LLVMInitializeAllTargetMCs();
  LLVMInitializeAllAsmPrinters();
  inited = 1;
}

typedef enum TypeKind {
  TYPE_INVALID = 0,
  TYPE_PRIM,
  TYPE_PTR,
  TYPE_ARRAY,
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
  int64_t len;

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
  unsigned ptr_bytes;
  unsigned ptr_bits;

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

static bool parse_type_ref_id(const JsonValue* v, int64_t* out_id) {
  if (!v) return false;
  int64_t id = 0;
  if (json_get_i64((JsonValue*)v, &id)) {
    *out_id = id;
    return true;
  }
  if (v->type != JSON_OBJECT) return false;
  const char* t = json_get_string(json_obj_get((JsonValue*)v, "t"));
  if (!t || strcmp(t, "ref") != 0) return false;
  const char* k = json_get_string(json_obj_get((JsonValue*)v, "k"));
  if (k && strcmp(k, "type") != 0) return false;
  if (!json_get_i64(json_obj_get((JsonValue*)v, "id"), &id)) return false;
  *out_id = id;
  return true;
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
      if (triple && !(opt && opt->target_triple)) p->target_triple = triple;
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
  tr->len = 0;
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
  } else if (strcmp(kind, "array") == 0) {
    tr->kind = TYPE_ARRAY;
    if (!must_i64(p, json_obj_get(obj, "of"), &tr->of, "type.of")) return false;
    if (!must_i64(p, json_obj_get(obj, "len"), &tr->len, "type.len")) return false;
    if (tr->len < 0) {
      errf(p, "sircc: type.array len must be >= 0");
      return false;
    }
  } else if (strcmp(kind, "fn") == 0) {
    tr->kind = TYPE_FN;
    JsonValue* params = json_obj_get(obj, "params");
    if (!params || params->type != JSON_ARRAY) {
      errf(p, "sircc: expected array for type.params");
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
    errf(p, "sircc: expected object for node.fields");
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

static bool validate_cfg_fn(SirProgram* p, NodeRec* fn);

static bool validate_program(SirProgram* p) {
  // Validate CFG-form functions even under --verify-only.
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if (strcmp(n->tag, "fn") != 0) continue;
    if (!n->fields) continue;
    JsonValue* blocks = json_obj_get(n->fields, "blocks");
    JsonValue* entry = json_obj_get(n->fields, "entry");
    if (blocks || entry) {
      if (!validate_cfg_fn(p, n)) return false;
    }
  }
  return true;
}

static size_t block_param_count(SirProgram* p, int64_t block_id) {
  NodeRec* b = get_node(p, block_id);
  if (!b || strcmp(b->tag, "block") != 0 || !b->fields) return 0;
  JsonValue* params = json_obj_get(b->fields, "params");
  if (!params) return 0;
  if (params->type != JSON_ARRAY) return (size_t)-1;
  return params->v.arr.len;
}

static bool validate_block_params(SirProgram* p, int64_t block_id) {
  NodeRec* b = get_node(p, block_id);
  if (!b || strcmp(b->tag, "block") != 0) {
    errf(p, "sircc: block ref %lld is not a block node", (long long)block_id);
    return false;
  }
  JsonValue* params = b->fields ? json_obj_get(b->fields, "params") : NULL;
  if (!params) return true;
  if (params->type != JSON_ARRAY) {
    errf(p, "sircc: block %lld params must be an array", (long long)block_id);
    return false;
  }
  for (size_t i = 0; i < params->v.arr.len; i++) {
    int64_t pid = 0;
    if (!parse_node_ref_id(params->v.arr.items[i], &pid)) {
      errf(p, "sircc: block %lld params[%zu] must be node refs", (long long)block_id, i);
      return false;
    }
    NodeRec* pn = get_node(p, pid);
    if (!pn || strcmp(pn->tag, "bparam") != 0) {
      errf(p, "sircc: block %lld params[%zu] must reference bparam nodes", (long long)block_id, i);
      return false;
    }
    if (pn->type_ref == 0) {
      errf(p, "sircc: bparam node %lld missing type_ref", (long long)pid);
      return false;
    }
  }
  return true;
}

static bool validate_branch_args(SirProgram* p, int64_t to_block_id, JsonValue* args) {
  size_t pc = block_param_count(p, to_block_id);
  if (pc == (size_t)-1) {
    errf(p, "sircc: block %lld params must be an array", (long long)to_block_id);
    return false;
  }
  size_t ac = 0;
  if (args) {
    if (args->type != JSON_ARRAY) {
      errf(p, "sircc: branch args must be an array");
      return false;
    }
    ac = args->v.arr.len;
  }
  if (pc != ac) {
    errf(p, "sircc: block %lld param/arg count mismatch (params=%zu, args=%zu)", (long long)to_block_id, pc, ac);
    return false;
  }
  for (size_t i = 0; i < ac; i++) {
    int64_t aid = 0;
    if (!parse_node_ref_id(args->v.arr.items[i], &aid)) {
      errf(p, "sircc: branch args[%zu] must be node refs", i);
      return false;
    }
    if (!get_node(p, aid)) {
      errf(p, "sircc: branch args[%zu] references unknown node %lld", i, (long long)aid);
      return false;
    }
  }
  return true;
}

static bool validate_terminator(SirProgram* p, int64_t term_id) {
  NodeRec* t = get_node(p, term_id);
  if (!t) {
    errf(p, "sircc: block terminator references unknown node %lld", (long long)term_id);
    return false;
  }
  if (strncmp(t->tag, "term.", 5) != 0 && strcmp(t->tag, "return") != 0) {
    errf(p, "sircc: block must end with a terminator (got '%s')", t->tag);
    return false;
  }

  if (strcmp(t->tag, "term.br") == 0) {
    if (!t->fields) {
      errf(p, "sircc: term.br missing fields");
      return false;
    }
    int64_t to_id = 0;
    if (!parse_node_ref_id(json_obj_get(t->fields, "to"), &to_id)) {
      errf(p, "sircc: term.br missing to ref");
      return false;
    }
    if (!validate_block_params(p, to_id)) return false;
    return validate_branch_args(p, to_id, json_obj_get(t->fields, "args"));
  }

  if (strcmp(t->tag, "term.cbr") == 0 || strcmp(t->tag, "term.condbr") == 0) {
    if (!t->fields) {
      errf(p, "sircc: %s missing fields", t->tag);
      return false;
    }
    int64_t cond_id = 0;
    if (!parse_node_ref_id(json_obj_get(t->fields, "cond"), &cond_id)) {
      errf(p, "sircc: %s missing cond ref", t->tag);
      return false;
    }
    if (!get_node(p, cond_id)) {
      errf(p, "sircc: %s cond references unknown node %lld", t->tag, (long long)cond_id);
      return false;
    }
    JsonValue* thenb = json_obj_get(t->fields, "then");
    JsonValue* elseb = json_obj_get(t->fields, "else");
    if (!thenb || thenb->type != JSON_OBJECT || !elseb || elseb->type != JSON_OBJECT) {
      errf(p, "sircc: %s requires then/else objects", t->tag);
      return false;
    }
    int64_t then_id = 0, else_id = 0;
    if (!parse_node_ref_id(json_obj_get(thenb, "to"), &then_id) || !parse_node_ref_id(json_obj_get(elseb, "to"), &else_id)) {
      errf(p, "sircc: %s then/else missing to ref", t->tag);
      return false;
    }
    if (!validate_block_params(p, then_id) || !validate_block_params(p, else_id)) return false;
    if (!validate_branch_args(p, then_id, json_obj_get(thenb, "args"))) return false;
    if (!validate_branch_args(p, else_id, json_obj_get(elseb, "args"))) return false;
    return true;
  }

  if (strcmp(t->tag, "term.switch") == 0) {
    if (!t->fields) {
      errf(p, "sircc: term.switch missing fields");
      return false;
    }
    int64_t scrut_id = 0;
    if (!parse_node_ref_id(json_obj_get(t->fields, "scrut"), &scrut_id)) {
      errf(p, "sircc: term.switch missing scrut ref");
      return false;
    }
    if (!get_node(p, scrut_id)) {
      errf(p, "sircc: term.switch scrut references unknown node %lld", (long long)scrut_id);
      return false;
    }
    JsonValue* def = json_obj_get(t->fields, "default");
    if (!def || def->type != JSON_OBJECT) {
      errf(p, "sircc: term.switch missing default branch");
      return false;
    }
    int64_t def_id = 0;
    if (!parse_node_ref_id(json_obj_get(def, "to"), &def_id)) {
      errf(p, "sircc: term.switch default missing to ref");
      return false;
    }
    if (!validate_block_params(p, def_id)) return false;
    if (!validate_branch_args(p, def_id, json_obj_get(def, "args"))) return false;
    JsonValue* cases = json_obj_get(t->fields, "cases");
    if (!cases || cases->type != JSON_ARRAY) {
      errf(p, "sircc: term.switch missing cases array");
      return false;
    }
    for (size_t i = 0; i < cases->v.arr.len; i++) {
      JsonValue* c = cases->v.arr.items[i];
      if (!c || c->type != JSON_OBJECT) {
        errf(p, "sircc: term.switch case[%zu] must be object", i);
        return false;
      }
      int64_t to_id = 0;
      if (!parse_node_ref_id(json_obj_get(c, "to"), &to_id)) {
        errf(p, "sircc: term.switch case[%zu] missing to ref", i);
        return false;
      }
      if (!validate_block_params(p, to_id)) return false;
      if (!validate_branch_args(p, to_id, json_obj_get(c, "args"))) return false;
      int64_t lit_id = 0;
      if (!parse_node_ref_id(json_obj_get(c, "lit"), &lit_id)) {
        errf(p, "sircc: term.switch case[%zu] missing lit ref", i);
        return false;
      }
      NodeRec* litn = get_node(p, lit_id);
      if (!litn || strncmp(litn->tag, "const.", 6) != 0) {
        errf(p, "sircc: term.switch case[%zu] lit must be const.* node", i);
        return false;
      }
    }
    return true;
  }

  return true;
}

static bool validate_cfg_fn(SirProgram* p, NodeRec* fn) {
  JsonValue* blocks = json_obj_get(fn->fields, "blocks");
  JsonValue* entry = json_obj_get(fn->fields, "entry");
  if (!blocks || blocks->type != JSON_ARRAY || !entry) {
    errf(p, "sircc: fn %lld CFG form requires fields.blocks (array) and fields.entry (ref)", (long long)fn->id);
    return false;
  }
  int64_t entry_id = 0;
  if (!parse_node_ref_id(entry, &entry_id)) {
    errf(p, "sircc: fn %lld entry must be a block ref", (long long)fn->id);
    return false;
  }

  // mark blocks in this fn for quick membership
  unsigned char* in_fn = (unsigned char*)calloc(p->nodes_cap ? p->nodes_cap : 1, 1);
  if (!in_fn) return false;
  for (size_t i = 0; i < blocks->v.arr.len; i++) {
    int64_t bid = 0;
    if (!parse_node_ref_id(blocks->v.arr.items[i], &bid)) {
      errf(p, "sircc: fn %lld blocks[%zu] must be block refs", (long long)fn->id, i);
      free(in_fn);
      return false;
    }
    if (bid >= 0 && (size_t)bid < p->nodes_cap) in_fn[bid] = 1;
    if (!validate_block_params(p, bid)) {
      free(in_fn);
      return false;
    }
  }
  if (entry_id < 0 || (size_t)entry_id >= p->nodes_cap || !in_fn[entry_id]) {
    errf(p, "sircc: fn %lld entry block %lld not in blocks list", (long long)fn->id, (long long)entry_id);
    free(in_fn);
    return false;
  }

  for (size_t i = 0; i < blocks->v.arr.len; i++) {
    int64_t bid = 0;
    (void)parse_node_ref_id(blocks->v.arr.items[i], &bid);
    NodeRec* b = get_node(p, bid);
    if (!b || strcmp(b->tag, "block") != 0) {
      errf(p, "sircc: fn %lld blocks[%zu] references non-block %lld", (long long)fn->id, i, (long long)bid);
      free(in_fn);
      return false;
    }
    JsonValue* stmts = b->fields ? json_obj_get(b->fields, "stmts") : NULL;
    if (!stmts || stmts->type != JSON_ARRAY || stmts->v.arr.len == 0) {
      errf(p, "sircc: block %lld must have non-empty stmts array", (long long)bid);
      free(in_fn);
      return false;
    }
    for (size_t si = 0; si < stmts->v.arr.len; si++) {
      int64_t sid = 0;
      if (!parse_node_ref_id(stmts->v.arr.items[si], &sid)) {
        errf(p, "sircc: block %lld stmts[%zu] must be node refs", (long long)bid, si);
        free(in_fn);
        return false;
      }
      NodeRec* sn = get_node(p, sid);
      if (!sn) {
        errf(p, "sircc: block %lld stmts[%zu] references unknown node %lld", (long long)bid, si, (long long)sid);
        free(in_fn);
        return false;
      }
      bool is_term = (strncmp(sn->tag, "term.", 5) == 0) || (strcmp(sn->tag, "return") == 0);
      if (is_term && si + 1 != stmts->v.arr.len) {
        errf(p, "sircc: block %lld has terminator before end (stmt %zu)", (long long)bid, si);
        free(in_fn);
        return false;
      }
      if (si + 1 == stmts->v.arr.len) {
        if (!is_term) {
          errf(p, "sircc: block %lld must end with a terminator (got '%s')", (long long)bid, sn->tag);
          free(in_fn);
          return false;
        }
        if (!validate_terminator(p, sid)) {
          free(in_fn);
          return false;
        }
      }
    }
  }

  free(in_fn);
  return true;
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

static bool type_size_align_rec(SirProgram* p, int64_t type_id, unsigned char* visiting, int64_t* out_size,
                                int64_t* out_align) {
  if (!p || !out_size || !out_align) return false;
  if (type_id < 0 || (size_t)type_id >= p->types_cap || !p->types[type_id]) return false;
  if (visiting && visiting[type_id]) return false;
  if (visiting) visiting[type_id] = 1;

  TypeRec* tr = p->types[type_id];
  int64_t size = 0;
  int64_t align = 0;
  switch (tr->kind) {
    case TYPE_PRIM:
      if (strcmp(tr->prim, "i1") == 0 || strcmp(tr->prim, "bool") == 0) {
        size = 1;
        align = 1;
      } else if (strcmp(tr->prim, "i8") == 0) {
        size = 1;
        align = 1;
      } else if (strcmp(tr->prim, "i16") == 0) {
        size = 2;
        align = 2;
      } else if (strcmp(tr->prim, "i32") == 0) {
        size = 4;
        align = 4;
      } else if (strcmp(tr->prim, "i64") == 0) {
        size = 8;
        align = 8;
      } else if (strcmp(tr->prim, "f32") == 0) {
        size = 4;
        align = 4;
      } else if (strcmp(tr->prim, "f64") == 0) {
        size = 8;
        align = 8;
      } else if (strcmp(tr->prim, "void") == 0) {
        if (visiting) visiting[type_id] = 0;
        return false;
      } else {
        if (visiting) visiting[type_id] = 0;
        return false;
      }
      break;
    case TYPE_PTR:
      size = (int64_t)(p->ptr_bytes ? p->ptr_bytes : (unsigned)sizeof(void*));
      align = (int64_t)(p->ptr_bytes ? p->ptr_bytes : (unsigned)sizeof(void*));
      break;
    case TYPE_ARRAY: {
      int64_t el_size = 0;
      int64_t el_align = 0;
      if (!type_size_align_rec(p, tr->of, visiting, &el_size, &el_align)) {
        if (visiting) visiting[type_id] = 0;
        return false;
      }
      if (el_align <= 0) {
        if (visiting) visiting[type_id] = 0;
        return false;
      }
      int64_t stride = el_size;
      int64_t rem = stride % el_align;
      if (rem) stride += (el_align - rem);
      if (tr->len < 0) {
        if (visiting) visiting[type_id] = 0;
        return false;
      }
      if (tr->len == 0) {
        size = 0;
        align = el_align;
        break;
      }
      if (stride != 0 && tr->len > INT64_MAX / stride) {
        if (visiting) visiting[type_id] = 0;
        return false;
      }
      size = stride * tr->len;
      align = el_align;
      break;
    }
    case TYPE_FN:
    case TYPE_INVALID:
    default:
      if (visiting) visiting[type_id] = 0;
      return false;
  }

  if (visiting) visiting[type_id] = 0;
  if (size < 0 || align <= 0) return false;
  *out_size = size;
  *out_align = align;
  return true;
}

static bool type_size_align(SirProgram* p, int64_t type_id, int64_t* out_size, int64_t* out_align) {
  if (!p || !out_size || !out_align) return false;
  if (type_id < 0 || (size_t)type_id >= p->types_cap || !p->types[type_id]) return false;
  unsigned char* visiting = (unsigned char*)calloc(p->types_cap ? p->types_cap : 1, 1);
  if (!visiting) return false;
  bool ok = type_size_align_rec(p, type_id, visiting, out_size, out_align);
  free(visiting);
  return ok;
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

static LLVMValueRef build_zext_or_trunc(LLVMBuilderRef b, LLVMValueRef v, LLVMTypeRef ty, const char* name) {
  if (!b || !v || !ty) return NULL;
  if (LLVMTypeOf(v) == ty) return v;
  LLVMTypeRef from_ty = LLVMTypeOf(v);
  if (LLVMGetTypeKind(from_ty) != LLVMIntegerTypeKind || LLVMGetTypeKind(ty) != LLVMIntegerTypeKind) {
    return LLVMBuildTruncOrBitCast(b, v, ty, name ? name : "");
  }
  unsigned from_w = LLVMGetIntTypeWidth(from_ty);
  unsigned to_w = LLVMGetIntTypeWidth(ty);
  if (from_w == to_w) return v;
  if (from_w < to_w) return LLVMBuildZExt(b, v, ty, name ? name : "");
  return LLVMBuildTrunc(b, v, ty, name ? name : "");
}

static LLVMValueRef build_sext_or_trunc(LLVMBuilderRef b, LLVMValueRef v, LLVMTypeRef ty, const char* name) {
  if (!b || !v || !ty) return NULL;
  if (LLVMTypeOf(v) == ty) return v;
  LLVMTypeRef from_ty = LLVMTypeOf(v);
  if (LLVMGetTypeKind(from_ty) != LLVMIntegerTypeKind || LLVMGetTypeKind(ty) != LLVMIntegerTypeKind) {
    return LLVMBuildTruncOrBitCast(b, v, ty, name ? name : "");
  }
  unsigned from_w = LLVMGetIntTypeWidth(from_ty);
  unsigned to_w = LLVMGetIntTypeWidth(ty);
  if (from_w == to_w) return v;
  if (from_w < to_w) return LLVMBuildSExt(b, v, ty, name ? name : "");
  return LLVMBuildTrunc(b, v, ty, name ? name : "");
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
    case TYPE_ARRAY: {
      LLVMTypeRef of = lower_type(p, ctx, tr->of);
      if (of && tr->len >= 0 && tr->len <= (int64_t)UINT_MAX) out = LLVMArrayType(of, (unsigned)tr->len);
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

typedef struct BlockBinding {
  int64_t node_id;
  LLVMBasicBlockRef bb;
} BlockBinding;

typedef struct FunctionCtx {
  SirProgram* p;
  LLVMContextRef ctx;
  LLVMModuleRef mod;
  LLVMBuilderRef builder;
  LLVMValueRef fn;

  Binding* binds;
  size_t bind_len;
  size_t bind_cap;

  LLVMBasicBlockRef* blocks_by_node; // indexed by NodeId (node records)
} FunctionCtx;

static LLVMValueRef canonical_qnan(FunctionCtx* f, LLVMTypeRef fty) {
  if (LLVMGetTypeKind(fty) == LLVMFloatTypeKind) {
    LLVMValueRef ib = LLVMConstInt(LLVMInt32TypeInContext(f->ctx), 0x7fc00000u, 0);
    return LLVMConstBitCast(ib, fty);
  }
  if (LLVMGetTypeKind(fty) == LLVMDoubleTypeKind) {
    LLVMValueRef ib = LLVMConstInt(LLVMInt64TypeInContext(f->ctx), 0x7ff8000000000000ULL, 0);
    return LLVMConstBitCast(ib, fty);
  }
  return LLVMGetUndef(fty);
}

static LLVMValueRef canonicalize_float(FunctionCtx* f, LLVMValueRef v) {
  LLVMTypeRef ty = LLVMTypeOf(v);
  if (LLVMGetTypeKind(ty) != LLVMFloatTypeKind && LLVMGetTypeKind(ty) != LLVMDoubleTypeKind) return v;
  LLVMValueRef isnan = LLVMBuildFCmp(f->builder, LLVMRealUNO, v, v, "isnan");
  LLVMValueRef qnan = canonical_qnan(f, ty);
  return LLVMBuildSelect(f->builder, isnan, qnan, v, "canon");
}

static void emit_trap_unreachable(FunctionCtx* f) {
  LLVMTypeRef v = LLVMVoidTypeInContext(f->ctx);
  LLVMValueRef fn = get_or_declare_intrinsic(f->mod, "llvm.trap", v, NULL, 0);
  LLVMBuildCall2(f->builder, LLVMGlobalGetValueType(fn), fn, NULL, 0, "");
  LLVMBuildUnreachable(f->builder);
}

static bool emit_trap_if(FunctionCtx* f, LLVMValueRef cond) {
  if (!f || !f->builder || !f->fn) return false;
  if (!cond || LLVMGetTypeKind(LLVMTypeOf(cond)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(cond)) != 1) return false;

  if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(f->builder))) return false;
  LLVMBasicBlockRef trap_bb = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "trap");
  LLVMBasicBlockRef cont_bb = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "cont");
  LLVMBuildCondBr(f->builder, cond, trap_bb, cont_bb);

  LLVMPositionBuilderAtEnd(f->builder, trap_bb);
  emit_trap_unreachable(f);

  LLVMPositionBuilderAtEnd(f->builder, cont_bb);
  return true;
}

static bool emit_trap_if_misaligned(FunctionCtx* f, LLVMValueRef ptr, unsigned align) {
  if (!f || !ptr) return false;
  if (align <= 1) return true;
  if ((align & (align - 1u)) != 0u) {
    errf(f->p, "sircc: align must be a power of two (got %u)", align);
    return false;
  }

  if (LLVMGetTypeKind(LLVMTypeOf(ptr)) != LLVMPointerTypeKind) {
    errf(f->p, "sircc: internal: alignment check requires ptr");
    return false;
  }

  unsigned ptr_bits = f->p->ptr_bits ? f->p->ptr_bits : (unsigned)(sizeof(void*) * 8u);
  LLVMTypeRef ip = LLVMIntTypeInContext(f->ctx, ptr_bits);
  LLVMValueRef addr = LLVMBuildPtrToInt(f->builder, ptr, ip, "addr.bits");
  LLVMValueRef mask = LLVMConstInt(ip, (unsigned long long)(align - 1u), 0);
  LLVMValueRef low = LLVMBuildAnd(f->builder, addr, mask, "addr.low");
  LLVMValueRef z = LLVMConstInt(ip, 0, 0);
  LLVMValueRef bad = LLVMBuildICmp(f->builder, LLVMIntNE, low, z, "misaligned");
  return emit_trap_if(f, bad);
}

static bool bind_add(FunctionCtx* f, const char* name, LLVMValueRef v) {
  if (!name) return false;
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
  for (size_t i = f->bind_len; i > 0; i--) {
    if (strcmp(f->binds[i - 1].name, name) == 0) return f->binds[i - 1].value;
  }
  return NULL;
}

static size_t bind_mark(FunctionCtx* f) { return f ? f->bind_len : 0; }
static void bind_restore(FunctionCtx* f, size_t mark) {
  if (!f) return;
  if (mark > f->bind_len) return;
  f->bind_len = mark;
}

static LLVMValueRef lower_expr(FunctionCtx* f, int64_t node_id);
static bool lower_stmt(FunctionCtx* f, int64_t node_id);
static bool lower_term_cfg(FunctionCtx* f, int64_t node_id);

static LLVMValueRef lower_expr(FunctionCtx* f, int64_t node_id) {
  NodeRec* n = get_node(f->p, node_id);
  if (!n) {
    errf(f->p, "sircc: unknown node id %lld", (long long)node_id);
    return NULL;
  }
  if ((strcmp(n->tag, "param") == 0 || strcmp(n->tag, "bparam") == 0) && n->llvm_value) {
    return n->llvm_value;
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
          if (strcmp(op, "eqz") == 0) {
            if (b) {
              errf(f->p, "sircc: %s node %lld requires 1 arg", n->tag, (long long)node_id);
              goto done;
            }
            LLVMTypeRef aty = LLVMTypeOf(a);
            if (LLVMGetTypeKind(aty) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(aty) != (unsigned)width) {
              errf(f->p, "sircc: %s requires i%d operand", n->tag, width);
              goto done;
            }
            LLVMValueRef zero = LLVMConstInt(aty, 0, 0);
            out = LLVMBuildICmp(f->builder, LLVMIntEQ, a, zero, "eqz");
            goto done;
          }
          if (strcmp(op, "min.s") == 0 || strcmp(op, "min.u") == 0 || strcmp(op, "max.s") == 0 || strcmp(op, "max.u") == 0) {
            if (!b) {
              errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
              goto done;
            }
            LLVMTypeRef aty = LLVMTypeOf(a);
            LLVMTypeRef bty = LLVMTypeOf(b);
            if (LLVMGetTypeKind(aty) != LLVMIntegerTypeKind || LLVMGetTypeKind(bty) != LLVMIntegerTypeKind ||
                LLVMGetIntTypeWidth(aty) != (unsigned)width || LLVMGetIntTypeWidth(bty) != (unsigned)width) {
              errf(f->p, "sircc: %s requires i%d operands", n->tag, width);
              goto done;
            }
            bool is_min = (strncmp(op, "min.", 4) == 0);
            bool is_signed = (op[4] == 's');
            LLVMIntPredicate pred;
            if (is_min) pred = is_signed ? LLVMIntSLE : LLVMIntULE;
            else pred = is_signed ? LLVMIntSGE : LLVMIntUGE;
            LLVMValueRef cmp = LLVMBuildICmp(f->builder, pred, a, b, "minmax.cmp");
            out = LLVMBuildSelect(f->builder, cmp, a, b, "minmax");
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
              shift = build_zext_or_trunc(f->builder, b, xty, "shift.cast");
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

          if (strcmp(op, "div.s.trap") == 0 || strcmp(op, "div.u.trap") == 0 || strcmp(op, "rem.s.trap") == 0 ||
              strcmp(op, "rem.u.trap") == 0) {
            if (!b) {
              errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
              goto done;
            }
            LLVMTypeRef aty = LLVMTypeOf(a);
            LLVMTypeRef bty = LLVMTypeOf(b);
            if (LLVMGetTypeKind(aty) != LLVMIntegerTypeKind || LLVMGetTypeKind(bty) != LLVMIntegerTypeKind ||
                LLVMGetIntTypeWidth(aty) != (unsigned)width || LLVMGetIntTypeWidth(bty) != (unsigned)width) {
              errf(f->p, "sircc: %s requires i%d operands", n->tag, width);
              goto done;
            }
            LLVMValueRef zero = LLVMConstInt(aty, 0, 0);
            LLVMValueRef b_is_zero = LLVMBuildICmp(f->builder, LLVMIntEQ, b, zero, "b.iszero");
            LLVMValueRef trap_cond = b_is_zero;

            bool is_div = (strncmp(op, "div.", 4) == 0);
            bool is_signed = (op[4] == 's');
            if (is_div && is_signed) {
              unsigned long long min_bits = 1ULL << (unsigned)(width - 1);
              LLVMValueRef minv = LLVMConstInt(aty, min_bits, 0);
              LLVMValueRef neg1 = LLVMConstAllOnes(aty);
              LLVMValueRef a_is_min = LLVMBuildICmp(f->builder, LLVMIntEQ, a, minv, "a.ismin");
              LLVMValueRef b_is_neg1 = LLVMBuildICmp(f->builder, LLVMIntEQ, b, neg1, "b.isneg1");
              LLVMValueRef ov = LLVMBuildAnd(f->builder, a_is_min, b_is_neg1, "div.ov");
              trap_cond = LLVMBuildOr(f->builder, trap_cond, ov, "trap.cond");
            }
            if (!emit_trap_if(f, trap_cond)) goto done;

            if (is_div) {
              out = is_signed ? LLVMBuildSDiv(f->builder, a, b, "div") : LLVMBuildUDiv(f->builder, a, b, "div");
            } else {
              out = is_signed ? LLVMBuildSRem(f->builder, a, b, "rem") : LLVMBuildURem(f->builder, a, b, "rem");
            }
            goto done;
          }

          if (strncmp(op, "trunc_sat_f", 11) == 0) {
            // iN.trunc_sat_f32.s / iN.trunc_sat_f32.u (and f64.*)
            if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
              errf(f->p, "sircc: %s node %lld requires args:[x]", n->tag, (long long)node_id);
              goto done;
            }
            int srcw = 0;
            char su = 0;
            if (sscanf(op, "trunc_sat_f%d.%c", &srcw, &su) != 2 || (srcw != 32 && srcw != 64) || (su != 's' && su != 'u')) {
              errf(f->p, "sircc: unsupported trunc_sat form '%s' in %s", op, n->tag);
              goto done;
            }
            int64_t x_id = 0;
            if (!parse_node_ref_id(args->v.arr.items[0], &x_id)) {
              errf(f->p, "sircc: %s node %lld arg must be node ref", n->tag, (long long)node_id);
              goto done;
            }
            LLVMValueRef x = lower_expr(f, x_id);
            if (!x) goto done;

            LLVMTypeRef ity = LLVMIntTypeInContext(f->ctx, (unsigned)width);
            LLVMTypeRef fty = (srcw == 32) ? LLVMFloatTypeInContext(f->ctx) : LLVMDoubleTypeInContext(f->ctx);
            if (LLVMTypeOf(x) != fty) {
              errf(f->p, "sircc: %s requires f%d operand", n->tag, srcw);
              goto done;
            }
            if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(f->builder))) goto done;

            LLVMBasicBlockRef bb_nan = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.nan");
            LLVMBasicBlockRef bb_chk1 = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.chk1");
            LLVMBasicBlockRef bb_min = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.min");
            LLVMBasicBlockRef bb_chk2 = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.chk2");
            LLVMBasicBlockRef bb_max = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.max");
            LLVMBasicBlockRef bb_conv = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.conv");
            LLVMBasicBlockRef bb_merge = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.merge");

            LLVMValueRef isnan = LLVMBuildFCmp(f->builder, LLVMRealUNO, x, x, "isnan");
            LLVMBuildCondBr(f->builder, isnan, bb_nan, bb_chk1);

            LLVMPositionBuilderAtEnd(f->builder, bb_nan);
            LLVMValueRef z = LLVMConstInt(ity, 0, 0);
            LLVMBuildBr(f->builder, bb_merge);

            LLVMPositionBuilderAtEnd(f->builder, bb_chk1);
            LLVMValueRef min_i = NULL;
            LLVMValueRef max_i = NULL;
            if (su == 's') {
              unsigned long long min_bits = 1ULL << (unsigned)(width - 1);
              min_i = LLVMConstInt(ity, min_bits, 0);
              max_i = LLVMConstInt(ity, min_bits - 1ULL, 0);
              LLVMValueRef min_f = LLVMBuildSIToFP(f->builder, min_i, fty, "min.f");
              LLVMValueRef too_low = LLVMBuildFCmp(f->builder, LLVMRealOLT, x, min_f, "too_low");
              LLVMBuildCondBr(f->builder, too_low, bb_min, bb_chk2);
            } else {
              min_i = LLVMConstInt(ity, 0, 0);
              max_i = LLVMConstAllOnes(ity);
              LLVMValueRef zf = LLVMConstReal(fty, 0.0);
              LLVMValueRef too_low = LLVMBuildFCmp(f->builder, LLVMRealOLE, x, zf, "too_low");
              LLVMBuildCondBr(f->builder, too_low, bb_min, bb_chk2);
            }

            LLVMPositionBuilderAtEnd(f->builder, bb_min);
            LLVMBuildBr(f->builder, bb_merge);

            LLVMPositionBuilderAtEnd(f->builder, bb_chk2);
            LLVMValueRef max_f = (su == 's') ? LLVMBuildSIToFP(f->builder, max_i, fty, "max.f") : LLVMBuildUIToFP(f->builder, max_i, fty, "max.f");
            LLVMValueRef too_high = LLVMBuildFCmp(f->builder, LLVMRealOGE, x, max_f, "too_high");
            LLVMBuildCondBr(f->builder, too_high, bb_max, bb_conv);

            LLVMPositionBuilderAtEnd(f->builder, bb_max);
            LLVMBuildBr(f->builder, bb_merge);

            LLVMPositionBuilderAtEnd(f->builder, bb_conv);
            LLVMValueRef conv = (su == 's') ? LLVMBuildFPToSI(f->builder, x, ity, "fptosi") : LLVMBuildFPToUI(f->builder, x, ity, "fptoui");
            LLVMBuildBr(f->builder, bb_merge);

            LLVMPositionBuilderAtEnd(f->builder, bb_merge);
            LLVMValueRef phi = LLVMBuildPhi(f->builder, ity, "trunc_sat");
            LLVMValueRef inc_vals[4] = {z, min_i, max_i, conv};
            LLVMBasicBlockRef inc_bbs[4] = {bb_nan, bb_min, bb_max, bb_conv};
            LLVMAddIncoming(phi, inc_vals, inc_bbs, 4);
            out = phi;
            goto done;
          }

          if (strcmp(op, "div.s.sat") == 0 || strcmp(op, "div.u.sat") == 0 || strcmp(op, "rem.s.sat") == 0 ||
              strcmp(op, "rem.u.sat") == 0) {
            if (!b) {
              errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
              goto done;
            }
            LLVMTypeRef aty = LLVMTypeOf(a);
            LLVMTypeRef bty = LLVMTypeOf(b);
            if (LLVMGetTypeKind(aty) != LLVMIntegerTypeKind || LLVMGetTypeKind(bty) != LLVMIntegerTypeKind ||
                LLVMGetIntTypeWidth(aty) != (unsigned)width || LLVMGetIntTypeWidth(bty) != (unsigned)width) {
              errf(f->p, "sircc: %s requires i%d operands", n->tag, width);
              goto done;
            }
            if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(f->builder))) goto done;

            bool is_div = (strncmp(op, "div.", 4) == 0);
            bool is_signed = (op[4] == 's');

            LLVMBasicBlockRef cur = LLVMGetInsertBlock(f->builder);
            LLVMBasicBlockRef bb_zero = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.zero");
            LLVMBasicBlockRef bb_chk = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.chk");
            LLVMBasicBlockRef bb_norm = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.norm");
            LLVMBasicBlockRef bb_over = NULL;
            LLVMBasicBlockRef bb_merge = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.merge");

            LLVMValueRef zero = LLVMConstInt(aty, 0, 0);
            LLVMValueRef b_is_zero = LLVMBuildICmp(f->builder, LLVMIntEQ, b, zero, "b.iszero");
            LLVMBuildCondBr(f->builder, b_is_zero, bb_zero, bb_chk);

            // b==0 case: result 0
            LLVMPositionBuilderAtEnd(f->builder, bb_zero);
            LLVMBuildBr(f->builder, bb_merge);

            // check overflow (signed div only), otherwise jump to normal
            LLVMPositionBuilderAtEnd(f->builder, bb_chk);
            if (is_div && is_signed) {
              bb_over = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.over");
              unsigned long long min_bits = 1ULL << (unsigned)(width - 1);
              LLVMValueRef minv = LLVMConstInt(aty, min_bits, 0);
              LLVMValueRef neg1 = LLVMConstAllOnes(aty);
              LLVMValueRef a_is_min = LLVMBuildICmp(f->builder, LLVMIntEQ, a, minv, "a.ismin");
              LLVMValueRef b_is_neg1 = LLVMBuildICmp(f->builder, LLVMIntEQ, b, neg1, "b.isneg1");
              LLVMValueRef ov = LLVMBuildAnd(f->builder, a_is_min, b_is_neg1, "div.ov");
              LLVMBuildCondBr(f->builder, ov, bb_over, bb_norm);

              LLVMPositionBuilderAtEnd(f->builder, bb_over);
              LLVMBuildBr(f->builder, bb_merge);
            } else {
              LLVMBuildBr(f->builder, bb_norm);
            }

            // normal division/rem
            LLVMPositionBuilderAtEnd(f->builder, bb_norm);
            LLVMValueRef norm = NULL;
            if (is_div) {
              norm = is_signed ? LLVMBuildSDiv(f->builder, a, b, "div") : LLVMBuildUDiv(f->builder, a, b, "div");
            } else {
              norm = is_signed ? LLVMBuildSRem(f->builder, a, b, "rem") : LLVMBuildURem(f->builder, a, b, "rem");
            }
            LLVMBuildBr(f->builder, bb_merge);

            // merge
            LLVMPositionBuilderAtEnd(f->builder, bb_merge);
            LLVMValueRef phi = LLVMBuildPhi(f->builder, aty, "sat");
            LLVMValueRef inc_vals[3];
            LLVMBasicBlockRef inc_bbs[3];
            unsigned inc_n = 0;
            inc_vals[inc_n] = zero;
            inc_bbs[inc_n] = bb_zero;
            inc_n++;
            if (bb_over) {
              unsigned long long min_bits = 1ULL << (unsigned)(width - 1);
              LLVMValueRef minv = LLVMConstInt(aty, min_bits, 0);
              inc_vals[inc_n] = minv;
              inc_bbs[inc_n] = bb_over;
              inc_n++;
            }
            inc_vals[inc_n] = norm;
            inc_bbs[inc_n] = bb_norm;
            inc_n++;
            LLVMAddIncoming(phi, inc_vals, inc_bbs, inc_n);
            (void)cur;
            out = phi;
            goto done;
          }

          if (strcmp(op, "rotl") == 0 || strcmp(op, "rotr") == 0) {
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
              errf(f->p, "sircc: %s node %lld requires integer rotate amount", n->tag, (long long)node_id);
              goto done;
            }
            LLVMValueRef amt = b;
            if (LLVMGetIntTypeWidth(sty) != LLVMGetIntTypeWidth(xty)) {
              amt = build_zext_or_trunc(f->builder, b, xty, "rot.cast");
            }
            unsigned mask = (unsigned)(width - 1);
            LLVMValueRef maskv = LLVMConstInt(xty, mask, 0);
            amt = LLVMBuildAnd(f->builder, amt, maskv, "rot.mask");

            char full[32];
            snprintf(full, sizeof(full), "llvm.%s.i%d", (strcmp(op, "rotl") == 0) ? "fshl" : "fshr", width);
            LLVMTypeRef params[3] = {xty, xty, xty};
            LLVMValueRef fn = get_or_declare_intrinsic(f->mod, full, xty, params, 3);
            LLVMValueRef argv[3] = {a, a, amt};
            out = LLVMBuildCall2(f->builder, LLVMGlobalGetValueType(fn), fn, argv, 3, "rot");
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
            out = LLVMBuildCall2(f->builder, LLVMGlobalGetValueType(fn), fn, argsv, 2, op);
            goto done;
          }

          if (strcmp(op, "popc") == 0) {
            char full[32];
            snprintf(full, sizeof(full), "llvm.ctpop.i%d", width);
            LLVMTypeRef ity = LLVMTypeOf(a);
            LLVMTypeRef params[1] = {ity};
            LLVMValueRef fn = get_or_declare_intrinsic(f->mod, full, ity, params, 1);
            LLVMValueRef argsv[1] = {a};
            out = LLVMBuildCall2(f->builder, LLVMGlobalGetValueType(fn), fn, argsv, 1, "popc");
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
    int64_t ty_id = 0;
    bool has_ty = false;
    if (n->fields) {
      JsonValue* tyv = json_obj_get(n->fields, "ty");
      if (tyv && parse_type_ref_id(tyv, &ty_id)) has_ty = true;
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
    if (LLVMGetTypeKind(LLVMTypeOf(c)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(c)) != 1) {
      errf(f->p, "sircc: select node %lld cond must be bool", (long long)node_id);
      goto done;
    }
    if (LLVMTypeOf(tv) != LLVMTypeOf(ev)) {
      errf(f->p, "sircc: select node %lld then/else types must match", (long long)node_id);
      goto done;
    }
    if (n->type_ref) {
      LLVMTypeRef want = lower_type(f->p, f->ctx, n->type_ref);
      if (!want || want != LLVMTypeOf(tv)) {
        errf(f->p, "sircc: select node %lld type_ref does not match operand type", (long long)node_id);
        goto done;
      }
    }
    if (has_ty) {
      LLVMTypeRef want = lower_type(f->p, f->ctx, ty_id);
      if (!want || want != LLVMTypeOf(tv)) {
        errf(f->p, "sircc: select node %lld ty does not match operand type", (long long)node_id);
        goto done;
      }
    }
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

    LLVMTypeRef callee_fty = LLVMGlobalGetValueType(callee);
    if (LLVMGetTypeKind(callee_fty) != LLVMFunctionTypeKind) {
      errf(f->p, "sircc: call node %lld callee is not a function pointer", (long long)node_id);
      free(argv);
      goto done;
    }

    unsigned param_count = LLVMCountParamTypes(callee_fty);
    bool is_varargs = LLVMIsFunctionVarArg(callee_fty) != 0;
    if (!is_varargs && (unsigned)argc != param_count) {
      errf(f->p, "sircc: call node %lld arg count mismatch (got %zu, want %u)", (long long)node_id, argc, param_count);
      free(argv);
      goto done;
    }
    if ((unsigned)argc < param_count) {
      errf(f->p, "sircc: call node %lld missing required args (got %zu, want >= %u)", (long long)node_id, argc, param_count);
      free(argv);
      goto done;
    }

    if (param_count) {
      LLVMTypeRef* params = (LLVMTypeRef*)malloc(param_count * sizeof(LLVMTypeRef));
      if (!params) {
        free(argv);
        goto done;
      }
      LLVMGetParamTypes(callee_fty, params);
      for (unsigned i = 0; i < param_count; i++) {
        LLVMTypeRef want = params[i];
        LLVMTypeRef got = LLVMTypeOf(argv[i]);
        if (want == got) continue;
        if (LLVMGetTypeKind(want) == LLVMPointerTypeKind && LLVMGetTypeKind(got) == LLVMPointerTypeKind) {
          argv[i] = LLVMBuildBitCast(f->builder, argv[i], want, "arg.cast");
          continue;
        }
        free(params);
        errf(f->p, "sircc: call node %lld arg[%u] type mismatch", (long long)node_id, i);
        free(argv);
        goto done;
      }
      free(params);
    }

    out = LLVMBuildCall2(f->builder, callee_fty, callee, argv, (unsigned)argc, "call");
    free(argv);
    if (out && n->type_ref) {
      LLVMTypeRef want = lower_type(f->p, f->ctx, n->type_ref);
      if (want && want != LLVMTypeOf(out)) {
        errf(f->p, "sircc: call node %lld return type does not match type_ref", (long long)node_id);
        out = NULL;
        goto done;
      }
    }
    goto done;
  }

  if (strcmp(n->tag, "call.indirect") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: call.indirect node %lld missing fields", (long long)node_id);
      goto done;
    }

    int64_t sig_id = 0;
    if (!parse_type_ref_id(json_obj_get(n->fields, "sig"), &sig_id)) {
      errf(f->p, "sircc: call.indirect node %lld missing fields.sig (fn type ref)", (long long)node_id);
      goto done;
    }
    LLVMTypeRef callee_fty = lower_type(f->p, f->ctx, sig_id);
    if (!callee_fty || LLVMGetTypeKind(callee_fty) != LLVMFunctionTypeKind) {
      errf(f->p, "sircc: call.indirect node %lld fields.sig must reference a fn type", (long long)node_id);
      goto done;
    }

    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len < 1) {
      errf(f->p, "sircc: call.indirect node %lld requires args:[callee_ptr, ...]", (long long)node_id);
      goto done;
    }

    int64_t callee_id = 0;
    if (!parse_node_ref_id(args->v.arr.items[0], &callee_id)) {
      errf(f->p, "sircc: call.indirect node %lld args[0] must be callee ptr ref", (long long)node_id);
      goto done;
    }
    LLVMValueRef callee = lower_expr(f, callee_id);
    if (!callee) goto done;
    if (LLVMGetTypeKind(LLVMTypeOf(callee)) != LLVMPointerTypeKind) {
      errf(f->p, "sircc: call.indirect node %lld callee must be a ptr", (long long)node_id);
      goto done;
    }

    size_t argc = args->v.arr.len - 1;
    LLVMValueRef* argv = NULL;
    if (argc) {
      argv = (LLVMValueRef*)malloc(argc * sizeof(LLVMValueRef));
      if (!argv) goto done;
      for (size_t i = 0; i < argc; i++) {
        int64_t aid = 0;
        if (!parse_node_ref_id(args->v.arr.items[i + 1], &aid)) {
          errf(f->p, "sircc: call.indirect node %lld arg[%zu] must be node ref", (long long)node_id, i);
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

    unsigned param_count = LLVMCountParamTypes(callee_fty);
    bool is_varargs = LLVMIsFunctionVarArg(callee_fty) != 0;
    if (!is_varargs && (unsigned)argc != param_count) {
      errf(f->p, "sircc: call.indirect node %lld arg count mismatch (got %zu, want %u)", (long long)node_id, argc, param_count);
      free(argv);
      goto done;
    }
    if ((unsigned)argc < param_count) {
      errf(f->p, "sircc: call.indirect node %lld missing required args (got %zu, want >= %u)", (long long)node_id, argc, param_count);
      free(argv);
      goto done;
    }

    if (param_count) {
      LLVMTypeRef* params = (LLVMTypeRef*)malloc(param_count * sizeof(LLVMTypeRef));
      if (!params) {
        free(argv);
        goto done;
      }
      LLVMGetParamTypes(callee_fty, params);
      for (unsigned i = 0; i < param_count; i++) {
        LLVMTypeRef want = params[i];
        LLVMTypeRef got = LLVMTypeOf(argv[i]);
        if (want == got) continue;
        if (LLVMGetTypeKind(want) == LLVMPointerTypeKind && LLVMGetTypeKind(got) == LLVMPointerTypeKind) {
          argv[i] = LLVMBuildBitCast(f->builder, argv[i], want, "arg.cast");
          continue;
        }
        free(params);
        errf(f->p, "sircc: call.indirect node %lld arg[%u] type mismatch", (long long)node_id, i);
        free(argv);
        goto done;
      }
      free(params);
    }

    out = LLVMBuildCall2(f->builder, callee_fty, callee, argv, (unsigned)argc, "call");
    free(argv);

    if (out && n->type_ref) {
      LLVMTypeRef want = lower_type(f->p, f->ctx, n->type_ref);
      if (want && want != LLVMTypeOf(out)) {
        errf(f->p, "sircc: call.indirect node %lld return type does not match type_ref", (long long)node_id);
        out = NULL;
        goto done;
      }
    }

    goto done;
  }

  if (strncmp(n->tag, "ptr.", 4) == 0) {
    const char* op = n->tag + 4;
    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;

    if (strcmp(op, "sym") == 0) {
      const char* name = NULL;
      if (n->fields) name = json_get_string(json_obj_get(n->fields, "name"));
      if (!name && args && args->type == JSON_ARRAY && args->v.arr.len == 1) {
        int64_t aid = 0;
        if (parse_node_ref_id(args->v.arr.items[0], &aid)) {
          NodeRec* an = get_node(f->p, aid);
          if (an && strcmp(an->tag, "name") == 0 && an->fields) {
            name = json_get_string(json_obj_get(an->fields, "name"));
          }
        }
      }
      if (!name) {
        errf(f->p, "sircc: ptr.sym node %lld requires fields.name or args:[name]", (long long)node_id);
        goto done;
      }
      LLVMValueRef fn = LLVMGetNamedFunction(f->mod, name);
      if (!fn) {
        errf(f->p, "sircc: ptr.sym references unknown function '%s'", name);
        goto done;
      }
      out = fn; // function values are pointers in LLVM
      goto done;
    }

    if (strcmp(op, "sizeof") == 0 || strcmp(op, "alignof") == 0 || strcmp(op, "offset") == 0) {
      if (!n->fields) {
        errf(f->p, "sircc: %s node %lld missing fields", n->tag, (long long)node_id);
        goto done;
      }
      int64_t ty_id = 0;
      if (!parse_type_ref_id(json_obj_get(n->fields, "ty"), &ty_id)) {
        errf(f->p, "sircc: %s node %lld missing fields.ty (type ref)", n->tag, (long long)node_id);
        goto done;
      }
      int64_t size = 0;
      int64_t align = 0;
      if (!type_size_align(f->p, ty_id, &size, &align)) {
        errf(f->p, "sircc: %s node %lld has invalid/unsized type %lld", n->tag, (long long)node_id, (long long)ty_id);
        goto done;
      }

      if (!args || args->type != JSON_ARRAY) {
        errf(f->p, "sircc: %s node %lld missing args array", n->tag, (long long)node_id);
        goto done;
      }

      if (strcmp(op, "sizeof") == 0) {
        if (args->v.arr.len != 0) {
          errf(f->p, "sircc: %s node %lld requires args:[]", n->tag, (long long)node_id);
          goto done;
        }
        out = LLVMConstInt(LLVMInt64TypeInContext(f->ctx), (unsigned long long)size, 0);
        goto done;
      }

      if (strcmp(op, "alignof") == 0) {
        if (args->v.arr.len != 0) {
          errf(f->p, "sircc: %s node %lld requires args:[]", n->tag, (long long)node_id);
          goto done;
        }
        out = LLVMConstInt(LLVMInt32TypeInContext(f->ctx), (unsigned long long)align, 0);
        goto done;
      }

      if (strcmp(op, "offset") == 0) {
        if (args->v.arr.len != 2) {
          errf(f->p, "sircc: %s node %lld requires args:[base,index]", n->tag, (long long)node_id);
          goto done;
        }
        int64_t base_id = 0, idx_id = 0;
        if (!parse_node_ref_id(args->v.arr.items[0], &base_id) || !parse_node_ref_id(args->v.arr.items[1], &idx_id)) {
          errf(f->p, "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
          goto done;
        }
        LLVMValueRef base = lower_expr(f, base_id);
        LLVMValueRef idx = lower_expr(f, idx_id);
        if (!base || !idx) goto done;
        if (LLVMGetTypeKind(LLVMTypeOf(base)) != LLVMPointerTypeKind) {
          errf(f->p, "sircc: %s requires ptr base", n->tag);
          goto done;
        }
        if (LLVMGetTypeKind(LLVMTypeOf(idx)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(idx)) != 64) {
          errf(f->p, "sircc: %s requires i64 index", n->tag);
          goto done;
        }

        unsigned ptr_bits = f->p->ptr_bits ? f->p->ptr_bits : (unsigned)(sizeof(void*) * 8u);
        LLVMTypeRef ip = LLVMIntTypeInContext(f->ctx, ptr_bits);
        LLVMValueRef base_bits = LLVMBuildPtrToInt(f->builder, base, ip, "base.bits");
        LLVMValueRef idx_bits = LLVMBuildTruncOrBitCast(f->builder, idx, ip, "idx.bits");
        LLVMValueRef scale = LLVMConstInt(ip, (unsigned long long)size, 0);
        LLVMValueRef off_bits = LLVMBuildMul(f->builder, idx_bits, scale, "off.bits");
        LLVMValueRef sum_bits = LLVMBuildAdd(f->builder, base_bits, off_bits, "addr.bits");
        out = LLVMBuildIntToPtr(f->builder, sum_bits, LLVMTypeOf(base), "ptr.off");
        goto done;
      }
    }

    if (!args || args->type != JSON_ARRAY) {
      errf(f->p, "sircc: %s node %lld missing args array", n->tag, (long long)node_id);
      goto done;
    }

    if (strcmp(op, "cmp.eq") == 0 || strcmp(op, "cmp.ne") == 0) {
      if (args->v.arr.len != 2) {
        errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
        goto done;
      }
      int64_t a_id = 0, b_id = 0;
      if (!parse_node_ref_id(args->v.arr.items[0], &a_id) || !parse_node_ref_id(args->v.arr.items[1], &b_id)) {
        errf(f->p, "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
        goto done;
      }
      LLVMValueRef a = lower_expr(f, a_id);
      LLVMValueRef b = lower_expr(f, b_id);
      if (!a || !b) goto done;
      if (LLVMGetTypeKind(LLVMTypeOf(a)) == LLVMPointerTypeKind && LLVMGetTypeKind(LLVMTypeOf(b)) == LLVMPointerTypeKind &&
          LLVMTypeOf(a) != LLVMTypeOf(b)) {
        LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
        a = LLVMBuildBitCast(f->builder, a, i8p, "pcmp.a");
        b = LLVMBuildBitCast(f->builder, b, i8p, "pcmp.b");
      }
      LLVMIntPredicate pred = (strcmp(op, "cmp.eq") == 0) ? LLVMIntEQ : LLVMIntNE;
      out = LLVMBuildICmp(f->builder, pred, a, b, "pcmp");
      goto done;
    }

    if (strcmp(op, "add") == 0 || strcmp(op, "sub") == 0) {
      if (args->v.arr.len != 2) {
        errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
        goto done;
      }
      int64_t p_id = 0, off_id = 0;
      if (!parse_node_ref_id(args->v.arr.items[0], &p_id) || !parse_node_ref_id(args->v.arr.items[1], &off_id)) {
        errf(f->p, "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
        goto done;
      }
      LLVMValueRef pval = lower_expr(f, p_id);
      LLVMValueRef oval = lower_expr(f, off_id);
      if (!pval || !oval) goto done;
      LLVMTypeRef pty = LLVMTypeOf(pval);
      if (LLVMGetTypeKind(pty) != LLVMPointerTypeKind) {
        errf(f->p, "sircc: %s requires pointer lhs", n->tag);
        goto done;
      }
      if (LLVMGetTypeKind(LLVMTypeOf(oval)) != LLVMIntegerTypeKind) {
        errf(f->p, "sircc: %s requires integer byte offset rhs", n->tag);
        goto done;
      }
      LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
      LLVMValueRef p8 = LLVMBuildBitCast(f->builder, pval, i8p, "p8");
      LLVMValueRef off = oval;
      LLVMTypeRef i64 = LLVMInt64TypeInContext(f->ctx);
      if (LLVMGetIntTypeWidth(LLVMTypeOf(off)) != 64) {
        off = build_sext_or_trunc(f->builder, off, i64, "off64");
      }
      if (strcmp(op, "sub") == 0) {
        off = LLVMBuildNeg(f->builder, off, "off.neg");
      }
      LLVMValueRef idx[1] = {off};
      LLVMValueRef gep = LLVMBuildGEP2(f->builder, LLVMInt8TypeInContext(f->ctx), p8, idx, 1, "p.gep");
      out = LLVMBuildBitCast(f->builder, gep, pty, "p.cast");
      goto done;
    }

    if (strcmp(op, "to_i64") == 0 || strcmp(op, "from_i64") == 0) {
      if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
        errf(f->p, "sircc: %s node %lld requires args:[x]", n->tag, (long long)node_id);
        goto done;
      }
      int64_t x_id = 0;
      if (!parse_node_ref_id(args->v.arr.items[0], &x_id)) {
        errf(f->p, "sircc: %s node %lld arg must be node ref", n->tag, (long long)node_id);
        goto done;
      }
      LLVMValueRef x = lower_expr(f, x_id);
      if (!x) goto done;

      LLVMTypeRef i64 = LLVMInt64TypeInContext(f->ctx);
      unsigned ptr_bits = f->p->ptr_bits ? f->p->ptr_bits : (unsigned)(sizeof(void*) * 8u);
      LLVMTypeRef ip = LLVMIntTypeInContext(f->ctx, ptr_bits);
      LLVMTypeRef pty = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);

      if (strcmp(op, "to_i64") == 0) {
        if (LLVMGetTypeKind(LLVMTypeOf(x)) != LLVMPointerTypeKind) {
          errf(f->p, "sircc: ptr.to_i64 requires ptr operand");
          goto done;
        }
        LLVMValueRef bits = LLVMBuildPtrToInt(f->builder, x, ip, "ptr.bits");
        out = build_zext_or_trunc(f->builder, bits, i64, "ptr.i64");
        goto done;
      }

      if (LLVMGetTypeKind(LLVMTypeOf(x)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(x)) != 64) {
        errf(f->p, "sircc: ptr.from_i64 requires i64 operand");
        goto done;
      }
      LLVMValueRef bits = LLVMBuildTruncOrBitCast(f->builder, x, ip, "i64.ptrbits");
      out = LLVMBuildIntToPtr(f->builder, bits, pty, "ptr");
      goto done;
    }
  }

  if (strcmp(n->tag, "alloca") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: alloca node %lld missing fields", (long long)node_id);
      goto done;
    }
    int64_t ty_id = 0;
    if (!parse_type_ref_id(json_obj_get(n->fields, "ty"), &ty_id)) {
      errf(f->p, "sircc: alloca node %lld missing fields.ty (type ref)", (long long)node_id);
      goto done;
    }

    int64_t el_size = 0;
    int64_t el_align = 0;
    if (!type_size_align(f->p, ty_id, &el_size, &el_align)) {
      errf(f->p, "sircc: alloca node %lld has invalid/unsized element type %lld", (long long)node_id, (long long)ty_id);
      goto done;
    }

    LLVMTypeRef el = lower_type(f->p, f->ctx, ty_id);
    if (!el) {
      errf(f->p, "sircc: alloca node %lld has invalid element type %lld", (long long)node_id, (long long)ty_id);
      goto done;
    }

    // Parse flags: count?:i64, align?:i32, zero?:bool
    int64_t align_i64 = 0;
    bool align_present = false;
    bool zero_init = false;
    LLVMValueRef count_val = NULL;
    JsonValue* flags = json_obj_get(n->fields, "flags");
    if (flags && flags->type == JSON_OBJECT) {
      JsonValue* av = json_obj_get(flags, "align");
      if (av) {
        align_present = true;
        if (!json_get_i64(av, &align_i64)) {
          errf(f->p, "sircc: alloca node %lld flags.align must be an integer", (long long)node_id);
          goto done;
        }
      }
      JsonValue* zv = json_obj_get(flags, "zero");
      if (zv && zv->type == JSON_BOOL) zero_init = zv->v.b;
    }
    JsonValue* countv = (flags && flags->type == JSON_OBJECT) ? json_obj_get(flags, "count") : NULL;
    if (!countv) countv = json_obj_get(n->fields, "count");
    JsonValue* alignv = json_obj_get(n->fields, "align");
    if (alignv) {
      align_present = true;
      if (!json_get_i64(alignv, &align_i64)) {
        errf(f->p, "sircc: alloca node %lld align must be an integer", (long long)node_id);
        goto done;
      }
    }
    JsonValue* zerov = json_obj_get(n->fields, "zero");
    if (zerov && zerov->type == JSON_BOOL) zero_init = zerov->v.b;

    LLVMTypeRef i64 = LLVMInt64TypeInContext(f->ctx);
    if (!countv) {
      count_val = LLVMConstInt(i64, 1, 0);
    } else {
      int64_t c = 0;
      if (json_get_i64(countv, &c)) {
        if (c < 0) {
          errf(f->p, "sircc: alloca node %lld count must be >= 0", (long long)node_id);
          goto done;
        }
        count_val = LLVMConstInt(i64, (unsigned long long)c, 0);
      } else {
        int64_t cid = 0;
        if (!parse_node_ref_id(countv, &cid)) {
          errf(f->p, "sircc: alloca node %lld count must be i64 or node ref", (long long)node_id);
          goto done;
        }
        count_val = lower_expr(f, cid);
        if (!count_val) goto done;
        if (LLVMGetTypeKind(LLVMTypeOf(count_val)) != LLVMIntegerTypeKind) {
          errf(f->p, "sircc: alloca node %lld count ref must be integer", (long long)node_id);
          goto done;
        }
        if (LLVMGetIntTypeWidth(LLVMTypeOf(count_val)) != 64) {
          count_val = build_zext_or_trunc(f->builder, count_val, i64, "count.i64");
        }
      }
    }

    LLVMValueRef alloca_i = NULL;
    bool is_one = false;
    if (LLVMIsAConstantInt(count_val)) {
      unsigned long long z = LLVMConstIntGetZExtValue(count_val);
      is_one = (z == 1);
    }
    if (is_one) {
      alloca_i = LLVMBuildAlloca(f->builder, el, "alloca");
    } else {
      alloca_i = LLVMBuildArrayAlloca(f->builder, el, count_val, "alloca");
    }
    if (!alloca_i) goto done;

    unsigned align = 0;
    if (align_present) {
      if (align_i64 <= 0 || align_i64 > (int64_t)UINT_MAX) {
        errf(f->p, "sircc: alloca node %lld align must be > 0", (long long)node_id);
        goto done;
      }
      align = (unsigned)align_i64;
    } else if (el_align > 0) {
      align = (unsigned)el_align;
    }
    if (align) LLVMSetAlignment(alloca_i, align);

    if (zero_init) {
      LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
      LLVMValueRef dst = LLVMBuildBitCast(f->builder, alloca_i, i8p, "alloca.i8p");
      LLVMValueRef byte = LLVMConstInt(LLVMInt8TypeInContext(f->ctx), 0, 0);
      LLVMValueRef bytes = LLVMConstInt(i64, (unsigned long long)el_size, 0);
      if (!is_one) bytes = LLVMBuildMul(f->builder, count_val, bytes, "alloca.bytes");
      LLVMBuildMemSet(f->builder, dst, byte, bytes, align ? align : 1);
    }

    // SIR mnemonic returns `ptr` (opaque). Represent as i8*.
    LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
    alloca_i = LLVMBuildBitCast(f->builder, alloca_i, i8p, "alloca.ptr");

    out = alloca_i;
    goto done;
  }

  if (strncmp(n->tag, "alloca.", 7) == 0) {
    const char* tname = n->tag + 7;
    LLVMTypeRef el = NULL;
    if (strcmp(tname, "ptr") == 0) {
      el = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
    } else {
      el = lower_type_prim(f->ctx, tname);
    }
    if (!el) {
      errf(f->p, "sircc: unsupported alloca type '%s'", tname);
      goto done;
    }
    out = LLVMBuildAlloca(f->builder, el, "alloca");
    goto done;
  }

  if (strncmp(n->tag, "load.", 5) == 0) {
    const char* tname = n->tag + 5;
    if (!n->fields) {
      errf(f->p, "sircc: %s node %lld missing fields", n->tag, (long long)node_id);
      goto done;
    }
    JsonValue* addr = json_obj_get(n->fields, "addr");
    int64_t aid = 0;
    if (!parse_node_ref_id(addr, &aid)) {
      errf(f->p, "sircc: %s node %lld missing fields.addr ref", n->tag, (long long)node_id);
      goto done;
    }
    LLVMValueRef pval = lower_expr(f, aid);
    if (!pval) goto done;
    LLVMTypeRef pty = LLVMTypeOf(pval);
    if (LLVMGetTypeKind(pty) != LLVMPointerTypeKind) {
      errf(f->p, "sircc: %s requires pointer addr", n->tag);
      goto done;
    }
    LLVMTypeRef el = NULL;
    if (strcmp(tname, "ptr") == 0) {
      el = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
    } else {
      el = lower_type_prim(f->ctx, tname);
    }
    if (!el) {
      errf(f->p, "sircc: unsupported load type '%s'", tname);
      goto done;
    }
    LLVMTypeRef want_ptr = LLVMPointerType(el, 0);
    if (want_ptr != pty) {
      pval = LLVMBuildBitCast(f->builder, pval, want_ptr, "ld.cast");
    }
    JsonValue* alignv = json_obj_get(n->fields, "align");
    unsigned align = 1;
    if (alignv) {
      int64_t a = 0;
      if (!json_get_i64(alignv, &a)) {
        errf(f->p, "sircc: %s node %lld align must be an integer", n->tag, (long long)node_id);
        goto done;
      }
      if (a <= 0 || a > (int64_t)UINT_MAX) {
        errf(f->p, "sircc: %s node %lld align must be > 0", n->tag, (long long)node_id);
        goto done;
      }
      align = (unsigned)a;
    }
    if ((align & (align - 1u)) != 0u) {
      errf(f->p, "sircc: %s node %lld align must be a power of two", n->tag, (long long)node_id);
      goto done;
    }
    if (!emit_trap_if_misaligned(f, pval, align)) goto done;
    out = LLVMBuildLoad2(f->builder, el, pval, "load");
    LLVMSetAlignment(out, align);
    JsonValue* volv = json_obj_get(n->fields, "vol");
    if (volv && volv->type == JSON_BOOL) LLVMSetVolatile(out, volv->v.b ? 1 : 0);
    if (LLVMGetTypeKind(el) == LLVMFloatTypeKind || LLVMGetTypeKind(el) == LLVMDoubleTypeKind) {
      out = canonicalize_float(f, out);
    }
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
      out = canonicalize_float(f, LLVMBuildFAdd(f->builder, a, b, "fadd"));
      goto done;
    }
    if (strcmp(op, "sub") == 0) {
      if (!b) {
        errf(f->p, "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      out = canonicalize_float(f, LLVMBuildFSub(f->builder, a, b, "fsub"));
      goto done;
    }
    if (strcmp(op, "mul") == 0) {
      if (!b) {
        errf(f->p, "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      out = canonicalize_float(f, LLVMBuildFMul(f->builder, a, b, "fmul"));
      goto done;
    }
    if (strcmp(op, "div") == 0) {
      if (!b) {
        errf(f->p, "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      out = canonicalize_float(f, LLVMBuildFDiv(f->builder, a, b, "fdiv"));
      goto done;
    }
    if (strcmp(op, "neg") == 0) {
      out = canonicalize_float(f, LLVMBuildFNeg(f->builder, a, "fneg"));
      goto done;
    }
    if (strcmp(op, "abs") == 0) {
      char full[32];
      snprintf(full, sizeof(full), "llvm.fabs.f%d", width);
      LLVMTypeRef params[1] = {fty};
      LLVMValueRef fn = get_or_declare_intrinsic(f->mod, full, fty, params, 1);
      LLVMValueRef argsv[1] = {a};
      out = canonicalize_float(f, LLVMBuildCall2(f->builder, LLVMGlobalGetValueType(fn), fn, argsv, 1, "fabs"));
      goto done;
    }
    if (strcmp(op, "sqrt") == 0) {
      char full[32];
      snprintf(full, sizeof(full), "llvm.sqrt.f%d", width);
      LLVMTypeRef params[1] = {fty};
      LLVMValueRef fn = get_or_declare_intrinsic(f->mod, full, fty, params, 1);
      LLVMValueRef argsv[1] = {a};
      out = canonicalize_float(f, LLVMBuildCall2(f->builder, LLVMGlobalGetValueType(fn), fn, argsv, 1, "fsqrt"));
      goto done;
    }

    if (strcmp(op, "min") == 0 || strcmp(op, "max") == 0) {
      if (!b) {
        errf(f->p, "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      LLVMValueRef isnan_a = LLVMBuildFCmp(f->builder, LLVMRealUNO, a, a, "isnan.a");
      LLVMValueRef isnan_b = LLVMBuildFCmp(f->builder, LLVMRealUNO, b, b, "isnan.b");
      LLVMValueRef anynan = LLVMBuildOr(f->builder, isnan_a, isnan_b, "isnan.any");
      LLVMValueRef qnan = canonical_qnan(f, fty);

      LLVMRealPredicate pred = (strcmp(op, "min") == 0) ? LLVMRealOLT : LLVMRealOGT;
      LLVMValueRef cmp = LLVMBuildFCmp(f->builder, pred, a, b, "fcmp");
      LLVMValueRef sel = LLVMBuildSelect(f->builder, cmp, a, b, "fsel");
      out = LLVMBuildSelect(f->builder, anynan, qnan, sel, "fminmax");
      goto done;
    }

    if (strncmp(op, "cmp.", 4) == 0) {
      if (!b) {
        errf(f->p, "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      const char* cc = op + 4;
      LLVMRealPredicate pred;
      if (strcmp(cc, "oeq") == 0) pred = LLVMRealOEQ;
      else if (strcmp(cc, "one") == 0) pred = LLVMRealONE;
      else if (strcmp(cc, "olt") == 0) pred = LLVMRealOLT;
      else if (strcmp(cc, "ole") == 0) pred = LLVMRealOLE;
      else if (strcmp(cc, "ogt") == 0) pred = LLVMRealOGT;
      else if (strcmp(cc, "oge") == 0) pred = LLVMRealOGE;
      else if (strcmp(cc, "ueq") == 0) pred = LLVMRealUEQ;
      else if (strcmp(cc, "une") == 0) pred = LLVMRealUNE;
      else if (strcmp(cc, "ult") == 0) pred = LLVMRealULT;
      else if (strcmp(cc, "ule") == 0) pred = LLVMRealULE;
      else if (strcmp(cc, "ugt") == 0) pred = LLVMRealUGT;
      else if (strcmp(cc, "uge") == 0) pred = LLVMRealUGE;
      else {
        errf(f->p, "sircc: unsupported float compare '%s' in %s", cc, n->tag);
        goto done;
      }
      out = LLVMBuildFCmp(f->builder, pred, a, b, "fcmp");
      goto done;
    }

    if (strncmp(op, "from_i", 6) == 0) {
      if (!a || b) {
        errf(f->p, "sircc: %s requires args:[x]", n->tag);
        goto done;
      }
      int srcw = 0;
      char su = 0;
      if (sscanf(op, "from_i%d.%c", &srcw, &su) != 2 || (srcw != 32 && srcw != 64) || (su != 's' && su != 'u')) {
        errf(f->p, "sircc: unsupported int->float conversion '%s' in %s", op, n->tag);
        goto done;
      }
      if (LLVMGetTypeKind(LLVMTypeOf(a)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(a)) != (unsigned)srcw) {
        errf(f->p, "sircc: %s requires i%d operand", n->tag, srcw);
        goto done;
      }
      LLVMTypeRef fty = (width == 32) ? LLVMFloatTypeInContext(f->ctx) : LLVMDoubleTypeInContext(f->ctx);
      out = (su == 's') ? LLVMBuildSIToFP(f->builder, a, fty, "sitofp") : LLVMBuildUIToFP(f->builder, a, fty, "uitofp");
      goto done;
    }
  }

  if (strncmp(n->tag, "const.", 6) == 0) {
    const char* tyname = n->tag + 6;
    if (!n->fields) goto done;
    LLVMTypeRef ty = lower_type_prim(f->ctx, tyname);
    if (!ty) {
      errf(f->p, "sircc: unsupported const type '%s'", tyname);
      goto done;
    }
    if (LLVMGetTypeKind(ty) == LLVMIntegerTypeKind) {
      int64_t value = 0;
      if (!must_i64(f->p, json_obj_get(n->fields, "value"), &value, "const.value")) goto done;
      out = LLVMConstInt(ty, (unsigned long long)value, 1);
      goto done;
    }
    if (LLVMGetTypeKind(ty) == LLVMFloatTypeKind || LLVMGetTypeKind(ty) == LLVMDoubleTypeKind) {
      // Prefer exact bit-pattern constants: fields.bits = "0x..." (hex).
      const char* bits = json_get_string(json_obj_get(n->fields, "bits"));
      if (!bits || strncmp(bits, "0x", 2) != 0) {
        errf(f->p, "sircc: const.%s requires fields.bits hex string (0x...)", tyname);
        goto done;
      }
      char* end = NULL;
      unsigned long long raw = strtoull(bits + 2, &end, 16);
      if (!end || *end != 0) {
        errf(f->p, "sircc: const.%s invalid bits '%s'", tyname, bits);
        goto done;
      }
      if (LLVMGetTypeKind(ty) == LLVMFloatTypeKind) {
        LLVMValueRef ib = LLVMConstInt(LLVMInt32TypeInContext(f->ctx), raw & 0xFFFFFFFFu, 0);
        out = LLVMConstBitCast(ib, ty);
      } else {
        LLVMValueRef ib = LLVMConstInt(LLVMInt64TypeInContext(f->ctx), raw, 0);
        out = LLVMConstBitCast(ib, ty);
      }
      goto done;
    }
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

  if (strcmp(n->tag, "let") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: let node %lld missing fields", (long long)node_id);
      return false;
    }
    const char* name = json_get_string(json_obj_get(n->fields, "name"));
    if (!name) {
      errf(f->p, "sircc: let node %lld missing fields.name", (long long)node_id);
      return false;
    }
    int64_t vid = 0;
    if (!parse_node_ref_id(json_obj_get(n->fields, "value"), &vid)) {
      errf(f->p, "sircc: let node %lld missing fields.value ref", (long long)node_id);
      return false;
    }
    LLVMValueRef v = lower_expr(f, vid);
    if (!v) return false;
    if (!bind_add(f, name, v)) return false;
    return true;
  }

  if (strncmp(n->tag, "store.", 6) == 0) {
    const char* tname = n->tag + 6;
    if (!n->fields) {
      errf(f->p, "sircc: %s node %lld missing fields", n->tag, (long long)node_id);
      return false;
    }
    int64_t aid = 0, vid = 0;
    if (!parse_node_ref_id(json_obj_get(n->fields, "addr"), &aid) || !parse_node_ref_id(json_obj_get(n->fields, "value"), &vid)) {
      errf(f->p, "sircc: %s node %lld requires fields.addr and fields.value refs", n->tag, (long long)node_id);
      return false;
    }
    LLVMValueRef pval = lower_expr(f, aid);
    LLVMValueRef vval = lower_expr(f, vid);
    if (!pval || !vval) return false;
    LLVMTypeRef el = NULL;
    if (strcmp(tname, "ptr") == 0) {
      el = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
    } else {
      el = lower_type_prim(f->ctx, tname);
    }
    if (!el) {
      errf(f->p, "sircc: unsupported store type '%s'", tname);
      return false;
    }
    if (LLVMGetTypeKind(el) == LLVMFloatTypeKind || LLVMGetTypeKind(el) == LLVMDoubleTypeKind) {
      vval = canonicalize_float(f, vval);
    }
    LLVMTypeRef want_ptr = LLVMPointerType(el, 0);
    LLVMTypeRef pty = LLVMTypeOf(pval);
    if (LLVMGetTypeKind(pty) != LLVMPointerTypeKind) {
      errf(f->p, "sircc: %s requires pointer addr", n->tag);
      return false;
    }
    if (want_ptr != pty) {
      pval = LLVMBuildBitCast(f->builder, pval, want_ptr, "st.cast");
    }
    JsonValue* alignv = json_obj_get(n->fields, "align");
    unsigned align = 1;
    if (alignv) {
      int64_t a = 0;
      if (!json_get_i64(alignv, &a)) {
        errf(f->p, "sircc: %s node %lld align must be an integer", n->tag, (long long)node_id);
        return false;
      }
      if (a <= 0 || a > (int64_t)UINT_MAX) {
        errf(f->p, "sircc: %s node %lld align must be > 0", n->tag, (long long)node_id);
        return false;
      }
      align = (unsigned)a;
    }
    if ((align & (align - 1u)) != 0u) {
      errf(f->p, "sircc: %s node %lld align must be a power of two", n->tag, (long long)node_id);
      return false;
    }
    if (!emit_trap_if_misaligned(f, pval, align)) return false;
    LLVMValueRef st = LLVMBuildStore(f->builder, vval, pval);
    LLVMSetAlignment(st, align);
    JsonValue* volv = json_obj_get(n->fields, "vol");
    if (volv && volv->type == JSON_BOOL) LLVMSetVolatile(st, volv->v.b ? 1 : 0);
    return true;
  }

  if (strcmp(n->tag, "mem.copy") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: mem.copy node %lld missing fields", (long long)node_id);
      return false;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) {
      errf(f->p, "sircc: mem.copy node %lld requires args:[dst, src, len]", (long long)node_id);
      return false;
    }
    int64_t did = 0, sid = 0, lid = 0;
    if (!parse_node_ref_id(args->v.arr.items[0], &did) || !parse_node_ref_id(args->v.arr.items[1], &sid) ||
        !parse_node_ref_id(args->v.arr.items[2], &lid)) {
      errf(f->p, "sircc: mem.copy node %lld args must be node refs", (long long)node_id);
      return false;
    }
    LLVMValueRef dst = lower_expr(f, did);
    LLVMValueRef src = lower_expr(f, sid);
    LLVMValueRef len = lower_expr(f, lid);
    if (!dst || !src || !len) return false;

    LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
    dst = LLVMBuildBitCast(f->builder, dst, i8p, "dst.i8p");
    src = LLVMBuildBitCast(f->builder, src, i8p, "src.i8p");

    LLVMTypeRef i64 = LLVMInt64TypeInContext(f->ctx);
    if (LLVMGetTypeKind(LLVMTypeOf(len)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(len)) != 64) {
      len = build_zext_or_trunc(f->builder, len, i64, "len.i64");
    }

    unsigned align_dst = 1;
    unsigned align_src = 1;
    bool use_memmove = false;
    JsonValue* flags = json_obj_get(n->fields, "flags");
    if (flags && flags->type == JSON_OBJECT) {
      JsonValue* adv = json_obj_get(flags, "alignDst");
      if (adv) {
        int64_t a = 0;
        if (!json_get_i64(adv, &a)) {
          errf(f->p, "sircc: mem.copy node %lld flags.alignDst must be an integer", (long long)node_id);
          return false;
        }
        if (a <= 0 || a > (int64_t)UINT_MAX) {
          errf(f->p, "sircc: mem.copy node %lld flags.alignDst must be > 0", (long long)node_id);
          return false;
        }
        align_dst = (unsigned)a;
      }
      JsonValue* asv = json_obj_get(flags, "alignSrc");
      if (asv) {
        int64_t a = 0;
        if (!json_get_i64(asv, &a)) {
          errf(f->p, "sircc: mem.copy node %lld flags.alignSrc must be an integer", (long long)node_id);
          return false;
        }
        if (a <= 0 || a > (int64_t)UINT_MAX) {
          errf(f->p, "sircc: mem.copy node %lld flags.alignSrc must be > 0", (long long)node_id);
          return false;
        }
        align_src = (unsigned)a;
      }
      const char* ov = json_get_string(json_obj_get(flags, "overlap"));
      if (ov) {
        if (strcmp(ov, "allow") == 0) use_memmove = true;
        else if (strcmp(ov, "disallow") == 0) use_memmove = false;
        else {
          errf(f->p, "sircc: mem.copy node %lld flags.overlap must be 'allow' or 'disallow'", (long long)node_id);
          return false;
        }
      }
    }

    if (use_memmove) {
      if ((align_dst & (align_dst - 1u)) != 0u) {
        errf(f->p, "sircc: mem.copy node %lld flags.alignDst must be a power of two", (long long)node_id);
        return false;
      }
      if ((align_src & (align_src - 1u)) != 0u) {
        errf(f->p, "sircc: mem.copy node %lld flags.alignSrc must be a power of two", (long long)node_id);
        return false;
      }
      if (!emit_trap_if_misaligned(f, dst, align_dst)) return false;
      if (!emit_trap_if_misaligned(f, src, align_src)) return false;
      LLVMBuildMemMove(f->builder, dst, align_dst, src, align_src, len);
    } else {
      // Deterministic trap on overlapping ranges: overlap = len!=0 && (dst < src+len) && (src < dst+len).
      if ((align_dst & (align_dst - 1u)) != 0u) {
        errf(f->p, "sircc: mem.copy node %lld flags.alignDst must be a power of two", (long long)node_id);
        return false;
      }
      if ((align_src & (align_src - 1u)) != 0u) {
        errf(f->p, "sircc: mem.copy node %lld flags.alignSrc must be a power of two", (long long)node_id);
        return false;
      }
      if (!emit_trap_if_misaligned(f, dst, align_dst)) return false;
      if (!emit_trap_if_misaligned(f, src, align_src)) return false;
      unsigned ptr_bits = f->p->ptr_bits ? f->p->ptr_bits : (unsigned)(sizeof(void*) * 8u);
      LLVMTypeRef ip = LLVMIntTypeInContext(f->ctx, ptr_bits);
      LLVMValueRef dst_i = LLVMBuildPtrToInt(f->builder, dst, ip, "dst.i");
      LLVMValueRef src_i = LLVMBuildPtrToInt(f->builder, src, ip, "src.i");
      LLVMValueRef len_i = LLVMBuildTruncOrBitCast(f->builder, len, ip, "len.i");
      LLVMValueRef z = LLVMConstInt(ip, 0, 0);
      LLVMValueRef nz = LLVMBuildICmp(f->builder, LLVMIntNE, len_i, z, "len.nz");
      LLVMValueRef src_end = LLVMBuildAdd(f->builder, src_i, len_i, "src.end");
      LLVMValueRef dst_end = LLVMBuildAdd(f->builder, dst_i, len_i, "dst.end");
      LLVMValueRef c1 = LLVMBuildICmp(f->builder, LLVMIntULT, dst_i, src_end, "ov.c1");
      LLVMValueRef c2 = LLVMBuildICmp(f->builder, LLVMIntULT, src_i, dst_end, "ov.c2");
      LLVMValueRef ov = LLVMBuildAnd(f->builder, c1, c2, "ov");
      LLVMValueRef trap = LLVMBuildAnd(f->builder, nz, ov, "ov.trap");
      if (!emit_trap_if(f, trap)) return false;
      LLVMBuildMemCpy(f->builder, dst, align_dst, src, align_src, len);
    }
    return true;
  }

  if (strcmp(n->tag, "mem.fill") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: mem.fill node %lld missing fields", (long long)node_id);
      return false;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) {
      errf(f->p, "sircc: mem.fill node %lld requires args:[dst, byte, len]", (long long)node_id);
      return false;
    }
    int64_t did = 0, bid = 0, lid = 0;
    if (!parse_node_ref_id(args->v.arr.items[0], &did) || !parse_node_ref_id(args->v.arr.items[1], &bid) ||
        !parse_node_ref_id(args->v.arr.items[2], &lid)) {
      errf(f->p, "sircc: mem.fill node %lld args must be node refs", (long long)node_id);
      return false;
    }
    LLVMValueRef dst = lower_expr(f, did);
    LLVMValueRef bytev = lower_expr(f, bid);
    LLVMValueRef len = lower_expr(f, lid);
    if (!dst || !bytev || !len) return false;

    LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
    dst = LLVMBuildBitCast(f->builder, dst, i8p, "dst.i8p");

    LLVMTypeRef i8 = LLVMInt8TypeInContext(f->ctx);
    if (LLVMGetTypeKind(LLVMTypeOf(bytev)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(bytev)) != 8) {
      bytev = LLVMBuildTruncOrBitCast(f->builder, bytev, i8, "byte.i8");
    }

    LLVMTypeRef i64 = LLVMInt64TypeInContext(f->ctx);
    if (LLVMGetTypeKind(LLVMTypeOf(len)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(len)) != 64) {
      len = build_zext_or_trunc(f->builder, len, i64, "len.i64");
    }

    unsigned align_dst = 1;
    JsonValue* flags = json_obj_get(n->fields, "flags");
    if (flags && flags->type == JSON_OBJECT) {
      JsonValue* adv = json_obj_get(flags, "alignDst");
      if (adv) {
        int64_t a = 0;
        if (!json_get_i64(adv, &a)) {
          errf(f->p, "sircc: mem.fill node %lld flags.alignDst must be an integer", (long long)node_id);
          return false;
        }
        if (a <= 0 || a > (int64_t)UINT_MAX) {
          errf(f->p, "sircc: mem.fill node %lld flags.alignDst must be > 0", (long long)node_id);
          return false;
        }
        align_dst = (unsigned)a;
      }
    }

    if ((align_dst & (align_dst - 1u)) != 0u) {
      errf(f->p, "sircc: mem.fill node %lld flags.alignDst must be a power of two", (long long)node_id);
      return false;
    }
    if (!emit_trap_if_misaligned(f, dst, align_dst)) return false;
    LLVMBuildMemSet(f->builder, dst, bytev, len, align_dst);
    return true;
  }

  if (strcmp(n->tag, "eff.fence") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: eff.fence node %lld missing fields", (long long)node_id);
      return false;
    }
    JsonValue* flags = json_obj_get(n->fields, "flags");
    const char* mode = NULL;
    if (flags && flags->type == JSON_OBJECT) mode = json_get_string(json_obj_get(flags, "mode"));
    if (!mode) mode = json_get_string(json_obj_get(n->fields, "mode"));
    if (!mode) {
      errf(f->p, "sircc: eff.fence node %lld missing flags.mode", (long long)node_id);
      return false;
    }

    if (strcmp(mode, "relaxed") == 0) {
      // Closed set includes relaxed; model it as a no-op fence.
      return true;
    }

    LLVMAtomicOrdering ord;
    if (strcmp(mode, "acquire") == 0) ord = LLVMAtomicOrderingAcquire;
    else if (strcmp(mode, "release") == 0) ord = LLVMAtomicOrderingRelease;
    else if (strcmp(mode, "acqrel") == 0) ord = LLVMAtomicOrderingAcquireRelease;
    else if (strcmp(mode, "seqcst") == 0) ord = LLVMAtomicOrderingSequentiallyConsistent;
    else {
      errf(f->p, "sircc: eff.fence node %lld invalid mode '%s'", (long long)node_id, mode);
      return false;
    }

    (void)LLVMBuildFence(f->builder, ord, 0, "");
    return true;
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
    LLVMBuildCall2(f->builder, LLVMGlobalGetValueType(fn), fn, NULL, 0, "");
    LLVMBuildUnreachable(f->builder);
    return true;
  }

  if (strncmp(n->tag, "term.", 5) == 0) {
    return lower_term_cfg(f, node_id);
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

  // Expression-as-statement: evaluate for side-effects (e.g. `call`) and discard.
  LLVMValueRef v = lower_expr(f, node_id);
  return v != NULL;
}

static LLVMBasicBlockRef bb_lookup(FunctionCtx* f, int64_t node_id) {
  if (!f->blocks_by_node || node_id < 0) return NULL;
  size_t i = (size_t)node_id;
  return (i < f->p->nodes_cap) ? f->blocks_by_node[i] : NULL;
}

static bool add_block_args(FunctionCtx* f, LLVMBasicBlockRef from_bb, int64_t to_block_id, JsonValue* args) {
  NodeRec* bn = get_node(f->p, to_block_id);
  if (!bn || strcmp(bn->tag, "block") != 0) {
    errf(f->p, "sircc: branch targets non-block node %lld", (long long)to_block_id);
    return false;
  }

  JsonValue* params = bn->fields ? json_obj_get(bn->fields, "params") : NULL;
  size_t pcount = 0;
  if (params) {
    if (params->type != JSON_ARRAY) {
      errf(f->p, "sircc: block %lld params must be an array", (long long)to_block_id);
      return false;
    }
    pcount = params->v.arr.len;
  }

  size_t acount = 0;
  if (args) {
    if (args->type != JSON_ARRAY) {
      errf(f->p, "sircc: branch args must be an array");
      return false;
    }
    acount = args->v.arr.len;
  }

  if (pcount != acount) {
    errf(f->p, "sircc: block %lld param/arg count mismatch (params=%zu, args=%zu)", (long long)to_block_id, pcount,
         acount);
    return false;
  }

  for (size_t i = 0; i < pcount; i++) {
    int64_t pid = 0;
    if (!parse_node_ref_id(params->v.arr.items[i], &pid)) {
      errf(f->p, "sircc: block %lld params[%zu] must be node refs", (long long)to_block_id, i);
      return false;
    }
    NodeRec* pn = get_node(f->p, pid);
    if (!pn || strcmp(pn->tag, "bparam") != 0 || !pn->llvm_value) {
      errf(f->p, "sircc: block %lld params[%zu] must reference a lowered bparam node", (long long)to_block_id, i);
      return false;
    }

    int64_t aid = 0;
    if (!parse_node_ref_id(args->v.arr.items[i], &aid)) {
      errf(f->p, "sircc: block %lld args[%zu] must be node refs", (long long)to_block_id, i);
      return false;
    }
    LLVMValueRef av = lower_expr(f, aid);
    if (!av) return false;

    LLVMValueRef phi = pn->llvm_value;
    LLVMAddIncoming(phi, &av, &from_bb, 1);
  }

  return true;
}

static bool lower_term_cfg(FunctionCtx* f, int64_t node_id) {
  NodeRec* n = get_node(f->p, node_id);
  if (!n) return false;

  if (strcmp(n->tag, "term.br") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: term.br node %lld missing fields", (long long)node_id);
      return false;
    }
    JsonValue* to = json_obj_get(n->fields, "to");
    int64_t bid = 0;
    if (!parse_node_ref_id(to, &bid)) {
      errf(f->p, "sircc: term.br node %lld missing to ref", (long long)node_id);
      return false;
    }
    LLVMBasicBlockRef bb = bb_lookup(f, bid);
    if (!bb) {
      errf(f->p, "sircc: term.br node %lld targets unknown block %lld", (long long)node_id, (long long)bid);
      return false;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!add_block_args(f, LLVMGetInsertBlock(f->builder), bid, args)) return false;
    LLVMBuildBr(f->builder, bb);
    return true;
  }

  if (strcmp(n->tag, "term.cbr") == 0 || strcmp(n->tag, "term.condbr") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: %s node %lld missing fields", n->tag, (long long)node_id);
      return false;
    }

    int64_t cond_id = 0;
    if (!parse_node_ref_id(json_obj_get(n->fields, "cond"), &cond_id)) {
      errf(f->p, "sircc: %s node %lld missing cond ref", n->tag, (long long)node_id);
      return false;
    }
    LLVMValueRef cond = lower_expr(f, cond_id);
    if (!cond) return false;
    if (LLVMGetTypeKind(LLVMTypeOf(cond)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(cond)) != 1) {
      errf(f->p, "sircc: %s cond must be bool/i1", n->tag);
      return false;
    }

    JsonValue* thenb = json_obj_get(n->fields, "then");
    JsonValue* elseb = json_obj_get(n->fields, "else");
    if (!thenb || thenb->type != JSON_OBJECT || !elseb || elseb->type != JSON_OBJECT) {
      errf(f->p, "sircc: %s node %lld requires then/else objects", n->tag, (long long)node_id);
      return false;
    }

    int64_t then_id = 0;
    int64_t else_id = 0;
    if (!parse_node_ref_id(json_obj_get(thenb, "to"), &then_id) || !parse_node_ref_id(json_obj_get(elseb, "to"), &else_id)) {
      errf(f->p, "sircc: %s node %lld then/else missing to ref", n->tag, (long long)node_id);
      return false;
    }
    LLVMBasicBlockRef then_bb = bb_lookup(f, then_id);
    LLVMBasicBlockRef else_bb = bb_lookup(f, else_id);
    if (!then_bb || !else_bb) {
      errf(f->p, "sircc: %s node %lld targets unknown blocks", n->tag, (long long)node_id);
      return false;
    }

    JsonValue* then_args = json_obj_get(thenb, "args");
    JsonValue* else_args = json_obj_get(elseb, "args");
    LLVMBasicBlockRef from_bb = LLVMGetInsertBlock(f->builder);
    if (!add_block_args(f, from_bb, then_id, then_args)) return false;
    if (!add_block_args(f, from_bb, else_id, else_args)) return false;

    LLVMBuildCondBr(f->builder, cond, then_bb, else_bb);
    return true;
  }

  if (strcmp(n->tag, "term.switch") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: term.switch node %lld missing fields", (long long)node_id);
      return false;
    }
    int64_t scrut_id = 0;
    if (!parse_node_ref_id(json_obj_get(n->fields, "scrut"), &scrut_id)) {
      errf(f->p, "sircc: term.switch node %lld missing scrut ref", (long long)node_id);
      return false;
    }
    LLVMValueRef scrut = lower_expr(f, scrut_id);
    if (!scrut) return false;
    LLVMTypeRef sty = LLVMTypeOf(scrut);
    if (LLVMGetTypeKind(sty) == LLVMPointerTypeKind) {
      // Spec allows ptr scrut; lower by casting to target pointer-sized integer.
      unsigned ptr_bits = f->p->ptr_bits ? f->p->ptr_bits : (unsigned)(sizeof(void*) * 8u);
      LLVMTypeRef ity = LLVMIntTypeInContext(f->ctx, ptr_bits);
      scrut = LLVMBuildPtrToInt(f->builder, scrut, ity, "ptr.switch");
      sty = LLVMTypeOf(scrut);
    }
    if (LLVMGetTypeKind(sty) != LLVMIntegerTypeKind) {
      errf(f->p, "sircc: term.switch scrut must be iN or ptr");
      return false;
    }

    JsonValue* def = json_obj_get(n->fields, "default");
    if (!def || def->type != JSON_OBJECT) {
      errf(f->p, "sircc: term.switch node %lld missing default branch", (long long)node_id);
      return false;
    }
    int64_t def_id = 0;
    if (!parse_node_ref_id(json_obj_get(def, "to"), &def_id)) {
      errf(f->p, "sircc: term.switch default missing to ref");
      return false;
    }
    LLVMBasicBlockRef def_bb = bb_lookup(f, def_id);
    if (!def_bb) {
      errf(f->p, "sircc: term.switch default targets unknown block %lld", (long long)def_id);
      return false;
    }
    JsonValue* def_args = json_obj_get(def, "args");
    if (!add_block_args(f, LLVMGetInsertBlock(f->builder), def_id, def_args)) return false;

    JsonValue* cases = json_obj_get(n->fields, "cases");
    if (!cases || cases->type != JSON_ARRAY) {
      errf(f->p, "sircc: term.switch node %lld missing cases array", (long long)node_id);
      return false;
    }
    LLVMValueRef sw = LLVMBuildSwitch(f->builder, scrut, def_bb, (unsigned)cases->v.arr.len);
    for (size_t i = 0; i < cases->v.arr.len; i++) {
      JsonValue* c = cases->v.arr.items[i];
      if (!c || c->type != JSON_OBJECT) {
        errf(f->p, "sircc: term.switch case[%zu] must be object", i);
        return false;
      }
      int64_t lit_id = 0;
      if (!parse_node_ref_id(json_obj_get(c, "lit"), &lit_id)) {
        errf(f->p, "sircc: term.switch case[%zu] missing lit ref", i);
        return false;
      }
      NodeRec* litn = get_node(f->p, lit_id);
      if (!litn || strncmp(litn->tag, "const.", 6) != 0 || !litn->fields) {
        errf(f->p, "sircc: term.switch case[%zu] lit must be const.* node", i);
        return false;
      }
      int64_t litv = 0;
      if (!must_i64(f->p, json_obj_get(litn->fields, "value"), &litv, "case lit")) return false;
      LLVMValueRef lit = LLVMConstInt(sty, (unsigned long long)litv, 1);

      int64_t to_id = 0;
      if (!parse_node_ref_id(json_obj_get(c, "to"), &to_id)) {
        errf(f->p, "sircc: term.switch case[%zu] missing to ref", i);
        return false;
      }
      LLVMBasicBlockRef to_bb = bb_lookup(f, to_id);
      if (!to_bb) {
        errf(f->p, "sircc: term.switch case[%zu] targets unknown block %lld", i, (long long)to_id);
        return false;
      }

      JsonValue* args = json_obj_get(c, "args");
      if (!add_block_args(f, LLVMGetInsertBlock(f->builder), to_id, args)) return false;

      LLVMAddCase(sw, lit, to_bb);
    }
    return true;
  }

  // fallthrough to existing term.* handled by lower_stmt
  errf(f->p, "sircc: unsupported terminator '%s' (node %lld)", n->tag, (long long)node_id);
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

static bool init_target_for_module(SirProgram* p, LLVMModuleRef mod, const char* triple) {
  if (!p || !mod || !triple) return false;

  llvm_init_targets_once();

  char* err = NULL;
  LLVMTargetRef target = NULL;
  if (LLVMGetTargetFromTriple(triple, &target, &err) != 0) {
    errf(p, "sircc: target triple '%s' unsupported: %s", triple, err ? err : "(unknown)");
    LLVMDisposeMessage(err);
    return false;
  }

  LLVMTargetMachineRef tm =
      LLVMCreateTargetMachine(target, triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocDefault, LLVMCodeModelDefault);
  if (!tm) {
    errf(p, "sircc: failed to create target machine");
    return false;
  }

  LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
  char* dl_str = LLVMCopyStringRepOfTargetData(td);
  LLVMSetTarget(mod, triple);
  LLVMSetDataLayout(mod, dl_str);

  p->ptr_bytes = LLVMPointerSize(td);
  p->ptr_bits = p->ptr_bytes * 8u;

  LLVMDisposeMessage(dl_str);
  LLVMDisposeTargetData(td);
  LLVMDisposeTargetMachine(tm);
  return true;
}

static bool emit_module_obj(LLVMModuleRef mod, const char* triple, const char* out_path) {
  llvm_init_targets_once();

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

bool sircc_print_target(const char* triple) {
  llvm_init_targets_once();

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
      LLVMCreateTargetMachine(target, use_triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocDefault, LLVMCodeModelDefault);
  if (!tm) {
    errf(NULL, "sircc: failed to create target machine");
    if (!triple) LLVMDisposeMessage((char*)use_triple);
    return false;
  }

  LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
  char* dl_str = LLVMCopyStringRepOfTargetData(td);

  unsigned ptr_bytes = LLVMPointerSize(td);
  unsigned ptr_bits = ptr_bytes * 8u;
  const char* endian = (dl_str && dl_str[0] == 'E') ? "big" : "little";

  printf("triple: %s\n", use_triple);
  printf("data_layout: %s\n", dl_str ? dl_str : "(null)");
  printf("endianness: %s\n", endian);
  printf("ptrBits: %u\n", ptr_bits);

  LLVMDisposeMessage(dl_str);
  LLVMDisposeTargetData(td);
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
    if (!paramsv || paramsv->type != JSON_ARRAY) {
      errf(p, "sircc: fn node %lld missing params array", (long long)n->id);
      return false;
    }

    FunctionCtx f = {.p = p, .ctx = ctx, .mod = mod, .builder = NULL, .fn = fn};

    unsigned param_count = LLVMCountParams(fn);
    if (paramsv->v.arr.len != (size_t)param_count) {
      errf(p, "sircc: fn node %lld param count mismatch: node has %zu, type has %u", (long long)n->id, paramsv->v.arr.len,
           param_count);
      free(f.binds);
      return false;
    }

    for (unsigned pi = 0; pi < param_count; pi++) {
      int64_t pid = 0;
      if (!parse_node_ref_id(paramsv->v.arr.items[pi], &pid)) {
        errf(p, "sircc: fn node %lld has non-ref param", (long long)n->id);
        free(f.binds);
        return false;
      }
      NodeRec* pn = get_node(p, pid);
      if (!pn || strcmp(pn->tag, "param") != 0) {
        errf(p, "sircc: fn node %lld param ref %lld is not a param node", (long long)n->id, (long long)pid);
        free(f.binds);
        return false;
      }
      const char* pname = pn->fields ? json_get_string(json_obj_get(pn->fields, "name")) : NULL;
      if (!pname) {
        errf(p, "sircc: param node %lld missing fields.name", (long long)pid);
        free(f.binds);
        return false;
      }
      LLVMValueRef pv = LLVMGetParam(fn, pi);
      LLVMSetValueName2(pv, pname, strlen(pname));
      pn->llvm_value = pv;
      if (!bind_add(&f, pname, pv)) {
        errf(p, "sircc: duplicate binding for '%s' in fn %lld", pname, (long long)n->id);
        free(f.binds);
        return false;
      }
    }

    JsonValue* blocks_v = n->fields ? json_obj_get(n->fields, "blocks") : NULL;
    JsonValue* entry_v = n->fields ? json_obj_get(n->fields, "entry") : NULL;
    if (blocks_v && blocks_v->type == JSON_ARRAY && entry_v) {
      // CFG form: explicit list of basic blocks + entry.
      int64_t entry_id = 0;
      if (!parse_node_ref_id(entry_v, &entry_id)) {
        errf(p, "sircc: fn node %lld entry must be a block ref", (long long)n->id);
        free(f.binds);
        return false;
      }

      f.blocks_by_node = (LLVMBasicBlockRef*)calloc(p->nodes_cap, sizeof(LLVMBasicBlockRef));
      if (!f.blocks_by_node) {
        free(f.binds);
        return false;
      }

      for (size_t bi = 0; bi < blocks_v->v.arr.len; bi++) {
        int64_t bid = 0;
        if (!parse_node_ref_id(blocks_v->v.arr.items[bi], &bid)) {
          errf(p, "sircc: fn node %lld blocks[%zu] must be block refs", (long long)n->id, bi);
          free(f.blocks_by_node);
          free(f.binds);
          return false;
        }
        NodeRec* bn = get_node(p, bid);
        if (!bn || strcmp(bn->tag, "block") != 0) {
          errf(p, "sircc: fn node %lld blocks[%zu] does not reference a block node", (long long)n->id, bi);
          free(f.blocks_by_node);
          free(f.binds);
          return false;
        }
        if (bid < 0 || (size_t)bid >= p->nodes_cap) continue;
        if (!f.blocks_by_node[bid]) {
          char namebuf[32];
          snprintf(namebuf, sizeof(namebuf), "B%lld", (long long)bid);
          f.blocks_by_node[bid] = LLVMAppendBasicBlockInContext(ctx, fn, namebuf);
        }
      }

      // Ensure entry exists.
      if (entry_id < 0 || (size_t)entry_id >= p->nodes_cap || !f.blocks_by_node[entry_id]) {
        errf(p, "sircc: fn node %lld entry block %lld not in blocks list", (long long)n->id, (long long)entry_id);
        free(f.blocks_by_node);
        free(f.binds);
        return false;
      }

      // Lower blocks in listed order.
      for (size_t bi = 0; bi < blocks_v->v.arr.len; bi++) {
        int64_t bid = 0;
        (void)parse_node_ref_id(blocks_v->v.arr.items[bi], &bid);
        NodeRec* bn = get_node(p, bid);
        LLVMBasicBlockRef bb = f.blocks_by_node[bid];
        if (!bn || !bb) continue;

        LLVMBuilderRef builder = LLVMCreateBuilderInContext(ctx);
        f.builder = builder;
        LLVMPositionBuilderAtEnd(builder, bb);

        size_t mark = bind_mark(&f);

        // Block params: lowered as PHIs (to be populated by predecessors via branch args).
        JsonValue* params = bn->fields ? json_obj_get(bn->fields, "params") : NULL;
        if (params) {
          if (params->type != JSON_ARRAY) {
            errf(p, "sircc: block %lld params must be an array", (long long)bid);
            LLVMDisposeBuilder(builder);
            free(f.blocks_by_node);
            free(f.binds);
            return false;
          }
          for (size_t pi = 0; pi < params->v.arr.len; pi++) {
            int64_t pid = 0;
            if (!parse_node_ref_id(params->v.arr.items[pi], &pid)) {
              errf(p, "sircc: block %lld params[%zu] must be node refs", (long long)bid, pi);
              LLVMDisposeBuilder(builder);
              free(f.blocks_by_node);
              free(f.binds);
              return false;
            }
            NodeRec* pn = get_node(p, pid);
            if (!pn || strcmp(pn->tag, "bparam") != 0) {
              errf(p, "sircc: block %lld params[%zu] must reference bparam nodes", (long long)bid, pi);
              LLVMDisposeBuilder(builder);
              free(f.blocks_by_node);
              free(f.binds);
              return false;
            }
            if (pn->llvm_value) continue;
            if (pn->type_ref == 0) {
              errf(p, "sircc: bparam node %lld missing type_ref", (long long)pid);
              LLVMDisposeBuilder(builder);
              free(f.blocks_by_node);
              free(f.binds);
              return false;
            }
            LLVMTypeRef pty = lower_type(p, ctx, pn->type_ref);
            if (!pty) {
              errf(p, "sircc: bparam node %lld has invalid type_ref", (long long)pid);
              LLVMDisposeBuilder(builder);
              free(f.blocks_by_node);
              free(f.binds);
              return false;
            }
            pn->llvm_value = LLVMBuildPhi(builder, pty, "bparam");
            const char* bname = pn->fields ? json_get_string(json_obj_get(pn->fields, "name")) : NULL;
            if (bname) {
              LLVMSetValueName2(pn->llvm_value, bname, strlen(bname));
              if (!bind_add(&f, bname, pn->llvm_value)) {
                errf(p, "sircc: failed to bind block param '%s' in fn %lld", bname, (long long)n->id);
                LLVMDisposeBuilder(builder);
                free(f.blocks_by_node);
                free(f.binds);
                return false;
              }
            }
          }
        }

        JsonValue* stmts = bn->fields ? json_obj_get(bn->fields, "stmts") : NULL;
        if (!stmts || stmts->type != JSON_ARRAY) {
          errf(p, "sircc: block node %lld missing stmts array", (long long)bid);
          LLVMDisposeBuilder(builder);
          free(f.blocks_by_node);
          free(f.binds);
          return false;
        }
        for (size_t si = 0; si < stmts->v.arr.len; si++) {
          int64_t sid = 0;
          if (!parse_node_ref_id(stmts->v.arr.items[si], &sid)) {
            errf(p, "sircc: block node %lld has non-ref stmt", (long long)bid);
            LLVMDisposeBuilder(builder);
            free(f.blocks_by_node);
            free(f.binds);
            return false;
          }
          if (!lower_stmt(&f, sid)) {
            LLVMDisposeBuilder(builder);
            free(f.blocks_by_node);
            free(f.binds);
            return false;
          }
          if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) break;
        }

        if (!LLVMGetBasicBlockTerminator(bb)) {
          errf(p, "sircc: block %lld missing terminator", (long long)bid);
          LLVMDisposeBuilder(builder);
          bind_restore(&f, mark);
          free(f.blocks_by_node);
          free(f.binds);
          return false;
        }

        LLVMDisposeBuilder(builder);
        bind_restore(&f, mark);
        f.builder = NULL;
      }

      // Ensure entry is first for execution: create a trampoline if needed.
      LLVMBasicBlockRef first = LLVMGetFirstBasicBlock(fn);
      if (first != f.blocks_by_node[entry_id]) {
        LLVMBasicBlockRef tramp = LLVMInsertBasicBlockInContext(ctx, first, "entry");
        LLVMBuilderRef builder = LLVMCreateBuilderInContext(ctx);
        LLVMPositionBuilderAtEnd(builder, tramp);
        LLVMBuildBr(builder, f.blocks_by_node[entry_id]);
        LLVMDisposeBuilder(builder);
      }

      free(f.blocks_by_node);
      free(f.binds);
      continue;
    }

    // Legacy form: single entry block with `body:ref`.
    JsonValue* bodyv = n->fields ? json_obj_get(n->fields, "body") : NULL;
    int64_t body_id = 0;
    if (!parse_node_ref_id(bodyv, &body_id)) {
      errf(p, "sircc: fn node %lld missing body ref", (long long)n->id);
      free(f.binds);
      return false;
    }

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx, fn, "entry");
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(ctx);
    f.builder = builder;
    LLVMPositionBuilderAtEnd(builder, entry);

    if (!lower_stmt(&f, body_id)) {
      LLVMDisposeBuilder(builder);
      free(f.binds);
      return false;
    }

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
      // Conservative default: fallthrough returns 0 for integer returns, otherwise void.
      LLVMTypeRef rty = LLVMGetReturnType(LLVMGlobalGetValueType(fn));
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
  char* owned_triple = NULL;

  bool ok = parse_program(&p, opt, opt->input_path);
  if (!ok) goto done;

  ok = validate_program(&p);
  if (!ok) goto done;

  if (opt->verify_only) {
    ok = true;
    goto done;
  }

  const char* use_triple = opt->target_triple ? opt->target_triple : p.target_triple;
  if (!use_triple) {
    owned_triple = LLVMGetDefaultTargetTriple();
    use_triple = owned_triple;
  }

  LLVMContextRef ctx = LLVMContextCreate();
  LLVMModuleRef mod = LLVMModuleCreateWithNameInContext("sir", ctx);

  if (!init_target_for_module(&p, mod, use_triple)) {
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    ok = false;
    goto done;
  }

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
    ok = emit_module_obj(mod, use_triple, opt->output_path);
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

  ok = emit_module_obj(mod, use_triple, tmp_obj);
  LLVMDisposeModule(mod);
  LLVMContextDispose(ctx);
  if (!ok) {
    unlink(tmp_obj);
    goto done;
  }

  ok = run_clang_link(opt->clang_path, tmp_obj, opt->output_path);
  unlink(tmp_obj);

done:
  if (owned_triple) LLVMDisposeMessage(owned_triple);
  free(p.srcs);
  free(p.syms);
  free(p.types);
  free(p.nodes);
  arena_free(&p.arena);
  return ok;
}
