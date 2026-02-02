%{
#include <stdio.h>
#include <stdlib.h>

#include "sirc_emit.h"

int yylex(void);
extern int yylineno;
extern int yycolumn;
void yyerror(const char* s) { fprintf(stderr, "parse error:%d:%d: %s\n", yylineno, yycolumn, s); }
%}

%union {
  char*         s;
  long long     i;
  int           b;
  int64_t       ty;
  SircParamList* params;
  SircNodeList*  stmts;
  SircExprList*  args;
  int64_t       node;
}

/* tokens with values */
%token <s>   T_ID T_STRING
%token <i>   T_INT
%token <b>   T_BOOL

/* keywords / structural */
%token T_NL T_ARROW
%token T_UNIT T_TARGET
%token T_FN T_PUBLIC T_EXTERN T_END T_RETURN T_LET

/* declared in lexer (unused for now, but must exist) */
%token T_FEATURES T_SIG T_DO
%token T_TYPE T_CONST T_GLOBAL
%token T_BLOCK T_TERM T_TO T_ARGS T_COND T_THEN T_ELSE T_VALUE

%destructor { free($$); } T_ID T_STRING

%type <ty> type
%type <params> params_opt params
%type <stmts> stmt_list
%type <node> stmt let_stmt return_stmt expr_stmt
%type <args> args_opt args
%type <node> expr value int_lit dotted_or_call
%type <s> dotted_name

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
  : '+' T_ID                 { free($2); }
  | '+' T_ID ':' T_ID        { free($2); free($4); }
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
  | T_BOOL                   { $$ = sirc_value_bool($1); }
  | T_STRING                 { $$ = sirc_value_string($1); }
  | dotted_or_call           { $$ = $1; }
  ;

int_lit
  : T_INT                    { $$ = sirc_value_int($1); }
  | T_INT ':' type           { $$ = sirc_typed_int($1, $3); }
  ;

dotted_or_call
  : dotted_name                                { $$ = sirc_value_ident($1); }
  | dotted_name '(' nl_star args_opt nl_star ')' { $$ = sirc_call($1, $4); }
  ;

type
  : T_ID                     { $$ = sirc_type_from_name($1); }
  | '^' T_ID                 { $$ = sirc_type_ptr_of(sirc_type_from_name($2)); }
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
