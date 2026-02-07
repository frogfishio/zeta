// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_ids.h"

#include "compiler_internal.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static uint64_t fnv1a64(const void* data, size_t len) {
  const unsigned char* p = (const unsigned char*)data;
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint64_t)p[i];
    h *= 1099511628211ull;
  }
  return h;
}

static uint64_t hash_i64(int64_t v) {
  // SplitMix64
  uint64_t x = (uint64_t)v;
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  x = x ^ (x >> 31);
  return x ? x : 1; // never 0
}

static void idmap_init(SirIdMap* m) {
  if (!m) return;
  memset(m, 0, sizeof(*m));
  m->next_id = 1;
}

static void idmap_free(SirIdMap* m) {
  if (!m) return;
  free(m->entries);
  memset(m, 0, sizeof(*m));
}

static bool key_eq(const SirIdMapEntry* e, bool is_str, int64_t ikey, const char* s, size_t slen) {
  if (!e->used) return false;
  if (e->is_str != is_str) return false;
  if (!is_str) return e->ikey == ikey;
  if (e->slen != slen) return false;
  return memcmp(e->skey, s, slen) == 0;
}

static bool idmap_grow(SirIdMap* m, size_t new_cap) {
  if (!m) return false;
  SirIdMapEntry* old = m->entries;
  size_t old_cap = m->cap;

  SirIdMapEntry* ne = (SirIdMapEntry*)calloc(new_cap, sizeof(SirIdMapEntry));
  if (!ne) return false;

  m->entries = ne;
  m->cap = new_cap;
  m->len = 0;

  for (size_t i = 0; i < old_cap; i++) {
    SirIdMapEntry* e = &old[i];
    if (!e->used) continue;

    size_t mask = new_cap - 1;
    size_t idx = (size_t)e->hash & mask;
    for (;;) {
      SirIdMapEntry* dst = &m->entries[idx];
      if (!dst->used) {
        *dst = *e;
        m->len++;
        break;
      }
      idx = (idx + 1) & mask;
    }
  }

  free(old);
  return true;
}

static bool idmap_get_or_put(SirProgram* p, SirIdMap* m, bool is_str, int64_t ikey, const char* s, size_t slen,
                             int64_t* out) {
  if (!p || !m || !out) return false;

  if (m->cap == 0) {
    if (!idmap_grow(m, 256)) return false;
  }
  if ((m->len + 1) * 10 >= m->cap * 7) {
    if (!idmap_grow(m, m->cap * 2)) return false;
  }

  uint64_t h = is_str ? fnv1a64(s, slen) : hash_i64(ikey);
  if (h == 0) h = 1;

  size_t mask = m->cap - 1;
  size_t idx = (size_t)h & mask;
  for (;;) {
    SirIdMapEntry* e = &m->entries[idx];
    if (!e->used) {
      e->used = true;
      e->hash = h;
      e->is_str = is_str;
      e->ikey = ikey;
      e->skey = s;
      e->slen = slen;
      e->val = m->next_id++;
      m->len++;
      *out = e->val;
      return true;
    }
    if (e->hash == h && key_eq(e, is_str, ikey, s, slen)) {
      *out = e->val;
      return true;
    }
    idx = (idx + 1) & mask;
  }
}

static SirIdMap* map_for(SirProgram* p, SirIdKind kind) {
  if (!p) return NULL;
  switch (kind) {
    case SIR_ID_SRC:
      return &p->src_ids;
    case SIR_ID_SYM:
      return &p->sym_ids;
    case SIR_ID_TYPE:
      return &p->type_ids;
    case SIR_ID_NODE:
      return &p->node_ids;
    default:
      return NULL;
  }
}

void sir_idmaps_init(SirProgram* p) {
  if (!p) return;
  idmap_init(&p->src_ids);
  idmap_init(&p->sym_ids);
  idmap_init(&p->type_ids);
  idmap_init(&p->node_ids);
}

void sir_idmaps_free(SirProgram* p) {
  if (!p) return;
  idmap_free(&p->src_ids);
  idmap_free(&p->sym_ids);
  idmap_free(&p->type_ids);
  idmap_free(&p->node_ids);
}

const char* sir_id_str_for_internal(SirProgram* p, SirIdKind kind, int64_t internal_id) {
  if (!p || internal_id == 0) return NULL;
  SirIdMap* m = map_for(p, kind);
  if (!m || !m->entries || m->cap == 0) return NULL;
  for (size_t i = 0; i < m->cap; i++) {
    const SirIdMapEntry* e = &m->entries[i];
    if (!e->used) continue;
    if (!e->is_str) continue;
    if (e->val != internal_id) continue;
    return e->skey;
  }
  return NULL;
}

bool sir_intern_id(SirProgram* p, SirIdKind kind, const JsonValue* v, int64_t* out_id, const char* ctx) {
  if (!p || !out_id) return false;
  if (!v) {
    err_codef(p, "sircc.id.missing", "sircc: missing id for %s", ctx ? ctx : "id");
    return false;
  }
  SirIdMap* m = map_for(p, kind);
  if (!m) return false;

  int64_t i = 0;
  if (json_get_i64((JsonValue*)v, &i)) {
    if (i < 0) {
      err_codef(p, "sircc.id.invalid", "sircc: id must be >= 0 for %s", ctx ? ctx : "id");
      return false;
    }
    // Preserve numeric ids as-is for stable diagnostics and compatibility with
    // existing corpora. Ensure string ids allocated later don't collide.
    if (m->next_id <= i) m->next_id = i + 1;
    *out_id = i;
    return true;
  }

  const char* s = json_get_string((JsonValue*)v);
  if (s && *s) {
    size_t slen = strlen(s);
    // json_parse allocates strings in the program arena, so the pointer is stable.
    return idmap_get_or_put(p, m, true, 0, s, slen, out_id);
  }

  err_codef(p, "sircc.id.invalid", "sircc: expected integer or string for %s", ctx ? ctx : "id");
  return false;
}

static bool parse_ref_id_kind(SirProgram* p, SirIdKind kind, const JsonValue* v, int64_t* out_id, const char* ctx) {
  if (!v || v->type != JSON_OBJECT) return false;
  const char* ts = json_get_string(json_obj_get((JsonValue*)v, "t"));
  if (!ts || strcmp(ts, "ref") != 0) return false;
  const char* k = json_get_string(json_obj_get((JsonValue*)v, "k"));
  if (k) {
    if (kind == SIR_ID_NODE && strcmp(k, "node") != 0) return false;
    if (kind == SIR_ID_TYPE && strcmp(k, "type") != 0) return false;
    if (kind == SIR_ID_SYM && strcmp(k, "sym") != 0) return false;
  }
  return sir_intern_id(p, kind, json_obj_get((JsonValue*)v, "id"), out_id, ctx);
}

bool parse_node_ref_id(SirProgram* p, const JsonValue* v, int64_t* out_id) {
  return parse_ref_id_kind(p, SIR_ID_NODE, v, out_id, "node ref");
}

bool parse_sym_ref_id(SirProgram* p, const JsonValue* v, int64_t* out_id) {
  return parse_ref_id_kind(p, SIR_ID_SYM, v, out_id, "sym ref");
}

bool parse_type_ref_id(SirProgram* p, const JsonValue* v, int64_t* out_id) {
  if (!v) return false;
  // Accept direct TypeId (int or string).
  if (v->type == JSON_NUMBER || v->type == JSON_STRING) {
    return sir_intern_id(p, SIR_ID_TYPE, v, out_id, "type ref");
  }
  // Or a typed ref object.
  if (parse_ref_id_kind(p, SIR_ID_TYPE, v, out_id, "type ref")) return true;
  return false;
}
