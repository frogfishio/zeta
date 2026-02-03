// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_zasm_internal.h"

#include <stdio.h>

void zasm_write_ir_k(FILE* out, const char* k) {
  fprintf(out, "{\"ir\":\"zasm-v1.1\",\"k\":");
  json_write_escaped(out, k);
}

void zasm_write_loc(FILE* out, int64_t line) { fprintf(out, ",\"loc\":{\"line\":%lld}", (long long)line); }

void zasm_write_op_reg(FILE* out, const char* r) {
  fprintf(out, "{\"t\":\"reg\",\"v\":");
  json_write_escaped(out, r);
  fprintf(out, "}");
}

void zasm_write_op_sym(FILE* out, const char* s) {
  fprintf(out, "{\"t\":\"sym\",\"v\":");
  json_write_escaped(out, s);
  fprintf(out, "}");
}

void zasm_write_op_lbl(FILE* out, const char* s) {
  fprintf(out, "{\"t\":\"lbl\",\"v\":");
  json_write_escaped(out, s);
  fprintf(out, "}");
}

void zasm_write_op_num(FILE* out, int64_t v) { fprintf(out, "{\"t\":\"num\",\"v\":%lld}", (long long)v); }

void zasm_write_op_str(FILE* out, const char* s) {
  fprintf(out, "{\"t\":\"str\",\"v\":");
  json_write_escaped(out, s ? s : "");
  fprintf(out, "}");
}

void zasm_write_op_mem(FILE* out, const ZasmOp* base, int64_t disp, int64_t size_hint) {
  fprintf(out, "{\"t\":\"mem\",\"base\":");
  if (base->k == ZOP_REG) {
    zasm_write_op_reg(out, base->s);
  } else {
    zasm_write_op_sym(out, base->s);
  }
  if (disp) fprintf(out, ",\"disp\":%lld", (long long)disp);
  if (size_hint) fprintf(out, ",\"size\":%lld", (long long)size_hint);
  fprintf(out, "}");
}

bool zasm_write_op(FILE* out, const ZasmOp* op) {
  if (!out || !op) return false;
  switch (op->k) {
    case ZOP_REG:
      zasm_write_op_reg(out, op->s);
      return true;
    case ZOP_SYM:
      zasm_write_op_sym(out, op->s);
      return true;
    case ZOP_LBL:
      zasm_write_op_lbl(out, op->s);
      return true;
    case ZOP_NUM:
      zasm_write_op_num(out, op->n);
      return true;
    default:
      return false;
  }
}

