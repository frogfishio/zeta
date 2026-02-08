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
  SircSemSwitchCaseList* sem_cases;
  SircSemMatchCaseList* match_cases;
  SircBranchList* branch_list;
  SircAttrList* attrs;
  SircBranch br;
  SircSemSwitchCase sem_case;
  SircSemMatchCase match_case;
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
%token T_SEM_IF T_SEM_COND T_SEM_AND_SC T_SEM_OR_SC T_SEM_SWITCH T_SEM_MATCH_SUM
%token T_SEM_WHILE T_SEM_BREAK T_SEM_CONTINUE T_SEM_DEFER T_SEM_SCOPE

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
%type <node> sem_stmt sem_value_expr
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
%type <br> branch_operand
%type <sem_cases> sem_switch_cases_opt sem_switch_cases
%type <sem_case> sem_switch_case
%type <match_cases> sem_match_cases_opt sem_match_cases
%type <match_case> sem_match_case
%type <branch_list> branch_list_opt branch_list
%type <node> block_value
%type <b> public_opt

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
  | error nl_plus              { yyerrok; }
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
    { sirc_fn_def($2, $4, $7, $8, $10); }
  | T_FN T_ID '(' params_opt ')' T_ARROW type public_opt nl_star
      { sirc_cfg_begin(); }
    cfg_blocks T_END
      { sirc_fn_def_cfg($2, $4, $7, $8, sirc_nodelist_first($11), $11); }
  ;

public_opt
  : /* empty */    { $$ = 0; }
  | T_PUBLIC       { $$ = 1; }
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
  | sem_stmt nl_star          { $$ = $1; }
  | error nl_plus             { $$ = 0; yyerrok; }
  ;

sem_stmt
  : T_SEM_WHILE '(' nl_star branch_operand comma_sep branch_operand nl_star ')'
    { $$ = sirc_sem_while($4, $6); }
  | T_SEM_BREAK
    { $$ = sirc_sem_break(); }
  | T_SEM_CONTINUE
    { $$ = sirc_sem_continue(); }
  | T_SEM_DEFER '(' nl_star branch_operand nl_star ')'
    { $$ = sirc_sem_defer($4); }
  | T_SEM_SCOPE '(' nl_star T_ID ':' '[' nl_star branch_list_opt nl_star ']' comma_sep T_ID ':' block_value nl_star ')'
    {
      if (strcmp($4, "defers") != 0) { yyerror("sem.scope: expected 'defers'"); free($4); free($12); YYERROR; }
      if (strcmp($12, "body") != 0) { yyerror("sem.scope: expected 'body'"); free($4); free($12); YYERROR; }
      free($4);
      free($12);
      $$ = sirc_sem_scope($8, $14);
    }
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
  | error nl_plus
    { $$ = 0; yyerrok; }
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
  | sem_value_expr            { $$ = $1; }
  | select_expr              { $$ = $1; }
  | ptr_sizeof_expr           { $$ = $1; }
  | ptr_alignof_expr          { $$ = $1; }
  | ptr_offset_expr           { $$ = $1; }
  ;

sem_value_expr
  : T_SEM_IF '(' nl_star expr comma_sep branch_operand comma_sep branch_operand nl_star ')' T_AS type
    { $$ = sirc_sem_if($4, $6, $8, $12); }
  | T_SEM_COND '(' nl_star expr comma_sep branch_operand comma_sep branch_operand nl_star ')' T_AS type
    { $$ = sirc_sem_cond($4, $6, $8, $12); }
  | T_SEM_AND_SC '(' nl_star expr comma_sep branch_operand nl_star ')'
    { $$ = sirc_sem_and_sc($4, $6); }
  | T_SEM_OR_SC '(' nl_star expr comma_sep branch_operand nl_star ')'
    { $$ = sirc_sem_or_sc($4, $6); }
  | T_SEM_SWITCH '(' nl_star expr ',' nl_star T_CASES ':' '[' nl_star sem_switch_cases_opt nl_star ']' ',' nl_star T_DEFAULT ':' branch_operand nl_star ')' T_AS type
    { $$ = sirc_sem_switch($4, $11, $18, $22); }
  | T_SEM_MATCH_SUM '(' nl_star type comma_sep expr ',' nl_star T_CASES ':' '[' nl_star sem_match_cases_opt nl_star ']' ',' nl_star T_DEFAULT ':' branch_operand nl_star ')' T_AS type
    { $$ = sirc_sem_match_sum($4, $6, $13, $20, $24); }
  ;

branch_operand
  : T_ID expr
    {
      if (strcmp($1, "val") == 0) { $$.kind = SIRC_BRANCH_VAL; $$.node = $2; free($1); }
      else if (strcmp($1, "thunk") == 0) { $$.kind = SIRC_BRANCH_THUNK; $$.node = $2; free($1); }
      else { yyerror("expected 'val' or 'thunk'"); free($1); YYERROR; }
    }
  ;

branch_list_opt
  : /* empty */                { $$ = sirc_branch_list_empty(); }
  | branch_list                { $$ = $1; }
  ;

branch_list
  : branch_operand             { $$ = sirc_branch_list_append(sirc_branch_list_empty(), $1); }
  | branch_list comma_sep branch_operand
                              { $$ = sirc_branch_list_append($1, $3); }
  ;

sem_switch_cases_opt
  : /* empty */                { $$ = sirc_sem_switch_cases_empty(); }
  | sem_switch_cases           { $$ = $1; }
  ;

sem_switch_cases
  : sem_switch_case            { $$ = sirc_sem_switch_cases_append(sirc_sem_switch_cases_empty(), $1.lit, $1.body); }
  | sem_switch_cases ',' nl_star sem_switch_case
                              { $$ = sirc_sem_switch_cases_append($1, $4.lit, $4.body); }
  ;

sem_switch_case
  : '{' nl_star T_LIT ':' int_lit ',' nl_star T_ID ':' branch_operand nl_star '}'
    {
      if (strcmp($8, "body") != 0) { yyerror("sem.switch: expected 'body'"); free($8); YYERROR; }
      free($8);
      $$.lit = $5;
      $$.body = $10;
    }
  ;

sem_match_cases_opt
  : /* empty */                { $$ = sirc_sem_match_cases_empty(); }
  | sem_match_cases            { $$ = $1; }
  ;

sem_match_cases
  : sem_match_case             { $$ = sirc_sem_match_cases_append(sirc_sem_match_cases_empty(), $1.variant, $1.body); }
  | sem_match_cases ',' nl_star sem_match_case
                              { $$ = sirc_sem_match_cases_append($1, $4.variant, $4.body); }
  ;

sem_match_case
  : '{' nl_star T_ID ':' T_INT ',' nl_star T_ID ':' branch_operand nl_star '}'
    {
      if (strcmp($3, "variant") != 0) { yyerror("sem.match_sum: expected 'variant'"); free($3); free($8); YYERROR; }
      if (strcmp($8, "body") != 0) { yyerror("sem.match_sum: expected 'body'"); free($3); free($8); YYERROR; }
      free($3);
      free($8);
      $$.variant = $5;
      $$.body = $10;
    }
  ;

block_value
  : T_DO nl_star stmt_list T_END
    { $$ = sirc_block_value($3); }
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
