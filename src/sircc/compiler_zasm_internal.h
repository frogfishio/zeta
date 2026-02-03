// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SIRCC_COMPILER_ZASM_INTERNAL_H
#define SIRCC_COMPILER_ZASM_INTERNAL_H

#include "compiler_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
  ZOP_NONE = 0,
  ZOP_REG,
  ZOP_SYM,
  ZOP_LBL,
  ZOP_NUM,
} ZasmOpKind;

typedef struct {
  ZasmOpKind k;
  const char* s;
  int64_t n;
} ZasmOp;

typedef struct {
  int64_t node_id;
  const char* sym;
  const char* value;
  int64_t len;
} ZasmStr;

typedef struct {
  int64_t node_id;
  const char* sym;
  int64_t size_bytes;
} ZasmAlloca;

// emit helpers
void zasm_write_ir_k(FILE* out, const char* k);
void zasm_write_loc(FILE* out, int64_t line);
void zasm_write_op_reg(FILE* out, const char* r);
void zasm_write_op_sym(FILE* out, const char* s);
void zasm_write_op_lbl(FILE* out, const char* s);
void zasm_write_op_num(FILE* out, int64_t v);
void zasm_write_op_str(FILE* out, const char* s);
void zasm_write_op_mem(FILE* out, const ZasmOp* base, int64_t disp, int64_t size_hint);
bool zasm_write_op(FILE* out, const ZasmOp* op);

// collection/prepass
NodeRec* zasm_find_fn(SirProgram* p, const char* name);
bool zasm_collect_cstrs(SirProgram* p, ZasmStr** out_strs, size_t* out_len);
bool zasm_collect_allocas(SirProgram* p, ZasmAlloca** out_as, size_t* out_len);
bool zasm_collect_decl_fns(SirProgram* p, const char*** out_names, size_t* out_len);
const char* zasm_sym_for_str(ZasmStr* strs, size_t n, int64_t node_id);
const char* zasm_sym_for_alloca(ZasmAlloca* as, size_t n, int64_t node_id);

// value lowering
bool zasm_lower_value_to_op(
    SirProgram* p,
    ZasmStr* strs,
    size_t strs_len,
    ZasmAlloca* allocas,
    size_t allocas_len,
    int64_t node_id,
    ZasmOp* out);

// stmt lowering
bool zasm_emit_call_stmt(
    FILE* out,
    SirProgram* p,
    ZasmStr* strs,
    size_t strs_len,
    ZasmAlloca* allocas,
    size_t allocas_len,
    int64_t call_id,
    int64_t line_no);
bool zasm_emit_store_stmt(
    FILE* out,
    SirProgram* p,
    ZasmStr* strs,
    size_t strs_len,
    ZasmAlloca* allocas,
    size_t allocas_len,
    NodeRec* s,
    int64_t line_no);
bool zasm_emit_mem_fill_stmt(
    FILE* out,
    SirProgram* p,
    ZasmStr* strs,
    size_t strs_len,
    ZasmAlloca* allocas,
    size_t allocas_len,
    NodeRec* s,
    int64_t line_no);
bool zasm_emit_mem_copy_stmt(
    FILE* out,
    SirProgram* p,
    ZasmStr* strs,
    size_t strs_len,
    ZasmAlloca* allocas,
    size_t allocas_len,
    NodeRec* s,
    int64_t line_no);
bool zasm_emit_ret_value_to_hl(
    FILE* out,
    SirProgram* p,
    ZasmStr* strs,
    size_t strs_len,
    ZasmAlloca* allocas,
    size_t allocas_len,
    int64_t value_id,
    int64_t line_no);

#endif

