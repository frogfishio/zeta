%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sirc_emit.h"

int yylex(void);
extern int yylineno;
extern int yycolumn;
void yyerror(const char* s);
%}

%union {
  char*         s;
  long long     i;
  double        d;
  int           b;
  int64_t       ty;
  SircTypeList* tys;
  SircSumVariantList* variants;
  SircParamList* params;
  SircNodeList*  stmts;
  SircExprList*  args;
  SircSwitchCaseList* cases;
  SircAttrList* attrs;
  int64_t       node;
}

/* tokens with values */
%token <s>   T_ID T_STRING
%token <i>   T_INT
%token <d>   T_FLOAT
%token <b>   T_BOOL

/* keywords / structural */
%token T_NL T_ARROW
%token T_UNIT T_TARGET
%token T_FN T_PUBLIC T_EXTERN T_END T_RETURN T_LET
%token T_AS
%token T_SELECT
%token T_ARRAY
%token T_PTR_SIZEOF T_PTR_ALIGNOF T_PTR_OFFSET
%token T_TERM_BR T_TERM_CBR T_TERM_SWITCH T_TERM_RET
%token T_TERM_UNREACHABLE T_TERM_TRAP
%token T_SCRUT T_CASES T_DEFAULT T_LIT
%token T_FLAGS T_COUNT

/* declared in lexer (unused for now, but must exist) */
%token T_FEATURES T_SIG T_DO
%token T_TYPE T_CONST T_GLOBAL
%token T_BLOCK T_TERM T_TO T_ARGS T_COND T_THEN T_ELSE T_VALUE

%destructor { free($$); } T_ID T_STRING
%destructor { sirc_attrs_free($$); } <attrs>

%type <ty> type type_ctor
%type <tys> type_list_opt type_list
%type <variants> sum_variants_opt sum_variants sum_variant
%type <params> params_opt params
%type <params> bparams_opt bparams
%type <stmts> stmt_list
%type <stmts> cfg_blocks
%type <node> stmt let_stmt return_stmt expr_stmt select_expr ptr_sizeof_expr ptr_alignof_expr ptr_offset_expr
%type <node> term_stmt term_br_stmt term_cbr_stmt term_switch_stmt term_ret_stmt
%type <node> block_decl
%type <args> args_opt args
%type <args> term_args_opt
%type <cases> switch_cases_opt switch_cases
%type <node> expr value int_lit float_lit dotted_or_call
%type <s> dotted_name
%type <attrs> attr_tail_opt attr_tail attr_item flags_list flags_id_list
%type <i> attr_int
%type <b> attr_bool
%type <s> attr_ident

%start program

%%

program
  : unit_header nl_star decls_opt nl_star
  ;

unit_header
  : T_UNIT T_ID T_TARGET T_ID features_opt
    { sirc_emit_unit($2, $4); }
  ;

features_opt
  : /* empty */
  | feature_list
  ;

feature_list
  : feature
  | feature_list feature
  ;

feature
  : '+' T_ID                 { sirc_add_feature($2); }
  | '+' T_ID ':' T_ID        { sirc_add_feature(sirc_colon_join($2, $4)); }
  ;

decls_opt
  : /* empty */
  | decls
  ;

decls
  : decl decls_rest
  ;

decl
  : fn_decl
  | extern_fn_decl
  | type_decl
  ;

type_decl
  : T_TYPE T_ID '=' type_ctor
    { sirc_type_alias($2, $4); }
  ;

decls_rest
  : /* empty */
  | nl_plus decl decls_rest
  | nl_plus
  ;

extern_fn_decl
  : T_EXTERN T_FN T_ID '(' params_opt ')' T_ARROW type
    { sirc_extern_fn($3, $5, $8); }
  ;

fn_decl
  : T_FN T_ID '(' params_opt ')' T_ARROW type public_opt nl_star stmt_list T_END
    { sirc_fn_def($2, $4, $7, $10); }
  | T_FN T_ID '(' params_opt ')' T_ARROW type public_opt nl_star
      { sirc_cfg_begin(); }
    cfg_blocks T_END
      { sirc_fn_def_cfg($2, $4, $7, sirc_nodelist_first($11), $11); }
  ;

public_opt
  : /* empty */
  | T_PUBLIC
  ;

params_opt
  : /* empty */              { $$ = sirc_params_empty(); }
  | params                   { $$ = $1; }
  ;

params
  : T_ID ':' type                        { $$ = sirc_params_single($1, $3); }
  | params comma_sep T_ID ':' type       { $$ = sirc_params_append($1, $3, $5); }
  ;

stmt_list
  : stmt                     { $$ = sirc_stmtlist_single($1); }
  | stmt_list stmt           { $$ = sirc_stmtlist_append($1, $2); }
  ;

stmt
  : let_stmt nl_star          { $$ = $1; }
  | return_stmt nl_star       { $$ = $1; }
  | expr_stmt nl_star         { $$ = $1; }
  | term_stmt nl_star         { $$ = $1; }
  ;

term_stmt
  : term_br_stmt              { $$ = $1; }
  | term_cbr_stmt             { $$ = $1; }
  | term_switch_stmt          { $$ = $1; }
  | term_ret_stmt             { $$ = $1; }
  | T_TERM_UNREACHABLE attr_tail_opt { $$ = sirc_term_unreachable($2); }
  | T_TERM_TRAP attr_tail_opt        { $$ = sirc_term_trap($2); }
  ;

term_args_opt
  : /* empty */               { $$ = NULL; }
  | nl_star T_ARGS ':' '[' nl_star args_opt nl_star ']'
                              { $$ = $6; }
  ;

term_br_stmt
  : T_TERM_BR T_TO T_ID term_args_opt
    { $$ = sirc_term_br($3, $4); }
  ;

term_cbr_stmt
  : T_TERM_CBR T_COND ':' expr ',' nl_star T_THEN ':' T_ID term_args_opt ',' nl_star T_ELSE ':' T_ID term_args_opt
    { $$ = sirc_term_cbr($4, $9, $10, $15, $16); }
  ;

switch_cases_opt
  : /* empty */               { $$ = sirc_cases_empty(); }
  | switch_cases              { $$ = $1; }
  ;

switch_cases
  : '{' nl_star T_LIT ':' int_lit ',' nl_star T_TO T_ID nl_star '}'
    { $$ = sirc_cases_append(sirc_cases_empty(), $5, $9); }
  | switch_cases ',' nl_star '{' nl_star T_LIT ':' int_lit ',' nl_star T_TO T_ID nl_star '}'
    { $$ = sirc_cases_append($1, $8, $12); }
  ;

term_switch_stmt
  : T_TERM_SWITCH T_SCRUT ':' expr ',' nl_star T_CASES ':' '[' nl_star switch_cases_opt nl_star ']' ',' nl_star T_DEFAULT ':' T_ID
    { $$ = sirc_term_switch($4, $11, $18); }
  ;

term_ret_stmt
  : T_TERM_RET
    { $$ = sirc_term_ret_opt(0, 0); }
  | T_TERM_RET T_VALUE ':' expr
    { $$ = sirc_term_ret_opt(1, $4); }
  ;

cfg_blocks
  : block_decl                { $$ = sirc_stmtlist_single($1); }
  | cfg_blocks nl_star block_decl
                              { $$ = sirc_stmtlist_append($1, $3); }
  ;

bparams_opt
  : /* empty */               { $$ = sirc_bparams_empty(); }
  | '(' bparams ')'           { $$ = $2; }
  ;

bparams
  : T_ID ':' type                        { $$ = sirc_bparams_single($1, $3); }
  | bparams comma_sep T_ID ':' type      { $$ = sirc_bparams_append($1, $3, $5); }
  ;

block_decl
  : T_BLOCK T_ID bparams_opt nl_star stmt_list T_END nl_star
    { $$ = sirc_block_def($2, $3, $5); }
  ;

let_stmt
  : T_LET T_ID ':' type '=' expr
    { $$ = sirc_stmt_let($2, $4, $6); }
  ;

return_stmt
  : T_RETURN expr
    { $$ = sirc_stmt_return($2); }
  ;

expr_stmt
  : expr                     { $$ = $1; }
  ;

expr
  : value                    { $$ = $1; }
  | select_expr              { $$ = $1; }
  | ptr_sizeof_expr           { $$ = $1; }
  | ptr_alignof_expr          { $$ = $1; }
  | ptr_offset_expr           { $$ = $1; }
  ;

select_expr
  : T_SELECT '(' nl_star type comma_sep expr comma_sep expr comma_sep expr nl_star ')'
    { $$ = sirc_select($4, $6, $8, $10); }
  ;

ptr_sizeof_expr
  : T_PTR_SIZEOF '(' nl_star type nl_star ')'
    { $$ = sirc_ptr_sizeof($4); }
  ;

ptr_alignof_expr
  : T_PTR_ALIGNOF '(' nl_star type nl_star ')'
    { $$ = sirc_ptr_alignof($4); }
  ;

ptr_offset_expr
  : T_PTR_OFFSET '(' nl_star type comma_sep expr comma_sep expr nl_star ')'
    { $$ = sirc_ptr_offset($4, $6, $8); }
  ;

dotted_name
  : T_ID                     { $$ = $1; }
  | dotted_name '.' T_ID     { $$ = sirc_dotted_join($1, $3); }
  ;

comma_sep
  : ',' nl_star
  ;

args_opt
  : /* empty */              { $$ = sirc_args_empty(); }
  | args                     { $$ = $1; }
  ;

args
  : expr                     { $$ = sirc_args_single($1); }
  | args comma_sep expr      { $$ = sirc_args_append($1, $3); }
  ;

value
  : int_lit                  { $$ = $1; }
  | float_lit                { $$ = $1; }
  | T_BOOL                   { $$ = sirc_value_bool($1); }
  | T_STRING                 { $$ = sirc_value_string($1); }
  | dotted_or_call           { $$ = $1; }
  ;

int_lit
  : T_INT                    { $$ = sirc_value_int($1); }
  | T_INT ':' type           { $$ = sirc_typed_int($1, $3); }
  ;

float_lit
  : T_FLOAT                  { $$ = sirc_value_float($1); }
  | T_FLOAT ':' type         { $$ = sirc_typed_float($1, $3); }
  ;

dotted_or_call
  : dotted_name                                { $$ = sirc_value_ident($1); }
  | dotted_name '(' nl_star args_opt nl_star ')' attr_tail_opt { $$ = sirc_call($1, $4, $7); }
  | dotted_name '(' nl_star args_opt nl_star ')' attr_tail_opt T_AS type { $$ = sirc_call_typed($1, $4, $7, $9); }
  ;

attr_tail_opt
  : /* empty */                 { $$ = NULL; }
  | attr_tail                   { $$ = $1; }
  ;

attr_tail
  : attr_item                   { $$ = $1; }
  | attr_tail attr_item         { $$ = sirc_attrs_merge($1, $2); }
  ;

attr_item
  : '+' T_ID                    { $$ = sirc_attrs_add_flag(sirc_attrs_empty(), $2); }
  | flags_list                  { $$ = $1; }
  | T_FLAGS T_ID attr_int       { $$ = sirc_attrs_add_flags_scalar_int(sirc_attrs_empty(), $2, $3); }
  | T_FLAGS T_ID T_STRING       { $$ = sirc_attrs_add_flags_scalar_str(sirc_attrs_empty(), $2, $3); }
  | T_FLAGS T_ID attr_bool      { $$ = sirc_attrs_add_flags_scalar_bool(sirc_attrs_empty(), $2, $3); }
  | T_FLAGS T_ID attr_ident     { $$ = sirc_attrs_add_flags_scalar_str(sirc_attrs_empty(), $2, $3); }
  | T_ID attr_int               { $$ = sirc_attrs_add_field_scalar_int(sirc_attrs_empty(), $1, $2); }
  | T_ID T_STRING               { $$ = sirc_attrs_add_field_scalar_str(sirc_attrs_empty(), $1, $2); }
  | T_ID attr_bool              { $$ = sirc_attrs_add_field_scalar_bool(sirc_attrs_empty(), $1, $2); }
  | T_ID attr_ident             { $$ = sirc_attrs_add_field_scalar_str(sirc_attrs_empty(), $1, $2); }
  | T_SIG T_ID                  { $$ = sirc_attrs_add_sig(sirc_attrs_empty(), $2); }
  | T_COUNT expr                { $$ = sirc_attrs_add_count(sirc_attrs_empty(), $2); }
  ;

flags_list
  : T_FLAGS '[' nl_star flags_id_list nl_star ']'
                                { $$ = $4; }
  ;

flags_id_list
  : T_ID                        { $$ = sirc_attrs_add_flag(sirc_attrs_empty(), $1); }
  | flags_id_list comma_sep T_ID { $$ = sirc_attrs_add_flag($1, $3); }
  ;

attr_int
  : T_INT                       { $$ = $1; }
  | T_INT ':' type              { $$ = $1; }
  ;

attr_bool
  : T_BOOL                      { $$ = $1; }
  ;

attr_ident
  : T_ID                        { $$ = $1; }
  ;

type
  : T_ID                     { $$ = sirc_type_from_name($1); }
  | '^' type                 { $$ = sirc_type_ptr_of($2); }
  | type_ctor                { $$ = $1; }
  ;

type_ctor
  : T_ARRAY '(' nl_star type comma_sep T_INT nl_star ')'
    { $$ = sirc_type_array_of($4, $6); }
  | T_ID '(' nl_star type nl_star ')'
    {
      if (strcmp($1, "fun") != 0) {
        yyerror("unknown type constructor");
        free($1);
        YYERROR;
      }
      $$ = sirc_type_fun_of($4);
      free($1);
    }
  | T_ID '(' nl_star type comma_sep type nl_star ')'
    {
      if (strcmp($1, "closure") != 0) {
        yyerror("unknown type constructor");
        free($1);
        YYERROR;
      }
      $$ = sirc_type_closure_of($4, $6);
      free($1);
    }
  | T_ID '{' nl_star sum_variants_opt nl_star '}'
    {
      if (strcmp($1, "sum") != 0) {
        yyerror("unknown type constructor");
        free($1);
        YYERROR;
      }
      $$ = sirc_type_sum_of($4);
      free($1);
    }
  | T_FN '(' nl_star type_list_opt nl_star ')' T_ARROW type
    { $$ = sirc_type_fn_of($4, $8); }
  ;

type_list_opt
  : /* empty */              { $$ = sirc_types_empty(); }
  | type_list                { $$ = $1; }
  ;

type_list
  : type                     { $$ = sirc_types_single($1); }
  | type_list comma_sep type { $$ = sirc_types_append($1, $3); }
  ;

sum_variants_opt
  : /* empty */              { $$ = sirc_sum_variants_empty(); }
  | sum_variants             { $$ = $1; }
  ;

sum_variants
  : sum_variant                    { $$ = $1; }
  | sum_variants comma_sep sum_variant { $$ = sirc_sum_variants_merge($1, $3); }
  ;

sum_variant
  : T_ID                     { $$ = sirc_sum_variants_append(sirc_sum_variants_empty(), $1, 0); }
  | T_ID ':' type            { $$ = sirc_sum_variants_append(sirc_sum_variants_empty(), $1, $3); }
  ;

nl_star
  : /* empty */
  | nl_star T_NL
  ;

nl_plus
  : T_NL
  | nl_plus T_NL
  ;

%%
