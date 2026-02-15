#include "gl_sig.h"

// GL (canonical): closed semantics vocabulary.
//
// Note: some constructors exist only to help surface parsing (precedence/chaining/
// parentheses/etc). These are *not* part of the canonical intrinsic AST ABI
// (LANG2); they must be folded away during Stage 4 lowering.
//
// Canonical shape reference: normative/intrinsics.grit.
//
// Design rule: prefer reusing canonical, language-agnostic constructors (e.g.
// TypeDecl/Record/Field/Array/TypeRef) over introducing surface-only helpers.
// Helpers are allowed only when they fold away deterministically in Stage 4.
//
// Field `type` is intentionally permissive (often "*") so multiple surface
// grammars can map into GL without sharing nonterminal/token names.

// --------------------
// Structural
// --------------------

static const Spec3FieldSig gl_fields_Block_0[] = {
    {"items", "rule", "*", false, true},
};
static const Spec3VariantSig gl_variants_Block[] = {
    {gl_fields_Block_0, sizeof(gl_fields_Block_0) / sizeof(gl_fields_Block_0[0])},
};

static const Spec3FieldSig gl_fields_Args_0[] = {
    {"items", "rule", "*", false, true},
};
static const Spec3VariantSig gl_variants_Args[] = {
    {gl_fields_Args_0, sizeof(gl_fields_Args_0) / sizeof(gl_fields_Args_0[0])},
};

// --------------------
// Program structure / declarations / types (language-agnostic)
// --------------------

static const Spec3FieldSig gl_fields_Unit_0[] = {
    {"name", "tok", "*", true, false},
    {"items", "rule", "*", false, true},
};
static const Spec3VariantSig gl_variants_Unit[] = {
    {gl_fields_Unit_0, sizeof(gl_fields_Unit_0) / sizeof(gl_fields_Unit_0[0])},
};

static const Spec3FieldSig gl_fields_Import_0[] = {
    {"name", "tok", "*", false, false},
    {"path", "tok", "*", true, false},
};
static const Spec3VariantSig gl_variants_Import[] = {
    {gl_fields_Import_0, sizeof(gl_fields_Import_0) / sizeof(gl_fields_Import_0[0])},
};

// Proc: canonical core shape (no explicit export metadata).
static const Spec3FieldSig gl_fields_Proc_0[] = {
    {"name", "tok", "*", false, false},
    {"params", "rule", "*", false, true},
    {"ret", "rule", "*", true, false},
    {"decls", "rule", "*", false, true},
    {"body", "rule", "*", false, false},
};

// Proc: extended shape with explicit export metadata.
// Keep this as a second variant so existing specs that emit 5-arg Proc(...) remain valid.
static const Spec3FieldSig gl_fields_Proc_1[] = {
    {"name", "tok", "*", false, false},
    {"params", "rule", "*", false, true},
    {"ret", "rule", "*", true, false},
    {"decls", "rule", "*", false, true},
    {"body", "rule", "*", false, false},
    {"extern", "tok", "*", true, false},
    {"link_name", "tok", "*", true, false},
};
static const Spec3VariantSig gl_variants_Proc[] = {
    {gl_fields_Proc_0, sizeof(gl_fields_Proc_0) / sizeof(gl_fields_Proc_0[0])},
    {gl_fields_Proc_1, sizeof(gl_fields_Proc_1) / sizeof(gl_fields_Proc_1[0])},
};

static const Spec3FieldSig gl_fields_ExternProc_0[] = {
    {"name", "tok", "*", false, false},
    {"params", "rule", "*", false, true},
    {"ret", "rule", "*", true, false},
    {"link_name", "tok", "*", true, false},
};
static const Spec3VariantSig gl_variants_ExternProc[] = {
    {gl_fields_ExternProc_0, sizeof(gl_fields_ExternProc_0) / sizeof(gl_fields_ExternProc_0[0])},
};

static const Spec3FieldSig gl_fields_Param_0[] = {
    {"name", "tok", "*", false, false},
    {"type", "rule", "*", true, false},
    {"mode", "tok", "*", true, false},
};
static const Spec3VariantSig gl_variants_Param[] = {
    {gl_fields_Param_0, sizeof(gl_fields_Param_0) / sizeof(gl_fields_Param_0[0])},
};

static const Spec3FieldSig gl_fields_ParamPat_0[] = {
    {"pat", "rule", "*", false, false},
    {"type", "rule", "*", true, false},
    {"mode", "tok", "*", true, false},
};
static const Spec3VariantSig gl_variants_ParamPat[] = {
    {gl_fields_ParamPat_0, sizeof(gl_fields_ParamPat_0) / sizeof(gl_fields_ParamPat_0[0])},
};

static const Spec3FieldSig gl_fields_Var_0[] = {
    {"name", "tok", "*", false, false},
    {"type", "rule", "*", true, false},
    {"init", "rule", "*", true, false},
};
static const Spec3VariantSig gl_variants_Var[] = {
    {gl_fields_Var_0, sizeof(gl_fields_Var_0) / sizeof(gl_fields_Var_0[0])},
};

static const Spec3FieldSig gl_fields_VarPat_0[] = {
    {"pat", "rule", "*", false, false},
    {"type", "rule", "*", true, false},
    {"init", "rule", "*", true, false},
};
static const Spec3VariantSig gl_variants_VarPat[] = {
    {gl_fields_VarPat_0, sizeof(gl_fields_VarPat_0) / sizeof(gl_fields_VarPat_0[0])},
};

// Surface parsing helper: represents a single multi-name VAR declaration.
// Folded away during Stage 4 lowering.
static const Spec3FieldSig gl_fields_VarGroup_0[] = {
    {"names", "tok", "*", false, true},
    {"type", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_VarGroup[] = {
    {gl_fields_VarGroup_0, sizeof(gl_fields_VarGroup_0) / sizeof(gl_fields_VarGroup_0[0])},
};

static const Spec3FieldSig gl_fields_Const_0[] = {
    {"name", "tok", "*", false, false},
    {"type", "rule", "*", true, false},
    {"value", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Const[] = {
    {gl_fields_Const_0, sizeof(gl_fields_Const_0) / sizeof(gl_fields_Const_0[0])},
};

static const Spec3FieldSig gl_fields_TypeDecl_0[] = {
    {"name", "tok", "*", false, false},
    {"type", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_TypeDecl[] = {
    {gl_fields_TypeDecl_0, sizeof(gl_fields_TypeDecl_0) / sizeof(gl_fields_TypeDecl_0[0])},
};

static const Spec3FieldSig gl_fields_TypeRef_0[] = {
    {"name", "tok", "*", false, false},
};
static const Spec3VariantSig gl_variants_TypeRef[] = {
    {gl_fields_TypeRef_0, sizeof(gl_fields_TypeRef_0) / sizeof(gl_fields_TypeRef_0[0])},
};

static const Spec3FieldSig gl_fields_Ptr_0[] = {
    {"base", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Ptr[] = {
    {gl_fields_Ptr_0, sizeof(gl_fields_Ptr_0) / sizeof(gl_fields_Ptr_0[0])},
};

static const Spec3FieldSig gl_fields_Array_0[] = {
    {"size", "rule", "*", true, false},
    {"base", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Array[] = {
    {gl_fields_Array_0, sizeof(gl_fields_Array_0) / sizeof(gl_fields_Array_0[0])},
};

static const Spec3FieldSig gl_fields_Record_0[] = {
    {"fields", "rule", "*", false, true},
};
static const Spec3VariantSig gl_variants_Record[] = {
    {gl_fields_Record_0, sizeof(gl_fields_Record_0) / sizeof(gl_fields_Record_0[0])},
};

static const Spec3FieldSig gl_fields_Field_0[] = {
    {"name", "tok", "*", false, false},
    {"type", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Field[] = {
    {gl_fields_Field_0, sizeof(gl_fields_Field_0) / sizeof(gl_fields_Field_0[0])},
};

static const Spec3FieldSig gl_fields_FuncType_0[] = {
    {"params", "rule", "*", false, true},
    {"ret", "rule", "*", true, false},
};
static const Spec3VariantSig gl_variants_FuncType[] = {
    {gl_fields_FuncType_0, sizeof(gl_fields_FuncType_0) / sizeof(gl_fields_FuncType_0[0])},
};

// --------------------
// Statements / control flow
// --------------------

static const Spec3FieldSig gl_fields_If_0[] = {
    {"cond", "rule", "*", false, false},
    {"then", "rule", "*", false, false},
    {"else", "rule", "*", true, false},
};
static const Spec3VariantSig gl_variants_If[] = {
    {gl_fields_If_0, sizeof(gl_fields_If_0) / sizeof(gl_fields_If_0[0])},
};

static const Spec3FieldSig gl_fields_While_0[] = {
    {"cond", "rule", "*", false, false},
    {"body", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_While[] = {
    {gl_fields_While_0, sizeof(gl_fields_While_0) / sizeof(gl_fields_While_0[0])},
};

static const Spec3FieldSig gl_fields_Loop_0[] = {
    {"body", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Loop[] = {
    {gl_fields_Loop_0, sizeof(gl_fields_Loop_0) / sizeof(gl_fields_Loop_0[0])},
};

static const Spec3FieldSig gl_fields_DoWhile_0[] = {
    {"body", "rule", "*", false, false},
    {"cond", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_DoWhile[] = {
    {gl_fields_DoWhile_0, sizeof(gl_fields_DoWhile_0) / sizeof(gl_fields_DoWhile_0[0])},
};

static const Spec3FieldSig gl_fields_For_0[] = {
    {"init", "rule", "*", true, false},
    {"cond", "rule", "*", true, false},
    {"step", "rule", "*", true, false},
    {"body", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_For[] = {
    {gl_fields_For_0, sizeof(gl_fields_For_0) / sizeof(gl_fields_For_0[0])},
};

static const Spec3FieldSig gl_fields_Switch_0[] = {
    {"cond", "rule", "*", false, false},
    {"body", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Switch[] = {
    {gl_fields_Switch_0, sizeof(gl_fields_Switch_0) / sizeof(gl_fields_Switch_0[0])},
};

static const Spec3FieldSig gl_fields_Case_0[] = {
    {"value", "rule", "*", false, false},
    {"body", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Case[] = {
    {gl_fields_Case_0, sizeof(gl_fields_Case_0) / sizeof(gl_fields_Case_0[0])},
};

static const Spec3FieldSig gl_fields_Default_0[] = {
    {"body", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Default[] = {
    {gl_fields_Default_0, sizeof(gl_fields_Default_0) / sizeof(gl_fields_Default_0[0])},
};

static const Spec3FieldSig gl_fields_Break_0[] = {};
static const Spec3VariantSig gl_variants_Break[] = {
    {gl_fields_Break_0, 0},
};

static const Spec3FieldSig gl_fields_Continue_0[] = {};
static const Spec3VariantSig gl_variants_Continue[] = {
    {gl_fields_Continue_0, 0},
};

static const Spec3FieldSig gl_fields_Return_0[] = {
    {"value", "rule", "*", true, false},
};
static const Spec3VariantSig gl_variants_Return[] = {
    {gl_fields_Return_0, sizeof(gl_fields_Return_0) / sizeof(gl_fields_Return_0[0])},
};

static const Spec3FieldSig gl_fields_Goto_0[] = {
    {"label", "tok", "*", false, false},
};
static const Spec3VariantSig gl_variants_Goto[] = {
    {gl_fields_Goto_0, sizeof(gl_fields_Goto_0) / sizeof(gl_fields_Goto_0[0])},
};

static const Spec3FieldSig gl_fields_Label_0[] = {
    {"label", "tok", "*", false, false},
    {"body", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Label[] = {
    {gl_fields_Label_0, sizeof(gl_fields_Label_0) / sizeof(gl_fields_Label_0[0])},
};

static const Spec3FieldSig gl_fields_ExprStmt_0[] = {
    {"expr", "rule", "*", true, false},
};
static const Spec3VariantSig gl_variants_ExprStmt[] = {
    {gl_fields_ExprStmt_0, sizeof(gl_fields_ExprStmt_0) / sizeof(gl_fields_ExprStmt_0[0])},
};

static const Spec3FieldSig gl_fields_Match_0[] = {
    {"cond", "rule", "*", false, false},
    {"arms", "rule", "*", false, true},
};
static const Spec3VariantSig gl_variants_Match[] = {
    {gl_fields_Match_0, sizeof(gl_fields_Match_0) / sizeof(gl_fields_Match_0[0])},
};

static const Spec3FieldSig gl_fields_MatchArm_0[] = {
    {"pat", "rule", "*", false, false},
    {"guard", "rule", "*", true, false},
    {"body", "rule", "*", true, false},
};
static const Spec3VariantSig gl_variants_MatchArm[] = {
    {gl_fields_MatchArm_0, sizeof(gl_fields_MatchArm_0) / sizeof(gl_fields_MatchArm_0[0])},
};

static const Spec3FieldSig gl_fields_ForInitExpr_0[] = {
    {"expr", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_ForInitExpr[] = {
    {gl_fields_ForInitExpr_0, sizeof(gl_fields_ForInitExpr_0) / sizeof(gl_fields_ForInitExpr_0[0])},
};

// --------------------
// Expressions
// --------------------

static const Spec3FieldSig gl_fields_Name_0[] = {
    {"id", "tok", "*", false, false},
};
static const Spec3VariantSig gl_variants_Name[] = {
    {gl_fields_Name_0, sizeof(gl_fields_Name_0) / sizeof(gl_fields_Name_0[0])},
};

static const Spec3FieldSig gl_fields_Int_0[] = {
    {"lit", "tok", "*", false, false},
};
static const Spec3VariantSig gl_variants_Int[] = {
    {gl_fields_Int_0, sizeof(gl_fields_Int_0) / sizeof(gl_fields_Int_0[0])},
};

static const Spec3FieldSig gl_fields_String_0[] = {
    {"lit", "tok", "*", false, false},
};
static const Spec3VariantSig gl_variants_String[] = {
    {gl_fields_String_0, sizeof(gl_fields_String_0) / sizeof(gl_fields_String_0[0])},
};

static const Spec3FieldSig gl_fields_CStr_0[] = {
    {"lit", "tok", "*", false, false},
};
static const Spec3VariantSig gl_variants_CStr[] = {
    {gl_fields_CStr_0, sizeof(gl_fields_CStr_0) / sizeof(gl_fields_CStr_0[0])},
};

static const Spec3FieldSig gl_fields_UnitVal_0[] = {
};
static const Spec3VariantSig gl_variants_UnitVal[] = {
    {gl_fields_UnitVal_0, sizeof(gl_fields_UnitVal_0) / sizeof(gl_fields_UnitVal_0[0])},
};

static const Spec3FieldSig gl_fields_Tuple_0[] = {
    {"items", "rule", "*", true, true},
};
static const Spec3VariantSig gl_variants_Tuple[] = {
    {gl_fields_Tuple_0, sizeof(gl_fields_Tuple_0) / sizeof(gl_fields_Tuple_0[0])},
};

static const Spec3FieldSig gl_fields_ArrayLit_0[] = {
    {"items", "rule", "*", true, true},
};
static const Spec3VariantSig gl_variants_ArrayLit[] = {
    {gl_fields_ArrayLit_0, sizeof(gl_fields_ArrayLit_0) / sizeof(gl_fields_ArrayLit_0[0])},
};

static const Spec3FieldSig gl_fields_ArrayRepeat_0[] = {
    {"elem", "rule", "*", false, false},
    {"len", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_ArrayRepeat[] = {
    {gl_fields_ArrayRepeat_0, sizeof(gl_fields_ArrayRepeat_0) / sizeof(gl_fields_ArrayRepeat_0[0])},
};

// Core string literal builtin: CSTR("...") should lower explicitly to ToCStr(lit).
// (Avoid pack meta toggles / Stage 4 token-text sniffing.)
static const Spec3FieldSig gl_fields_ToCStr_0[] = {
    {"lit", "tok", "*", false, false},
};
static const Spec3VariantSig gl_variants_ToCStr[] = {
    {gl_fields_ToCStr_0, sizeof(gl_fields_ToCStr_0) / sizeof(gl_fields_ToCStr_0[0])},
};

static const Spec3FieldSig gl_fields_Bytes_0[] = {
    {"lit", "tok", "*", false, false},
};
static const Spec3VariantSig gl_variants_Bytes[] = {
    {gl_fields_Bytes_0, sizeof(gl_fields_Bytes_0) / sizeof(gl_fields_Bytes_0[0])},
};

// Surface builtin: ADR(x) => AddrOf(expr=x)
static const Spec3FieldSig gl_fields_AddrOf_0[] = {
    {"expr", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_AddrOf[] = {
    {gl_fields_AddrOf_0, sizeof(gl_fields_AddrOf_0) / sizeof(gl_fields_AddrOf_0[0])},
};

// Surface builtin: LEN(x) => Len(expr=x)
// Prefer an explicit intrinsic over desugaring to Member(Deref(x), "len") with invented tokens.
static const Spec3FieldSig gl_fields_Len_0[] = {
    {"expr", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Len[] = {
    {gl_fields_Len_0, sizeof(gl_fields_Len_0) / sizeof(gl_fields_Len_0[0])},
};

// Explicit unary intrinsics (pack-driven; avoid Stage 4 token-text heuristics).
static const Spec3FieldSig gl_fields_Not_0[] = {
    {"expr", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Not[] = {
    {gl_fields_Not_0, sizeof(gl_fields_Not_0) / sizeof(gl_fields_Not_0[0])},
};

static const Spec3FieldSig gl_fields_BitNot_0[] = {
    {"expr", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_BitNot[] = {
    {gl_fields_BitNot_0, sizeof(gl_fields_BitNot_0) / sizeof(gl_fields_BitNot_0[0])},
};

static const Spec3FieldSig gl_fields_UPlus_0[] = {
    {"expr", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_UPlus[] = {
    {gl_fields_UPlus_0, sizeof(gl_fields_UPlus_0) / sizeof(gl_fields_UPlus_0[0])},
};

static const Spec3FieldSig gl_fields_Neg_0[] = {
    {"expr", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Neg[] = {
    {gl_fields_Neg_0, sizeof(gl_fields_Neg_0) / sizeof(gl_fields_Neg_0[0])},
};

static const Spec3FieldSig gl_fields_Deref_0[] = {
    {"expr", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Deref[] = {
    {gl_fields_Deref_0, sizeof(gl_fields_Deref_0) / sizeof(gl_fields_Deref_0[0])},
};

static const Spec3FieldSig gl_fields_Real_0[] = {
    {"lit", "tok", "*", false, false},
};
static const Spec3VariantSig gl_variants_Real[] = {
    {gl_fields_Real_0, sizeof(gl_fields_Real_0) / sizeof(gl_fields_Real_0[0])},
};

static const Spec3FieldSig gl_fields_Char_0[] = {
    {"lit", "tok", "*", false, false},
};
static const Spec3VariantSig gl_variants_Char[] = {
    {gl_fields_Char_0, sizeof(gl_fields_Char_0) / sizeof(gl_fields_Char_0[0])},
};

static const Spec3FieldSig gl_fields_True_0[] = {};
static const Spec3VariantSig gl_variants_True[] = {
    {gl_fields_True_0, 0},
};

static const Spec3FieldSig gl_fields_False_0[] = {};
static const Spec3VariantSig gl_variants_False[] = {
    {gl_fields_False_0, 0},
};

static const Spec3FieldSig gl_fields_Nil_0[] = {};
static const Spec3VariantSig gl_variants_Nil[] = {
    {gl_fields_Nil_0, 0},
};

static const Spec3FieldSig gl_fields_PatWild_0[] = {};
static const Spec3VariantSig gl_variants_PatWild[] = {
    {gl_fields_PatWild_0, 0},
};

static const Spec3FieldSig gl_fields_PatBind_0[] = {
    {"name", "tok", "*", false, false},
};
static const Spec3VariantSig gl_variants_PatBind[] = {
    {gl_fields_PatBind_0, sizeof(gl_fields_PatBind_0) / sizeof(gl_fields_PatBind_0[0])},
};

static const Spec3FieldSig gl_fields_PatInt_0[] = {
    {"lit", "tok", "*", false, false},
};
static const Spec3VariantSig gl_variants_PatInt[] = {
    {gl_fields_PatInt_0, sizeof(gl_fields_PatInt_0) / sizeof(gl_fields_PatInt_0[0])},
};

static const Spec3FieldSig gl_fields_PatTuple_0[] = {
    {"items", "rule", "*", false, true},
};
static const Spec3VariantSig gl_variants_PatTuple[] = {
    {gl_fields_PatTuple_0, sizeof(gl_fields_PatTuple_0) / sizeof(gl_fields_PatTuple_0[0])},
};

static const Spec3FieldSig gl_fields_PatStruct_0[] = {
    {"name", "tok", "*", false, false},
    {"fields", "rule", "*", false, true},
    {"rest", "tok", "*", true, false},
};
static const Spec3VariantSig gl_variants_PatStruct[] = {
    {gl_fields_PatStruct_0, sizeof(gl_fields_PatStruct_0) / sizeof(gl_fields_PatStruct_0[0])},
};

static const Spec3FieldSig gl_fields_PatStructField_0[] = {
    {"name", "tok", "*", false, false},
    {"pat", "rule", "*", true, false},
};
static const Spec3VariantSig gl_variants_PatStructField[] = {
    {gl_fields_PatStructField_0, sizeof(gl_fields_PatStructField_0) / sizeof(gl_fields_PatStructField_0[0])},
};

static const Spec3FieldSig gl_fields_PatCtor_0[] = {
    {"name", "tok", "*", false, false},
    {"args", "rule", "*", true, false},
};
static const Spec3VariantSig gl_variants_PatCtor[] = {
    {gl_fields_PatCtor_0, sizeof(gl_fields_PatCtor_0) / sizeof(gl_fields_PatCtor_0[0])},
};

static const Spec3FieldSig gl_fields_PatArgs_0[] = {
    {"items", "rule", "*", false, true},
};
static const Spec3VariantSig gl_variants_PatArgs[] = {
    {gl_fields_PatArgs_0, sizeof(gl_fields_PatArgs_0) / sizeof(gl_fields_PatArgs_0[0])},
};

// --------------------
// Operators
// --------------------

// --------------------
// Surface parsing helpers (must fold away)
// --------------------

static const Spec3FieldSig gl_fields_BinChain_0[] = {
    {"head", "rule", "*", false, false},
    {"rest", "rule", "*", false, true},
};
static const Spec3VariantSig gl_variants_BinChain[] = {
    {gl_fields_BinChain_0, sizeof(gl_fields_BinChain_0) / sizeof(gl_fields_BinChain_0[0])},
};

static const Spec3FieldSig gl_fields_BinTail_0[] = {
    {"op", "tok", "*", false, false},
    {"rhs", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_BinTail[] = {
    {gl_fields_BinTail_0, sizeof(gl_fields_BinTail_0) / sizeof(gl_fields_BinTail_0[0])},
};

static const Spec3FieldSig gl_fields_Paren_0[] = {
    {"expr", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Paren[] = {
    {gl_fields_Paren_0, sizeof(gl_fields_Paren_0) / sizeof(gl_fields_Paren_0[0])},
};

static const Spec3FieldSig gl_fields_Unary_0[] = {
    {"op", "tok", "*", false, false},
    {"op_tok", "tok", "*", true, false},
    {"expr", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Unary[] = {
    {gl_fields_Unary_0, sizeof(gl_fields_Unary_0) / sizeof(gl_fields_Unary_0[0])},
};

static const Spec3FieldSig gl_fields_Bin_0[] = {
    {"op", "tok", "*", false, false},
    {"op_tok", "tok", "*", true, false},
    {"lhs", "rule", "*", false, false},
    {"rhs", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Bin[] = {
    {gl_fields_Bin_0, sizeof(gl_fields_Bin_0) / sizeof(gl_fields_Bin_0[0])},
};

static const Spec3FieldSig gl_fields_Assign_0[] = {
    {"op", "tok", "*", false, false},
    {"op_tok", "tok", "*", true, false},
    {"lhs", "rule", "*", false, false},
    {"rhs", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Assign[] = {
    {gl_fields_Assign_0, sizeof(gl_fields_Assign_0) / sizeof(gl_fields_Assign_0[0])},
};

static const Spec3FieldSig gl_fields_Cond_0[] = {
    {"cond", "rule", "*", false, false},
    {"then", "rule", "*", false, false},
    {"els", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Cond[] = {
    {gl_fields_Cond_0, sizeof(gl_fields_Cond_0) / sizeof(gl_fields_Cond_0[0])},
};

static const Spec3FieldSig gl_fields_Comma_0[] = {
    {"items", "rule", "*", false, true},
};
static const Spec3VariantSig gl_variants_Comma[] = {
    {gl_fields_Comma_0, sizeof(gl_fields_Comma_0) / sizeof(gl_fields_Comma_0[0])},
};

// --------------------
// Postfix / access / calls
// --------------------

// Structural helper: represents chained selectors as base + suffix list.
// This is intentionally permissive (wildcard types) so surface grammars don't
// have to share nonterminal/token names.
static const Spec3FieldSig gl_fields_Postfix_0[] = {
    {"base", "rule", "*", false, false},
    {"suffixes", "rule", "*", true, true},
};
static const Spec3VariantSig gl_variants_Postfix[] = {
    {gl_fields_Postfix_0, sizeof(gl_fields_Postfix_0) / sizeof(gl_fields_Postfix_0[0])},
};

// Structural helpers: individual selector/call suffixes.
// These are folded away during Stage 4 lowering.
static const Spec3FieldSig gl_fields_FieldSuffix_0[] = {
    {"name", "tok", "*", false, false},
};
static const Spec3VariantSig gl_variants_FieldSuffix[] = {
    {gl_fields_FieldSuffix_0, sizeof(gl_fields_FieldSuffix_0) / sizeof(gl_fields_FieldSuffix_0[0])},
};

static const Spec3FieldSig gl_fields_IndexSuffix_0[] = {
    {"index", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_IndexSuffix[] = {
    {gl_fields_IndexSuffix_0, sizeof(gl_fields_IndexSuffix_0) / sizeof(gl_fields_IndexSuffix_0[0])},
};

static const Spec3FieldSig gl_fields_CallSuffix_0[] = {
    {"args", "rule", "*", true, false},
};
static const Spec3VariantSig gl_variants_CallSuffix[] = {
    {gl_fields_CallSuffix_0, sizeof(gl_fields_CallSuffix_0) / sizeof(gl_fields_CallSuffix_0[0])},
};

// Surface parsing helper: represents a deref selector suffix (e.g. Oberon `p^`).
// Folded away during Stage 4 lowering into canonical `Deref(expr)`.
static const Spec3FieldSig gl_fields_DerefSuffix_0[] = {};
static const Spec3VariantSig gl_variants_DerefSuffix[] = {
    {gl_fields_DerefSuffix_0, 0},
};

static const Spec3FieldSig gl_fields_Call_0[] = {
    {"callee", "rule", "*", false, false},
    {"args", "rule", "*", true, false},
};
static const Spec3VariantSig gl_variants_Call[] = {
    {gl_fields_Call_0, sizeof(gl_fields_Call_0) / sizeof(gl_fields_Call_0[0])},
};

static const Spec3FieldSig gl_fields_Index_0[] = {
    {"base", "rule", "*", false, false},
    {"index", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Index[] = {
    {gl_fields_Index_0, sizeof(gl_fields_Index_0) / sizeof(gl_fields_Index_0[0])},
};

static const Spec3FieldSig gl_fields_Member_0[] = {
    {"base", "rule", "*", false, false},
    {"name", "tok", "*", false, false},
};
static const Spec3VariantSig gl_variants_Member[] = {
    {gl_fields_Member_0, sizeof(gl_fields_Member_0) / sizeof(gl_fields_Member_0[0])},
};

static const Spec3FieldSig gl_fields_PtrMember_0[] = {
    {"base", "rule", "*", false, false},
    {"name", "tok", "*", false, false},
};
static const Spec3VariantSig gl_variants_PtrMember[] = {
    {gl_fields_PtrMember_0, sizeof(gl_fields_PtrMember_0) / sizeof(gl_fields_PtrMember_0[0])},
};

static const Spec3FieldSig gl_fields_Cast_0[] = {
    {"type", "rule", "*", false, false},
    {"expr", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_Cast[] = {
    {gl_fields_Cast_0, sizeof(gl_fields_Cast_0) / sizeof(gl_fields_Cast_0[0])},
};

static const Spec3FieldSig gl_fields_SizeofExpr_0[] = {
    {"expr", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_SizeofExpr[] = {
    {gl_fields_SizeofExpr_0, sizeof(gl_fields_SizeofExpr_0) / sizeof(gl_fields_SizeofExpr_0[0])},
};

static const Spec3FieldSig gl_fields_SizeofType_0[] = {
    {"type", "rule", "*", false, false},
};
static const Spec3VariantSig gl_variants_SizeofType[] = {
    {gl_fields_SizeofType_0, sizeof(gl_fields_SizeofType_0) / sizeof(gl_fields_SizeofType_0[0])},
};

// --------------------
// Registry
// --------------------

const Spec3IntrinsicSig GL_SIG[] = {
    {"Array", gl_variants_Array, sizeof(gl_variants_Array) / sizeof(gl_variants_Array[0])},
    {"ArrayLit", gl_variants_ArrayLit, sizeof(gl_variants_ArrayLit) / sizeof(gl_variants_ArrayLit[0])},
    {"ArrayRepeat", gl_variants_ArrayRepeat, sizeof(gl_variants_ArrayRepeat) / sizeof(gl_variants_ArrayRepeat[0])},
    {"Args", gl_variants_Args, sizeof(gl_variants_Args) / sizeof(gl_variants_Args[0])},
    {"AddrOf", gl_variants_AddrOf, sizeof(gl_variants_AddrOf) / sizeof(gl_variants_AddrOf[0])},
    {"Assign", gl_variants_Assign, sizeof(gl_variants_Assign) / sizeof(gl_variants_Assign[0])},
    {"Bin", gl_variants_Bin, sizeof(gl_variants_Bin) / sizeof(gl_variants_Bin[0])},
    {"BinChain", gl_variants_BinChain, sizeof(gl_variants_BinChain) / sizeof(gl_variants_BinChain[0])},
    {"BinTail", gl_variants_BinTail, sizeof(gl_variants_BinTail) / sizeof(gl_variants_BinTail[0])},
    {"Block", gl_variants_Block, sizeof(gl_variants_Block) / sizeof(gl_variants_Block[0])},
    {"Break", gl_variants_Break, sizeof(gl_variants_Break) / sizeof(gl_variants_Break[0])},
    {"Bytes", gl_variants_Bytes, sizeof(gl_variants_Bytes) / sizeof(gl_variants_Bytes[0])},
    {"Call", gl_variants_Call, sizeof(gl_variants_Call) / sizeof(gl_variants_Call[0])},
    {"Case", gl_variants_Case, sizeof(gl_variants_Case) / sizeof(gl_variants_Case[0])},
    {"Cast", gl_variants_Cast, sizeof(gl_variants_Cast) / sizeof(gl_variants_Cast[0])},
    {"Char", gl_variants_Char, sizeof(gl_variants_Char) / sizeof(gl_variants_Char[0])},
    {"Const", gl_variants_Const, sizeof(gl_variants_Const) / sizeof(gl_variants_Const[0])},
    {"Comma", gl_variants_Comma, sizeof(gl_variants_Comma) / sizeof(gl_variants_Comma[0])},
    {"Cond", gl_variants_Cond, sizeof(gl_variants_Cond) / sizeof(gl_variants_Cond[0])},
    {"Continue", gl_variants_Continue, sizeof(gl_variants_Continue) / sizeof(gl_variants_Continue[0])},
    {"CStr", gl_variants_CStr, sizeof(gl_variants_CStr) / sizeof(gl_variants_CStr[0])},
    {"Default", gl_variants_Default, sizeof(gl_variants_Default) / sizeof(gl_variants_Default[0])},
    {"DoWhile", gl_variants_DoWhile, sizeof(gl_variants_DoWhile) / sizeof(gl_variants_DoWhile[0])},
    {"ExprStmt", gl_variants_ExprStmt, sizeof(gl_variants_ExprStmt) / sizeof(gl_variants_ExprStmt[0])},
    {"ForInitExpr", gl_variants_ForInitExpr, sizeof(gl_variants_ForInitExpr) / sizeof(gl_variants_ForInitExpr[0])},
    {"False", gl_variants_False, sizeof(gl_variants_False) / sizeof(gl_variants_False[0])},
    {"Field", gl_variants_Field, sizeof(gl_variants_Field) / sizeof(gl_variants_Field[0])},
    {"For", gl_variants_For, sizeof(gl_variants_For) / sizeof(gl_variants_For[0])},
    {"FuncType", gl_variants_FuncType, sizeof(gl_variants_FuncType) / sizeof(gl_variants_FuncType[0])},
    {"Goto", gl_variants_Goto, sizeof(gl_variants_Goto) / sizeof(gl_variants_Goto[0])},
    {"If", gl_variants_If, sizeof(gl_variants_If) / sizeof(gl_variants_If[0])},
    {"Index", gl_variants_Index, sizeof(gl_variants_Index) / sizeof(gl_variants_Index[0])},
    {"Import", gl_variants_Import, sizeof(gl_variants_Import) / sizeof(gl_variants_Import[0])},
    {"Int", gl_variants_Int, sizeof(gl_variants_Int) / sizeof(gl_variants_Int[0])},
    {"Label", gl_variants_Label, sizeof(gl_variants_Label) / sizeof(gl_variants_Label[0])},
    {"Len", gl_variants_Len, sizeof(gl_variants_Len) / sizeof(gl_variants_Len[0])},
    {"Loop", gl_variants_Loop, sizeof(gl_variants_Loop) / sizeof(gl_variants_Loop[0])},
    {"Match", gl_variants_Match, sizeof(gl_variants_Match) / sizeof(gl_variants_Match[0])},
    {"MatchArm", gl_variants_MatchArm, sizeof(gl_variants_MatchArm) / sizeof(gl_variants_MatchArm[0])},
    {"Not", gl_variants_Not, sizeof(gl_variants_Not) / sizeof(gl_variants_Not[0])},
    {"BitNot", gl_variants_BitNot, sizeof(gl_variants_BitNot) / sizeof(gl_variants_BitNot[0])},
    {"UPlus", gl_variants_UPlus, sizeof(gl_variants_UPlus) / sizeof(gl_variants_UPlus[0])},
    {"Neg", gl_variants_Neg, sizeof(gl_variants_Neg) / sizeof(gl_variants_Neg[0])},
    {"Member", gl_variants_Member, sizeof(gl_variants_Member) / sizeof(gl_variants_Member[0])},
    {"Deref", gl_variants_Deref, sizeof(gl_variants_Deref) / sizeof(gl_variants_Deref[0])},
    {"Name", gl_variants_Name, sizeof(gl_variants_Name) / sizeof(gl_variants_Name[0])},
    {"Nil", gl_variants_Nil, sizeof(gl_variants_Nil) / sizeof(gl_variants_Nil[0])},
    {"Param", gl_variants_Param, sizeof(gl_variants_Param) / sizeof(gl_variants_Param[0])},
    {"ParamPat", gl_variants_ParamPat, sizeof(gl_variants_ParamPat) / sizeof(gl_variants_ParamPat[0])},
    {"Paren", gl_variants_Paren, sizeof(gl_variants_Paren) / sizeof(gl_variants_Paren[0])},
    {"PatArgs", gl_variants_PatArgs, sizeof(gl_variants_PatArgs) / sizeof(gl_variants_PatArgs[0])},
    {"PatBind", gl_variants_PatBind, sizeof(gl_variants_PatBind) / sizeof(gl_variants_PatBind[0])},
    {"PatCtor", gl_variants_PatCtor, sizeof(gl_variants_PatCtor) / sizeof(gl_variants_PatCtor[0])},
    {"PatInt", gl_variants_PatInt, sizeof(gl_variants_PatInt) / sizeof(gl_variants_PatInt[0])},
    {"PatStruct", gl_variants_PatStruct, sizeof(gl_variants_PatStruct) / sizeof(gl_variants_PatStruct[0])},
    {"PatStructField", gl_variants_PatStructField, sizeof(gl_variants_PatStructField) / sizeof(gl_variants_PatStructField[0])},
    {"PatTuple", gl_variants_PatTuple, sizeof(gl_variants_PatTuple) / sizeof(gl_variants_PatTuple[0])},
    {"PatWild", gl_variants_PatWild, sizeof(gl_variants_PatWild) / sizeof(gl_variants_PatWild[0])},
    {"CallSuffix", gl_variants_CallSuffix, sizeof(gl_variants_CallSuffix) / sizeof(gl_variants_CallSuffix[0])},
    {"DerefSuffix", gl_variants_DerefSuffix, sizeof(gl_variants_DerefSuffix) / sizeof(gl_variants_DerefSuffix[0])},
    {"FieldSuffix", gl_variants_FieldSuffix, sizeof(gl_variants_FieldSuffix) / sizeof(gl_variants_FieldSuffix[0])},
    {"IndexSuffix", gl_variants_IndexSuffix, sizeof(gl_variants_IndexSuffix) / sizeof(gl_variants_IndexSuffix[0])},
    {"Postfix", gl_variants_Postfix, sizeof(gl_variants_Postfix) / sizeof(gl_variants_Postfix[0])},
    {"Proc", gl_variants_Proc, sizeof(gl_variants_Proc) / sizeof(gl_variants_Proc[0])},
    {"ExternProc", gl_variants_ExternProc, sizeof(gl_variants_ExternProc) / sizeof(gl_variants_ExternProc[0])},
    {"Ptr", gl_variants_Ptr, sizeof(gl_variants_Ptr) / sizeof(gl_variants_Ptr[0])},
    {"PtrMember", gl_variants_PtrMember, sizeof(gl_variants_PtrMember) / sizeof(gl_variants_PtrMember[0])},
    {"Real", gl_variants_Real, sizeof(gl_variants_Real) / sizeof(gl_variants_Real[0])},
    {"Record", gl_variants_Record, sizeof(gl_variants_Record) / sizeof(gl_variants_Record[0])},
    {"Return", gl_variants_Return, sizeof(gl_variants_Return) / sizeof(gl_variants_Return[0])},
    {"SizeofExpr", gl_variants_SizeofExpr, sizeof(gl_variants_SizeofExpr) / sizeof(gl_variants_SizeofExpr[0])},
    {"SizeofType", gl_variants_SizeofType, sizeof(gl_variants_SizeofType) / sizeof(gl_variants_SizeofType[0])},
    {"String", gl_variants_String, sizeof(gl_variants_String) / sizeof(gl_variants_String[0])},
    {"Switch", gl_variants_Switch, sizeof(gl_variants_Switch) / sizeof(gl_variants_Switch[0])},
    {"ToCStr", gl_variants_ToCStr, sizeof(gl_variants_ToCStr) / sizeof(gl_variants_ToCStr[0])},
    {"True", gl_variants_True, sizeof(gl_variants_True) / sizeof(gl_variants_True[0])},
    {"Tuple", gl_variants_Tuple, sizeof(gl_variants_Tuple) / sizeof(gl_variants_Tuple[0])},
    {"TypeDecl", gl_variants_TypeDecl, sizeof(gl_variants_TypeDecl) / sizeof(gl_variants_TypeDecl[0])},
    {"TypeRef", gl_variants_TypeRef, sizeof(gl_variants_TypeRef) / sizeof(gl_variants_TypeRef[0])},
    {"Unary", gl_variants_Unary, sizeof(gl_variants_Unary) / sizeof(gl_variants_Unary[0])},
    {"Unit", gl_variants_Unit, sizeof(gl_variants_Unit) / sizeof(gl_variants_Unit[0])},
    {"UnitVal", gl_variants_UnitVal, sizeof(gl_variants_UnitVal) / sizeof(gl_variants_UnitVal[0])},
    {"Var", gl_variants_Var, sizeof(gl_variants_Var) / sizeof(gl_variants_Var[0])},
    {"VarPat", gl_variants_VarPat, sizeof(gl_variants_VarPat) / sizeof(gl_variants_VarPat[0])},
    {"VarGroup", gl_variants_VarGroup, sizeof(gl_variants_VarGroup) / sizeof(gl_variants_VarGroup[0])},
    {"While", gl_variants_While, sizeof(gl_variants_While) / sizeof(gl_variants_While[0])},
};

const size_t GL_SIG_COUNT = sizeof(GL_SIG) / sizeof(GL_SIG[0]);
