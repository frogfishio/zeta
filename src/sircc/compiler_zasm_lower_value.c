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
    ZasmNameBinding* names,
    size_t names_len,
    ZasmBParamSlot* bps,
    size_t bps_len,
    int64_t node_id,
    ZasmOp* out) {
  if (!p || !out) return false;
  *out = (ZasmOp){0};

  NodeRec* n = get_node(p, node_id);
  if (!n) {
    zasm_err_node_codef(p, node_id, NULL, "sircc.zasm.node.unknown", "sircc: zasm: unknown node id %lld", (long long)node_id);
    return false;
  }

  if (strncmp(n->tag, "const.i", 7) == 0) {
    if (!n->fields) {
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.node.missing_fields", "sircc: zasm: %s node %lld missing fields", n->tag,
                          (long long)node_id);
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
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.mapping.missing",
                          "sircc: zasm: missing alloca symbol mapping for node %lld", (long long)node_id);
      return false;
    }
    out->k = ZOP_SYM;
    out->s = sym;
    return true;
  }

  if (strcmp(n->tag, "bparam") == 0) {
    for (size_t i = 0; i < bps_len; i++) {
      if (bps[i].node_id == node_id) {
        out->k = ZOP_SLOT;
        out->s = bps[i].sym;
        out->n = bps[i].size_bytes;
        return true;
      }
    }
    zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.mapping.missing", "sircc: zasm: missing bparam slot mapping for node %lld",
                        (long long)node_id);
    return false;
  }

  if (strcmp(n->tag, "cstr") == 0) {
    const char* sym = zasm_sym_for_str(strs, strs_len, node_id);
    if (!sym) {
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.mapping.missing", "sircc: zasm: missing cstr symbol mapping for node %lld",
                          (long long)node_id);
      return false;
    }
    out->k = ZOP_SYM;
    out->s = sym;
    return true;
  }

  if (strcmp(n->tag, "decl.fn") == 0) {
    const char* name = n->fields ? json_get_string(json_obj_get(n->fields, "name")) : NULL;
    if (!name) {
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.node.missing_field",
                          "sircc: zasm: decl.fn node %lld missing fields.name", (long long)node_id);
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
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.node.missing_field",
                          "sircc: zasm: ptr.sym node %lld missing fields.name", (long long)node_id);
      return false;
    }
    out->k = ZOP_SYM;
    out->s = name;
    return true;
  }

  if (strcmp(n->tag, "ptr.to_i64") == 0) {
    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.value.bad_args",
                          "sircc: zasm: ptr.to_i64 node %lld requires args:[x]", (long long)node_id);
      return false;
    }
    int64_t x_id = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &x_id)) {
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.value.bad_args",
                          "sircc: zasm: ptr.to_i64 node %lld arg must be node ref", (long long)node_id);
      return false;
    }
    return zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, x_id, out);
  }

  if (strcmp(n->tag, "name") == 0) {
    const char* name = n->fields ? json_get_string(json_obj_get(n->fields, "name")) : NULL;
    if (!name) {
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.node.missing_field", "sircc: zasm: name node %lld missing fields.name",
                          (long long)node_id);
      return false;
    }
    for (size_t i = 0; i < names_len; i++) {
      if (names[i].name && strcmp(names[i].name, name) == 0) {
        if (names[i].is_slot) {
          out->k = ZOP_SLOT;
          out->s = names[i].op.s;
          out->n = names[i].slot_size_bytes;
          return true;
        }
        *out = names[i].op;
        return true;
      }
    }
    zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.name.unknown", "sircc: zasm: unknown name '%s' (node %lld)", name,
                        (long long)node_id);
    return false;
  }

  zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.value.unsupported", "sircc: zasm: unsupported value node '%s' (node %lld)", n->tag,
                      (long long)node_id);
  return false;
}
