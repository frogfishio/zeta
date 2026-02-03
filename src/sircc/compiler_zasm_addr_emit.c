// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_zasm_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

static bool emit_ld(FILE* out, const char* dst_reg, const ZasmOp* src, int64_t line_no) {
  if (!out || !dst_reg || !src) return false;
  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"LD\",\"ops\":[");
  zasm_write_op_reg(out, dst_reg);
  fprintf(out, ",");
  if (!zasm_write_op(out, src)) return false;
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
  return true;
}

static bool emit_hl_binop(FILE* out, const char* m, const ZasmOp* rhs, int64_t line_no) {
  if (!out || !m || !rhs) return false;
  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":");
  json_write_escaped(out, m);
  fprintf(out, ",\"ops\":[");
  zasm_write_op_reg(out, "HL");
  fprintf(out, ",");
  if (!zasm_write_op(out, rhs)) return false;
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
  return true;
}

static bool emit_load_slot_into_reg(FILE* out, const char* dst_reg, const char* slot_sym, int64_t width_bytes, int64_t line_no) {
  if (!out || !dst_reg || !slot_sym) return false;

  const char* m = NULL;
  int64_t hint = 0;
  if (width_bytes == 1) {
    m = "LD8U";
    hint = 1;
  } else if (width_bytes == 2) {
    m = "LD16U";
    hint = 2;
  } else if (width_bytes == 4) {
    m = "LD32U64";
    hint = 4;
  } else if (width_bytes == 8) {
    m = "LD64";
    hint = 8;
  } else {
    return false;
  }

  ZasmOp base = {.k = ZOP_SYM, .s = slot_sym};
  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":");
  json_write_escaped(out, m);
  fprintf(out, ",\"ops\":[");
  zasm_write_op_reg(out, dst_reg);
  fprintf(out, ",");
  zasm_write_op_mem(out, &base, 0, hint);
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
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

static bool add_checked_i64(int64_t a, int64_t b, int64_t* out) {
  if (!out) return false;
  if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;
  *out = a + b;
  return true;
}

static bool mul_checked_i64(int64_t a, int64_t b, int64_t* out) {
  if (!out) return false;
  if (a == 0 || b == 0) {
    *out = 0;
    return true;
  }
  if (a == -1 && b == INT64_MIN) return false;
  if (b == -1 && a == INT64_MIN) return false;
  __int128 prod = (__int128)a * (__int128)b;
  if (prod > INT64_MAX || prod < INT64_MIN) return false;
  *out = (int64_t)prod;
  return true;
}

static bool try_lower_addr_const(
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
  if (!n) return false;

  if (strncmp(n->tag, "alloca.", 7) == 0) {
    const char* sym = zasm_sym_for_alloca(allocas, allocas_len, addr_id);
    if (!sym) return false;
    out_base->k = ZOP_SYM;
    out_base->s = sym;
    *out_disp = 0;
    return true;
  }

  if (strcmp(n->tag, "ptr.sym") == 0) {
    const char* name = n->fields ? json_get_string(json_obj_get(n->fields, "name")) : NULL;
    if (!name) return false;
    out_base->k = ZOP_SYM;
    out_base->s = name;
    *out_disp = 0;
    return true;
  }

  if (strcmp(n->tag, "name") == 0) {
    ZasmOp op = {0};
    if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, addr_id, &op)) return false;
    if (op.k != ZOP_SYM) return false;
    *out_base = op;
    *out_disp = 0;
    return true;
  }

  if (strcmp(n->tag, "ptr.add") == 0) {
    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) return false;
    int64_t base_id = 0, off_id = 0;
    if (!parse_node_ref_id(args->v.arr.items[0], &base_id) || !parse_node_ref_id(args->v.arr.items[1], &off_id)) return false;
    int64_t off = 0;
    if (!is_const_i64(p, off_id, &off)) return false;

    ZasmOp base = {0};
    int64_t disp = 0;
    if (!try_lower_addr_const(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, base_id, &base, &disp)) return false;
    if (!add_checked_i64(disp, off, &disp)) return false;
    *out_base = base;
    *out_disp = disp;
    return true;
  }

  if (strcmp(n->tag, "ptr.offset") == 0) {
    int64_t ty_id = 0;
    if (!parse_type_ref_id(n->fields ? json_obj_get(n->fields, "ty") : NULL, &ty_id)) return false;
    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) return false;
    int64_t base_id = 0, idx_id = 0;
    if (!parse_node_ref_id(args->v.arr.items[0], &base_id) || !parse_node_ref_id(args->v.arr.items[1], &idx_id)) return false;
    int64_t idx = 0;
    if (!is_const_i64(p, idx_id, &idx)) return false;

    int64_t elem_size = 0;
    int64_t elem_align = 0;
    if (!type_size_align(p, ty_id, &elem_size, &elem_align)) return false;
    int64_t scaled = 0;
    if (!mul_checked_i64(idx, elem_size, &scaled)) return false;

    ZasmOp base = {0};
    int64_t disp = 0;
    if (!try_lower_addr_const(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, base_id, &base, &disp)) return false;
    if (!add_checked_i64(disp, scaled, &disp)) return false;
    *out_base = base;
    *out_disp = disp;
    return true;
  }

  return false;
}

static bool materialize_value_i64_into_reg(
    FILE* out,
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
    const char* dst_reg,
    int64_t* io_line) {
  if (!out || !p || !dst_reg || !io_line) return false;
  ZasmOp op = {0};
  if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, node_id, &op)) return false;
  if (op.k == ZOP_SLOT) return emit_load_slot_into_reg(out, dst_reg, op.s, op.n, (*io_line)++);
  if (op.k == ZOP_NUM || op.k == ZOP_SYM || op.k == ZOP_REG) return emit_ld(out, dst_reg, &op, (*io_line)++);
  return false;
}

static bool materialize_addr_into_hl(
    FILE* out,
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
    int64_t* io_line) {
  if (!out || !p || !io_line) return false;

  ZasmOp base = {0};
  int64_t disp = 0;
  if (try_lower_addr_const(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, addr_id, &base, &disp)) {
    if (!emit_ld(out, "HL", &base, (*io_line)++)) return false;
    if (disp) {
      ZasmOp imm = {.k = ZOP_NUM, .n = disp};
      if (!emit_hl_binop(out, "ADD64", &imm, (*io_line)++)) return false;
    }
    return true;
  }

  NodeRec* n = get_node(p, addr_id);
  if (!n) {
    errf(p, "sircc: zasm: unknown address node %lld", (long long)addr_id);
    return false;
  }

  if (strcmp(n->tag, "name") == 0) {
    return materialize_value_i64_into_reg(out, p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, addr_id, "HL", io_line);
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

    if (!materialize_addr_into_hl(out, p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, base_id, io_line)) return false;
    if (!materialize_value_i64_into_reg(out, p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, off_id, "DE", io_line))
      return false;
    ZasmOp rhs = {.k = ZOP_REG, .s = "DE"};
    if (!emit_hl_binop(out, "ADD64", &rhs, (*io_line)++)) return false;
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

    // Base -> HL, then preserve into DE.
    if (!materialize_addr_into_hl(out, p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, base_id, io_line)) return false;
    ZasmOp hl = {.k = ZOP_REG, .s = "HL"};
    if (!emit_ld(out, "DE", &hl, (*io_line)++)) return false;

    // idx -> HL
    if (!materialize_value_i64_into_reg(out, p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, idx_id, "HL", io_line))
      return false;

    int64_t elem_size = 0;
    int64_t elem_align = 0;
    if (!type_size_align(p, ty_id, &elem_size, &elem_align)) return false;

    ZasmOp sz = {.k = ZOP_NUM, .n = elem_size};
    if (!emit_hl_binop(out, "MUL64", &sz, (*io_line)++)) return false;

    ZasmOp rhs = {.k = ZOP_REG, .s = "DE"};
    if (!emit_hl_binop(out, "ADD64", &rhs, (*io_line)++)) return false;
    return true;
  }

  errf(p, "sircc: zasm: unsupported dynamic address node '%s' (node %lld)", n->tag, (long long)addr_id);
  return false;
}

bool zasm_emit_addr_to_mem(
    FILE* out,
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
    int64_t* out_disp,
    int64_t* io_line) {
  if (!out || !p || !out_base || !out_disp || !io_line) return false;

  // If the address is const-foldable into (sym + disp), don't emit any code.
  if (try_lower_addr_const(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, addr_id, out_base, out_disp)) return true;

  // Otherwise, compute address into HL and move into DE for use as mem base.
  if (!materialize_addr_into_hl(out, p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, addr_id, io_line)) return false;
  ZasmOp hl = {.k = ZOP_REG, .s = "HL"};
  if (!emit_ld(out, "DE", &hl, (*io_line)++)) return false;
  out_base->k = ZOP_REG;
  out_base->s = "DE";
  *out_disp = 0;
  return true;
}
