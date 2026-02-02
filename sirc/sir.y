%{
#include <stdio.h>
#include <stdlib.h>

int yylex(void);
extern int yylineno;
extern int yycolumn;
void yyerror(const char* s) { fprintf(stderr, "parse error:%d:%d: %s\n", yylineno, yycolumn, s); }
%}

%union {
  char*     s;
  long long i;
  int       b;
}

/* tokens with values */
%token <s>   T_ID T_STRING
%token <i>   T_INT
%token <b>   T_BOOL

/* keywords / structural */
%token T_NL T_ARROW
%token T_UNIT T_TARGET
%token T_FN T_PUBLIC T_END T_RETURN T_LET
%token T_TYPE T_CONST T_GLOBAL
%token T_BLOCK T_TERM T_TO T_ARGS T_COND T_THEN T_ELSE T_VALUE

%start program

%%

program
  : unit_header nl_opt decls_opt
  ;

unit_header
  : T_UNIT T_ID T_TARGET T_ID features_opt
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
  : '+' T_ID
  | '+' T_ID ':' T_ID          /* +agg:v1 style */
  ;

decls_opt
  : /* empty */
  | decls
  ;

decls
  : decl
  | decls decl
  ;

decl
  : fn_decl
  | type_decl
  | const_decl
  | global_decl
  | nl_opt                     /* allow blank lines */
  ;

fn_decl
  : T_FN T_ID '(' params_opt ')' T_ARROW type public_opt nl_opt stmt_list T_END nl_opt
  ;

public_opt
  : /* empty */
  | T_PUBLIC
  ;

params_opt
  : /* empty */
  | params
  ;

params
  : param
  | params ',' param
  ;

param
  : T_ID ':' type
  ;

type_decl
  : T_TYPE T_ID '=' type_expr nl_opt
  ;

type_expr
  : type                         /* allow `type t = i32` if you want */
  | call_expr                     /* e.g. array(i32, 4) */
  ;

const_decl
  : T_CONST T_ID ':' type '=' value nl_opt
  ;

global_decl
  : T_GLOBAL T_ID ':' type public_opt '=' value nl_opt
  ;

stmt_list
  : stmt
  | stmt_list stmt
  ;

stmt
  : let_stmt nl_opt
  | return_stmt nl_opt
  | expr_stmt nl_opt
  | block_stmt                   /* blocks include their own ends */
  ;

let_stmt
  : T_LET T_ID ':' type '=' expr
  ;

return_stmt
  : T_RETURN expr
  ;

expr_stmt
  : expr
  ;

block_stmt
  : T_BLOCK T_ID block_params_opt nl_opt stmt_list T_END nl_opt
  ;

block_params_opt
  : /* empty */
  | '(' params_opt ')'
  ;

expr
  : value
  | call_expr
  ;

call_expr
  : dotted_name '(' args_opt ')'
  ;

dotted_name
  : T_ID
  | dotted_name '.' T_ID
  ;

args_opt
  : /* empty */
  | args
  ;

args
  : expr
  | args ',' expr
  ;

/* ---- values / literals ---- */

value
  : typed_int
  | T_BOOL
  | T_STRING
  | ref_value
  | object_lit
  | array_lit
  | T_ID                      /* variable / symbol reference */
  ;

typed_int
  : T_INT ':' type            /* 3:i32 */
  ;

ref_value
  : '^' T_ID                  /* ^c_vec */
  ;

array_lit
  : '[' array_elems_opt ']'
  ;

array_elems_opt
  : /* empty */
  | array_elems
  ;

array_elems
  : value
  | array_elems ',' value
  ;

object_lit
  : '{' obj_fields_opt '}'
  ;

obj_fields_opt
  : /* empty */
  | obj_fields
  ;

obj_fields
  : obj_field
  | obj_fields ',' obj_field
  ;

obj_field
  : T_ID ':' value
  ;

/* ---- types ---- */

type
  : T_ID
  | '^' T_ID                  /* pointer-to-type sugar in your sample */
  ;

nl_opt
  : /* empty */
  | nl_opt T_NL
  ;

%%
