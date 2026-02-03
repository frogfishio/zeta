// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_zasm_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char* sym;
  int64_t size_bytes;
} ZasmTempSlot;

static int64_t width_for_prim(const char* prim) {
  if (!prim) return 0;
  if (strcmp(prim, "i8") == 0 || strcmp(prim, "bool") == 0) return 1;
  if (strcmp(prim, "i16") == 0) return 2;
  if (strcmp(prim, "i32") == 0 || strcmp(prim, "f32") == 0) return 4;
  if (strcmp(prim, "i64") == 0 || strcmp(prim, "f64") == 0 || strcmp(prim, "ptr") == 0) return 8;
  return 0;
}

static int64_t width_for_type_id(SirProgram* p, int64_t type_id) {
  if (!p) return 0;
  TypeRec* t = get_type(p, type_id);
  if (!t) return 0;
  switch (t->kind) {
    case TYPE_PRIM:
      return width_for_prim(t->prim);
    case TYPE_PTR:
      return 8;
    default:
      return 0;
  }
}

static const char* sym_for_bparam(SirProgram* p, int64_t bparam_id) {
  char buf[64];
  snprintf(buf, sizeof(buf), "bp_%lld", (long long)bparam_id);
  return arena_strdup(&p->arena, buf);
}

static bool ensure_bparam_slot(
    SirProgram* p,
    ZasmBParamSlot** bps,
    size_t* bps_len,
    size_t* bps_cap,
    int64_t bparam_id,
    int64_t size_bytes,
    const char** out_sym) {
  if (!p || !bps || !bps_len || !bps_cap || !out_sym) return false;
  *out_sym = NULL;

  for (size_t i = 0; i < *bps_len; i++) {
    if ((*bps)[i].node_id == bparam_id) {
      *out_sym = (*bps)[i].sym;
      return true;
    }
  }

  if (*bps_len == *bps_cap) {
    size_t next = *bps_cap ? (*bps_cap * 2) : 16;
    ZasmBParamSlot* bigger = (ZasmBParamSlot*)realloc(*bps, next * sizeof(ZasmBParamSlot));
    if (!bigger) return false;
    *bps = bigger;
    *bps_cap = next;
  }

  const char* sym = sym_for_bparam(p, bparam_id);
  if (!sym) return false;
  (*bps)[(*bps_len)++] = (ZasmBParamSlot){.node_id = bparam_id, .sym = sym, .size_bytes = size_bytes};
  *out_sym = sym;
  return true;
}

static bool add_temp_slot(
    SirProgram* p,
    ZasmTempSlot** slots,
    size_t* slots_len,
    size_t* slots_cap,
    int64_t id_hint,
    int64_t size_bytes,
    const char** out_sym) {
  if (!p || !slots || !slots_len || !slots_cap || !out_sym) return false;
  *out_sym = NULL;

  if (*slots_len == *slots_cap) {
    size_t next = *slots_cap ? (*slots_cap * 2) : 16;
    ZasmTempSlot* bigger = (ZasmTempSlot*)realloc(*slots, next * sizeof(ZasmTempSlot));
    if (!bigger) return false;
    *slots = bigger;
    *slots_cap = next;
  }

  char name_buf[96];
  snprintf(name_buf, sizeof(name_buf), "tmp_%lld", (long long)id_hint);
  char* sym = arena_strdup(&p->arena, name_buf);
  if (!sym) return false;

  (*slots)[(*slots_len)++] = (ZasmTempSlot){.sym = sym, .size_bytes = size_bytes};
  *out_sym = sym;
  return true;
}

static bool emit_st64_slot_from_hl(FILE* out, const char* slot_sym, int64_t line_no) {
  if (!out || !slot_sym) return false;
  ZasmOp base = {.k = ZOP_SYM, .s = slot_sym};
  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"ST64\",\"ops\":[");
  zasm_write_op_mem(out, &base, 0, 8);
  fprintf(out, ",");
  zasm_write_op_reg(out, "HL");
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
  return true;
}

static bool emit_store_reg_to_slot(FILE* out, const char* slot_sym, int64_t size_bytes, const char* reg, int64_t line_no) {
  if (!out || !slot_sym || !reg) return false;
  const char* m = NULL;
  int64_t hint = 0;
  if (size_bytes == 1) {
    m = "ST8";
    hint = 1;
  } else if (size_bytes == 2) {
    m = "ST16";
    hint = 2;
  } else if (size_bytes == 4) {
    m = "ST32";
    hint = 4;
  } else if (size_bytes == 8) {
    m = "ST64";
    hint = 8;
  } else {
    return false;
  }

  ZasmOp base = {.k = ZOP_SYM, .s = slot_sym};
  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":");
  json_write_escaped(out, m);
  fprintf(out, ",\"ops\":[");
  zasm_write_op_mem(out, &base, 0, hint);
  fprintf(out, ",");
  zasm_write_op_reg(out, reg);
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
  return true;
}

static const char* reg_for_width(int64_t width_bytes) {
  if (width_bytes == 1) return "A";
  // For now, keep 16/32/64-bit values in HL.
  if (width_bytes == 2 || width_bytes == 4 || width_bytes == 8) return "HL";
  return NULL;
}

static bool emit_load_slot_to_reg(FILE* out, const char* slot_sym, int64_t width_bytes, const char* dst_reg, int64_t line_no) {
  if (!out || !slot_sym || !dst_reg) return false;
  ZasmOp base = {.k = ZOP_SYM, .s = slot_sym};

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

static bool emit_ld_reg_or_imm(FILE* out, const char* dst_reg, const ZasmOp* op, int64_t line_no) {
  if (!out || !dst_reg || !op) return false;
  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"LD\",\"ops\":[");
  zasm_write_op_reg(out, dst_reg);
  fprintf(out, ",");
  if (!zasm_write_op(out, op)) return false;
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
  return true;
}

static bool emit_binop_into_hl(
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
    NodeRec* vn,
    const char* m32,
    const char* m64,
    int64_t width_bytes,
    int64_t* io_line) {
  if (!out || !p || !vn || !vn->fields || !io_line) return false;
  JsonValue* args = json_obj_get(vn->fields, "args");
  if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) {
    errf(p, "sircc: zasm: %s node %lld requires args:[a,b]", vn->tag, (long long)vn->id);
    return false;
  }

  int64_t a_id = 0, b_id = 0;
  if (!parse_node_ref_id(args->v.arr.items[0], &a_id) || !parse_node_ref_id(args->v.arr.items[1], &b_id)) {
    errf(p, "sircc: zasm: %s node %lld args must be node refs", vn->tag, (long long)vn->id);
    return false;
  }

  ZasmOp a = {0};
  if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, a_id, &a)) return false;
  if (a.k == ZOP_SLOT) {
    if (!emit_load_slot_to_reg(out, a.s, a.n, reg_for_width(width_bytes), (*io_line)++)) return false;
  } else {
    if (!emit_ld_reg_or_imm(out, reg_for_width(width_bytes), &a, (*io_line)++)) return false;
  }

  ZasmOp b = {0};
  if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, b_id, &b)) return false;
  ZasmOp rhs = b;
  if (b.k == ZOP_SLOT) {
    // Materialize RHS into DE for binary ops.
    if (!emit_load_slot_to_reg(out, b.s, b.n, "DE", (*io_line)++)) return false;
    rhs.k = ZOP_REG;
    rhs.s = "DE";
  }

  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":");
  json_write_escaped(out, (width_bytes == 8) ? m64 : m32);
  fprintf(out, ",\"ops\":[");
  zasm_write_op_reg(out, "HL");
  fprintf(out, ",");
  if (!zasm_write_op(out, &rhs)) return false;
  fprintf(out, "]");
  zasm_write_loc(out, (*io_line)++);
  fprintf(out, "}\n");
  return true;
}

static bool emit_jr(FILE* out, const char* lbl, int64_t line_no) {
  if (!out || !lbl) return false;
  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"JR\",\"ops\":[");
  zasm_write_op_lbl(out, lbl);
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
  return true;
}

static bool ensure_name_binding_cap(SirProgram* p, ZasmNameBinding** names, size_t* cap, size_t need) {
  if (!names || !cap) return false;
  if (*cap >= need) return true;
  size_t next = *cap ? *cap : 16;
  while (next < need) next *= 2;
  ZasmNameBinding* bigger = (ZasmNameBinding*)realloc(*names, next * sizeof(ZasmNameBinding));
  if (!bigger) {
    if (p) errf(p, "sircc: zasm: out of memory");
    return false;
  }
  *names = bigger;
  *cap = next;
  return true;
}

static bool emit_bind_slot(
    SirProgram* p,
    ZasmNameBinding** names,
    size_t* name_len,
    size_t* name_cap,
    const char* bind_name,
    const char* slot_sym,
    int64_t slot_size_bytes) {
  if (!p || !names || !name_len || !name_cap || !bind_name || !slot_sym) return false;
  if (!ensure_name_binding_cap(p, names, name_cap, (*name_len) + 1)) return false;
  // Shadowing allowed; last binding wins.
  (*names)[(*name_len)++] = (ZasmNameBinding){
      .name = bind_name,
      .is_slot = true,
      .op = {.k = ZOP_SYM, .s = slot_sym},
      .slot_size_bytes = slot_size_bytes,
  };
  return true;
}

static bool emit_bind_op(
    SirProgram* p,
    ZasmNameBinding** names,
    size_t* name_len,
    size_t* name_cap,
    const char* bind_name,
    ZasmOp op) {
  if (!p || !names || !name_len || !name_cap || !bind_name) return false;
  if (!ensure_name_binding_cap(p, names, name_cap, (*name_len) + 1)) return false;
  (*names)[(*name_len)++] = (ZasmNameBinding){.name = bind_name, .is_slot = false, .op = op, .slot_size_bytes = 0};
  return true;
}

static bool emit_zir_nonterm_stmt(
    FILE* out,
    SirProgram* p,
    ZasmStr* strs,
    size_t strs_len,
    ZasmAlloca* allocas,
    size_t allocas_len,
    ZasmNameBinding** names,
    size_t* name_len,
    size_t* name_cap,
    ZasmBParamSlot* bps,
    size_t bps_len,
    ZasmTempSlot** tmps,
    size_t* tmp_len,
    size_t* tmp_cap,
    NodeRec* s,
    int64_t* io_line) {
  if (!out || !p || !names || !name_len || !name_cap || !tmps || !tmp_len || !tmp_cap || !s || !io_line) return false;

  if (strcmp(s->tag, "let") == 0) {
    const char* bind_name = s->fields ? json_get_string(json_obj_get(s->fields, "name")) : NULL;
    JsonValue* v = s->fields ? json_obj_get(s->fields, "value") : NULL;
    int64_t vid = 0;
    if (!parse_node_ref_id(v, &vid)) {
      errf(p, "sircc: zasm: let node %lld missing fields.value ref", (long long)s->id);
      return false;
    }
    NodeRec* vn = get_node(p, vid);
    if (!vn) {
      errf(p, "sircc: zasm: let node %lld value references unknown node", (long long)s->id);
      return false;
    }

    if (strcmp(vn->tag, "call") == 0 || strcmp(vn->tag, "call.indirect") == 0) {
      if (!zasm_emit_call_stmt(out, p, strs, strs_len, allocas, allocas_len, *names, *name_len, bps, bps_len, vid, io_line)) return false;

      if (bind_name && strcmp(bind_name, "_") != 0) {
        const char* slot_sym = NULL;
        if (!add_temp_slot(p, tmps, tmp_len, tmp_cap, s->id, 8, &slot_sym)) {
          errf(p, "sircc: zasm: out of memory");
          return false;
        }
        if (!emit_st64_slot_from_hl(out, slot_sym, (*io_line)++)) return false;
        if (!emit_bind_slot(p, names, name_len, name_cap, bind_name, slot_sym, 8)) return false;
      }
      return true;
    }

    if (strncmp(vn->tag, "load.", 5) == 0) {
      int64_t width = 0;
      const char* m = NULL;
      const char* dst_reg = NULL;
      if (strcmp(vn->tag, "load.i8") == 0) {
        width = 1;
        m = "LD8U";
        dst_reg = "A";
      } else if (strcmp(vn->tag, "load.i16") == 0) {
        width = 2;
        m = "LD16U";
        dst_reg = "HL";
      } else if (strcmp(vn->tag, "load.i32") == 0) {
        width = 4;
        m = "LD32U64";
        dst_reg = "HL";
      } else if (strcmp(vn->tag, "load.i64") == 0 || strcmp(vn->tag, "load.ptr") == 0) {
        width = 8;
        m = "LD64";
        dst_reg = "HL";
      } else {
        errf(p, "sircc: zasm: unsupported load '%s'", vn->tag);
        return false;
      }

      int64_t addr_id = 0;
      JsonValue* av = vn->fields ? json_obj_get(vn->fields, "addr") : NULL;
      if (!parse_node_ref_id(av, &addr_id)) {
        errf(p, "sircc: zasm: %s node %lld requires fields.addr node ref", vn->tag, (long long)vn->id);
        return false;
      }
      ZasmOp base = {0};
      int64_t disp = 0;
      if (!zasm_lower_addr_to_mem(
              p, strs, strs_len, allocas, allocas_len, *names, *name_len, bps, bps_len, addr_id, &base, &disp))
        return false;

      zasm_write_ir_k(out, "instr");
      fprintf(out, ",\"m\":");
      json_write_escaped(out, m);
      fprintf(out, ",\"ops\":[");
      zasm_write_op_reg(out, dst_reg);
      fprintf(out, ",");
      zasm_write_op_mem(out, &base, disp, width);
      fprintf(out, "]");
      zasm_write_loc(out, (*io_line)++);
      fprintf(out, "}\n");

      if (bind_name && strcmp(bind_name, "_") != 0) {
        const char* slot_sym = NULL;
        if (!add_temp_slot(p, tmps, tmp_len, tmp_cap, s->id, width, &slot_sym)) {
          errf(p, "sircc: zasm: out of memory");
          return false;
        }
        if (!emit_store_reg_to_slot(out, slot_sym, width, dst_reg, (*io_line)++)) return false;
        if (!emit_bind_slot(p, names, name_len, name_cap, bind_name, slot_sym, width)) return false;
      }
      return true;
    }

    if (strcmp(vn->tag, "i32.add") == 0 || strcmp(vn->tag, "i32.sub") == 0) {
      if (!bind_name || strcmp(bind_name, "_") == 0) {
        errf(p, "sircc: zasm: %s must be bound via let name", vn->tag);
        return false;
      }
      const char* m32 = (strcmp(vn->tag, "i32.add") == 0) ? "ADD" : "SUB";
      if (!emit_binop_into_hl(out, p, strs, strs_len, allocas, allocas_len, *names, *name_len, bps, bps_len, vn, m32, NULL, 4, io_line))
        return false;
      const char* slot_sym = NULL;
      if (!add_temp_slot(p, tmps, tmp_len, tmp_cap, s->id, 4, &slot_sym)) {
        errf(p, "sircc: zasm: out of memory");
        return false;
      }
      if (!emit_store_reg_to_slot(out, slot_sym, 4, "HL", (*io_line)++)) return false;
      if (!emit_bind_slot(p, names, name_len, name_cap, bind_name, slot_sym, 4)) return false;
      return true;
    }

    if (strcmp(vn->tag, "i64.add") == 0 || strcmp(vn->tag, "i64.sub") == 0) {
      if (!bind_name || strcmp(bind_name, "_") == 0) {
        errf(p, "sircc: zasm: %s must be bound via let name", vn->tag);
        return false;
      }
      const char* m64 = (strcmp(vn->tag, "i64.add") == 0) ? "ADD64" : "SUB64";
      if (!emit_binop_into_hl(out, p, strs, strs_len, allocas, allocas_len, *names, *name_len, bps, bps_len, vn, NULL, m64, 8, io_line))
        return false;
      const char* slot_sym = NULL;
      if (!add_temp_slot(p, tmps, tmp_len, tmp_cap, s->id, 8, &slot_sym)) {
        errf(p, "sircc: zasm: out of memory");
        return false;
      }
      if (!emit_store_reg_to_slot(out, slot_sym, 8, "HL", (*io_line)++)) return false;
      if (!emit_bind_slot(p, names, name_len, name_cap, bind_name, slot_sym, 8)) return false;
      return true;
    }

    // Pure-ish binding of stable values (consts/symbols); no code emitted.
    if (bind_name && strcmp(bind_name, "_") != 0) {
      ZasmOp op = {0};
      if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, *names, *name_len, bps, bps_len, vid, &op)) return false;
      if (!emit_bind_op(p, names, name_len, name_cap, bind_name, op)) return false;
    }
    return true;
  }

  if (strcmp(s->tag, "mem.fill") == 0) {
    if (!zasm_emit_mem_fill_stmt(out, p, strs, strs_len, allocas, allocas_len, *names, *name_len, bps, bps_len, s, *io_line)) return false;
    *io_line += 4;
    return true;
  }

  if (strcmp(s->tag, "mem.copy") == 0) {
    if (!zasm_emit_mem_copy_stmt(out, p, strs, strs_len, allocas, allocas_len, *names, *name_len, bps, bps_len, s, *io_line)) return false;
    *io_line += 4;
    return true;
  }

  if (strncmp(s->tag, "store.", 6) == 0) {
    if (!zasm_emit_store_stmt(out, p, strs, strs_len, allocas, allocas_len, *names, *name_len, bps, bps_len, s, *io_line)) return false;
    *io_line += 2;
    return true;
  }

  errf(p, "sircc: zasm: unsupported stmt tag '%s' in zir_main", s->tag);
  return false;
}

static bool emit_jr_cond(FILE* out, const char* cond_sym, const char* lbl, int64_t line_no) {
  if (!out || !cond_sym || !lbl) return false;
  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"JR\",\"ops\":[");
  zasm_write_op_sym(out, cond_sym);
  fprintf(out, ",");
  zasm_write_op_lbl(out, lbl);
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
  return true;
}

static bool emit_cp_hl(FILE* out, const ZasmOp* rhs, int64_t line_no) {
  if (!out || !rhs) return false;
  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"CP\",\"ops\":[");
  zasm_write_op_reg(out, "HL");
  fprintf(out, ",");
  if (!zasm_write_op(out, rhs)) return false;
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
  return true;
}

static const char* label_for_block(SirProgram* p, int64_t entry_id, int64_t block_id) {
  if (!p) return NULL;
  if (block_id == entry_id) return "zir_main";
  char buf[64];
  snprintf(buf, sizeof(buf), "b_%lld", (long long)block_id);
  return arena_strdup(&p->arena, buf);
}

bool emit_zasm_v11(SirProgram* p, const char* out_path) {
  if (!p || !out_path) return false;

  NodeRec* zir_main = zasm_find_fn(p, "zir_main");
  if (!zir_main) {
    errf(p, "sircc: --emit-zasm currently requires a function named 'zir_main'");
    return false;
  }

  ZasmStr* strs = NULL;
  size_t strs_len = 0;
  if (!zasm_collect_cstrs(p, &strs, &strs_len)) return false;

  ZasmAlloca* allocas = NULL;
  size_t allocas_len = 0;
  if (!zasm_collect_allocas(p, &allocas, &allocas_len)) {
    free(strs);
    return false;
  }

  const char** decls = NULL;
  size_t decls_len = 0;
  if (!zasm_collect_decl_fns(p, &decls, &decls_len)) {
    free(strs);
    free(allocas);
    return false;
  }

  FILE* out = fopen(out_path, "wb");
  if (!out) {
    free(strs);
    free(allocas);
    free(decls);
    errf(p, "sircc: failed to open output: %s", strerror(errno));
    return false;
  }

  int64_t line = 1;
  size_t name_cap = 0;
  size_t name_len = 0;
  ZasmNameBinding* names = NULL;
  size_t bp_cap = 0;
  size_t bp_len = 0;
  ZasmBParamSlot* bps = NULL;
  size_t tmp_cap = 0;
  size_t tmp_len = 0;
  ZasmTempSlot* tmps = NULL;

  zasm_write_ir_k(out, "meta");
  fprintf(out, ",\"producer\":\"sircc\"");
  if (p->unit_name) {
    fprintf(out, ",\"unit\":");
    json_write_escaped(out, p->unit_name);
  }
  zasm_write_loc(out, line++);
  fprintf(out, "}\n");

  for (size_t i = 0; i < decls_len; i++) {
    zasm_write_ir_k(out, "dir");
    fprintf(out, ",\"d\":\"EXTERN\",\"args\":[");
    zasm_write_op_str(out, "c");
    fprintf(out, ",");
    zasm_write_op_str(out, decls[i]);
    fprintf(out, ",");
    zasm_write_op_sym(out, decls[i]);
    fprintf(out, "]");
    zasm_write_loc(out, line++);
    fprintf(out, "}\n");
  }

  zasm_write_ir_k(out, "dir");
  fprintf(out, ",\"d\":\"PUBLIC\",\"args\":[");
  zasm_write_op_sym(out, "zir_main");
  fprintf(out, "]");
  zasm_write_loc(out, line++);
  fprintf(out, "}\n\n");

  // CFG-form zir_main: fields.entry + fields.blocks (minimal subset).
  JsonValue* entryv = zir_main->fields ? json_obj_get(zir_main->fields, "entry") : NULL;
  int64_t entry_id = 0;
  if (entryv && parse_node_ref_id(entryv, &entry_id)) {
    JsonValue* blocks = json_obj_get(zir_main->fields, "blocks");
    if (!blocks || blocks->type != JSON_ARRAY || blocks->v.arr.len == 0) {
      fclose(out);
      free(strs);
      free(allocas);
      free(decls);
      free(names);
      free(tmps);
      errf(p, "sircc: zasm: zir_main CFG form requires fields.blocks");
      return false;
    }

    for (size_t bi = 0; bi < blocks->v.arr.len; bi++) {
      int64_t bid = 0;
      if (!parse_node_ref_id(blocks->v.arr.items[bi], &bid)) {
        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        free(names);
        free(tmps);
        errf(p, "sircc: zasm: blocks[%zu] must be node ref", bi);
        return false;
      }
      NodeRec* b = get_node(p, bid);
      if (!b || strcmp(b->tag, "block") != 0 || !b->fields) {
        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        free(names);
        free(tmps);
        errf(p, "sircc: zasm: blocks[%zu] must be a block node", bi);
        return false;
      }
      // Collect/allocate slots for block params (bparam) so term.br args can store into them.
      JsonValue* params = json_obj_get(b->fields, "params");
      if (params && params->type == JSON_ARRAY) {
        for (size_t pi = 0; pi < params->v.arr.len; pi++) {
          int64_t pid = 0;
          if (!parse_node_ref_id(params->v.arr.items[pi], &pid)) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            free(bps);
            errf(p, "sircc: zasm: block %lld params[%zu] must be node ref", (long long)bid, pi);
            return false;
          }
          NodeRec* pn = get_node(p, pid);
          if (!pn || strcmp(pn->tag, "bparam") != 0) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            free(bps);
            errf(p, "sircc: zasm: block %lld param %lld must be bparam", (long long)bid, (long long)pid);
            return false;
          }
          int64_t w = width_for_type_id(p, pn->type_ref);
          if (!w) w = 8;
          const char* sym = NULL;
          if (!ensure_bparam_slot(p, &bps, &bp_len, &bp_cap, pid, w, &sym)) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            free(bps);
            errf(p, "sircc: zasm: out of memory");
            return false;
          }
        }
      }

      const char* lbl = label_for_block(p, entry_id, bid);
      if (!lbl) {
        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        free(names);
        free(tmps);
        return false;
      }

      zasm_write_ir_k(out, "label");
      fprintf(out, ",\"name\":");
      json_write_escaped(out, lbl);
      zasm_write_loc(out, line++);
      fprintf(out, "}\n");

      JsonValue* stmts = json_obj_get(b->fields, "stmts");
      if (!stmts || stmts->type != JSON_ARRAY) {
        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        free(names);
        free(tmps);
        errf(p, "sircc: zasm: block %lld missing stmts array", (long long)bid);
        return false;
      }

      // Names are block-local in CFG form. Use bparam + term.br args for cross-block values.
      size_t saved_name_len = name_len;
      bool terminated = false;
      for (size_t si = 0; si < stmts->v.arr.len; si++) {
        int64_t sid = 0;
        if (!parse_node_ref_id(stmts->v.arr.items[si], &sid)) {
          fclose(out);
          free(strs);
          free(allocas);
          free(decls);
          free(names);
          free(tmps);
          errf(p, "sircc: zasm: block stmt[%zu] must be node ref", si);
          return false;
        }
        NodeRec* s = get_node(p, sid);
        if (!s) {
          fclose(out);
          free(strs);
          free(allocas);
          free(decls);
          free(names);
          free(tmps);
          errf(p, "sircc: zasm: unknown stmt node %lld", (long long)sid);
          return false;
        }

        if (strncmp(s->tag, "term.", 5) != 0 && strcmp(s->tag, "return") != 0) {
          if (!emit_zir_nonterm_stmt(
                  out, p, strs, strs_len, allocas, allocas_len, &names, &name_len, &name_cap, bps, bp_len, &tmps, &tmp_len, &tmp_cap, s,
                  &line)) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            free(bps);
            return false;
          }
          continue;
        }

        if (strcmp(s->tag, "term.ret") == 0 || strcmp(s->tag, "return") == 0) {
          JsonValue* rv = s->fields ? json_obj_get(s->fields, "value") : NULL;
          int64_t rid = 0;
          if (rv && parse_node_ref_id(rv, &rid)) {
            if (!zasm_emit_ret_value_to_hl(out, p, strs, strs_len, allocas, allocas_len, names, name_len, bps, bp_len, rid, line++)) {
              fclose(out);
              free(strs);
              free(allocas);
              free(decls);
              free(names);
              free(tmps);
              free(bps);
              return false;
            }
          } else {
            zasm_write_ir_k(out, "instr");
            fprintf(out, ",\"m\":\"LD\",\"ops\":[");
            zasm_write_op_reg(out, "HL");
            fprintf(out, ",");
            zasm_write_op_num(out, 0);
            fprintf(out, "]");
            zasm_write_loc(out, line++);
            fprintf(out, "}\n");
          }

          zasm_write_ir_k(out, "instr");
          fprintf(out, ",\"m\":\"RET\",\"ops\":[]");
          zasm_write_loc(out, line++);
          fprintf(out, "}\n");
          terminated = true;
          break;
        }

        if (strcmp(s->tag, "term.br") == 0) {
          int64_t to_id = 0;
          if (!parse_node_ref_id(s->fields ? json_obj_get(s->fields, "to") : NULL, &to_id)) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            free(bps);
            errf(p, "sircc: zasm: term.br node %lld missing fields.to", (long long)s->id);
            return false;
          }

          JsonValue* args = json_obj_get(s->fields, "args");
          if (args && args->type == JSON_ARRAY && args->v.arr.len) {
            NodeRec* to_blk = get_node(p, to_id);
            JsonValue* to_params = (to_blk && to_blk->fields) ? json_obj_get(to_blk->fields, "params") : NULL;
            if (!to_params || to_params->type != JSON_ARRAY || to_params->v.arr.len != args->v.arr.len) {
              fclose(out);
              free(strs);
              free(allocas);
              free(decls);
              free(names);
              free(tmps);
              free(bps);
              errf(p, "sircc: zasm: term.br args must match destination block params");
              return false;
            }

            for (size_t ai = 0; ai < args->v.arr.len; ai++) {
              int64_t arg_id = 0;
              int64_t param_id = 0;
              if (!parse_node_ref_id(args->v.arr.items[ai], &arg_id) || !parse_node_ref_id(to_params->v.arr.items[ai], &param_id)) {
                fclose(out);
                free(strs);
                free(allocas);
                free(decls);
                free(names);
                free(tmps);
                free(bps);
                errf(p, "sircc: zasm: term.br arg/param must be node refs");
                return false;
              }

              const char* slot_sym = NULL;
              int64_t slot_w = 0;
              for (size_t bi2 = 0; bi2 < bp_len; bi2++) {
                if (bps[bi2].node_id == param_id) {
                  slot_sym = bps[bi2].sym;
                  slot_w = bps[bi2].size_bytes;
                  break;
                }
              }
              if (!slot_sym) {
                NodeRec* pn = get_node(p, param_id);
                slot_w = pn ? width_for_type_id(p, pn->type_ref) : 0;
                if (!slot_w) slot_w = 8;
                if (!ensure_bparam_slot(p, &bps, &bp_len, &bp_cap, param_id, slot_w, &slot_sym)) {
                  fclose(out);
                  free(strs);
                  free(allocas);
                  free(decls);
                  free(names);
                  free(tmps);
                  free(bps);
                  errf(p, "sircc: zasm: out of memory");
                  return false;
                }
              }

              const char* reg = reg_for_width(slot_w);
              if (!reg) {
                fclose(out);
                free(strs);
                free(allocas);
                free(decls);
                free(names);
                free(tmps);
                free(bps);
                errf(p, "sircc: zasm: unsupported bparam width %lld", (long long)slot_w);
                return false;
              }

              ZasmOp op = {0};
              if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, name_len, bps, bp_len, arg_id, &op)) {
                fclose(out);
                free(strs);
                free(allocas);
                free(decls);
                free(names);
                free(tmps);
                free(bps);
                return false;
              }
              if (op.k == ZOP_SLOT) {
                if (!emit_load_slot_to_reg(out, op.s, op.n, reg, line++)) {
                  fclose(out);
                  free(strs);
                  free(allocas);
                  free(decls);
                  free(names);
                  free(tmps);
                  free(bps);
                  return false;
                }
              } else {
                if (!emit_ld_reg_or_imm(out, reg, &op, line++)) {
                  fclose(out);
                  free(strs);
                  free(allocas);
                  free(decls);
                  free(names);
                  free(tmps);
                  free(bps);
                  return false;
                }
              }
              if (!emit_store_reg_to_slot(out, slot_sym, slot_w, reg, line++)) {
                fclose(out);
                free(strs);
                free(allocas);
                free(decls);
                free(names);
                free(tmps);
                free(bps);
                return false;
              }
            }
          }

          const char* to_lbl = label_for_block(p, entry_id, to_id);
          if (!emit_jr(out, to_lbl, line++)) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            free(bps);
            return false;
          }
          terminated = true;
          break;
        }

        if (strcmp(s->tag, "term.cbr") == 0 || strcmp(s->tag, "term.condbr") == 0) {
          int64_t cond_id = 0;
          if (!parse_node_ref_id(json_obj_get(s->fields, "cond"), &cond_id)) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            errf(p, "sircc: zasm: %s node %lld missing fields.cond ref", s->tag, (long long)s->id);
            return false;
          }
          JsonValue* thenv = json_obj_get(s->fields, "then");
          JsonValue* elsev = json_obj_get(s->fields, "else");
          int64_t then_id = 0, else_id = 0;
          if (!parse_node_ref_id(thenv ? json_obj_get(thenv, "to") : NULL, &then_id) ||
              !parse_node_ref_id(elsev ? json_obj_get(elsev, "to") : NULL, &else_id)) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            errf(p, "sircc: zasm: %s node %lld missing then/else to refs", s->tag, (long long)s->id);
            return false;
          }
          const char* then_lbl = label_for_block(p, entry_id, then_id);
          const char* else_lbl = label_for_block(p, entry_id, else_id);

          NodeRec* c = get_node(p, cond_id);
          if (!c) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            errf(p, "sircc: zasm: condbr references unknown cond node %lld", (long long)cond_id);
            return false;
          }
          if (strcmp(c->tag, "i32.cmp.eq") != 0) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            errf(p, "sircc: zasm: CFG condbr only supports i32.cmp.eq for now");
            return false;
          }
          JsonValue* args = c->fields ? json_obj_get(c->fields, "args") : NULL;
          if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            errf(p, "sircc: zasm: i32.cmp.eq node %lld requires args:[a,b]", (long long)cond_id);
            return false;
          }
          int64_t a_id = 0, b_id = 0;
          if (!parse_node_ref_id(args->v.arr.items[0], &a_id) || !parse_node_ref_id(args->v.arr.items[1], &b_id)) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            errf(p, "sircc: zasm: i32.cmp.eq node %lld args must be node refs", (long long)cond_id);
            return false;
          }

          ZasmOp a = {0};
          if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, name_len, bps, bp_len, a_id, &a)) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            free(bps);
            return false;
          }
          if (!emit_ld_reg_or_imm(out, "HL", &a, line++)) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            return false;
          }
          ZasmOp b_op = {0};
          if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, name_len, bps, bp_len, b_id, &b_op)) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            free(bps);
            return false;
          }
          if (!emit_cp_hl(out, &b_op, line++)) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            return false;
          }
          if (!emit_jr_cond(out, "EQ", then_lbl, line++)) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            return false;
          }
          if (!emit_jr(out, else_lbl, line++)) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            return false;
          }
          terminated = true;
          break;
        }

        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        free(names);
        free(tmps);
        errf(p, "sircc: zasm: unsupported CFG stmt tag '%s'", s->tag);
        return false;
      }

      name_len = saved_name_len;
      if (!terminated) {
        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        free(names);
        free(tmps);
        errf(p, "sircc: zasm: CFG block %lld missing terminator", (long long)bid);
        return false;
      }

      fprintf(out, "\n");
    }

    goto emit_data;
  }

  // Legacy form: fn.fields.body is a block with stmts.
  zasm_write_ir_k(out, "label");
  fprintf(out, ",\"name\":\"zir_main\"");
  zasm_write_loc(out, line++);
  fprintf(out, "}\n");

  JsonValue* bodyv = zir_main->fields ? json_obj_get(zir_main->fields, "body") : NULL;
  int64_t body_id = 0;
  if (!parse_node_ref_id(bodyv, &body_id)) {
    fclose(out);
    free(strs);
    free(allocas);
    free(decls);
    errf(p, "sircc: zasm: fn zir_main missing body ref");
    return false;
  }
  NodeRec* body = get_node(p, body_id);
  if (!body || strcmp(body->tag, "block") != 0 || !body->fields) {
    fclose(out);
    free(strs);
    free(allocas);
    free(decls);
    errf(p, "sircc: zasm: zir_main body must be a block node");
    return false;
  }
  JsonValue* stmts = json_obj_get(body->fields, "stmts");
  if (!stmts || stmts->type != JSON_ARRAY) {
    fclose(out);
    free(strs);
    free(allocas);
    free(decls);
    errf(p, "sircc: zasm: zir_main body block missing stmts array");
    return false;
  }

  for (size_t si = 0; si < stmts->v.arr.len; si++) {
    int64_t sid = 0;
    if (!parse_node_ref_id(stmts->v.arr.items[si], &sid)) {
      fclose(out);
      free(strs);
      free(allocas);
      free(decls);
      free(names);
      errf(p, "sircc: zasm: block stmt[%zu] must be node ref", si);
      return false;
    }
    NodeRec* s = get_node(p, sid);
    if (!s) {
      fclose(out);
      free(strs);
      free(allocas);
      free(decls);
      free(names);
      errf(p, "sircc: zasm: unknown stmt node %lld", (long long)sid);
      return false;
    }

    if (strcmp(s->tag, "term.ret") != 0 && strcmp(s->tag, "return") != 0) {
      if (!emit_zir_nonterm_stmt(
              out, p, strs, strs_len, allocas, allocas_len, &names, &name_len, &name_cap, bps, bp_len, &tmps, &tmp_len, &tmp_cap, s,
              &line)) {
        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        free(names);
        free(tmps);
        free(bps);
        return false;
      }
      continue;
    }

    if (strcmp(s->tag, "term.ret") == 0 || strcmp(s->tag, "return") == 0) {
      JsonValue* rv = s->fields ? json_obj_get(s->fields, "value") : NULL;
      int64_t rid = 0;
      if (rv && parse_node_ref_id(rv, &rid)) {
        if (!zasm_emit_ret_value_to_hl(out, p, strs, strs_len, allocas, allocas_len, names, name_len, bps, bp_len, rid, line++)) {
          fclose(out);
          free(strs);
          free(allocas);
          free(decls);
          free(names);
          free(tmps);
          free(bps);
          return false;
        }
      } else {
        zasm_write_ir_k(out, "instr");
        fprintf(out, ",\"m\":\"LD\",\"ops\":[");
        zasm_write_op_reg(out, "HL");
        fprintf(out, ",");
        zasm_write_op_num(out, 0);
        fprintf(out, "]");
        zasm_write_loc(out, line++);
        fprintf(out, "}\n");
      }

      zasm_write_ir_k(out, "instr");
      fprintf(out, ",\"m\":\"RET\",\"ops\":[]");
      zasm_write_loc(out, line++);
      fprintf(out, "}\n");
      break;
    }
  }

emit_data:
  if (strs_len) fprintf(out, "\n");
  for (size_t i = 0; i < strs_len; i++) {
    zasm_write_ir_k(out, "dir");
    fprintf(out, ",\"d\":\"STR\",\"name\":");
    json_write_escaped(out, strs[i].sym);
    fprintf(out, ",\"args\":[");
    zasm_write_op_str(out, strs[i].value);
    fprintf(out, "]");
    zasm_write_loc(out, line++);
    fprintf(out, "}\n");
  }

  if (allocas_len) fprintf(out, "\n");
  for (size_t i = 0; i < allocas_len; i++) {
    zasm_write_ir_k(out, "dir");
    fprintf(out, ",\"d\":\"RESB\",\"name\":");
    json_write_escaped(out, allocas[i].sym);
    fprintf(out, ",\"args\":[");
    zasm_write_op_num(out, allocas[i].size_bytes);
    fprintf(out, "]");
    zasm_write_loc(out, line++);
    fprintf(out, "}\n");
  }

  if (bp_len) fprintf(out, "\n");
  for (size_t i = 0; i < bp_len; i++) {
    zasm_write_ir_k(out, "dir");
    fprintf(out, ",\"d\":\"RESB\",\"name\":");
    json_write_escaped(out, bps[i].sym);
    fprintf(out, ",\"args\":[");
    zasm_write_op_num(out, bps[i].size_bytes);
    fprintf(out, "]");
    zasm_write_loc(out, line++);
    fprintf(out, "}\n");
  }

  if (tmp_len) fprintf(out, "\n");
  for (size_t i = 0; i < tmp_len; i++) {
    zasm_write_ir_k(out, "dir");
    fprintf(out, ",\"d\":\"RESB\",\"name\":");
    json_write_escaped(out, tmps[i].sym);
    fprintf(out, ",\"args\":[");
    zasm_write_op_num(out, tmps[i].size_bytes);
    fprintf(out, "]");
    zasm_write_loc(out, line++);
    fprintf(out, "}\n");
  }

  fclose(out);
  free(strs);
  free(allocas);
  free(decls);
  free(names);
  free(tmps);
  free(bps);
  return true;
}
