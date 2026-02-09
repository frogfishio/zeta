%option noyywrap nodefault yylineno

%{
#include "sirc_emit.h"
#include "sir.tab.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

/* 1-based column tracking for diagnostics (exported for yyerror) */
int yycolumn = 1;

static char* dup_yytext(void) {
  size_t n = strlen(yytext);
  char* s = (char*)malloc(n + 1);
  memcpy(s, yytext, n + 1);
  return s;
}

static void sirc_mark(const char* txt, int line, int col) {
  sirc_last_line = line;
  sirc_last_col = col;
  if (!txt) {
    sirc_last_tok[0] = 0;
    return;
  }
  size_t n = strlen(txt);
  if (n >= sizeof(sirc_last_tok)) n = sizeof(sirc_last_tok) - 1;
  memcpy(sirc_last_tok, txt, n);
  sirc_last_tok[n] = 0;
}

static void col_advance(void) {
  yycolumn += (int)yyleng;
}

static char* dup_strip_underscores(const char* s) {
  size_t n = strlen(s);
  char* out = (char*)malloc(n + 1);
  size_t j = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] != '_') out[j++] = s[i];
  }
  out[j] = 0;
  return out;
}

static long long parse_bin_literal(const char* s) {
  /* s points at either "0b..." or "-0b..." (underscores already removed) */
  int neg = 0;
  if (*s == '-') { neg = 1; s++; }
  s += 2; /* skip 0b */
  long long v = 0;
  while (*s) {
    if (*s == '0' || *s == '1') {
      v = (v << 1) | (*s - '0');
    }
    s++;
  }
  return neg ? -v : v;
}

static long long parse_int_literal(const char* raw) {
  char* s = dup_strip_underscores(raw);
  long long v = 0;
  if ((s[0] == '-' && s[1] == '0' && (s[2] == 'b' || s[2] == 'B')) ||
      (s[0] == '0' && (s[1] == 'b' || s[1] == 'B'))) {
    v = parse_bin_literal(s);
  } else {
    /* base 0 handles -?, decimal, and 0x/0X */
    v = (long long)strtoll(s, NULL, 0);
  }
  free(s);
  return v;
}

static double parse_float_literal(const char* raw) {
  char* s = dup_strip_underscores(raw);
  double v = strtod(s, NULL);
  free(s);
  return v;
}

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

static char* unescape_c_string(const char* yy) {
  /* yy is the raw token text including surrounding quotes */
  size_t n = strlen(yy);
  if (n < 2) {
    char* out = (char*)malloc(1);
    out[0] = 0;
    return out;
  }

  /* Worst case output length <= input length - 2 (quotes), so allocate n */
  char* out = (char*)malloc(n);
  size_t j = 0;

  /* walk inside the quotes */
  for (size_t i = 1; i + 1 < n; i++) {
    unsigned char c = (unsigned char)yy[i];
    if (c != '\\') {
      out[j++] = (char)c;
      continue;
    }

    /* escape */
    if (i + 1 >= n - 1) break; /* stray backslash at end */
    unsigned char e = (unsigned char)yy[++i];

    switch (e) {
      case 'n': out[j++] = '\n'; break;
      case 'r': out[j++] = '\r'; break;
      case 't': out[j++] = '\t'; break;
      case '\\': out[j++] = '\\'; break;
      case '"': out[j++] = '"'; break;
      case '0': out[j++] = '\0'; break;

      case 'x': {
        /* \xHH (two hex digits) */
        if (i + 2 < n - 1) {
          int h1 = hexval((unsigned char)yy[i + 1]);
          int h2 = hexval((unsigned char)yy[i + 2]);
          if (h1 >= 0 && h2 >= 0) {
            out[j++] = (char)((h1 << 4) | h2);
            i += 2;
            break;
          }
        }
        /* invalid \x escape: keep it literal */
        out[j++] = 'x';
        break;
      }

      default:
        /* Unknown escape: keep the escaped char as-is (e.g. \q -> q) */
        out[j++] = (char)e;
        break;
    }
  }

  out[j] = 0;
  return out;
}

/* Bison usually provides this; declare to use it for lexer diagnostics. */
void yyerror(const char* s);
%}

WS          [ \t\r]+
NL          \n
ID          [A-Za-z_$][A-Za-z0-9_$]*
INT10       -?[0-9][0-9_]*
INTHEX      -?0[xX][0-9A-Fa-f][0-9A-Fa-f_]*
INTBIN      -?0[bB][01][01_]*
FLOAT10     -?(([0-9][0-9_]*\.[0-9_]+)|([0-9][0-9_]*\.)|(\.[0-9_]+)|([0-9][0-9_]*[eE][+-]?[0-9][0-9_]*))
STR         \"([^\"\\\n]|\\.)*\"
STR_UNTERM1  \"([^\"\\\n]|\\.)*\n

%%

{WS}                        { col_advance(); }
";;"[^\n]*                  { col_advance(); }                 /* comment to EOL */
{NL}                        { sirc_mark("\\n", yylineno, 1); yycolumn = 1; return T_NL; }

{STR_UNTERM1}               {
                              yyerror("unterminated string literal");
                              /* we consumed a newline as part of the token */
                              yycolumn = 1;
                              return T_NL;
                            }

{STR}                       {
                              sirc_mark(yytext, yylineno, yycolumn);
                              yylval.s = unescape_c_string(yytext);
                              col_advance();
                              return T_STRING;
                            }

{FLOAT10}                    {
                              sirc_mark(yytext, yylineno, yycolumn);
                              yylval.d = parse_float_literal(yytext);
                              col_advance();
                              return T_FLOAT;
                            }

{INTBIN}                     { sirc_mark(yytext, yylineno, yycolumn); yylval.i = parse_int_literal(yytext); col_advance(); return T_INT; }
{INTHEX}                     { sirc_mark(yytext, yylineno, yycolumn); yylval.i = parse_int_literal(yytext); col_advance(); return T_INT; }
{INT10}                      { sirc_mark(yytext, yylineno, yycolumn); yylval.i = parse_int_literal(yytext); col_advance(); return T_INT; }

"unit"                      { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_UNIT; }
"@mod"                      { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_ATMOD; }
"target"                    { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_TARGET; }
"features"                  { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_FEATURES; }
"type"                      { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_TYPE; }
"array"                     { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_ARRAY; }
"sig"                       { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_SIG; }
"as"                        { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_AS; }
"fn"                        { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_FN; }
"const"                     { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_CONST; }
"global"                    { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_GLOBAL; }
"public"                    { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_PUBLIC; }
"extern"                    { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_EXTERN; }
"vec.select"                { sirc_mark(yytext, yylineno, yycolumn); yylval.s = dup_yytext(); col_advance(); return T_ID; }
"select"                    { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_SELECT; }
"do"                        { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_DO; }
"end"                       { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_END; }
"block"                     { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_BLOCK; }
"let"                       { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_LET; }
"return"                    { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_RETURN; }
"term.br"                   { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_TERM_BR; }
"term.cbr"                  { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_TERM_CBR; }
"term.condbr"               { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_TERM_CBR; }
"term.switch"               { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_TERM_SWITCH; }
"term.ret"                  { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_TERM_RET; }
"term.unreachable"          { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_TERM_UNREACHABLE; }
"term.trap"                 { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_TERM_TRAP; }

"sem.if"                    { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_SEM_IF; }
"sem.cond"                  { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_SEM_COND; }
"sem.and_sc"                { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_SEM_AND_SC; }
"sem.or_sc"                 { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_SEM_OR_SC; }
"sem.switch"                { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_SEM_SWITCH; }
"sem.match_sum"             { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_SEM_MATCH_SUM; }
"sem.while"                 { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_SEM_WHILE; }
"sem.break"                 { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_SEM_BREAK; }
"sem.continue"              { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_SEM_CONTINUE; }
"sem.defer"                 { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_SEM_DEFER; }
"sem.scope"                 { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_SEM_SCOPE; }

"scrut"                     { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_SCRUT; }
"case"                      { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_CASE; }
"flags"                     { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_FLAGS; }
"count"                     { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_COUNT; }
"cases"                     { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_CASES; }
"default"                   { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_DEFAULT; }
"lit"                       { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_LIT; }

"term"                      { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_TERM; }
"to"                        { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_TO; }
"args"                      { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_ARGS; }
"cond"                      { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_COND; }
"then"                      { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_THEN; }
"else"                      { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_ELSE; }
"value"                     { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_VALUE; }
"true"                      { sirc_mark(yytext, yylineno, yycolumn); yylval.b = 1; col_advance(); return T_BOOL; }
"false"                     { sirc_mark(yytext, yylineno, yycolumn); yylval.b = 0; col_advance(); return T_BOOL; }

"ptr.sizeof"                { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_PTR_SIZEOF; }
"ptr.alignof"               { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_PTR_ALIGNOF; }
"ptr.offset"                { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_PTR_OFFSET; }

"->"                        { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return T_ARROW; }

{ID}                        { sirc_mark(yytext, yylineno, yycolumn); yylval.s = dup_yytext(); col_advance(); return T_ID; }

"("                         { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return '('; }
")"                         { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return ')'; }
"["                         { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return '['; }
"]"                         { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return ']'; }
"{"                         { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return '{'; }
"}"                         { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return '}'; }
","                         { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return ','; }
":"                         { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return ':'; }
"."                         { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return '.'; }
"+"                         { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return '+'; }
"="                         { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return '='; }
"^"                         { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return '^'; }
"-"                         { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return '-'; }
">"                         { sirc_mark(yytext, yylineno, yycolumn); col_advance(); return '>'; }

.                           {
                              char buf[128];
                              unsigned char c = (unsigned char)yytext[0];
                              if (isprint(c)) {
                                snprintf(buf, sizeof(buf), "unexpected character '%c'", yytext[0]);
                              } else {
                                snprintf(buf, sizeof(buf), "unexpected character 0x%02X", (unsigned int)c);
                              }
                              yyerror(buf);
                              col_advance();
                              return yytext[0];
                            }

<<EOF>>                     { return 0; }

%%
