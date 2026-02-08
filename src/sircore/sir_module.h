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
typedef uint32_t sir_global_id_t;

typedef enum sir_prim_type {
  SIR_PRIM_INVALID = 0,
  SIR_PRIM_VOID,
  SIR_PRIM_I1,
  SIR_PRIM_I8,
  SIR_PRIM_I16,
  SIR_PRIM_I32,
  SIR_PRIM_I64,
  SIR_PRIM_PTR,
  SIR_PRIM_BOOL,
  SIR_PRIM_F32,
  SIR_PRIM_F64,
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

typedef struct sir_global {
  const char* name;          // owned by module
  uint32_t size;             // bytes
  uint32_t align;            // bytes (power-of-two preferred)
  const uint8_t* init_bytes; // module-owned; may be NULL when init_len==0
  uint32_t init_len;         // 0 for zero-init / bss
} sir_global_t;

typedef enum sir_val_kind {
  SIR_VAL_INVALID = 0,
  SIR_VAL_I1,
  SIR_VAL_I8,
  SIR_VAL_I16,
  SIR_VAL_I32,
  SIR_VAL_I64,
  SIR_VAL_PTR,
  SIR_VAL_BOOL,
  SIR_VAL_F32,
  SIR_VAL_F64,
} sir_val_kind_t;

typedef struct sir_value {
  sir_val_kind_t kind;
  union {
    uint8_t u1;
    uint8_t u8;
    uint16_t u16;
    int32_t i32;
    int64_t i64;
    zi_ptr_t ptr;
    uint8_t b;
    uint32_t f32_bits;
    uint64_t f64_bits;
  } u;
} sir_value_t;

typedef enum sir_inst_kind {
  SIR_INST_INVALID = 0,
  SIR_INST_CONST_I1,
  SIR_INST_CONST_I8,
  SIR_INST_CONST_I16,
  SIR_INST_CONST_I32,
  SIR_INST_CONST_I64,
  SIR_INST_CONST_BOOL,
  SIR_INST_CONST_F32,
  SIR_INST_CONST_F64,
  SIR_INST_CONST_PTR,
  SIR_INST_CONST_PTR_NULL,
  SIR_INST_CONST_BYTES, // yields {ptr, i64 len}
  SIR_INST_I32_ADD,
  SIR_INST_I32_SUB,
  SIR_INST_I32_MUL,
  SIR_INST_I32_AND,
  SIR_INST_I32_OR,
  SIR_INST_I32_XOR,
  SIR_INST_I32_NOT,
  SIR_INST_I32_NEG,
  SIR_INST_I32_SHL,
  SIR_INST_I32_SHR_S,
  SIR_INST_I32_SHR_U,
  SIR_INST_I32_DIV_S_SAT,
  SIR_INST_I32_DIV_S_TRAP,
  SIR_INST_I32_DIV_U_SAT,
  SIR_INST_I32_REM_S_SAT,
  SIR_INST_I32_REM_U_SAT,
  SIR_INST_I32_CMP_EQ,
  SIR_INST_I32_CMP_NE,
  SIR_INST_I32_CMP_SLT,
  SIR_INST_I32_CMP_SLE,
  SIR_INST_I32_CMP_SGT,
  SIR_INST_I32_CMP_SGE,
  SIR_INST_I32_CMP_ULT,
  SIR_INST_I32_CMP_ULE,
  SIR_INST_I32_CMP_UGT,
  SIR_INST_I32_CMP_UGE,
  SIR_INST_F32_CMP_UEQ,
  SIR_INST_F64_CMP_OLT,
  SIR_INST_GLOBAL_ADDR, // yields ptr to module global
  SIR_INST_PTR_OFFSET,  // yields ptr = base + index*scale
  SIR_INST_PTR_ADD,     // yields ptr = base + off (bytes)
  SIR_INST_PTR_SUB,     // yields ptr = base - off (bytes)
  SIR_INST_PTR_CMP_EQ,  // yields bool
  SIR_INST_PTR_CMP_NE,  // yields bool
  SIR_INST_PTR_TO_I64,
  SIR_INST_PTR_FROM_I64,
  SIR_INST_BOOL_NOT,
  SIR_INST_BOOL_AND,
  SIR_INST_BOOL_OR,
  SIR_INST_BOOL_XOR,
  SIR_INST_I32_ZEXT_I8,
  SIR_INST_I32_ZEXT_I16,
  SIR_INST_I64_ZEXT_I32,
  SIR_INST_I32_TRUNC_I64,
  SIR_INST_SELECT,      // yields value: cond ? a : b
  SIR_INST_BR,
  SIR_INST_CBR,
  SIR_INST_SWITCH,
  SIR_INST_MEM_COPY,
  SIR_INST_MEM_FILL,
  SIR_INST_ALLOCA,
  SIR_INST_STORE_I8,
  SIR_INST_STORE_I16,
  SIR_INST_STORE_I32,
  SIR_INST_STORE_I64,
  SIR_INST_STORE_PTR,
  SIR_INST_LOAD_I8,
  SIR_INST_LOAD_I16,
  SIR_INST_LOAD_I32,
  SIR_INST_LOAD_I64,
  SIR_INST_LOAD_PTR,
  SIR_INST_STORE_F32,
  SIR_INST_STORE_F64,
  SIR_INST_LOAD_F32,
  SIR_INST_LOAD_F64,
  SIR_INST_CALL_EXTERN, // currently supports zi_write/zi_end/zi_alloc/zi_free/zi_telemetry
  SIR_INST_CALL_FUNC,
  // Calls an in-module function via a tagged function pointer value:
  //   ptr = 0xF000... | fid  (same encoding used by sem's fun.sym)
  // This is the minimal "indirect call" needed for closure:v1 in `sem`.
  SIR_INST_CALL_FUNC_PTR,
  SIR_INST_RET,
  SIR_INST_RET_VAL,
  SIR_INST_EXIT,
  SIR_INST_EXIT_VAL, // exits with i32 value in slot
} sir_inst_kind_t;

typedef struct sir_inst {
  sir_inst_kind_t k;
  sir_val_id_t results[2];
  uint8_t result_count;
  uint32_t src_node_id; // 0 when unknown
  uint32_t src_line;    // 0 when unknown

  union {
    struct {
      uint8_t v;
      sir_val_id_t dst;
    } const_i1;
    struct {
      uint8_t v;
      sir_val_id_t dst;
    } const_i8;
    struct {
      uint16_t v;
      sir_val_id_t dst;
    } const_i16;
    struct {
      int32_t v;
      sir_val_id_t dst;
    } const_i32;
    struct {
      int64_t v;
      sir_val_id_t dst;
    } const_i64;
    struct {
      uint8_t v;
      sir_val_id_t dst;
    } const_bool;
    struct {
      uint32_t bits;
      sir_val_id_t dst;
    } const_f32;
    struct {
      uint64_t bits;
      sir_val_id_t dst;
    } const_f64;
    struct {
      zi_ptr_t v;
      sir_val_id_t dst;
    } const_ptr;
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
      sir_val_id_t x;
      sir_val_id_t dst;
    } i32_un;
    struct {
      sir_val_id_t a;
      sir_val_id_t b;
      sir_val_id_t dst;
    } i32_cmp_eq;
    struct {
      sir_val_id_t a;
      sir_val_id_t b;
      sir_val_id_t dst;
    } f_cmp;
    struct {
      sir_global_id_t gid;
      sir_val_id_t dst;
    } global_addr;
    struct {
      sir_val_id_t base;
      sir_val_id_t index;
      uint32_t scale;
      sir_val_id_t dst;
    } ptr_offset;
    struct {
      sir_val_id_t base;
      sir_val_id_t off;
      sir_val_id_t dst;
    } ptr_add;
    struct {
      sir_val_id_t base;
      sir_val_id_t off;
      sir_val_id_t dst;
    } ptr_sub;
    struct {
      sir_val_id_t a;
      sir_val_id_t b;
      sir_val_id_t dst;
    } ptr_cmp;
    struct {
      sir_val_id_t x;
      sir_val_id_t dst;
    } ptr_to_i64;
    struct {
      sir_val_id_t x;
      sir_val_id_t dst;
    } ptr_from_i64;
    struct {
      sir_val_id_t x;
      sir_val_id_t dst;
    } bool_not;
    struct {
      sir_val_id_t a;
      sir_val_id_t b;
      sir_val_id_t dst;
    } bool_bin;
    struct {
      sir_val_id_t x;
      sir_val_id_t dst;
    } i32_trunc_i64;
    struct {
      sir_val_id_t x;
      sir_val_id_t dst;
    } i32_zext_i8;
    struct {
      sir_val_id_t x;
      sir_val_id_t dst;
    } i32_zext_i16;
    struct {
      sir_val_id_t x;
      sir_val_id_t dst;
    } i64_zext_i32;
    struct {
      sir_val_id_t cond;
      sir_val_id_t a;
      sir_val_id_t b;
      sir_val_id_t dst;
    } select;
    struct {
      uint32_t target_ip;
      const sir_val_id_t* src_slots; // module-owned; len=arg_count
      const sir_val_id_t* dst_slots; // module-owned; len=arg_count
      uint32_t arg_count;
    } br;
    struct {
      sir_val_id_t cond;
      uint32_t then_ip;
      uint32_t else_ip;
    } cbr;
    struct {
      sir_val_id_t scrut;
      const int32_t* case_lits;    // module-owned; len=case_count
      const uint32_t* case_target; // module-owned; len=case_count
      uint32_t case_count;
      uint32_t default_ip;
    } sw;
    struct {
      sir_val_id_t dst;
      sir_val_id_t src;
      sir_val_id_t len;
      uint8_t overlap_allow;
    } mem_copy;
    struct {
      sir_val_id_t dst;
      sir_val_id_t byte;
      sir_val_id_t len;
    } mem_fill;
    struct {
      uint32_t size;
      uint32_t align;
      sir_val_id_t dst;
    } alloca_;
    struct {
      sir_val_id_t addr;
      sir_val_id_t value;
      uint32_t align;
    } store;
    struct {
      sir_val_id_t addr;
      uint32_t align;
      sir_val_id_t dst;
    } load;
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
      sir_val_id_t callee_ptr;   // SIR_VAL_PTR holding a tagged fid
      const sir_val_id_t* args;  // module-owned
      uint32_t arg_count;
    } call_func_ptr;
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

  const sir_global_t* globals;
  uint32_t global_count;

  const sir_func_t* funcs;
  uint32_t func_count;

  sir_func_id_t entry; // 1-based id into funcs (0 is invalid)
} sir_module_t;

// Execution events (optional).
// Used by frontends (sem/instrument) to build trace/coverage/profiling without forking the VM.
typedef enum sir_mem_event_kind {
  SIR_MEM_READ = 0,
  SIR_MEM_WRITE = 1,
} sir_mem_event_kind_t;

typedef struct sir_exec_event_sink {
  void* user;
  void (*on_step)(void* user, const sir_module_t* m, sir_func_id_t fid, uint32_t ip, sir_inst_kind_t k);
  void (*on_mem)(void* user, const sir_module_t* m, sir_func_id_t fid, uint32_t ip, sir_mem_event_kind_t k, zi_ptr_t addr, uint32_t size);
  void (*on_hostcall)(void* user, const sir_module_t* m, sir_func_id_t fid, uint32_t ip, const char* callee, int32_t rc);
} sir_exec_event_sink_t;

// Returns a stable short name for an instruction kind (for trace output).
// Never returns NULL; unknown kinds return "unknown".
const char* sir_inst_kind_name(sir_inst_kind_t k);

// Builder
typedef struct sir_module_builder sir_module_builder_t;

sir_module_builder_t* sir_mb_new(void);
void sir_mb_free(sir_module_builder_t* b);

// Set/clear the source context for subsequently emitted instructions.
// This is a lightweight mapping hook used by SEM for trace/coverage back to SIR nodes.
void sir_mb_set_src(sir_module_builder_t* b, uint32_t node_id, uint32_t line);
void sir_mb_clear_src(sir_module_builder_t* b);

sir_type_id_t sir_mb_type_prim(sir_module_builder_t* b, sir_prim_type_t prim);
sir_sym_id_t sir_mb_sym_extern_fn(sir_module_builder_t* b, const char* name, sir_sig_t sig);
sir_global_id_t sir_mb_global(sir_module_builder_t* b, const char* name, uint32_t size, uint32_t align, const uint8_t* init_bytes,
                              uint32_t init_len);

sir_func_id_t sir_mb_func_begin(sir_module_builder_t* b, const char* name);
bool sir_mb_func_set_entry(sir_module_builder_t* b, sir_func_id_t f);
bool sir_mb_func_set_value_count(sir_module_builder_t* b, sir_func_id_t f, uint32_t value_count);
bool sir_mb_func_set_sig(sir_module_builder_t* b, sir_func_id_t f, sir_sig_t sig);

bool sir_mb_emit_const_i1(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, bool v);
bool sir_mb_emit_const_i32(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, int32_t v);
bool sir_mb_emit_const_i64(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, int64_t v);
bool sir_mb_emit_const_i8(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, uint8_t v);
bool sir_mb_emit_const_i16(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, uint16_t v);
bool sir_mb_emit_const_bool(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, bool v);
bool sir_mb_emit_const_f32_bits(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, uint32_t bits);
bool sir_mb_emit_const_f64_bits(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, uint64_t bits);
bool sir_mb_emit_const_ptr(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, zi_ptr_t v);
bool sir_mb_emit_const_null_ptr(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst);
bool sir_mb_emit_const_bytes(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst_ptr, sir_val_id_t dst_len, const uint8_t* bytes,
                             uint32_t len);
bool sir_mb_emit_i32_add(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_sub(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_mul(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_and(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_or(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_xor(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_not(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x);
bool sir_mb_emit_i32_neg(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x);
bool sir_mb_emit_i32_shl(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x, sir_val_id_t shift);
bool sir_mb_emit_i32_shr_s(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x, sir_val_id_t shift);
bool sir_mb_emit_i32_shr_u(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x, sir_val_id_t shift);
bool sir_mb_emit_i32_div_s_sat(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_div_s_trap(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_div_u_sat(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_rem_s_sat(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_rem_u_sat(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_cmp_eq(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_cmp_ne(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_cmp_slt(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_cmp_sle(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_cmp_sgt(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_cmp_sge(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_cmp_ult(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_cmp_ule(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_cmp_ugt(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_cmp_uge(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_f32_cmp_ueq(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_f64_cmp_olt(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_global_addr(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_global_id_t gid);
bool sir_mb_emit_ptr_offset(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t base, sir_val_id_t index, uint32_t scale);
bool sir_mb_emit_ptr_add(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t base, sir_val_id_t off);
bool sir_mb_emit_ptr_sub(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t base, sir_val_id_t off);
bool sir_mb_emit_ptr_cmp_eq(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_ptr_cmp_ne(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_ptr_to_i64(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x);
bool sir_mb_emit_ptr_from_i64(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x);
bool sir_mb_emit_bool_not(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x);
bool sir_mb_emit_bool_and(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_bool_or(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_bool_xor(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_i32_zext_i8(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x);
bool sir_mb_emit_i32_zext_i16(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x);
bool sir_mb_emit_i64_zext_i32(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x);
bool sir_mb_emit_i32_trunc_i64(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x);
bool sir_mb_emit_select(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t cond, sir_val_id_t a, sir_val_id_t b_);
bool sir_mb_emit_br_args(sir_module_builder_t* b, sir_func_id_t f, uint32_t target_ip, const sir_val_id_t* src_slots, const sir_val_id_t* dst_slots,
                         uint32_t arg_count, uint32_t* out_ip);
bool sir_mb_emit_br(sir_module_builder_t* b, sir_func_id_t f, uint32_t target_ip, uint32_t* out_ip);
bool sir_mb_emit_cbr(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t cond, uint32_t then_ip, uint32_t else_ip, uint32_t* out_ip);
bool sir_mb_emit_switch(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t scrut, const int32_t* case_lits, const uint32_t* case_target,
                        uint32_t case_count, uint32_t default_ip, uint32_t* out_ip);
bool sir_mb_emit_mem_copy(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t src, sir_val_id_t len, bool overlap_allow);
bool sir_mb_emit_mem_fill(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t byte, sir_val_id_t len);
bool sir_mb_emit_alloca(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, uint32_t size, uint32_t align);
bool sir_mb_emit_store_i8(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t addr, sir_val_id_t value, uint32_t align);
bool sir_mb_emit_store_i16(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t addr, sir_val_id_t value, uint32_t align);
bool sir_mb_emit_store_i32(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t addr, sir_val_id_t value, uint32_t align);
bool sir_mb_emit_store_i64(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t addr, sir_val_id_t value, uint32_t align);
bool sir_mb_emit_store_ptr(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t addr, sir_val_id_t value, uint32_t align);
bool sir_mb_emit_store_f32(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t addr, sir_val_id_t value, uint32_t align);
bool sir_mb_emit_store_f64(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t addr, sir_val_id_t value, uint32_t align);
bool sir_mb_emit_load_i8(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t addr, uint32_t align);
bool sir_mb_emit_load_i16(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t addr, uint32_t align);
bool sir_mb_emit_load_i32(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t addr, uint32_t align);
bool sir_mb_emit_load_i64(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t addr, uint32_t align);
bool sir_mb_emit_load_ptr(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t addr, uint32_t align);
bool sir_mb_emit_load_f32(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t addr, uint32_t align);
bool sir_mb_emit_load_f64(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t addr, uint32_t align);
bool sir_mb_emit_call_extern(sir_module_builder_t* b, sir_func_id_t f, sir_sym_id_t callee, const sir_val_id_t* args, uint32_t arg_count);
bool sir_mb_emit_call_extern_res(sir_module_builder_t* b, sir_func_id_t f, sir_sym_id_t callee, const sir_val_id_t* args, uint32_t arg_count,
                                 const sir_val_id_t* results, uint8_t result_count);
bool sir_mb_emit_call_func_res(sir_module_builder_t* b, sir_func_id_t f, sir_func_id_t callee, const sir_val_id_t* args, uint32_t arg_count,
                               const sir_val_id_t* results, uint8_t result_count);
bool sir_mb_emit_call_func_ptr_res(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t callee_ptr, const sir_val_id_t* args, uint32_t arg_count,
                                   const sir_val_id_t* results, uint8_t result_count);
bool sir_mb_emit_exit(sir_module_builder_t* b, sir_func_id_t f, int32_t code);
bool sir_mb_emit_exit_val(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t code);
bool sir_mb_emit_ret(sir_module_builder_t* b, sir_func_id_t f);
bool sir_mb_emit_ret_val(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t value);

// CFG helpers (ip is 0-based instruction index within the function).
uint32_t sir_mb_func_ip(const sir_module_builder_t* b, sir_func_id_t f);
bool sir_mb_patch_br(sir_module_builder_t* b, sir_func_id_t f, uint32_t ip, uint32_t target_ip);
bool sir_mb_patch_cbr(sir_module_builder_t* b, sir_func_id_t f, uint32_t ip, uint32_t then_ip, uint32_t else_ip);
bool sir_mb_patch_switch(sir_module_builder_t* b, sir_func_id_t f, uint32_t ip, const uint32_t* case_target, uint32_t case_count,
                         uint32_t default_ip);

// Finalize: produces an immutable module. The module owns all memory.
// Returns NULL on OOM or malformed builder state.
sir_module_t* sir_mb_finalize(sir_module_builder_t* b);

// Free a finalized module (returned by sir_mb_finalize). Safe to pass NULL.
void sir_module_free(sir_module_t* m);

// Validate a module for basic semantic/structural invariants.
// Returns true if valid; on failure, writes a short message into `err` when provided.
bool sir_module_validate(const sir_module_t* m, char* err, size_t err_cap);

// Structured validator output (preferred).
// `src_node_id`/`src_line` come from instruction-level source mapping when available.
typedef struct sir_validate_diag {
  const char* code; // stable category/code string (e.g. "sir.validate.inst")
  char message[256];
  sir_func_id_t fid;      // 0 when unknown / not applicable
  uint32_t ip;            // 0 when unknown / not applicable
  sir_inst_kind_t op;     // SIR_INST_INVALID when not applicable
  uint32_t src_node_id;   // 0 when unknown
  uint32_t src_line;      // 0 when unknown
} sir_validate_diag_t;

// Validate a module for basic semantic/structural invariants.
// Returns true if valid; on failure, fills `out` when provided.
bool sir_module_validate_ex(const sir_module_t* m, sir_validate_diag_t* out);

// Execution: run module entry function.
// Returns exit code (>=0) or negative ZI_E_*.
int32_t sir_module_run(const sir_module_t* m, sem_guest_mem_t* mem, sir_host_t host);

// Execution with an optional event sink.
// The sink callbacks are best-effort and must not affect execution.
int32_t sir_module_run_ex(const sir_module_t* m, sem_guest_mem_t* mem, sir_host_t host, const sir_exec_event_sink_t* sink);
