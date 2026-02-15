#ifndef SEM2SIR_PROFILE_H
#define SEM2SIR_PROFILE_H

#include <stdbool.h>
#include <stddef.h>

// Hardcoded closed vocabularies for sem2sir.
// This is intentionally an ABI-like contract: unknown words should be rejected.

typedef enum {
  SEM2SIR_TYPE_INVALID = 0,
  // Needed by SIR data:v1 canonical types (bytes/string.utf8/cstr).
  SEM2SIR_TYPE_I8,
  SEM2SIR_TYPE_I32,
  SEM2SIR_TYPE_BOOL,
  // Common normalized type IDs observed in Stage 4 meta.types.
  SEM2SIR_TYPE_U8,
  SEM2SIR_TYPE_U32,
  SEM2SIR_TYPE_U64,
  SEM2SIR_TYPE_I64,
  SEM2SIR_TYPE_F32,
  SEM2SIR_TYPE_F64,
  SEM2SIR_TYPE_VOID,
  SEM2SIR_TYPE_PTR,
  SEM2SIR_TYPE_SLICE,
  // data:v1 canonical named types
  SEM2SIR_TYPE_BYTES,
  SEM2SIR_TYPE_STRING_UTF8,
  SEM2SIR_TYPE_CSTR,
} sem2sir_type_id;

typedef enum {
  SEM2SIR_OP_INVALID = 0,

  // Assignment
  SEM2SIR_OP_CORE_ASSIGN,

  // Boolean short-circuit (semantic IDs, not punctuation)
  SEM2SIR_OP_CORE_BOOL_OR_SC,
  SEM2SIR_OP_CORE_BOOL_AND_SC,

  // Arithmetic (i32)
  SEM2SIR_OP_CORE_ADD,
  SEM2SIR_OP_CORE_SUB,
  SEM2SIR_OP_CORE_MUL,
  SEM2SIR_OP_CORE_DIV,
  SEM2SIR_OP_CORE_REM,

  // Bitwise / shifts
  SEM2SIR_OP_CORE_SHL,
  SEM2SIR_OP_CORE_SHR,
  SEM2SIR_OP_CORE_BITAND,
  SEM2SIR_OP_CORE_BITOR,
  SEM2SIR_OP_CORE_BITXOR,

  // Comparisons (i32 -> bool)
  SEM2SIR_OP_CORE_EQ,
  SEM2SIR_OP_CORE_NE,
  SEM2SIR_OP_CORE_LT,
  SEM2SIR_OP_CORE_LTE,
  SEM2SIR_OP_CORE_GT,
  SEM2SIR_OP_CORE_GTE,
} sem2sir_op_id;

typedef enum {
  SEM2SIR_INTRINSIC_INVALID = 0,

  // Unit-level / decls
  SEM2SIR_INTRINSIC_Unit,
  SEM2SIR_INTRINSIC_Proc,

  // Statements
  SEM2SIR_INTRINSIC_Block,
  SEM2SIR_INTRINSIC_Var,
  // Untyped pattern-binding var form (seen in Stage4 Lumen fixtures)
  SEM2SIR_INTRINSIC_VarPat,
  SEM2SIR_INTRINSIC_ExprStmt,
  SEM2SIR_INTRINSIC_Return,
  SEM2SIR_INTRINSIC_If,
  SEM2SIR_INTRINSIC_While,
  SEM2SIR_INTRINSIC_Loop,
  SEM2SIR_INTRINSIC_DoWhile,
  SEM2SIR_INTRINSIC_For,
  SEM2SIR_INTRINSIC_ForInt,
  SEM2SIR_INTRINSIC_Break,
  SEM2SIR_INTRINSIC_Continue,

  SEM2SIR_INTRINSIC_Param,
  // Typed parameter form with a pattern binder (seen in Stage4 Lumen fixtures)
  SEM2SIR_INTRINSIC_ParamPat,
  SEM2SIR_INTRINSIC_Call,
  SEM2SIR_INTRINSIC_Args,

  // Patterns (seen under VarPat.pat)
  SEM2SIR_INTRINSIC_PatBind,
  SEM2SIR_INTRINSIC_PatInt,
  SEM2SIR_INTRINSIC_PatWild,

  // Expressions
  SEM2SIR_INTRINSIC_Name,
  // Types (minimal; explicit typing only)
  SEM2SIR_INTRINSIC_TypeRef,
  SEM2SIR_INTRINSIC_Int,
  // Float literals (lossless via IEEE-754 bits)
  SEM2SIR_INTRINSIC_F32,
  SEM2SIR_INTRINSIC_F64,
  // Void unique value
  SEM2SIR_INTRINSIC_UnitVal,
  // data literals / interop
  SEM2SIR_INTRINSIC_Bytes,
  SEM2SIR_INTRINSIC_StringUtf8,
  SEM2SIR_INTRINSIC_CStr,
  SEM2SIR_INTRINSIC_Char,
  // Explicit integer width conversions (committed, 1:1 to SIR)
  SEM2SIR_INTRINSIC_ZExtI64FromI32,
  SEM2SIR_INTRINSIC_SExtI64FromI32,
  SEM2SIR_INTRINSIC_TruncI32FromI64,
  // Explicit int/float conversions (committed, 1:1 to SIR)
  SEM2SIR_INTRINSIC_F64FromI32S,
  SEM2SIR_INTRINSIC_F32FromI32S,
  SEM2SIR_INTRINSIC_TruncSatI32FromF64S,
  SEM2SIR_INTRINSIC_TruncSatI32FromF32S,
  // Explicit pointer casts (committed, 1:1 to SIR)
  SEM2SIR_INTRINSIC_PtrFromI64,
  SEM2SIR_INTRINSIC_I64FromPtr,
  SEM2SIR_INTRINSIC_True,
  SEM2SIR_INTRINSIC_False,
  SEM2SIR_INTRINSIC_Nil,
  SEM2SIR_INTRINSIC_Paren,
  SEM2SIR_INTRINSIC_Not,
  SEM2SIR_INTRINSIC_Neg,
  SEM2SIR_INTRINSIC_BitNot,
  SEM2SIR_INTRINSIC_AddrOf,
  SEM2SIR_INTRINSIC_Deref,
  SEM2SIR_INTRINSIC_Bin,

  // Control-flow expressions
  SEM2SIR_INTRINSIC_Match,
  SEM2SIR_INTRINSIC_MatchArm,

  // Sentinel (not a real intrinsic)
  SEM2SIR_INTRINSIC__MAX,
} sem2sir_intrinsic_id;

// Parse/format helpers. These are strict: they only accept the closed vocab.
// Return *_INVALID on unknown input.

sem2sir_type_id sem2sir_type_parse(const char *s, size_t n);
const char *sem2sir_type_to_string(sem2sir_type_id t);

sem2sir_op_id sem2sir_op_parse(const char *s, size_t n);
const char *sem2sir_op_to_string(sem2sir_op_id op);

sem2sir_intrinsic_id sem2sir_intrinsic_parse(const char *s, size_t n);
const char *sem2sir_intrinsic_to_string(sem2sir_intrinsic_id k);

// Convenience helpers

static inline bool sem2sir_op_is_cmp(sem2sir_op_id op) {
  switch (op) {
  case SEM2SIR_OP_CORE_EQ:
  case SEM2SIR_OP_CORE_NE:
  case SEM2SIR_OP_CORE_LT:
  case SEM2SIR_OP_CORE_LTE:
  case SEM2SIR_OP_CORE_GT:
  case SEM2SIR_OP_CORE_GTE:
    return true;
  default:
    return false;
  }
}

static inline bool sem2sir_op_is_arith(sem2sir_op_id op) {
  switch (op) {
  case SEM2SIR_OP_CORE_ADD:
  case SEM2SIR_OP_CORE_SUB:
  case SEM2SIR_OP_CORE_MUL:
  case SEM2SIR_OP_CORE_DIV:
  case SEM2SIR_OP_CORE_REM:
    return true;
  default:
    return false;
  }
}

#endif
