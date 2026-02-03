// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void write_ir_k(FILE* out, const char* k) {
  fprintf(out, "{\"ir\":\"zasm-v1.1\",\"k\":");
  json_write_escaped(out, k);
}

static void write_loc(FILE* out, int64_t line) {
  fprintf(out, ",\"loc\":{\"line\":%lld}", (long long)line);
}

static void write_op_reg(FILE* out, const char* r) {
  fprintf(out, "{\"t\":\"reg\",\"v\":");
  json_write_escaped(out, r);
  fprintf(out, "}");
}

static void write_op_sym(FILE* out, const char* s) {
  fprintf(out, "{\"t\":\"sym\",\"v\":");
  json_write_escaped(out, s);
  fprintf(out, "}");
}

static void write_op_num(FILE* out, int64_t v) {
  fprintf(out, "{\"t\":\"num\",\"v\":%lld}", (long long)v);
}

static void write_op_str(FILE* out, const char* s) {
  fprintf(out, "{\"t\":\"str\",\"v\":");
  json_write_escaped(out, s ? s : "");
  fprintf(out, "}");
}

static const char* find_cstr(SirProgram* p) {
  if (!p) return NULL;
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if (strcmp(n->tag, "cstr") != 0) continue;
    if (!n->fields) continue;
    const char* s = json_get_string(json_obj_get(n->fields, "value"));
    if (s) return s;
  }
  return NULL;
}

static NodeRec* find_fn(SirProgram* p, const char* name) {
  if (!p || !name) return NULL;
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if (strcmp(n->tag, "fn") != 0) continue;
    const char* nm = n->fields ? json_get_string(json_obj_get(n->fields, "name")) : NULL;
    if (nm && strcmp(nm, name) == 0) return n;
  }
  return NULL;
}

bool emit_zasm_v11(SirProgram* p, const char* out_path) {
  if (!p || !out_path) return false;

  // v0: only handle a simple zABI-style program that prints one cstr via zi_write.
  // We'll grow this into a real lowering pass.
  NodeRec* zir_main = find_fn(p, "zir_main");
  if (!zir_main) {
    errf(p, "sircc: --emit-zasm currently requires a function named 'zir_main'");
    return false;
  }

  const char* msg = find_cstr(p);
  if (!msg) {
    errf(p, "sircc: --emit-zasm currently requires a cstr node (message literal)");
    return false;
  }
  for (const char* it = msg; *it; it++) {
    if (*it == '\0') {
      errf(p, "sircc: zasm STR cannot contain NUL bytes");
      return false;
    }
  }
  int64_t msg_len = (int64_t)strlen(msg);

  FILE* out = fopen(out_path, "wb");
  if (!out) {
    errf(p, "sircc: failed to open output: %s", strerror(errno));
    return false;
  }

  int64_t line = 1;

  // meta (optional, but nice for tooling)
  write_ir_k(out, "meta");
  fprintf(out, ",\"producer\":\"sircc\"");
  if (p->unit_name) {
    fprintf(out, ",\"unit\":");
    json_write_escaped(out, p->unit_name);
  }
  write_loc(out, line++);
  fprintf(out, "}\n");

  // EXTERN c zi_write zi_write
  write_ir_k(out, "dir");
  fprintf(out, ",\"d\":\"EXTERN\",\"args\":[");
  write_op_str(out, "c");
  fprintf(out, ",");
  write_op_str(out, "zi_write");
  fprintf(out, ",");
  write_op_sym(out, "zi_write");
  fprintf(out, "]");
  write_loc(out, line++);
  fprintf(out, "}\n");

  // PUBLIC zir_main
  write_ir_k(out, "dir");
  fprintf(out, ",\"d\":\"PUBLIC\",\"args\":[");
  write_op_sym(out, "zir_main");
  fprintf(out, "]");
  write_loc(out, line++);
  fprintf(out, "}\n\n");

  // label zir_main
  write_ir_k(out, "label");
  fprintf(out, ",\"name\":\"zir_main\"");
  write_loc(out, line++);
  fprintf(out, "}\n");

  // LD DE, msg
  write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"LD\",\"ops\":[");
  write_op_reg(out, "DE");
  fprintf(out, ",");
  write_op_sym(out, "msg");
  fprintf(out, "]");
  write_loc(out, line++);
  fprintf(out, "}\n");

  // LD BC, <len>
  write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"LD\",\"ops\":[");
  write_op_reg(out, "BC");
  fprintf(out, ",");
  write_op_num(out, msg_len);
  fprintf(out, "]");
  write_loc(out, line++);
  fprintf(out, "}\n");

  // LD HL, #1  (stdout)
  write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"LD\",\"ops\":[");
  write_op_reg(out, "HL");
  fprintf(out, ",");
  write_op_num(out, 1);
  fprintf(out, "]");
  write_loc(out, line++);
  fprintf(out, "}\n");

  // CALL zi_write
  write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"CALL\",\"ops\":[");
  write_op_sym(out, "zi_write");
  fprintf(out, "]");
  write_loc(out, line++);
  fprintf(out, "}\n");

  // LD HL, #0 ; return 0
  write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"LD\",\"ops\":[");
  write_op_reg(out, "HL");
  fprintf(out, ",");
  write_op_num(out, 0);
  fprintf(out, "]");
  write_loc(out, line++);
  fprintf(out, "}\n");

  // RET
  write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"RET\",\"ops\":[]");
  write_loc(out, line++);
  fprintf(out, "}\n\n");

  // msg: STR "..."
  write_ir_k(out, "dir");
  fprintf(out, ",\"d\":\"STR\",\"name\":\"msg\",\"args\":[");
  write_op_str(out, msg);
  fprintf(out, "]");
  write_loc(out, line++);
  fprintf(out, "}\n");

  fclose(out);

  (void)zir_main;
  return true;
}

