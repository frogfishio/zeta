// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "compiler.h"
#include "compiler_ids.h"
#include "json.h"
#include "sircc.h"

#include <llvm-c/Core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum TypeKind {
  TYPE_INVALID = 0,
  TYPE_PRIM,
  TYPE_PTR,
  TYPE_ARRAY,
  TYPE_FN,
} TypeKind;

typedef struct SrcRec {
  int64_t id;
  const char* file;
  int64_t line;
  int64_t col;
  int64_t end_line;
  int64_t end_col;
  const char* text;
} SrcRec;

typedef struct LocRec {
  const char* unit;
  int64_t line;
  int64_t col;
} LocRec;

typedef struct SymRec {
  int64_t id;
  const char* name;
  const char* kind;
  const char* linkage;
} SymRec;

typedef struct TypeRec {
  int64_t id;
  TypeKind kind;

  const char* prim;
  int64_t of;
  int64_t len;

  int64_t* params;
  size_t param_len;
  int64_t ret;
  bool varargs;

  LLVMTypeRef llvm;
  bool resolving;
} TypeRec;

typedef struct NodeRec {
  int64_t id;
  const char* tag;
  int64_t type_ref;  // 0 means absent
  JsonValue* fields; // JSON object (or NULL)

  LLVMValueRef llvm_value; // cached when lowered (expressions); for fn nodes this is the LLVM function
  bool resolving;
} NodeRec;

typedef struct SirProgram {
  Arena arena;

  const SirccOptions* opt;
  int exit_code;

  const char* cur_path;
  size_t cur_line;
  const char* cur_kind;
  int64_t cur_rec_id;
  const char* cur_rec_tag; // node.tag / instr.m / dir.d when available
  int64_t cur_src_ref;
  LocRec cur_loc;

  const char* unit_name;
  const char* target_triple;
  unsigned ptr_bytes;
  unsigned ptr_bits;

  bool feat_atomics_v1;
  bool feat_simd_v1;
  bool feat_adt_v1;
  bool feat_fun_v1;
  bool feat_closure_v1;
  bool feat_coro_v1;
  bool feat_eh_v1;
  bool feat_gc_v1;
  bool feat_sem_v1;

  // Input IDs may be integers or strings; we intern them into dense internal ids
  // to keep storage compact while allowing stable, non-brittle identifiers.
  SirIdMap src_ids;
  SirIdMap sym_ids;
  SirIdMap type_ids;
  SirIdMap node_ids;

  SrcRec** srcs;
  size_t srcs_cap;

  SymRec** syms;
  size_t syms_cap;

  TypeRec** types;
  size_t types_cap;

  NodeRec** nodes;
  size_t nodes_cap;
} SirProgram;

// Diagnostics
void bump_exit_code(SirProgram* p, int code);
void errf(SirProgram* p, const char* fmt, ...);
void err_codef(SirProgram* p, const char* code, const char* fmt, ...);

typedef struct SirDiagSaved {
  const char* kind;
  int64_t rec_id;
  const char* rec_tag;
} SirDiagSaved;

SirDiagSaved sir_diag_push(SirProgram* p, const char* kind, int64_t rec_id, const char* rec_tag);
SirDiagSaved sir_diag_push_node(SirProgram* p, const NodeRec* n);
void sir_diag_pop(SirProgram* p, SirDiagSaved saved);

#define SIRCC_ERR(p, code, ...) err_codef((p), (code), __VA_ARGS__)
#define SIRCC_ERR_NODE(p, n, code, ...)                \
  do {                                                 \
    SirDiagSaved _saved = sir_diag_push_node((p), (n)); \
    err_codef((p), (code), __VA_ARGS__);               \
    sir_diag_pop((p), _saved);                         \
  } while (0)

#define SIRCC_ERR_NODE_ID(p, node_id, node_tag, code, ...)               \
  do {                                                                   \
    SirDiagSaved _saved = sir_diag_push((p), "node", (node_id), (node_tag)); \
    err_codef((p), (code), __VA_ARGS__);                                 \
    sir_diag_pop((p), _saved);                                           \
  } while (0)

// Shared parsing helpers (used by lowering/validation).
JsonValue* must_obj(SirProgram* p, JsonValue* v, const char* ctx);
const char* must_string(SirProgram* p, JsonValue* v, const char* ctx);
bool must_i64(SirProgram* p, JsonValue* v, int64_t* out, const char* ctx);
bool is_ident(const char* s);

bool read_line(FILE* f, char** buf, size_t* cap, size_t* out_len);
bool is_blank_line(const char* s);

// Tables
TypeRec* get_type(SirProgram* p, int64_t id);
NodeRec* get_node(SirProgram* p, int64_t id);

// Frontend
bool parse_program(SirProgram* p, const SirccOptions* opt, const char* input_path);
bool validate_program(SirProgram* p);

// Type/codegen helpers
LLVMValueRef get_or_declare_intrinsic(LLVMModuleRef mod, const char* name, LLVMTypeRef ret, LLVMTypeRef* params, unsigned param_count);
LLVMValueRef build_zext_or_trunc(LLVMBuilderRef b, LLVMValueRef v, LLVMTypeRef ty, const char* name);
LLVMValueRef build_sext_or_trunc(LLVMBuilderRef b, LLVMValueRef v, LLVMTypeRef ty, const char* name);
LLVMTypeRef lower_type_prim(LLVMContextRef ctx, const char* prim);
LLVMTypeRef lower_type(SirProgram* p, LLVMContextRef ctx, int64_t id);
bool type_size_align(SirProgram* p, int64_t type_id, int64_t* out_size, int64_t* out_align);

// Lowering
bool lower_functions(SirProgram* p, LLVMContextRef ctx, LLVMModuleRef mod);

// Emission
bool emit_module_ir(SirProgram* p, LLVMModuleRef mod, const char* out_path);
bool init_target_for_module(SirProgram* p, LLVMModuleRef mod, const char* triple);
bool emit_module_obj(SirProgram* p, LLVMModuleRef mod, const char* triple, const char* out_path);

// ZASM (zir) emission (zasm-v1.1 JSONL).
bool emit_zasm_v11(SirProgram* p, const char* out_path);

// Link
bool run_clang_link(SirProgram* p, const char* clang_path, const char* obj_path, const char* out_path);
bool run_clang_link_zabi25(SirProgram* p, const char* clang_path, const char* guest_obj_path, const char* out_path);
bool run_strip(SirProgram* p, const char* exe_path);
bool make_tmp_obj(char* out, size_t out_cap);
