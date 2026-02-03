// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_zasm_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NodeRec* zasm_find_fn(SirProgram* p, const char* name) {
  if (!p || !name) return NULL;
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if (strcmp(n->tag, "fn") != 0) continue;
    const char* nm = n->fields ? json_get_string(json_obj_get(n->fields, "name")) : NULL;
    if (nm && strcmp(nm, name) == 0) return n;
  }
  return NULL;
}

const char* zasm_sym_for_str(ZasmStr* strs, size_t n, int64_t node_id) {
  for (size_t i = 0; i < n; i++) {
    if (strs[i].node_id == node_id) return strs[i].sym;
  }
  return NULL;
}

const char* zasm_sym_for_alloca(ZasmAlloca* as, size_t n, int64_t node_id) {
  for (size_t i = 0; i < n; i++) {
    if (as[i].node_id == node_id) return as[i].sym;
  }
  return NULL;
}

bool zasm_collect_cstrs(SirProgram* p, ZasmStr** out_strs, size_t* out_len) {
  if (!p || !out_strs || !out_len) return false;
  *out_strs = NULL;
  *out_len = 0;

  size_t cap = 0;
  size_t len = 0;
  ZasmStr* strs = NULL;

  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if (strcmp(n->tag, "cstr") != 0) continue;
    if (!n->fields) continue;
    const char* s = json_get_string(json_obj_get(n->fields, "value"));
    if (!s) continue;

    if (len == cap) {
      size_t next = cap ? cap * 2 : 8;
      ZasmStr* bigger = (ZasmStr*)realloc(strs, next * sizeof(ZasmStr));
      if (!bigger) {
        free(strs);
        return false;
      }
      strs = bigger;
      cap = next;
    }

    char name_buf[64];
    snprintf(name_buf, sizeof(name_buf), "str_%lld", (long long)n->id);
    char* sym = arena_strdup(&p->arena, name_buf);
    if (!sym) {
      free(strs);
      return false;
    }

    for (const char* it = s; *it; it++) {
      if (*it == '\0') {
        free(strs);
        errf(p, "sircc: zasm STR cannot contain NUL bytes");
        return false;
      }
    }

    strs[len++] = (ZasmStr){
        .node_id = n->id,
        .sym = sym,
        .value = s,
        .len = (int64_t)strlen(s),
    };
  }

  *out_strs = strs;
  *out_len = len;
  return true;
}

static bool alloca_size_for_tag(const char* tag, int64_t* out_size_bytes) {
  if (!tag || !out_size_bytes) return false;
  *out_size_bytes = 0;

  const char* suffix = NULL;
  if (strncmp(tag, "alloca.", 7) == 0) suffix = tag + 7;
  if (!suffix || !*suffix) return false;

  if (strcmp(suffix, "i8") == 0) {
    *out_size_bytes = 1;
    return true;
  }
  if (strcmp(suffix, "i16") == 0) {
    *out_size_bytes = 2;
    return true;
  }
  if (strcmp(suffix, "i32") == 0 || strcmp(suffix, "f32") == 0) {
    *out_size_bytes = 4;
    return true;
  }
  if (strcmp(suffix, "i64") == 0 || strcmp(suffix, "f64") == 0 || strcmp(suffix, "ptr") == 0) {
    *out_size_bytes = 8;
    return true;
  }
  return false;
}

bool zasm_collect_allocas(SirProgram* p, ZasmAlloca** out_as, size_t* out_len) {
  if (!p || !out_as || !out_len) return false;
  *out_as = NULL;
  *out_len = 0;

  size_t cap = 0;
  size_t len = 0;
  ZasmAlloca* as = NULL;

  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if (strncmp(n->tag, "alloca.", 7) != 0) continue;

    int64_t size_bytes = 0;
    if (!alloca_size_for_tag(n->tag, &size_bytes)) continue;

    if (len == cap) {
      size_t next = cap ? cap * 2 : 8;
      ZasmAlloca* bigger = (ZasmAlloca*)realloc(as, next * sizeof(ZasmAlloca));
      if (!bigger) {
        free(as);
        return false;
      }
      as = bigger;
      cap = next;
    }

    char name_buf[64];
    snprintf(name_buf, sizeof(name_buf), "alloc_%lld", (long long)n->id);
    char* sym = arena_strdup(&p->arena, name_buf);
    if (!sym) {
      free(as);
      return false;
    }

    as[len++] = (ZasmAlloca){
        .node_id = n->id,
        .sym = sym,
        .size_bytes = size_bytes,
    };
  }

  *out_as = as;
  *out_len = len;
  return true;
}

bool zasm_collect_decl_fns(SirProgram* p, const char*** out_names, size_t* out_len) {
  if (!p || !out_names || !out_len) return false;
  *out_names = NULL;
  *out_len = 0;

  size_t cap = 0;
  size_t len = 0;
  const char** names = NULL;

  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if (strcmp(n->tag, "decl.fn") != 0) continue;
    const char* name = n->fields ? json_get_string(json_obj_get(n->fields, "name")) : NULL;
    if (!name) continue;

    bool seen = false;
    for (size_t j = 0; j < len; j++) {
      if (strcmp(names[j], name) == 0) {
        seen = true;
        break;
      }
    }
    if (seen) continue;

    if (len == cap) {
      size_t next = cap ? cap * 2 : 8;
      const char** bigger = (const char**)realloc(names, next * sizeof(const char*));
      if (!bigger) {
        free(names);
        return false;
      }
      names = bigger;
      cap = next;
    }
    names[len++] = name;
  }

  *out_names = names;
  *out_len = len;
  return true;
}

