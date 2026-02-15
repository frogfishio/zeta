#include "sem2sir_profile.h"

#include <string.h>

typedef struct {
  const char *s;
  size_t n;
  int id;
} entry;

static bool match(const char *a, size_t an, const char *b, size_t bn) {
  return an == bn && memcmp(a, b, an) == 0;
}

// Types

static const entry TYPE_TABLE[] = {
    {"i32", 3, SEM2SIR_TYPE_I32},
    {"bool", 4, SEM2SIR_TYPE_BOOL},
  {"u8", 2, SEM2SIR_TYPE_U8},
  {"u32", 3, SEM2SIR_TYPE_U32},
  {"u64", 3, SEM2SIR_TYPE_U64},
  {"i64", 3, SEM2SIR_TYPE_I64},
  {"f64", 3, SEM2SIR_TYPE_F64},
  {"void", 4, SEM2SIR_TYPE_VOID},
  {"ptr", 3, SEM2SIR_TYPE_PTR},
  {"slice", 5, SEM2SIR_TYPE_SLICE},
  {"string.utf8", 11, SEM2SIR_TYPE_STRING_UTF8},
};

sem2sir_type_id sem2sir_type_parse(const char *s, size_t n) {
  if (!s)
    return SEM2SIR_TYPE_INVALID;
  for (size_t i = 0; i < sizeof(TYPE_TABLE) / sizeof(TYPE_TABLE[0]); i++) {
    if (match(s, n, TYPE_TABLE[i].s, TYPE_TABLE[i].n))
      return (sem2sir_type_id)TYPE_TABLE[i].id;
  }
  return SEM2SIR_TYPE_INVALID;
}

const char *sem2sir_type_to_string(sem2sir_type_id t) {
  switch (t) {
  case SEM2SIR_TYPE_I32:
    return "i32";
  case SEM2SIR_TYPE_BOOL:
    return "bool";
  case SEM2SIR_TYPE_U8:
    return "u8";
  case SEM2SIR_TYPE_U32:
    return "u32";
  case SEM2SIR_TYPE_U64:
    return "u64";
  case SEM2SIR_TYPE_I64:
    return "i64";
  case SEM2SIR_TYPE_F64:
    return "f64";
  case SEM2SIR_TYPE_VOID:
    return "void";
  case SEM2SIR_TYPE_PTR:
    return "ptr";
  case SEM2SIR_TYPE_SLICE:
    return "slice";
  case SEM2SIR_TYPE_STRING_UTF8:
    return "string.utf8";
  default:
    return NULL;
  }
}

// Ops (semantic operator IDs; Stage4 Bin.op must already be normalized)

static const entry OP_TABLE[] = {
    {"core.assign", 11, SEM2SIR_OP_CORE_ASSIGN},

  {"core.bool.or_sc", 15, SEM2SIR_OP_CORE_BOOL_OR_SC},
  {"core.bool.and_sc", 16, SEM2SIR_OP_CORE_BOOL_AND_SC},

    {"core.add", 8, SEM2SIR_OP_CORE_ADD},
    {"core.sub", 8, SEM2SIR_OP_CORE_SUB},
    {"core.mul", 8, SEM2SIR_OP_CORE_MUL},
    {"core.div", 8, SEM2SIR_OP_CORE_DIV},
    {"core.rem", 8, SEM2SIR_OP_CORE_REM},

  {"core.shl", 8, SEM2SIR_OP_CORE_SHL},
  {"core.shr", 8, SEM2SIR_OP_CORE_SHR},
  {"core.bitand", 11, SEM2SIR_OP_CORE_BITAND},
  {"core.bitor", 10, SEM2SIR_OP_CORE_BITOR},
  {"core.bitxor", 11, SEM2SIR_OP_CORE_BITXOR},

    {"core.eq", 7, SEM2SIR_OP_CORE_EQ},
    {"core.ne", 7, SEM2SIR_OP_CORE_NE},
    {"core.lt", 7, SEM2SIR_OP_CORE_LT},
    {"core.lte", 8, SEM2SIR_OP_CORE_LTE},
    {"core.gt", 7, SEM2SIR_OP_CORE_GT},
    {"core.gte", 8, SEM2SIR_OP_CORE_GTE},
};

sem2sir_op_id sem2sir_op_parse(const char *s, size_t n) {
  if (!s)
    return SEM2SIR_OP_INVALID;
  for (size_t i = 0; i < sizeof(OP_TABLE) / sizeof(OP_TABLE[0]); i++) {
    if (match(s, n, OP_TABLE[i].s, OP_TABLE[i].n))
      return (sem2sir_op_id)OP_TABLE[i].id;
  }
  return SEM2SIR_OP_INVALID;
}

const char *sem2sir_op_to_string(sem2sir_op_id op) {
  switch (op) {
  case SEM2SIR_OP_CORE_ASSIGN:
    return "core.assign";
  case SEM2SIR_OP_CORE_BOOL_OR_SC:
    return "core.bool.or_sc";
  case SEM2SIR_OP_CORE_BOOL_AND_SC:
    return "core.bool.and_sc";
  case SEM2SIR_OP_CORE_ADD:
    return "core.add";
  case SEM2SIR_OP_CORE_SUB:
    return "core.sub";
  case SEM2SIR_OP_CORE_MUL:
    return "core.mul";
  case SEM2SIR_OP_CORE_DIV:
    return "core.div";
  case SEM2SIR_OP_CORE_REM:
    return "core.rem";
  case SEM2SIR_OP_CORE_SHL:
    return "core.shl";
  case SEM2SIR_OP_CORE_SHR:
    return "core.shr";
  case SEM2SIR_OP_CORE_BITAND:
    return "core.bitand";
  case SEM2SIR_OP_CORE_BITOR:
    return "core.bitor";
  case SEM2SIR_OP_CORE_BITXOR:
    return "core.bitxor";
  case SEM2SIR_OP_CORE_EQ:
    return "core.eq";
  case SEM2SIR_OP_CORE_NE:
    return "core.ne";
  case SEM2SIR_OP_CORE_LT:
    return "core.lt";
  case SEM2SIR_OP_CORE_LTE:
    return "core.lte";
  case SEM2SIR_OP_CORE_GT:
    return "core.gt";
  case SEM2SIR_OP_CORE_GTE:
    return "core.gte";
  default:
    return NULL;
  }
}

// Intrinsics (Stage4 AST node constructors)

static const entry INTRINSIC_TABLE[] = {
    {"Unit", 4, SEM2SIR_INTRINSIC_Unit},
    {"Proc", 4, SEM2SIR_INTRINSIC_Proc},

    {"Block", 5, SEM2SIR_INTRINSIC_Block},
    {"Var", 3, SEM2SIR_INTRINSIC_Var},
    {"VarPat", 6, SEM2SIR_INTRINSIC_VarPat},
    {"ExprStmt", 8, SEM2SIR_INTRINSIC_ExprStmt},
    {"Return", 6, SEM2SIR_INTRINSIC_Return},
    {"If", 2, SEM2SIR_INTRINSIC_If},
    {"While", 5, SEM2SIR_INTRINSIC_While},
    {"Loop", 4, SEM2SIR_INTRINSIC_Loop},
    {"Break", 5, SEM2SIR_INTRINSIC_Break},
    {"Continue", 8, SEM2SIR_INTRINSIC_Continue},

    {"Param", 5, SEM2SIR_INTRINSIC_Param},
    {"ParamPat", 8, SEM2SIR_INTRINSIC_ParamPat},
    {"Call", 4, SEM2SIR_INTRINSIC_Call},
    {"Args", 4, SEM2SIR_INTRINSIC_Args},

    {"PatBind", 7, SEM2SIR_INTRINSIC_PatBind},
    {"PatInt", 6, SEM2SIR_INTRINSIC_PatInt},
    {"PatWild", 7, SEM2SIR_INTRINSIC_PatWild},

    {"Name", 4, SEM2SIR_INTRINSIC_Name},
  {"TypeRef", 7, SEM2SIR_INTRINSIC_TypeRef},
    {"Int", 3, SEM2SIR_INTRINSIC_Int},
    {"True", 4, SEM2SIR_INTRINSIC_True},
    {"False", 5, SEM2SIR_INTRINSIC_False},
    {"Nil", 3, SEM2SIR_INTRINSIC_Nil},
    {"Paren", 5, SEM2SIR_INTRINSIC_Paren},
    {"Not", 3, SEM2SIR_INTRINSIC_Not},
    {"Neg", 3, SEM2SIR_INTRINSIC_Neg},
    {"BitNot", 6, SEM2SIR_INTRINSIC_BitNot},
    {"AddrOf", 6, SEM2SIR_INTRINSIC_AddrOf},
    {"Deref", 5, SEM2SIR_INTRINSIC_Deref},
    {"Bin", 3, SEM2SIR_INTRINSIC_Bin},

    {"Match", 5, SEM2SIR_INTRINSIC_Match},
    {"MatchArm", 8, SEM2SIR_INTRINSIC_MatchArm},
};

sem2sir_intrinsic_id sem2sir_intrinsic_parse(const char *s, size_t n) {
  if (!s)
    return SEM2SIR_INTRINSIC_INVALID;
  for (size_t i = 0; i < sizeof(INTRINSIC_TABLE) / sizeof(INTRINSIC_TABLE[0]); i++) {
    if (match(s, n, INTRINSIC_TABLE[i].s, INTRINSIC_TABLE[i].n))
      return (sem2sir_intrinsic_id)INTRINSIC_TABLE[i].id;
  }
  return SEM2SIR_INTRINSIC_INVALID;
}

const char *sem2sir_intrinsic_to_string(sem2sir_intrinsic_id k) {
  switch (k) {
  case SEM2SIR_INTRINSIC_Unit:
    return "Unit";
  case SEM2SIR_INTRINSIC_Proc:
    return "Proc";
  case SEM2SIR_INTRINSIC_Block:
    return "Block";
  case SEM2SIR_INTRINSIC_Var:
    return "Var";
  case SEM2SIR_INTRINSIC_VarPat:
    return "VarPat";
  case SEM2SIR_INTRINSIC_ExprStmt:
    return "ExprStmt";
  case SEM2SIR_INTRINSIC_Return:
    return "Return";
  case SEM2SIR_INTRINSIC_If:
    return "If";
  case SEM2SIR_INTRINSIC_While:
    return "While";
  case SEM2SIR_INTRINSIC_Loop:
    return "Loop";
  case SEM2SIR_INTRINSIC_Break:
    return "Break";
  case SEM2SIR_INTRINSIC_Continue:
    return "Continue";
  case SEM2SIR_INTRINSIC_Param:
    return "Param";
  case SEM2SIR_INTRINSIC_ParamPat:
    return "ParamPat";
  case SEM2SIR_INTRINSIC_Call:
    return "Call";
  case SEM2SIR_INTRINSIC_Args:
    return "Args";
  case SEM2SIR_INTRINSIC_PatBind:
    return "PatBind";
  case SEM2SIR_INTRINSIC_PatInt:
    return "PatInt";
  case SEM2SIR_INTRINSIC_PatWild:
    return "PatWild";
  case SEM2SIR_INTRINSIC_Name:
    return "Name";
  case SEM2SIR_INTRINSIC_TypeRef:
    return "TypeRef";
  case SEM2SIR_INTRINSIC_Int:
    return "Int";
  case SEM2SIR_INTRINSIC_True:
    return "True";
  case SEM2SIR_INTRINSIC_False:
    return "False";
  case SEM2SIR_INTRINSIC_Nil:
    return "Nil";
  case SEM2SIR_INTRINSIC_Paren:
    return "Paren";
  case SEM2SIR_INTRINSIC_Not:
    return "Not";
  case SEM2SIR_INTRINSIC_Neg:
    return "Neg";
  case SEM2SIR_INTRINSIC_BitNot:
    return "BitNot";
  case SEM2SIR_INTRINSIC_AddrOf:
    return "AddrOf";
  case SEM2SIR_INTRINSIC_Deref:
    return "Deref";
  case SEM2SIR_INTRINSIC_Bin:
    return "Bin";
  case SEM2SIR_INTRINSIC_Match:
    return "Match";
  case SEM2SIR_INTRINSIC_MatchArm:
    return "MatchArm";
  default:
    return NULL;
  }
}
