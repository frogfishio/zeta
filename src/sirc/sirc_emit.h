// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct SircParamList SircParamList;
typedef struct SircNodeList SircNodeList;
typedef struct SircExprList SircExprList;
typedef struct SircSwitchCaseList SircSwitchCaseList;
typedef struct SircAttrList SircAttrList;
typedef struct SircIntList SircIntList;
typedef struct SircTypeList SircTypeList;
typedef struct SircSumVariantList SircSumVariantList;
typedef struct SircSemSwitchCaseList SircSemSwitchCaseList;
typedef struct SircSemMatchCaseList SircSemMatchCaseList;
typedef struct SircBranchList SircBranchList;

typedef enum SircBranchKind {
  SIRC_BRANCH_VAL = 1,
  SIRC_BRANCH_THUNK = 2,
} SircBranchKind;

typedef struct SircBranch {
  SircBranchKind kind;
  int64_t node;
} SircBranch;

typedef struct SircSemSwitchCase {
  int64_t lit;
  SircBranch body;
} SircSemSwitchCase;

typedef struct SircSemMatchCase {
  long long variant;
  SircBranch body;
} SircSemMatchCase;

// Diagnostics context (set by lexer).
extern int sirc_last_line;
extern int sirc_last_col;
extern char sirc_last_tok[64];
const char* sirc_input_path(void);

// Unit/meta
void sirc_emit_unit(char* unit, char* target);
void sirc_add_feature(char* feature); // takes ownership

// Compile-time directives (no emitted records).
void sirc_set_id_scope(char* scope); // takes ownership

// Types (return type ids)
int64_t sirc_type_from_name(char* name); // takes ownership of name
int64_t sirc_type_ptr_of(int64_t of);
int64_t sirc_type_array_of(int64_t of, long long len);
int64_t sirc_type_fn_of(SircTypeList* params, int64_t ret); // takes ownership of params
int64_t sirc_type_fun_of(int64_t sig);
int64_t sirc_type_closure_of(int64_t call_sig, int64_t env);
int64_t sirc_type_sum_of(SircSumVariantList* variants); // takes ownership of variants
int64_t sirc_type_vec_of(int64_t lane, long long lanes);
void sirc_type_alias(char* name, int64_t ty); // takes ownership of name

// Type lists (used by type constructors).
SircTypeList* sirc_types_empty(void);
SircTypeList* sirc_types_single(int64_t ty);
SircTypeList* sirc_types_append(SircTypeList* l, int64_t ty);

// Sum variant lists (used by sum(...) type constructor).
SircSumVariantList* sirc_sum_variants_empty(void);
SircSumVariantList* sirc_sum_variants_append(SircSumVariantList* l, char* name, int64_t payload_ty); // takes ownership of name
SircSumVariantList* sirc_sum_variants_merge(SircSumVariantList* a, SircSumVariantList* b); // takes ownership of b

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

// Attribute tails (call/term metadata and flags)
SircAttrList* sirc_attrs_empty(void);
SircAttrList* sirc_attrs_merge(SircAttrList* a, SircAttrList* b);               // takes ownership of b
SircAttrList* sirc_attrs_add_flag(SircAttrList* l, char* name);                 // takes ownership of name
SircAttrList* sirc_attrs_add_field_scalar_str(SircAttrList* l, char* key, char* v); // takes ownership
SircAttrList* sirc_attrs_add_field_scalar_int(SircAttrList* l, char* key, long long v); // takes ownership of key
SircAttrList* sirc_attrs_add_field_scalar_bool(SircAttrList* l, char* key, int v); // takes ownership of key
SircAttrList* sirc_attrs_add_flags_scalar_str(SircAttrList* l, char* key, char* v); // takes ownership
SircAttrList* sirc_attrs_add_flags_scalar_int(SircAttrList* l, char* key, long long v); // takes ownership of key
SircAttrList* sirc_attrs_add_flags_scalar_bool(SircAttrList* l, char* key, int v); // takes ownership of key
SircAttrList* sirc_attrs_add_flags_scalar_int_list(SircAttrList* l, char* key, SircIntList* v); // takes ownership
SircAttrList* sirc_attrs_add_sig(SircAttrList* l, char* fn_name);               // takes ownership
SircAttrList* sirc_attrs_add_count(SircAttrList* l, int64_t node_ref);
void sirc_attrs_free(SircAttrList* l);

SircIntList* sirc_ints_empty(void);
SircIntList* sirc_ints_append(SircIntList* l, long long v);
void sirc_ints_free(SircIntList* l);

int64_t sirc_call(char* name, SircExprList* args, SircAttrList* attrs);
int64_t sirc_call_typed(char* name, SircExprList* args, SircAttrList* attrs, int64_t type_ref);
int64_t sirc_select(int64_t ty, int64_t cond, int64_t then_v, int64_t else_v);
int64_t sirc_ptr_sizeof(int64_t ty);
int64_t sirc_ptr_alignof(int64_t ty);
int64_t sirc_ptr_offset(int64_t ty, int64_t base, int64_t index);

// CFG lowering helpers
void sirc_cfg_begin(void);
SircSwitchCaseList* sirc_cases_empty(void);
SircSwitchCaseList* sirc_cases_append(SircSwitchCaseList* l, int64_t lit_node, char* to_block_name); // takes ownership of to_block_name

int64_t sirc_term_br(char* to_block_name, SircExprList* args); // takes ownership of to_block_name
int64_t sirc_term_cbr(int64_t cond, char* then_block_name, SircExprList* then_args, char* else_block_name, SircExprList* else_args); // takes ownership
int64_t sirc_term_switch(int64_t scrut, SircSwitchCaseList* cases, char* default_block_name); // takes ownership
int64_t sirc_term_ret_opt(int has_value, int64_t value_node);
int64_t sirc_term_unreachable(SircAttrList* attrs);
int64_t sirc_term_trap(SircAttrList* attrs);

int64_t sirc_block_def(char* name, SircParamList* bparams, SircNodeList* stmts); // takes ownership of name
void sirc_fn_def_cfg(char* name, SircParamList* params, int64_t ret, bool is_public, int64_t entry_block,
                     SircNodeList* blocks); // takes ownership of name

// Block reference by name (used by sem.scope bodies).
int64_t sirc_block_ref(char* name); // takes ownership of name

// sem:v1 nodes
SircSemSwitchCaseList* sirc_sem_switch_cases_empty(void);
SircSemSwitchCaseList* sirc_sem_switch_cases_append(SircSemSwitchCaseList* l, int64_t lit_node, SircBranch body);

SircSemMatchCaseList* sirc_sem_match_cases_empty(void);
SircSemMatchCaseList* sirc_sem_match_cases_append(SircSemMatchCaseList* l, long long variant, SircBranch body);

SircBranchList* sirc_branch_list_empty(void);
SircBranchList* sirc_branch_list_append(SircBranchList* l, SircBranch b);

int64_t sirc_sem_if(int64_t cond, SircBranch then_b, SircBranch else_b, int64_t ty);
int64_t sirc_sem_cond(int64_t cond, SircBranch then_b, SircBranch else_b, int64_t ty);
int64_t sirc_sem_and_sc(int64_t lhs, SircBranch rhs_b);
int64_t sirc_sem_or_sc(int64_t lhs, SircBranch rhs_b);
int64_t sirc_sem_switch(int64_t scrut, SircSemSwitchCaseList* cases, SircBranch def_b, int64_t ty);
int64_t sirc_sem_match_sum(int64_t sum_ty, int64_t scrut, SircSemMatchCaseList* cases, SircBranch def_b, int64_t ty);
int64_t sirc_sem_while(SircBranch cond_thunk, SircBranch body_thunk);
int64_t sirc_sem_break(void);
int64_t sirc_sem_continue(void);
int64_t sirc_sem_defer(SircBranch thunk);
int64_t sirc_sem_scope(SircBranchList* defers, int64_t body_block);

// Structural block node constructor (used by sem.scope bodies).
int64_t sirc_block_value(SircNodeList* stmts); // takes ownership of stmts

// Statements/decls
int64_t sirc_stmt_let(char* name, int64_t ty, int64_t value);
int64_t sirc_stmt_return(int64_t value);

void sirc_extern_fn(char* name, SircParamList* params, int64_t ret);
void sirc_fn_def(char* name, SircParamList* params, int64_t ret, bool is_public, SircNodeList* stmts);
