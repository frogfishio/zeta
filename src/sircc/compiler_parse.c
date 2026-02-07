// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

JsonValue* must_obj(SirProgram* p, JsonValue* v, const char* ctx) {
  if (!v || v->type != JSON_OBJECT) {
    err_codef(p, "sircc.json.expected_object", "sircc: expected object for %s", ctx);
    return NULL;
  }
  return v;
}

const char* must_string(SirProgram* p, JsonValue* v, const char* ctx) {
  const char* s = json_get_string(v);
  if (!s) err_codef(p, "sircc.json.expected_string", "sircc: expected string for %s", ctx);
  return s;
}

bool must_i64(SirProgram* p, JsonValue* v, int64_t* out, const char* ctx) {
  if (!json_get_i64(v, out)) {
    err_codef(p, "sircc.json.expected_int", "sircc: expected integer for %s", ctx);
    return false;
  }
  return true;
}

// NOTE: parse_{node,type,sym}_ref_id are implemented in compiler_ids.c to support
// both integer and string ids.

bool is_ident(const char* s) {
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
    err_codef(p, "sircc.schema.value.not_object", "sircc: %s must be an object value", what);
    return false;
  }
  const char* t = json_get_string(json_obj_get(v, "t"));
  if (!t) {
    err_codef(p, "sircc.schema.value.missing_t", "sircc: %s missing string field 't'", what);
    return false;
  }

  if (strcmp(t, "sym") == 0 || strcmp(t, "lbl") == 0 || strcmp(t, "reg") == 0) {
    const char* name = json_get_string(json_obj_get(v, "v"));
    if (!name || !is_ident(name)) {
      err_codef(p, "sircc.schema.value.ident.bad", "sircc: %s %s.v must be an Ident", what, t);
      return false;
    }
    return true;
  }
  if (strcmp(t, "num") == 0) {
    int64_t n = 0;
    if (!json_get_i64(json_obj_get(v, "v"), &n)) {
      err_codef(p, "sircc.schema.value.num.bad", "sircc: %s num.v must be an integer", what);
      return false;
    }
    return true;
  }
  if (strcmp(t, "str") == 0) {
    const char* s = json_get_string(json_obj_get(v, "v"));
    if (!s) {
      err_codef(p, "sircc.schema.value.str.bad", "sircc: %s str.v must be a string", what);
      return false;
    }
    return true;
  }
  if (strcmp(t, "mem") == 0) {
    const JsonValue* base = json_obj_get(v, "base");
    if (!base || base->type != JSON_OBJECT) {
      err_codef(p, "sircc.schema.value.mem.base.bad", "sircc: %s mem.base must be an object", what);
      return false;
    }
    const char* bt = json_get_string(json_obj_get(base, "t"));
    if (!bt || (strcmp(bt, "reg") != 0 && strcmp(bt, "sym") != 0)) {
      err_codef(p, "sircc.schema.value.mem.base.bad", "sircc: %s mem.base must be reg or sym", what);
      return false;
    }
    if (!validate_value(p, base, what)) return false;

    JsonValue* disp = json_obj_get(v, "disp");
    if (disp) {
      int64_t d = 0;
      if (!json_get_i64(disp, &d)) {
        err_codef(p, "sircc.schema.value.mem.disp.bad", "sircc: %s mem.disp must be an integer", what);
        return false;
      }
    }
    JsonValue* size = json_obj_get(v, "size");
    if (size) {
      int64_t s = 0;
      if (!json_get_i64(size, &s) || !(s == 1 || s == 2 || s == 4 || s == 8 || s == 16)) {
        err_codef(p, "sircc.schema.value.mem.size.bad", "sircc: %s mem.size must be one of 1,2,4,8,16", what);
        return false;
      }
    }
    return true;
  }
  if (strcmp(t, "ref") == 0) {
    int64_t id = 0;
    const JsonValue* idv = json_obj_get(v, "id");
    const char* ids = json_get_string(idv);
    if (!json_get_i64(idv, &id) && !(ids && *ids)) {
      err_codef(p, "sircc.schema.value.ref.bad", "sircc: %s ref.id must be an integer or string", what);
      return false;
    }
    JsonValue* k = json_obj_get(v, "k");
    if (k) {
      const char* ks = json_get_string(k);
      if (!ks || (strcmp(ks, "sym") != 0 && strcmp(ks, "type") != 0 && strcmp(ks, "node") != 0)) {
        err_codef(p, "sircc.schema.value.ref.k.bad", "sircc: %s ref.k must be one of sym/type/node", what);
        return false;
      }
    }
    return true;
  }

  err_codef(p, "sircc.schema.value.t.unknown", "sircc: %s has unknown value tag t='%s'", what, t);
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
  else if (strcmp(name, "data:v1") == 0) p->feat_data_v1 = true;
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
  if (strcmp(name, "data:v1") == 0) return p->feat_data_v1;
  return false;
}

static bool note_pending_feature_use(SirProgram* p, const char* mnemonic, const char* need) {
  if (!p || !mnemonic || !need) return false;

  if (p->pending_features_len == p->pending_features_cap) {
    size_t next = p->pending_features_cap ? (p->pending_features_cap * 2) : 16;
    PendingFeatureUse* bigger = (PendingFeatureUse*)realloc(p->pending_features, next * sizeof(PendingFeatureUse));
    if (!bigger) return false;
    p->pending_features = bigger;
    p->pending_features_cap = next;
  }

  p->pending_features[p->pending_features_len++] = (PendingFeatureUse){
      .path = p->cur_path,
      .line = p->cur_line,
      .rec_id = p->cur_rec_id,
      .mnemonic = mnemonic,
      .need = need,
  };
  return true;
}

static bool parse_env_u64(const char* name, uint64_t* out) {
  if (!name || !out) return false;
  const char* s = getenv(name);
  if (!s || !*s) return false;
  errno = 0;
  char* end = NULL;
  unsigned long long v = strtoull(s, &end, 10);
  if (errno != 0 || !end || *end != 0) return false;
  *out = (uint64_t)v;
  return true;
}

bool read_line(FILE* f, char** buf, size_t* cap, size_t* out_len, size_t max_line_bytes, bool* out_too_long) {
  if (out_too_long) *out_too_long = false;
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
      if (max_line_bytes && next > max_line_bytes) {
        if (out_too_long) *out_too_long = true;
        return false;
      }
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

bool is_blank_line(const char* s) {
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
  err_codef(p, "sircc.schema.unknown_field", "sircc: invalid %s: unknown field '%s'", what, bad);
  return false;
}

static bool is_pow2_u64(uint64_t x) { return x && ((x & (x - 1ULL)) == 0ULL); }

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

      // Optional LLVM codegen tuning knobs (passed through to LLVM target machine creation).
      const char* cpu = json_get_string(json_obj_get(target, "cpu"));
      if (cpu && *cpu) p->target_cpu = cpu;
      const char* features = json_get_string(json_obj_get(target, "features"));
      if (features && *features) p->target_features = features;

      // Optional explicit target contract overrides (used for determinism / cross-target verification).
      // If provided, these must match the LLVM ABI for the chosen triple (when compiling).
      JsonValue* ptrBitsV = json_obj_get(target, "ptrBits");
      if (ptrBitsV) {
        int64_t ptrBits = 0;
        if (!must_i64(p, ptrBitsV, &ptrBits, "meta.ext.target.ptrBits")) return false;
        if (ptrBits != 32 && ptrBits != 64) {
          err_codef(p, "sircc.meta.target.ptrBits.bad", "sircc: meta.ext.target.ptrBits must be 32 or 64");
          return false;
        }
        p->ptr_bits = (unsigned)ptrBits;
        p->ptr_bytes = (unsigned)ptrBits / 8u;
        p->target_ptrbits_override = true;
      }

      const char* endian = json_get_string(json_obj_get(target, "endian"));
      if (endian && *endian) {
        if (strcmp(endian, "le") == 0) p->target_big_endian = false;
        else if (strcmp(endian, "be") == 0) p->target_big_endian = true;
        else {
          err_codef(p, "sircc.meta.target.endian.bad", "sircc: meta.ext.target.endian must be 'le' or 'be'");
          return false;
        }
        p->target_endian_override = true;
      }

      const char* structAlign = json_get_string(json_obj_get(target, "structAlign"));
      if (structAlign && *structAlign) {
        if (strcmp(structAlign, "max") != 0 && strcmp(structAlign, "packed1") != 0) {
          err_codef(p, "sircc.meta.target.structAlign.bad", "sircc: meta.ext.target.structAlign must be 'max' or 'packed1'");
          return false;
        }
        p->struct_align = structAlign;
        p->target_structalign_override = true;
      }

      JsonValue* intAlign = json_obj_get(target, "intAlign");
      if (intAlign && intAlign->type == JSON_OBJECT) {
        struct {
          const char* key;
          unsigned* out;
          const char* ctx;
        } ints[] = {
            {"i8", &p->align_i8, "meta.ext.target.intAlign.i8"},   {"i16", &p->align_i16, "meta.ext.target.intAlign.i16"},
            {"i32", &p->align_i32, "meta.ext.target.intAlign.i32"}, {"i64", &p->align_i64, "meta.ext.target.intAlign.i64"},
            {"ptr", &p->align_ptr, "meta.ext.target.intAlign.ptr"},
        };
        for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); i++) {
          JsonValue* vj = json_obj_get(intAlign, ints[i].key);
          if (!vj) continue;
          int64_t v = 0;
          if (!must_i64(p, vj, &v, ints[i].ctx)) return false;
          if (v <= 0 || v > 1024 || !is_pow2_u64((uint64_t)v)) {
            err_codef(p, "sircc.meta.target.align.bad", "sircc: %s must be a positive power-of-two <= 1024", ints[i].ctx);
            return false;
          }
          *ints[i].out = (unsigned)v;
          p->target_intalign_override = true;
        }
      }

      JsonValue* floatAlign = json_obj_get(target, "floatAlign");
      if (floatAlign && floatAlign->type == JSON_OBJECT) {
        struct {
          const char* key;
          unsigned* out;
          const char* ctx;
        } floats[] = {
            {"f32", &p->align_f32, "meta.ext.target.floatAlign.f32"},
            {"f64", &p->align_f64, "meta.ext.target.floatAlign.f64"},
        };
        for (size_t i = 0; i < sizeof(floats) / sizeof(floats[0]); i++) {
          JsonValue* vj = json_obj_get(floatAlign, floats[i].key);
          if (!vj) continue;
          int64_t v = 0;
          if (!must_i64(p, vj, &v, floats[i].ctx)) return false;
          if (v <= 0 || v > 1024 || !is_pow2_u64((uint64_t)v)) {
            err_codef(p, "sircc.meta.target.align.bad", "sircc: %s must be a positive power-of-two <= 1024", floats[i].ctx);
            return false;
          }
          *floats[i].out = (unsigned)v;
          p->target_floatalign_override = true;
        }
      }
    }

    // Convention (sircc-defined): ext.features (array of strings)
    JsonValue* features = json_obj_get(ext, "features");
    if (features && features->type == JSON_ARRAY) {
      for (size_t i = 0; i < features->v.arr.len; i++) {
        const char* f = json_get_string(features->v.arr.items[i]);
        if (!f) {
          err_codef(p, "sircc.meta.features.bad", "sircc: meta.ext.features[%zu] must be a string", i);
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
  if (!sir_intern_id(p, SIR_ID_SRC, json_obj_get(obj, "id"), &id, "src.id")) return false;
  if (!ensure_src_slot(p, id)) return false;
  if (p->srcs[id]) {
    err_codef(p, "sircc.schema.duplicate_id", "sircc: duplicate src id %lld", (long long)id);
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
    err_codef(p, "sircc.src.end_loc.partial", "sircc: src record %lld must include both end_line and end_col (or neither)",
              (long long)id);
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
  if (!sir_intern_id(p, SIR_ID_SYM, json_obj_get(obj, "id"), &id, "sym.id")) return false;
  if (!ensure_sym_slot(p, id)) return false;
  if (p->syms[id]) {
    err_codef(p, "sircc.schema.duplicate_id", "sircc: duplicate sym id %lld", (long long)id);
    return false;
  }

  SymRec* s = (SymRec*)arena_alloc(&p->arena, sizeof(SymRec));
  if (!s) return false;
  s->id = id;
  s->name = must_string(p, json_obj_get(obj, "name"), "sym.name");
  s->kind = must_string(p, json_obj_get(obj, "kind"), "sym.kind");
  s->linkage = json_get_string(json_obj_get(obj, "linkage"));
  s->type_ref = 0;
  s->value = NULL;
  if (!s->name || !s->kind) return false;
  if (!is_ident(s->name)) {
    err_codef(p, "sircc.schema.ident.bad", "sircc: sym.name must be an Ident");
    return false;
  }

  JsonValue* tr = json_obj_get(obj, "type_ref");
  if (tr) {
    if (!sir_intern_id(p, SIR_ID_TYPE, tr, &s->type_ref, "sym.type_ref")) return false;
  }
  JsonValue* v = json_obj_get(obj, "value");
  if (v) {
    if (!validate_value(p, v, "sym.value")) return false;
    s->value = v;
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
    err_codef(p, "sircc.schema.ident.bad", "sircc: label.name must be an Ident");
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
    err_codef(p, "sircc.schema.instr.ops.not_array", "sircc: expected array for instr.ops");
    return false;
  }
  for (size_t i = 0; i < ops->v.arr.len; i++) {
    if (!validate_value(p, ops->v.arr.items[i], "instr operand")) return false;
  }
  const char* m = json_get_string(json_obj_get(obj, "m"));
  const char* need = required_feature_for_mnemonic(m);
  if (need && !has_feature(p, need)) {
    // Defer feature checks until end-of-parse, so meta.ext.features can appear anywhere in the stream.
    if (!note_pending_feature_use(p, m, need)) return false;
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
    err_codef(p, "sircc.schema.dir.args.not_array", "sircc: expected array for dir.args");
    return false;
  }
  for (size_t i = 0; i < args->v.arr.len; i++) {
    if (!validate_value(p, args->v.arr.items[i], "dir arg")) return false;
  }
  return true;
}

static bool parse_type_record(SirProgram* p, JsonValue* obj) {
  static const char* const keys[] = {"ir",   "k",      "id",     "kind",   "name",   "prim",  "of",      "len",
                                     "lane", "lanes",  "params", "ret",    "varargs", "fields", "variants", "sig", "callSig", "env", "attrs", "src_ref", "loc"};
  if (!require_only_keys(p, obj, keys, sizeof(keys) / sizeof(keys[0]), "type record")) return false;

  int64_t id = 0;
  if (!sir_intern_id(p, SIR_ID_TYPE, json_obj_get(obj, "id"), &id, "type.id")) return false;
  const char* kind = must_string(p, json_obj_get(obj, "kind"), "type.kind");
  if (!kind) return false;
  if (!ensure_type_slot(p, id)) return false;
  if (p->types[id]) {
    err_codef(p, "sircc.schema.duplicate_id", "sircc: duplicate type id %lld", (long long)id);
    return false;
  }

  TypeRec* tr = (TypeRec*)arena_alloc(&p->arena, sizeof(TypeRec));
  if (!tr) return false;
  tr->id = id;
  tr->kind = TYPE_INVALID;
  tr->of = 0;
  tr->len = 0;
  tr->name = json_get_string(json_obj_get(obj, "name"));
  tr->ret = 0;
  tr->params = NULL;
  tr->param_len = 0;
  tr->varargs = false;
  tr->fields = NULL;
  tr->field_len = 0;
  tr->lane_ty = 0;
  tr->lanes = 0;
  tr->sig = 0;
  tr->call_sig = 0;
  tr->env_ty = 0;
  tr->variants = NULL;
  tr->variant_len = 0;

  if (strcmp(kind, "prim") == 0) {
    tr->kind = TYPE_PRIM;
    tr->prim = must_string(p, json_obj_get(obj, "prim"), "type.prim");
    if (!tr->prim) return false;
  } else if (strcmp(kind, "ptr") == 0) {
    tr->kind = TYPE_PTR;
    if (!sir_intern_id(p, SIR_ID_TYPE, json_obj_get(obj, "of"), &tr->of, "type.of")) return false;
  } else if (strcmp(kind, "array") == 0) {
    tr->kind = TYPE_ARRAY;
    if (!sir_intern_id(p, SIR_ID_TYPE, json_obj_get(obj, "of"), &tr->of, "type.of")) return false;
    if (!must_i64(p, json_obj_get(obj, "len"), &tr->len, "type.len")) return false;
    if (tr->len < 0) {
      err_codef(p, "sircc.type.array.len.bad", "sircc: type.array len must be >= 0");
      return false;
    }
  } else if (strcmp(kind, "fn") == 0) {
    tr->kind = TYPE_FN;
    JsonValue* params = json_obj_get(obj, "params");
    if (!params || params->type != JSON_ARRAY) {
      err_codef(p, "sircc.type.fn.params.not_array", "sircc: expected array for type.params");
      return false;
    }
    tr->param_len = params->v.arr.len;
    tr->params = (int64_t*)arena_alloc(&p->arena, tr->param_len * sizeof(int64_t));
    if (!tr->params) return false;
    for (size_t i = 0; i < tr->param_len; i++) {
      int64_t pid = 0;
      if (!sir_intern_id(p, SIR_ID_TYPE, params->v.arr.items[i], &pid, "type.params[i]")) return false;
      tr->params[i] = pid;
    }
    if (!sir_intern_id(p, SIR_ID_TYPE, json_obj_get(obj, "ret"), &tr->ret, "type.ret")) return false;
    JsonValue* va = json_obj_get(obj, "varargs");
    if (va && va->type == JSON_BOOL) tr->varargs = va->v.b;
  } else if (strcmp(kind, "struct") == 0) {
    tr->kind = TYPE_STRUCT;
    JsonValue* fields = json_obj_get(obj, "fields");
    if (!fields || fields->type != JSON_ARRAY) {
      err_codef(p, "sircc.type.struct.fields.not_array", "sircc: expected array for type.fields");
      return false;
    }
    tr->field_len = fields->v.arr.len;
    if (tr->field_len) {
      tr->fields = (TypeFieldRec*)arena_alloc(&p->arena, tr->field_len * sizeof(TypeFieldRec));
      if (!tr->fields) return false;
      memset(tr->fields, 0, tr->field_len * sizeof(TypeFieldRec));
    }
    for (size_t i = 0; i < tr->field_len; i++) {
      JsonValue* fo = fields->v.arr.items[i];
      if (!fo || fo->type != JSON_OBJECT) {
        err_codef(p, "sircc.type.struct.field.bad", "sircc: type.fields[%zu] must be an object", i);
        return false;
      }
      const char* fname = must_string(p, json_obj_get(fo, "name"), "type.fields[i].name");
      if (!fname || !is_ident(fname)) {
        err_codef(p, "sircc.schema.ident.bad", "sircc: type.fields[%zu].name must be an Ident", i);
        return false;
      }
      int64_t fty = 0;
      if (!sir_intern_id(p, SIR_ID_TYPE, json_obj_get(fo, "type_ref"), &fty, "type.fields[i].type_ref")) return false;
      tr->fields[i] = (TypeFieldRec){.name = fname, .type_ref = fty};
    }
  } else if (strcmp(kind, "fun") == 0) {
    tr->kind = TYPE_FUN;
    if (!sir_intern_id(p, SIR_ID_TYPE, json_obj_get(obj, "sig"), &tr->sig, "type.sig")) return false;
  } else if (strcmp(kind, "closure") == 0) {
    tr->kind = TYPE_CLOSURE;
    if (!sir_intern_id(p, SIR_ID_TYPE, json_obj_get(obj, "callSig"), &tr->call_sig, "type.callSig")) return false;
    if (!sir_intern_id(p, SIR_ID_TYPE, json_obj_get(obj, "env"), &tr->env_ty, "type.env")) return false;
  } else if (strcmp(kind, "vec") == 0) {
    tr->kind = TYPE_VEC;
    if (!sir_intern_id(p, SIR_ID_TYPE, json_obj_get(obj, "lane"), &tr->lane_ty, "type.lane")) return false;
    if (!must_i64(p, json_obj_get(obj, "lanes"), &tr->lanes, "type.lanes")) return false;
    if (tr->lanes <= 0) {
      err_codef(p, "sircc.type.vec.lanes.bad", "sircc: type.vec lanes must be > 0");
      return false;
    }
  } else if (strcmp(kind, "sum") == 0) {
    tr->kind = TYPE_SUM;
    JsonValue* vars = json_obj_get(obj, "variants");
    if (!vars || vars->type != JSON_ARRAY) {
      err_codef(p, "sircc.type.sum.variants.not_array", "sircc: expected array for type.variants");
      return false;
    }
    tr->variant_len = vars->v.arr.len;
    if (tr->variant_len) {
      tr->variants = (TypeVariantRec*)arena_alloc(&p->arena, tr->variant_len * sizeof(TypeVariantRec));
      if (!tr->variants) return false;
      memset(tr->variants, 0, tr->variant_len * sizeof(TypeVariantRec));
    }
    for (size_t i = 0; i < tr->variant_len; i++) {
      JsonValue* vo = vars->v.arr.items[i];
      if (!vo || vo->type != JSON_OBJECT) {
        err_codef(p, "sircc.type.sum.variant.bad", "sircc: type.variants[%zu] must be an object", i);
        return false;
      }
      const char* vname = json_get_string(json_obj_get(vo, "name"));
      if (vname && *vname && !is_ident(vname)) {
        err_codef(p, "sircc.schema.ident.bad", "sircc: type.variants[%zu].name must be an Ident", i);
        return false;
      }
      int64_t vty = 0;
      JsonValue* tyv = json_obj_get(vo, "ty");
      if (tyv) {
        if (!sir_intern_id(p, SIR_ID_TYPE, tyv, &vty, "type.variants[i].ty")) return false;
      }
      tr->variants[i] = (TypeVariantRec){.name = vname, .ty = vty};
    }
  } else {
    err_codef(p, "sircc.type.kind.unsupported", "sircc: unsupported type kind '%s' (v1 subset)", kind);
    return false;
  }

  p->types[id] = tr;
  return true;
}

static bool parse_node_record(SirProgram* p, JsonValue* obj) {
  static const char* const keys[] = {"ir", "k", "id", "tag", "type_ref", "inputs", "fields", "src_ref", "loc"};
  if (!require_only_keys(p, obj, keys, sizeof(keys) / sizeof(keys[0]), "node record")) return false;

  int64_t id = 0;
  if (!sir_intern_id(p, SIR_ID_NODE, json_obj_get(obj, "id"), &id, "node.id")) return false;
  const char* tag = must_string(p, json_obj_get(obj, "tag"), "node.tag");
  if (!tag) return false;

  int64_t type_ref = 0;
  JsonValue* tr = json_obj_get(obj, "type_ref");
  if (tr) {
    if (!sir_intern_id(p, SIR_ID_TYPE, tr, &type_ref, "node.type_ref")) return false;
  }

  JsonValue* fields = json_obj_get(obj, "fields");
  if (fields && fields->type != JSON_OBJECT) {
    err_codef(p, "sircc.schema.node.fields.not_object", "sircc: expected object for node.fields");
    return false;
  }

  if (!ensure_node_slot(p, id)) return false;
  if (p->nodes[id]) {
    err_codef(p, "sircc.schema.duplicate_id", "sircc: duplicate node id %lld", (long long)id);
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

static bool parse_program_file(SirProgram* p, const SirccOptions* opt, const char* path, size_t max_line_bytes, size_t max_records,
                               size_t* records, char** line, size_t* cap, size_t* len) {
  if (!p || !path || !records || !line || !cap || !len) return false;
  FILE* f = fopen(path, "rb");
  if (!f) {
    err_codef(p, "sircc.io.open_failed", "sircc: failed to open: %s", strerror(errno));
    return false;
  }

  size_t line_no = 0;
  bool too_long = false;
  while (read_line(f, line, cap, len, max_line_bytes, &too_long)) {
    line_no++;
    if (*len == 0 || is_blank_line(*line)) continue;
    (*records)++;
    if (max_records && *records > max_records) {
      err_codef(p, "sircc.limit.records",
                "sircc: input exceeded record limit (%zu) (override via SIRCC_MAX_RECORDS)", max_records);
      fclose(f);
      return false;
    }

    p->cur_path = path;
    p->cur_line = line_no;
    p->cur_kind = NULL;
    p->cur_rec_id = -1;
    p->cur_rec_tag = NULL;
    p->cur_src_ref = -1;
    p->cur_loc.unit = NULL;
    p->cur_loc.line = 0;
    p->cur_loc.col = 0;

    JsonError jerr = {0};
    JsonValue* root = NULL;
    if (!json_parse(&p->arena, *line, &root, &jerr)) {
      err_codef(p, "sircc.json.parse_error", "sircc: JSON parse error at column %zu: %s", jerr.offset + 1, jerr.msg ? jerr.msg : "unknown");
      fclose(f);
      return false;
    }
    if (!must_obj(p, root, "record")) {
      fclose(f);
      return false;
    }

    const char* ir = must_string(p, json_obj_get(root, "ir"), "record.ir");
    const char* k = must_string(p, json_obj_get(root, "k"), "record.k");
    if (!ir || !k) {
      fclose(f);
      return false;
    }
    p->cur_kind = k;

    // Best-effort record metadata for diagnostics.
    JsonValue* idv = json_obj_get(root, "id");
    if (idv) (void)json_get_i64(idv, &p->cur_rec_id);
    if (strcmp(k, "node") == 0) p->cur_rec_tag = json_get_string(json_obj_get(root, "tag"));
    else if (strcmp(k, "instr") == 0) p->cur_rec_tag = json_get_string(json_obj_get(root, "m"));
    else if (strcmp(k, "dir") == 0) p->cur_rec_tag = json_get_string(json_obj_get(root, "d"));

    JsonValue* src_ref = json_obj_get(root, "src_ref");
    if (src_ref) {
      int64_t sid = -1;
      if (!sir_intern_id(p, SIR_ID_SRC, src_ref, &sid, "src_ref")) {
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
      err_codef(p, "sircc.schema.ir.unsupported", "sircc: unsupported ir '%s' (expected sir-v1.0)", ir);
      fclose(f);
      return false;
    }

    if (strcmp(k, "meta") == 0) {
      if (!parse_meta_record(p, opt, root)) {
        fclose(f);
        return false;
      }
      if (opt && opt->dump_records) fprintf(stderr, "%s:%zu: meta\n", p->cur_path, p->cur_line);
      continue;
    }
    if (strcmp(k, "src") == 0) {
      if (!parse_src_record(p, root)) {
        fclose(f);
        return false;
      }
      if (opt && opt->dump_records) fprintf(stderr, "%s:%zu: src\n", p->cur_path, p->cur_line);
      continue;
    }
    if (strcmp(k, "diag") == 0) {
      if (!parse_diag_record(p, root)) {
        fclose(f);
        return false;
      }
      if (opt && opt->dump_records) fprintf(stderr, "%s:%zu: diag\n", p->cur_path, p->cur_line);
      continue;
    }
    if (strcmp(k, "sym") == 0) {
      if (!parse_sym_record(p, root)) {
        fclose(f);
        return false;
      }
      if (opt && opt->dump_records) fprintf(stderr, "%s:%zu: sym\n", p->cur_path, p->cur_line);
      continue;
    }
    if (strcmp(k, "type") == 0) {
      if (!parse_type_record(p, root)) {
        fclose(f);
        return false;
      }
      if (opt && opt->dump_records) fprintf(stderr, "%s:%zu: type\n", p->cur_path, p->cur_line);
      continue;
    }
    if (strcmp(k, "node") == 0) {
      if (!parse_node_record(p, root)) {
        fclose(f);
        return false;
      }
      if (opt && opt->dump_records) fprintf(stderr, "%s:%zu: node\n", p->cur_path, p->cur_line);
      continue;
    }
    if (strcmp(k, "ext") == 0) {
      if (!parse_ext_record(p, root)) {
        fclose(f);
        return false;
      }
      if (opt && opt->dump_records) fprintf(stderr, "%s:%zu: ext\n", p->cur_path, p->cur_line);
      continue;
    }
    if (strcmp(k, "label") == 0) {
      if (!parse_label_record(p, root)) {
        fclose(f);
        return false;
      }
      if (opt && opt->dump_records) fprintf(stderr, "%s:%zu: label\n", p->cur_path, p->cur_line);
      continue;
    }
    if (strcmp(k, "instr") == 0) {
      if (!parse_instr_record(p, opt, root)) {
        fclose(f);
        return false;
      }
      continue;
    }
    if (strcmp(k, "dir") == 0) {
      if (!parse_dir_record(p, root)) {
        fclose(f);
        return false;
      }
      if (opt && opt->dump_records) fprintf(stderr, "%s:%zu: dir\n", p->cur_path, p->cur_line);
      continue;
    }

    err_codef(p, "sircc.schema.record_kind.unknown", "sircc: unknown record kind '%s'", k);
    fclose(f);
    return false;
  }
  fclose(f);
  if (too_long) {
    err_codef(p, "sircc.limit.line_too_long",
              "sircc: JSONL line exceeded limit (%zu bytes) (override via SIRCC_MAX_LINE_BYTES)", max_line_bytes);
    return false;
  }
  return true;
}

bool parse_program(SirProgram* p, const SirccOptions* opt, const char* input_path) {
  p->cur_path = input_path;
  p->cur_line = 0;

  char* line = NULL;
  size_t cap = 0;
  size_t len = 0;

  // Safety limits to keep JSONL ingestion robust under adversarial inputs.
  // These defaults are intentionally high; override via env vars if needed:
  //   SIRCC_MAX_LINE_BYTES, SIRCC_MAX_RECORDS.
  size_t max_line_bytes = 16u * 1024u * 1024u; // 16 MiB per JSONL record line
  uint64_t max_records_u64 = 5ull * 1000ull * 1000ull; // 5,000,000 records
  uint64_t max_line_u64 = 0;
  if (parse_env_u64("SIRCC_MAX_LINE_BYTES", &max_line_u64)) {
    if (max_line_u64 > (uint64_t)SIZE_MAX) max_line_bytes = SIZE_MAX;
    else max_line_bytes = (size_t)max_line_u64;
  }
  (void)parse_env_u64("SIRCC_MAX_RECORDS", &max_records_u64);
  if (max_line_bytes == 0) max_line_bytes = 16u * 1024u * 1024u;
  if (max_records_u64 == 0) max_records_u64 = 5ull * 1000ull * 1000ull;
  size_t max_records = (max_records_u64 > (uint64_t)SIZE_MAX) ? SIZE_MAX : (size_t)max_records_u64;
  size_t records = 0;

  if (opt && opt->prelude_paths && opt->prelude_paths_len) {
    for (size_t i = 0; i < opt->prelude_paths_len; i++) {
      const char* path = opt->prelude_paths[i];
      if (!path || !*path) continue;
      if (!parse_program_file(p, opt, path, max_line_bytes, max_records, &records, &line, &cap, &len)) {
        free(line);
        return false;
      }
    }
  }

  if (!parse_program_file(p, opt, input_path, max_line_bytes, max_records, &records, &line, &cap, &len)) {
    free(line);
    return false;
  }

  free(line);

  for (size_t i = 0; i < p->pending_features_len; i++) {
    const PendingFeatureUse* u = &p->pending_features[i];
    if (!u->need || !u->mnemonic) continue;
    if (has_feature(p, u->need)) continue;

    // Restore the originating record's location metadata so the diagnostic points at the right line.
    p->cur_path = u->path;
    p->cur_line = u->line;
    p->cur_kind = "instr";
    p->cur_rec_id = u->rec_id;
    p->cur_rec_tag = u->mnemonic;
    p->cur_src_ref = -1;
    p->cur_loc.unit = NULL;
    p->cur_loc.line = 0;
    p->cur_loc.col = 0;

    err_codef(p, "sircc.feature.gate", "sircc: mnemonic '%s' requires feature %s (enable via meta.ext.features)", u->mnemonic, u->need);
    return false;
  }

  return true;
}
