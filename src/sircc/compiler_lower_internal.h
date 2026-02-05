// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "compiler_internal.h"

#include <llvm-c/Core.h>

#define LOWER_ERR_NODE(f, n, code, ...) SIRCC_ERR_NODE((f)->p, (n), (code), __VA_ARGS__)

typedef struct Binding {
  const char* name;
  LLVMValueRef value;
} Binding;

typedef struct BlockBinding {
  int64_t node_id;
  LLVMBasicBlockRef bb;
} BlockBinding;

typedef struct FunctionCtx {
  SirProgram* p;
  LLVMContextRef ctx;
  LLVMModuleRef mod;
  LLVMBuilderRef builder;
  LLVMValueRef fn;

  Binding* binds;
  size_t bind_len;
  size_t bind_cap;

  LLVMBasicBlockRef* blocks_by_node; // indexed by NodeId (node records)
} FunctionCtx;

// Lowering helpers shared across lowering modules.
LLVMValueRef canonical_qnan(FunctionCtx* f, LLVMTypeRef fty);
LLVMValueRef canonicalize_float(FunctionCtx* f, LLVMValueRef v);
void emit_trap_unreachable(FunctionCtx* f);
bool emit_trap_if(FunctionCtx* f, LLVMValueRef cond);
bool emit_trap_if_misaligned(FunctionCtx* f, LLVMValueRef ptr, unsigned align);

bool bind_add(FunctionCtx* f, const char* name, LLVMValueRef v);
LLVMValueRef bind_get(FunctionCtx* f, const char* name);
size_t bind_mark(FunctionCtx* f);
void bind_restore(FunctionCtx* f, size_t mark);

// Lowering entrypoints used by lower_functions.
LLVMValueRef lower_expr(FunctionCtx* f, int64_t node_id);
bool lower_stmt(FunctionCtx* f, int64_t node_id);
bool lower_term_cfg(FunctionCtx* f, int64_t node_id);

// Internal helper: second half of lower_expr dispatch.
bool lower_expr_part_b(FunctionCtx* f, int64_t node_id, NodeRec* n, LLVMValueRef* out);
