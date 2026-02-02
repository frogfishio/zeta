// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdint.h>

typedef struct SircParamList SircParamList;
typedef struct SircNodeList SircNodeList;
typedef struct SircExprList SircExprList;
typedef struct SircSwitchCaseList SircSwitchCaseList;

// Diagnostics context (set by lexer).
extern int sirc_last_line;
extern int sirc_last_col;
extern char sirc_last_tok[64];
const char* sirc_input_path(void);

// Unit/meta
void sirc_emit_unit(char* unit, char* target);
void sirc_add_feature(char* feature); // takes ownership

// Types (return type ids)
int64_t sirc_type_from_name(char* name); // takes ownership of name
int64_t sirc_type_ptr_of(int64_t of);
int64_t sirc_type_array_of(int64_t of, long long len);
void sirc_type_alias(char* name, int64_t ty); // takes ownership of name

// Params/stmts/args lists
SircParamList* sirc_params_empty(void);
SircParamList* sirc_params_single(char* name, int64_t ty);
SircParamList* sirc_params_append(SircParamList* p, char* name, int64_t ty);

// Block params (CFG form): creates bparam nodes (not param nodes).
SircParamList* sirc_bparams_empty(void);
SircParamList* sirc_bparams_single(char* name, int64_t ty);
SircParamList* sirc_bparams_append(SircParamList* p, char* name, int64_t ty);

SircNodeList* sirc_stmtlist_empty(void);
SircNodeList* sirc_stmtlist_single(int64_t n);
SircNodeList* sirc_stmtlist_append(SircNodeList* l, int64_t n);
int64_t sirc_nodelist_first(const SircNodeList* l);

SircExprList* sirc_args_empty(void);
SircExprList* sirc_args_single(int64_t n);
SircExprList* sirc_args_append(SircExprList* l, int64_t n);

// Values/expressions (return node ids)
int64_t sirc_value_ident(char* name);
int64_t sirc_value_string(char* s);
int64_t sirc_value_bool(int b);
int64_t sirc_value_int(long long v);
int64_t sirc_typed_int(long long v, int64_t ty);
int64_t sirc_value_float(double v);         // defaults to f64
int64_t sirc_typed_float(double v, int64_t ty); // f32/f64

char* sirc_dotted_join(char* a, char* b);
char* sirc_colon_join(char* a, char* b);
int64_t sirc_call(char* name, SircExprList* args);
int64_t sirc_select(int64_t ty, int64_t cond, int64_t then_v, int64_t else_v);
int64_t sirc_ptr_sizeof(int64_t ty);
int64_t sirc_ptr_alignof(int64_t ty);
int64_t sirc_ptr_offset(int64_t ty, int64_t base, int64_t index);

// CFG lowering helpers
void sirc_cfg_begin(void);
SircSwitchCaseList* sirc_cases_empty(void);
SircSwitchCaseList* sirc_cases_append(SircSwitchCaseList* l, int64_t lit_node, char* to_block_name); // takes ownership of to_block_name

int64_t sirc_term_br(char* to_block_name, SircExprList* args); // takes ownership of to_block_name
int64_t sirc_term_cbr(int64_t cond, char* then_block_name, char* else_block_name); // takes ownership
int64_t sirc_term_switch(int64_t scrut, SircSwitchCaseList* cases, char* default_block_name); // takes ownership
int64_t sirc_term_ret_opt(int has_value, int64_t value_node);

int64_t sirc_block_def(char* name, SircParamList* bparams, SircNodeList* stmts); // takes ownership of name
void sirc_fn_def_cfg(char* name, SircParamList* params, int64_t ret, int64_t entry_block, SircNodeList* blocks); // takes ownership of name

// Statements/decls
int64_t sirc_stmt_let(char* name, int64_t ty, int64_t value);
int64_t sirc_stmt_return(int64_t value);

void sirc_extern_fn(char* name, SircParamList* params, int64_t ret);
void sirc_fn_def(char* name, SircParamList* params, int64_t ret, SircNodeList* stmts);
