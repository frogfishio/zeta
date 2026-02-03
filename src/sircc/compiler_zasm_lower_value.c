// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_zasm_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

bool zasm_lower_value_to_op(
    SirProgram* p,
    ZasmStr* strs,
    size_t strs_len,
    ZasmAlloca* allocas,
    size_t allocas_len,
    int64_t node_id,
    ZasmOp* out) {
  if (!p || !out) return false;
  *out = (ZasmOp){0};

  NodeRec* n = get_node(p, node_id);
  if (!n) {
    errf(p, "sircc: zasm: unknown node id %lld", (long long)node_id);
    return false;
  }

  if (strncmp(n->tag, "const.i", 7) == 0) {
    if (!n->fields) {
      errf(p, "sircc: zasm: %s node %lld missing fields", n->tag, (long long)node_id);
      return false;
    }
    int64_t v = 0;
    if (!must_i64(p, json_obj_get(n->fields, "value"), &v, "const.value")) return false;
    out->k = ZOP_NUM;
    out->n = v;
    return true;
  }

  if (strncmp(n->tag, "alloca.", 7) == 0) {
    const char* sym = zasm_sym_for_alloca(allocas, allocas_len, node_id);
    if (!sym) {
      errf(p, "sircc: zasm: missing alloca symbol mapping for node %lld", (long long)node_id);
      return false;
    }
    out->k = ZOP_SYM;
    out->s = sym;
    return true;
  }

  if (strcmp(n->tag, "cstr") == 0) {
    const char* sym = zasm_sym_for_str(strs, strs_len, node_id);
    if (!sym) {
      errf(p, "sircc: zasm: missing cstr symbol mapping for node %lld", (long long)node_id);
      return false;
    }
    out->k = ZOP_SYM;
    out->s = sym;
    return true;
  }

  if (strcmp(n->tag, "decl.fn") == 0) {
    const char* name = n->fields ? json_get_string(json_obj_get(n->fields, "name")) : NULL;
    if (!name) {
      errf(p, "sircc: zasm: decl.fn node %lld missing fields.name", (long long)node_id);
      return false;
    }
    out->k = ZOP_SYM;
    out->s = name;
    return true;
  }

  if (strcmp(n->tag, "ptr.sym") == 0) {
    const char* name = NULL;
    if (n->fields) name = json_get_string(json_obj_get(n->fields, "name"));
    if (!name) {
      errf(p, "sircc: zasm: ptr.sym node %lld missing fields.name", (long long)node_id);
      return false;
    }
    out->k = ZOP_SYM;
    out->s = name;
    return true;
  }

  if (strcmp(n->tag, "ptr.to_i64") == 0) {
    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
      errf(p, "sircc: zasm: ptr.to_i64 node %lld requires args:[x]", (long long)node_id);
      return false;
    }
    int64_t x_id = 0;
    if (!parse_node_ref_id(args->v.arr.items[0], &x_id)) {
      errf(p, "sircc: zasm: ptr.to_i64 node %lld arg must be node ref", (long long)node_id);
      return false;
    }
    return zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, x_id, out);
  }

  if (strcmp(n->tag, "name") == 0) {
    const char* name = n->fields ? json_get_string(json_obj_get(n->fields, "name")) : NULL;
    errf(p, "sircc: zasm: name '%s' not supported yet (node %lld)", name ? name : "(null)", (long long)node_id);
    return false;
  }

  errf(p, "sircc: zasm: unsupported value node '%s' (node %lld)", n->tag, (long long)node_id);
  return false;
}

