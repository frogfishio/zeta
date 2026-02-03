// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_zasm_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool add_checked_i64(SirProgram* p, int64_t a, int64_t b, int64_t* out) {
  if (!out) return false;
  if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
    errf(p, "sircc: zasm: address displacement overflow");
    return false;
  }
  *out = a + b;
  return true;
}

static bool mul_checked_i64(SirProgram* p, int64_t a, int64_t b, int64_t* out) {
  if (!out) return false;
  if (a == 0 || b == 0) {
    *out = 0;
    return true;
  }
  if (a == -1 && b == INT64_MIN) {
    errf(p, "sircc: zasm: address displacement overflow");
    return false;
  }
  if (b == -1 && a == INT64_MIN) {
    errf(p, "sircc: zasm: address displacement overflow");
    return false;
  }
  __int128 prod = (__int128)a * (__int128)b;
  if (prod > INT64_MAX || prod < INT64_MIN) {
    errf(p, "sircc: zasm: address displacement overflow");
    return false;
  }
  *out = (int64_t)prod;
  return true;
}

static bool is_const_i64(SirProgram* p, int64_t node_id, int64_t* out) {
  if (!p || !out) return false;
  NodeRec* n = get_node(p, node_id);
  if (!n || strcmp(n->tag, "const.i64") != 0 || !n->fields) return false;
  int64_t v = 0;
  if (!must_i64(p, json_obj_get(n->fields, "value"), &v, "const.value")) return false;
  *out = v;
  return true;
}

bool zasm_lower_addr_to_mem(
    SirProgram* p,
    ZasmStr* strs,
    size_t strs_len,
    ZasmAlloca* allocas,
    size_t allocas_len,
    ZasmNameBinding* names,
    size_t names_len,
    ZasmBParamSlot* bps,
    size_t bps_len,
    int64_t addr_id,
    ZasmOp* out_base,
    int64_t* out_disp) {
  if (!p || !out_base || !out_disp) return false;
  *out_base = (ZasmOp){0};
  *out_disp = 0;

  NodeRec* n = get_node(p, addr_id);
  if (!n) {
    errf(p, "sircc: zasm: unknown address node %lld", (long long)addr_id);
    return false;
  }

  if (strncmp(n->tag, "alloca.", 7) == 0) {
    const char* sym = zasm_sym_for_alloca(allocas, allocas_len, addr_id);
    if (!sym) {
      errf(p, "sircc: zasm: missing alloca symbol mapping for node %lld", (long long)addr_id);
      return false;
    }
    out_base->k = ZOP_SYM;
    out_base->s = sym;
    *out_disp = 0;
    return true;
  }

  if (strcmp(n->tag, "ptr.sym") == 0) {
    const char* name = n->fields ? json_get_string(json_obj_get(n->fields, "name")) : NULL;
    if (!name) {
      errf(p, "sircc: zasm: ptr.sym node %lld missing fields.name", (long long)addr_id);
      return false;
    }
    out_base->k = ZOP_SYM;
    out_base->s = name;
    *out_disp = 0;
    return true;
  }

  if (strcmp(n->tag, "name") == 0) {
    ZasmOp op = {0};
    if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, addr_id, &op)) return false;
    if (op.k == ZOP_SYM) {
      *out_base = op;
      *out_disp = 0;
      return true;
    }
    errf(p, "sircc: zasm: address name must resolve to a symbol (got kind %d)", (int)op.k);
    return false;
  }

  if (strcmp(n->tag, "ptr.add") == 0) {
    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) {
      errf(p, "sircc: zasm: ptr.add node %lld requires args:[base, off]", (long long)addr_id);
      return false;
    }
    int64_t base_id = 0, off_id = 0;
    if (!parse_node_ref_id(args->v.arr.items[0], &base_id) || !parse_node_ref_id(args->v.arr.items[1], &off_id)) {
      errf(p, "sircc: zasm: ptr.add node %lld args must be node refs", (long long)addr_id);
      return false;
    }
    int64_t off = 0;
    if (!is_const_i64(p, off_id, &off)) {
      errf(p, "sircc: zasm: ptr.add offset must be const.i64 (node %lld)", (long long)off_id);
      return false;
    }

    ZasmOp base = {0};
    int64_t disp = 0;
    if (!zasm_lower_addr_to_mem(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, base_id, &base, &disp)) return false;
    if (!add_checked_i64(p, disp, off, &disp)) return false;
    *out_base = base;
    *out_disp = disp;
    return true;
  }

  if (strcmp(n->tag, "ptr.offset") == 0) {
    int64_t ty_id = 0;
    if (!parse_type_ref_id(n->fields ? json_obj_get(n->fields, "ty") : NULL, &ty_id)) {
      errf(p, "sircc: zasm: ptr.offset node %lld missing fields.ty type ref", (long long)addr_id);
      return false;
    }
    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) {
      errf(p, "sircc: zasm: ptr.offset node %lld requires args:[base, idx]", (long long)addr_id);
      return false;
    }
    int64_t base_id = 0, idx_id = 0;
    if (!parse_node_ref_id(args->v.arr.items[0], &base_id) || !parse_node_ref_id(args->v.arr.items[1], &idx_id)) {
      errf(p, "sircc: zasm: ptr.offset node %lld args must be node refs", (long long)addr_id);
      return false;
    }
    int64_t idx = 0;
    if (!is_const_i64(p, idx_id, &idx)) {
      errf(p, "sircc: zasm: ptr.offset idx must be const.i64 (node %lld)", (long long)idx_id);
      return false;
    }

    int64_t elem_size = 0;
    int64_t elem_align = 0;
    if (!type_size_align(p, ty_id, &elem_size, &elem_align)) return false;
    int64_t scaled = 0;
    if (!mul_checked_i64(p, idx, elem_size, &scaled)) return false;

    ZasmOp base = {0};
    int64_t disp = 0;
    if (!zasm_lower_addr_to_mem(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, base_id, &base, &disp)) return false;
    if (!add_checked_i64(p, disp, scaled, &disp)) return false;
    *out_base = base;
    *out_disp = disp;
    return true;
  }

  errf(p, "sircc: zasm: unsupported address node '%s' (node %lld)", n->tag, (long long)addr_id);
  return false;
}
