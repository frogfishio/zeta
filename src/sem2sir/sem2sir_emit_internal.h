#pragma once

#include "json_min.h"

#include "sem2sir_check.h"
#include "sem2sir_profile.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Internal implementation header for the sem2sir emitter.
//
// Not a stable public API; it exists to split the former monolithic
// sem2sir_emit.c into cohesive compilation units.

typedef struct {
  char *name;
  sem2sir_type_id type;
  sem2sir_type_id ptr_of; // SEM2SIR_TYPE_INVALID unless this is a derived ptr(T)
  const char *sir_type_id;
  bool is_slot;
} Local;

typedef struct {
  char **ids;
  size_t count;
} StmtList;

typedef struct {
  sem2sir_type_id base;
  sem2sir_type_id ptr_of; // SEM2SIR_TYPE_INVALID unless base==SEM2SIR_TYPE_PTR and this is ptr(T)
  const char *sir_id;
} SemTypeInfo;

typedef struct {
  char *id; // SIR node id for this block
  char **stmt_ids;
  size_t stmt_count;
  size_t stmt_cap;
  bool terminated;
} SirBlockBuild;

typedef struct {
  SirBlockBuild *blocks;
  size_t block_count;
  size_t block_cap;
  size_t cur_block;
  size_t entry_block;
} SirFnBuild;

typedef struct {
  // Indices into SirFnBuild.blocks
  size_t break_to;
  size_t continue_to;
} LoopTargets;

#define SEM2SIR_TYPE_COUNT (SEM2SIR_TYPE_STRING_UTF8 + 1)

typedef struct ProcInfo ProcInfo;

typedef struct {
  const char *in_path;
  const char *out_path;
  FILE *out;
  uint32_t next_node;

  ProcInfo *procs;
  size_t proc_count;
  size_t proc_cap;

  char **emitted_fn_type_ids;
  size_t emitted_fn_type_count;
  size_t emitted_fn_type_cap;

  Local *locals;
  size_t local_count;
  StmtList *effects;
  sem2sir_type_id fn_ret;
  sem2sir_type_id default_int;
  sem2sir_type_id default_ptr_pointee;
  bool meta_sem_v1;
  bool emitted_i32;
  bool emitted_i64;
  bool emitted_bool;
  bool emitted_u8;
  bool emitted_u32;
  bool emitted_u64;
  bool emitted_f64;
  bool emitted_ptr;
  bool emitted_slice;
  bool emitted_string_utf8;
  bool emitted_void;

  // Derived pointer types by pointee base type.
  char *derived_ptr_type_id[SEM2SIR_TYPE_COUNT];
  bool emitted_derived_ptr_type[SEM2SIR_TYPE_COUNT];
} EmitCtx;

struct ProcInfo {
  char *name;
  char *fn_id;
  char *fn_type_id;
  SemTypeInfo *params;
  size_t param_count;
  sem2sir_type_id ret;
  SemTypeInfo ret_ti;
};

typedef struct {
  char *id; // sir node id
  sem2sir_type_id type;
  sem2sir_type_id ptr_of; // SEM2SIR_TYPE_INVALID unless type==SEM2SIR_TYPE_PTR and this is ptr(T)
  const char *sir_type_id; // optional: the SIR type_ref id for this expression
} SirExpr;

// -----------------
// JSON/file helpers
// -----------------

void emit_json_string(FILE *out, const char *s);
char *read_file(const char *path, size_t *len_out);
void err(const char *in_path, const char *msg);

bool json_peek_non_ws(GritJsonCursor *c, char *out);
bool json_expect_key(GritJsonCursor *c, char **out_key);
bool parse_tok_text_alloc_strict(GritJsonCursor *c, const char *in_path, char **out_text);

// -----------------
// Core emit helpers
// -----------------

char *new_node_id(EmitCtx *ctx);

const char *sir_type_id_for(sem2sir_type_id t);

bool emit_typeinfo_if_needed(EmitCtx *ctx, const SemTypeInfo *ti);
bool emit_type_if_needed(EmitCtx *ctx, sem2sir_type_id t);

// Derived ptr(T) helpers
const char *get_derived_ptr_type_id(EmitCtx *ctx, sem2sir_type_id pointee);
bool type_supports_slot_storage(sem2sir_type_id t);

int type_align_bytes(sem2sir_type_id t);
const char *type_store_tag(sem2sir_type_id t);
const char *type_load_tag(sem2sir_type_id t);

// Proc table
void proc_table_free(EmitCtx *ctx);
ProcInfo *proc_table_find(EmitCtx *ctx, const char *name);
bool proc_table_add(EmitCtx *ctx, const char *name, const SemTypeInfo *params, size_t param_count, SemTypeInfo ret_ti);
bool emit_fn_type_if_needed(EmitCtx *ctx, const ProcInfo *p);

// CFG/fn builder
bool fn_build_new_block(SirFnBuild *fn, EmitCtx *ctx, size_t *out_idx);
bool fn_build_append_stmt(SirFnBuild *fn, EmitCtx *ctx, char *stmt_id, bool is_terminator);
bool fn_build_append_effects(SirFnBuild *fn, EmitCtx *ctx, StmtList *effects);

bool emit_term_ret(EmitCtx *ctx, sem2sir_type_id fn_ret, const char *value_id, char **out_term_id);
bool emit_term_br(EmitCtx *ctx, const char *to_block_id, char **out_term_id);
bool emit_term_condbr(EmitCtx *ctx, const char *cond_id, const char *then_block_id, const char *else_block_id,
                      char **out_term_id);

// Locals
bool locals_lookup(EmitCtx *ctx, const char *name, sem2sir_type_id *out_type, sem2sir_type_id *out_ptr_of,
                   const char **out_sir_type_id, bool *out_is_slot);
bool locals_push_binding(EmitCtx *ctx, const char *name, SemTypeInfo ti, bool is_slot);
void locals_free(EmitCtx *ctx);

// Lists / probes
bool stmtlist_push(StmtList *sl, char *id);
bool capture_json_value_alloc(GritJsonCursor *c, char **out_buf, size_t *out_len);

// Best-effort probes (do not emit SIR)
sem2sir_type_id probe_expr_type_no_expected(const char *expr_json, size_t expr_len, EmitCtx *ctx);
sem2sir_type_id probe_deref_expr_pointee_no_expected(const char *deref_json, size_t deref_len, EmitCtx *ctx);

// -----------------
// AST parsing
// -----------------

bool parse_node_k_string(GritJsonCursor *c, EmitCtx *ctx, char **out_k);

bool parse_name_id_alloc(GritJsonCursor *c, EmitCtx *ctx, char **out_name);
bool parse_name_id_only(GritJsonCursor *c, EmitCtx *ctx, char **out_name_text);

bool parse_type_typeinfo(GritJsonCursor *c, EmitCtx *ctx, SemTypeInfo *out_ti);

bool prescan_root_for_meta_defaults(const char *buf, size_t len, EmitCtx *ctx);

// Expression parsing
bool parse_expr_call(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out);
bool parse_expr_int(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out);
bool parse_expr_name(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out);
bool parse_expr_addrof(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out);
bool parse_expr_deref(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out);
bool parse_expr_true_false(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, bool v, SirExpr *out);
bool parse_expr_paren(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out);
bool parse_expr_not(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out);
bool parse_expr_neg(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out);
bool parse_expr_bitnot(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out);
bool parse_expr_bin(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out);
bool parse_expr_match(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out);

bool parse_expr(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id expected, SirExpr *out);

// Lvalues / assignment statement helper
bool parse_lvalue_addr(GritJsonCursor *c, EmitCtx *ctx, sem2sir_type_id store_ty, SirExpr *out_addr);
bool parse_stmt_bin_assign_emit_store(GritJsonCursor *c, EmitCtx *ctx, char **out_store_id);

// Statements / unit
bool parse_block(GritJsonCursor *c, EmitCtx *ctx, SirFnBuild *fn, bool require_return, const LoopTargets *loop);

bool skip_remaining_object_fields(GritJsonCursor *c, EmitCtx *ctx, const char *what);

bool parse_unit_item_and_maybe_emit(GritJsonCursor *c, EmitCtx *ctx);

bool prescan_ast_for_procs(const char *buf, size_t len, EmitCtx *ctx);
