// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_zasm_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool zasm_op_is_value(const ZasmOp* op) {
  if (!op) return false;
  return op->k == ZOP_REG || op->k == ZOP_SYM || op->k == ZOP_NUM;
}

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

static const char* call_arg_reg(size_t idx1) {
  // ZASM64 Lembeh ABI ordering: HL, DE, BC, IX
  switch (idx1) {
    case 1:
      return "HL";
    case 2:
      return "DE";
    case 3:
      return "BC";
    case 4:
      return "IX";
    default:
      return NULL;
  }
}

static bool emit_load_slot_into_reg(FILE* out, const char* dst_reg, const ZasmOp* slot, int64_t line_no) {
  if (!out || !dst_reg || !slot || slot->k != ZOP_SLOT || !slot->s) return false;

  ZasmOp base = {.k = ZOP_SYM, .s = slot->s};
  if (slot->n == 1) {
    zasm_write_ir_k(out, "instr");
    fprintf(out, ",\"m\":\"LD8U\",\"ops\":[");
    zasm_write_op_reg(out, dst_reg);
    fprintf(out, ",");
    zasm_write_op_mem(out, &base, 0, 1);
    fprintf(out, "]");
    zasm_write_loc(out, line_no);
    fprintf(out, "}\n");
    return true;
  }

  if (slot->n == 2) {
    zasm_write_ir_k(out, "instr");
    fprintf(out, ",\"m\":\"LD16U\",\"ops\":[");
    zasm_write_op_reg(out, dst_reg);
    fprintf(out, ",");
    zasm_write_op_mem(out, &base, 0, 2);
    fprintf(out, "]");
    zasm_write_loc(out, line_no);
    fprintf(out, "}\n");
    return true;
  }

  if (slot->n == 4) {
    zasm_write_ir_k(out, "instr");
    fprintf(out, ",\"m\":\"LD32U64\",\"ops\":[");
    zasm_write_op_reg(out, dst_reg);
    fprintf(out, ",");
    zasm_write_op_mem(out, &base, 0, 4);
    fprintf(out, "]");
    zasm_write_loc(out, line_no);
    fprintf(out, "}\n");
    return true;
  }

  if (slot->n != 8) return false;

  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"LD64\",\"ops\":[");
  zasm_write_op_reg(out, dst_reg);
  fprintf(out, ",");
  zasm_write_op_mem(out, &base, 0, 8);
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
  return true;
}

bool zasm_emit_call_stmt(
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
    int64_t call_id,
    int64_t* io_line) {
  if (!io_line) return false;
  NodeRec* n = get_node(p, call_id);
  if (!n || !n->fields) return false;

  JsonValue* args = json_obj_get(n->fields, "args");
  if (!args || args->type != JSON_ARRAY || args->v.arr.len < 1) {
    errf(p, "sircc: zasm: %s node %lld missing args array", n->tag, (long long)call_id);
    return false;
  }

  int64_t callee_id = 0;
  if (!parse_node_ref_id(args->v.arr.items[0], &callee_id)) {
    errf(p, "sircc: zasm: %s node %lld args[0] must be node ref", n->tag, (long long)call_id);
    return false;
  }
  ZasmOp callee = {0};
  if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, callee_id, &callee) || callee.k != ZOP_SYM) {
    errf(p, "sircc: zasm: %s node %lld callee must be a direct symbol (decl.fn/ptr.sym)", n->tag, (long long)call_id);
    return false;
  }

  size_t op_count = args->v.arr.len;
  ZasmOp* lowered = (ZasmOp*)calloc(op_count, sizeof(ZasmOp));
  if (!lowered) {
    errf(p, "sircc: zasm: out of memory");
    return false;
  }
  lowered[0] = callee;

  for (size_t i = 1; i < args->v.arr.len; i++) {
    int64_t aid = 0;
    if (!parse_node_ref_id(args->v.arr.items[i], &aid)) {
      free(lowered);
      errf(p, "sircc: zasm: %s node %lld arg[%zu] must be node ref", n->tag, (long long)call_id, i);
      return false;
    }
    ZasmOp op = {0};
    if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, aid, &op)) {
      free(lowered);
      return false;
    }

    if (!zasm_op_is_value(&op)) {
      if (op.k != ZOP_SLOT) {
        free(lowered);
        errf(p, "sircc: zasm: %s node %lld arg[%zu] unsupported", n->tag, (long long)call_id, i);
        return false;
      }
      const char* reg = call_arg_reg(i);
      if (!reg) {
        free(lowered);
        errf(p, "sircc: zasm: %s node %lld has too many args for current ABI model", n->tag, (long long)call_id);
        return false;
      }
      if (!emit_load_slot_into_reg(out, reg, &op, (*io_line)++)) {
        free(lowered);
        return false;
      }
      op = (ZasmOp){.k = ZOP_REG, .s = reg};
    }
    lowered[i] = op;
  }

  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"CALL\",\"ops\":[");
  zasm_write_op_sym(out, lowered[0].s);
  for (size_t i = 1; i < op_count; i++) {
    fprintf(out, ",");
    if (!zasm_write_op(out, &lowered[i])) {
      free(lowered);
      return false;
    }
  }
  fprintf(out, "]");
  zasm_write_loc(out, (*io_line)++);
  fprintf(out, "}\n");

  free(lowered);
  return true;
}

bool zasm_emit_store_stmt(
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
    NodeRec* s,
    int64_t* io_line) {
  if (!out || !p || !s || !s->fields || !io_line) return false;
  int64_t width = 0;
  const char* mnemonic = NULL;
  const char* value_reg = NULL;
  if (strcmp(s->tag, "store.i8") == 0) {
    width = 1;
    mnemonic = "ST8";
    value_reg = "A";
  } else if (strcmp(s->tag, "store.i16") == 0) {
    width = 2;
    mnemonic = "ST16";
    value_reg = "HL";
  } else if (strcmp(s->tag, "store.i32") == 0) {
    width = 4;
    mnemonic = "ST32";
    value_reg = "HL";
  } else if (strcmp(s->tag, "store.i64") == 0) {
    width = 8;
    mnemonic = "ST64";
    value_reg = "HL";
  } else {
    errf(p, "sircc: zasm: unsupported store width '%s'", s->tag);
    return false;
  }

  int64_t addr_id = 0;
  int64_t value_id = 0;
  JsonValue* av = json_obj_get(s->fields, "addr");
  JsonValue* vv = json_obj_get(s->fields, "value");
  if (!parse_node_ref_id(av, &addr_id) || !parse_node_ref_id(vv, &value_id)) {
    errf(p, "sircc: zasm: %s node %lld requires fields.addr/value node refs", s->tag, (long long)s->id);
    return false;
  }

  ZasmOp base = {0};
  int64_t disp = 0;
  if (!zasm_emit_addr_to_mem(out, p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, addr_id, &base, &disp, io_line))
    return false;
  ZasmOp val = {0};
  if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, value_id, &val)) return false;

  if (val.k == ZOP_SLOT) {
    if (!emit_load_slot_into_reg(out, value_reg, &val, (*io_line)++)) return false;
  } else {
    if (width == 1) {
      if (val.k == ZOP_NUM) {
        ZasmOp byte = {.k = ZOP_NUM, .n = (uint8_t)val.n};
        if (!emit_ld(out, value_reg, &byte, (*io_line)++)) return false;
      } else if (val.k == ZOP_SYM || val.k == ZOP_REG) {
        if (!emit_ld(out, value_reg, &val, (*io_line)++)) return false;
      } else {
        errf(p, "sircc: zasm: %s value unsupported", s->tag);
        return false;
      }
    } else {
      if (val.k == ZOP_NUM || val.k == ZOP_SYM || val.k == ZOP_REG) {
        if (!emit_ld(out, value_reg, &val, (*io_line)++)) return false;
      } else {
        errf(p, "sircc: zasm: %s value unsupported", s->tag);
        return false;
      }
    }
  }

  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":");
  json_write_escaped(out, mnemonic);
  fprintf(out, ",\"ops\":[");
  zasm_write_op_mem(out, &base, disp, width);
  fprintf(out, ",");
  zasm_write_op_reg(out, value_reg);
  fprintf(out, "]");
  zasm_write_loc(out, (*io_line)++);
  fprintf(out, "}\n");
  return true;
}

bool zasm_emit_mem_fill_stmt(
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
    NodeRec* s,
    int64_t* io_line) {
  if (!out || !p || !s || !s->fields || !io_line) return false;
  JsonValue* args = json_obj_get(s->fields, "args");
  if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) {
    errf(p, "sircc: zasm: mem.fill node %lld requires args:[dst, byte, len]", (long long)s->id);
    return false;
  }

  int64_t dst_id = 0, byte_id = 0, len_id = 0;
  if (!parse_node_ref_id(args->v.arr.items[0], &dst_id) || !parse_node_ref_id(args->v.arr.items[1], &byte_id) ||
      !parse_node_ref_id(args->v.arr.items[2], &len_id)) {
    errf(p, "sircc: zasm: mem.fill node %lld args must be node refs", (long long)s->id);
    return false;
  }

  ZasmOp dst = {0};
  if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, dst_id, &dst) || dst.k != ZOP_SYM) {
    errf(p, "sircc: zasm: mem.fill dst must be an alloca symbol (node %lld)", (long long)dst_id);
    return false;
  }
  ZasmOp byte = {0};
  if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, byte_id, &byte) || byte.k != ZOP_NUM) {
    errf(p, "sircc: zasm: mem.fill byte must be an immediate const (node %lld)", (long long)byte_id);
    return false;
  }
  ZasmOp len = {0};
  if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, len_id, &len) || len.k != ZOP_NUM) {
    errf(p, "sircc: zasm: mem.fill len must be an immediate const (node %lld)", (long long)len_id);
    return false;
  }

  if (!emit_ld(out, "HL", &dst, (*io_line)++)) return false;
  ZasmOp b8 = {.k = ZOP_NUM, .n = (uint8_t)byte.n};
  if (!emit_ld(out, "A", &b8, (*io_line)++)) return false;
  if (!emit_ld(out, "BC", &len, (*io_line)++)) return false;

  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"FILL\",\"ops\":[]");
  zasm_write_loc(out, (*io_line)++);
  fprintf(out, "}\n");
  return true;
}

bool zasm_emit_mem_copy_stmt(
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
    NodeRec* s,
    int64_t* io_line) {
  if (!out || !p || !s || !s->fields || !io_line) return false;
  JsonValue* args = json_obj_get(s->fields, "args");
  if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) {
    errf(p, "sircc: zasm: mem.copy node %lld requires args:[dst, src, len]", (long long)s->id);
    return false;
  }

  int64_t dst_id = 0, src_id = 0, len_id = 0;
  if (!parse_node_ref_id(args->v.arr.items[0], &dst_id) || !parse_node_ref_id(args->v.arr.items[1], &src_id) ||
      !parse_node_ref_id(args->v.arr.items[2], &len_id)) {
    errf(p, "sircc: zasm: mem.copy node %lld args must be node refs", (long long)s->id);
    return false;
  }

  ZasmOp dst = {0};
  if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, dst_id, &dst) || dst.k != ZOP_SYM) {
    errf(p, "sircc: zasm: mem.copy dst must be an alloca symbol (node %lld)", (long long)dst_id);
    return false;
  }
  ZasmOp src = {0};
  if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, src_id, &src) || src.k != ZOP_SYM) {
    errf(p, "sircc: zasm: mem.copy src must be an alloca symbol (node %lld)", (long long)src_id);
    return false;
  }
  ZasmOp len = {0};
  if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, len_id, &len) || len.k != ZOP_NUM) {
    errf(p, "sircc: zasm: mem.copy len must be an immediate const (node %lld)", (long long)len_id);
    return false;
  }

  if (!emit_ld(out, "DE", &dst, (*io_line)++)) return false;
  if (!emit_ld(out, "HL", &src, (*io_line)++)) return false;
  if (!emit_ld(out, "BC", &len, (*io_line)++)) return false;

  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"LDIR\",\"ops\":[]");
  zasm_write_loc(out, (*io_line)++);
  fprintf(out, "}\n");
  return true;
}

bool zasm_emit_ret_value_to_hl(
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
    int64_t value_id,
    int64_t* io_line) {
  if (!out || !p || !io_line) return false;

  NodeRec* v = get_node(p, value_id);
  if (!v) {
    errf(p, "sircc: zasm: return references unknown node %lld", (long long)value_id);
    return false;
  }

  if (strcmp(v->tag, "i32.zext.i8") == 0) {
    JsonValue* args = v->fields ? json_obj_get(v->fields, "args") : NULL;
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
      errf(p, "sircc: zasm: i32.zext.i8 node %lld requires args:[x]", (long long)value_id);
      return false;
    }
    int64_t x_id = 0;
    if (!parse_node_ref_id(args->v.arr.items[0], &x_id)) {
      errf(p, "sircc: zasm: i32.zext.i8 node %lld arg must be node ref", (long long)value_id);
      return false;
    }
    NodeRec* x = get_node(p, x_id);
    if (!x) {
      errf(p, "sircc: zasm: i32.zext.i8 references unknown node %lld", (long long)x_id);
      return false;
    }

    if (strcmp(x->tag, "load.i8") == 0) {
      int64_t addr_id = 0;
      JsonValue* av = x->fields ? json_obj_get(x->fields, "addr") : NULL;
      if (!parse_node_ref_id(av, &addr_id)) {
        errf(p, "sircc: zasm: load.i8 node %lld requires fields.addr node ref", (long long)x_id);
        return false;
      }
      ZasmOp base = {0};
      int64_t disp = 0;
      if (!zasm_emit_addr_to_mem(out, p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, addr_id, &base, &disp, io_line))
        return false;

      zasm_write_ir_k(out, "instr");
      fprintf(out, ",\"m\":\"LD8U\",\"ops\":[");
      zasm_write_op_reg(out, "HL");
      fprintf(out, ",");
      zasm_write_op_mem(out, &base, disp, 1);
      fprintf(out, "]");
      zasm_write_loc(out, (*io_line)++);
      fprintf(out, "}\n");
      return true;
    }

    ZasmOp op = {0};
    if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, x_id, &op) || op.k != ZOP_NUM) {
      errf(p, "sircc: zasm: i32.zext.i8 arg must be load.i8 or const.i8 (node %lld)", (long long)x_id);
      return false;
    }
    ZasmOp z = {.k = ZOP_NUM, .n = (uint8_t)op.n};
    return emit_ld(out, "HL", &z, (*io_line)++);
  }

  if (strcmp(v->tag, "load.i8") == 0) {
    int64_t addr_id = 0;
    JsonValue* av = v->fields ? json_obj_get(v->fields, "addr") : NULL;
    if (!parse_node_ref_id(av, &addr_id)) {
      errf(p, "sircc: zasm: load.i8 node %lld requires fields.addr node ref", (long long)value_id);
      return false;
    }
    ZasmOp base = {0};
    int64_t disp = 0;
    if (!zasm_emit_addr_to_mem(out, p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, addr_id, &base, &disp, io_line))
      return false;

    zasm_write_ir_k(out, "instr");
    fprintf(out, ",\"m\":\"LD8U\",\"ops\":[");
    zasm_write_op_reg(out, "HL");
    fprintf(out, ",");
    zasm_write_op_mem(out, &base, disp, 1);
    fprintf(out, "]");
    zasm_write_loc(out, (*io_line)++);
    fprintf(out, "}\n");
    return true;
  }

  ZasmOp rop = {0};
  if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, value_id, &rop)) return false;
  if (rop.k == ZOP_SLOT) return emit_load_slot_into_reg(out, "HL", &rop, (*io_line)++);
  if (rop.k == ZOP_NUM || rop.k == ZOP_SYM) return emit_ld(out, "HL", &rop, (*io_line)++);
  if (rop.k == ZOP_REG) {
    if (!rop.s || strcmp(rop.s, "HL") == 0) return true;
    return emit_ld(out, "HL", &rop, (*io_line)++);
  }

  errf(p, "sircc: zasm: unsupported return value shape");
  return false;
}
