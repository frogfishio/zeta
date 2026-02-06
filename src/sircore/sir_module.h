#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "guest_mem.h"
#include "sircore_vm.h"

// Minimal structured module IR for sircore (MVP).
//
// This is intentionally small: it is the "first slice" of a full SIR interpreter.
// We represent:
// - primitive types (i32/i64/ptr/bool)
// - extern function symbols (by name + signature)
// - a single entry function with a linear instruction list
//
// The executor will interpret this module deterministically and call out through
// the minimal zABI host vtable (zi_write/zi_alloc/etc).

typedef uint32_t sir_type_id_t;
typedef uint32_t sir_sym_id_t;
typedef uint32_t sir_func_id_t;
typedef uint32_t sir_val_id_t;

typedef enum sir_prim_type {
  SIR_PRIM_INVALID = 0,
  SIR_PRIM_I32,
  SIR_PRIM_I64,
  SIR_PRIM_PTR,
  SIR_PRIM_BOOL,
} sir_prim_type_t;

typedef struct sir_type {
  sir_prim_type_t prim;
} sir_type_t;

typedef struct sir_sig {
  const sir_type_id_t* params;
  uint32_t param_count;
  const sir_type_id_t* results;
  uint32_t result_count;
} sir_sig_t;

typedef enum sir_sym_kind {
  SIR_SYM_INVALID = 0,
  SIR_SYM_EXTERN_FN,
} sir_sym_kind_t;

typedef struct sir_sym {
  sir_sym_kind_t kind;
  const char* name; // owned by module
  sir_sig_t sig;    // points into module-owned arrays
} sir_sym_t;

typedef enum sir_val_kind {
  SIR_VAL_INVALID = 0,
  SIR_VAL_I32,
  SIR_VAL_I64,
  SIR_VAL_PTR,
  SIR_VAL_BOOL,
} sir_val_kind_t;

typedef struct sir_value {
  sir_val_kind_t kind;
  union {
    int32_t i32;
    int64_t i64;
    zi_ptr_t ptr;
    uint8_t b;
  } u;
} sir_value_t;

typedef enum sir_inst_kind {
  SIR_INST_INVALID = 0,
  SIR_INST_CONST_I32,
  SIR_INST_CONST_I64,
  SIR_INST_CONST_PTR_NULL,
  SIR_INST_CONST_BYTES, // yields {ptr, i64 len}
  SIR_INST_I32_ADD,
  SIR_INST_CALL_EXTERN, // currently supports zi_write/zi_end/zi_alloc/zi_free/zi_telemetry
  SIR_INST_CALL_FUNC,
  SIR_INST_RET,
  SIR_INST_RET_VAL,
  SIR_INST_EXIT,
  SIR_INST_EXIT_VAL, // exits with i32 value in slot
} sir_inst_kind_t;

typedef struct sir_inst {
  sir_inst_kind_t k;
  sir_val_id_t results[2];
  uint8_t result_count;

  union {
    struct {
      int32_t v;
      sir_val_id_t dst;
    } const_i32;
    struct {
      int64_t v;
      sir_val_id_t dst;
    } const_i64;
    struct {
      sir_val_id_t dst;
    } const_null;
    struct {
      const uint8_t* bytes; // module-owned
      uint32_t len;
      sir_val_id_t dst_ptr;
      sir_val_id_t dst_len;
    } const_bytes;
    struct {
      sir_val_id_t a;
      sir_val_id_t b;
      sir_val_id_t dst;
    } i32_add;
    struct {
      sir_sym_id_t callee;
      const sir_val_id_t* args; // module-owned
      uint32_t arg_count;
    } call_extern;
    struct {
      sir_func_id_t callee;
      const sir_val_id_t* args; // module-owned
      uint32_t arg_count;
    } call_func;
    struct {
      // no payload
      uint8_t _unused;
    } ret_;
    struct {
      sir_val_id_t value;
    } ret_val;
    struct {
      int32_t code;
    } exit_;
    struct {
      sir_val_id_t code;
    } exit_val;
  } u;
} sir_inst_t;

typedef struct sir_func {
  const char* name;           // owned by module
  const sir_inst_t* insts;    // module-owned
  uint32_t inst_count;
  uint32_t value_count;       // number of value slots (0..N-1), for executor table sizing
  sir_sig_t sig;              // points into module-owned arrays (0 or 1 result for MVP)
} sir_func_t;

typedef struct sir_module {
  const sir_type_t* types;
  uint32_t type_count;

  const sir_sym_t* syms;
  uint32_t sym_count;

  const sir_func_t* funcs;
  uint32_t func_count;

  sir_func_id_t entry; // 1-based id into funcs (0 is invalid)
} sir_module_t;

// Builder
typedef struct sir_module_builder sir_module_builder_t;

sir_module_builder_t* sir_mb_new(void);
void sir_mb_free(sir_module_builder_t* b);

sir_type_id_t sir_mb_type_prim(sir_module_builder_t* b, sir_prim_type_t prim);
sir_sym_id_t sir_mb_sym_extern_fn(sir_module_builder_t* b, const char* name, sir_sig_t sig);

sir_func_id_t sir_mb_func_begin(sir_module_builder_t* b, const char* name);
bool sir_mb_func_set_entry(sir_module_builder_t* b, sir_func_id_t f);
bool sir_mb_func_set_value_count(sir_module_builder_t* b, sir_func_id_t f, uint32_t value_count);
bool sir_mb_func_set_sig(sir_module_builder_t* b, sir_func_id_t f, sir_sig_t sig);

bool sir_mb_emit_const_i32(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, int32_t v);
bool sir_mb_emit_const_i64(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, int64_t v);
bool sir_mb_emit_const_null_ptr(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst);
bool sir_mb_emit_const_bytes(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst_ptr, sir_val_id_t dst_len, const uint8_t* bytes,
                             uint32_t len);
bool sir_mb_emit_i32_add(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_call_extern(sir_module_builder_t* b, sir_func_id_t f, sir_sym_id_t callee, const sir_val_id_t* args, uint32_t arg_count);
bool sir_mb_emit_call_extern_res(sir_module_builder_t* b, sir_func_id_t f, sir_sym_id_t callee, const sir_val_id_t* args, uint32_t arg_count,
                                 const sir_val_id_t* results, uint8_t result_count);
bool sir_mb_emit_call_func_res(sir_module_builder_t* b, sir_func_id_t f, sir_func_id_t callee, const sir_val_id_t* args, uint32_t arg_count,
                               const sir_val_id_t* results, uint8_t result_count);
bool sir_mb_emit_exit(sir_module_builder_t* b, sir_func_id_t f, int32_t code);
bool sir_mb_emit_exit_val(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t code);
bool sir_mb_emit_ret(sir_module_builder_t* b, sir_func_id_t f);
bool sir_mb_emit_ret_val(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t value);

// Finalize: produces an immutable module. The module owns all memory.
// Returns NULL on OOM or malformed builder state.
sir_module_t* sir_mb_finalize(sir_module_builder_t* b);

// Free a finalized module (returned by sir_mb_finalize). Safe to pass NULL.
void sir_module_free(sir_module_t* m);

// Validate a module for basic semantic/structural invariants.
// Returns true if valid; on failure, writes a short message into `err` when provided.
bool sir_module_validate(const sir_module_t* m, char* err, size_t err_cap);

// Execution: run module entry function.
// Returns exit code (>=0) or negative ZI_E_*.
int32_t sir_module_run(const sir_module_t* m, sem_guest_mem_t* mem, sir_host_t host);
