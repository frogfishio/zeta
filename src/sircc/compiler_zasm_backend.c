// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_zasm_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool emit_zasm_v11(SirProgram* p, const char* out_path) {
  if (!p || !out_path) return false;

  NodeRec* zir_main = zasm_find_fn(p, "zir_main");
  if (!zir_main) {
    errf(p, "sircc: --emit-zasm currently requires a function named 'zir_main'");
    return false;
  }

  ZasmStr* strs = NULL;
  size_t strs_len = 0;
  if (!zasm_collect_cstrs(p, &strs, &strs_len)) return false;

  ZasmAlloca* allocas = NULL;
  size_t allocas_len = 0;
  if (!zasm_collect_allocas(p, &allocas, &allocas_len)) {
    free(strs);
    return false;
  }

  const char** decls = NULL;
  size_t decls_len = 0;
  if (!zasm_collect_decl_fns(p, &decls, &decls_len)) {
    free(strs);
    free(allocas);
    return false;
  }

  FILE* out = fopen(out_path, "wb");
  if (!out) {
    free(strs);
    free(allocas);
    free(decls);
    errf(p, "sircc: failed to open output: %s", strerror(errno));
    return false;
  }

  int64_t line = 1;

  zasm_write_ir_k(out, "meta");
  fprintf(out, ",\"producer\":\"sircc\"");
  if (p->unit_name) {
    fprintf(out, ",\"unit\":");
    json_write_escaped(out, p->unit_name);
  }
  zasm_write_loc(out, line++);
  fprintf(out, "}\n");

  for (size_t i = 0; i < decls_len; i++) {
    zasm_write_ir_k(out, "dir");
    fprintf(out, ",\"d\":\"EXTERN\",\"args\":[");
    zasm_write_op_str(out, "c");
    fprintf(out, ",");
    zasm_write_op_str(out, decls[i]);
    fprintf(out, ",");
    zasm_write_op_sym(out, decls[i]);
    fprintf(out, "]");
    zasm_write_loc(out, line++);
    fprintf(out, "}\n");
  }

  zasm_write_ir_k(out, "dir");
  fprintf(out, ",\"d\":\"PUBLIC\",\"args\":[");
  zasm_write_op_sym(out, "zir_main");
  fprintf(out, "]");
  zasm_write_loc(out, line++);
  fprintf(out, "}\n\n");

  zasm_write_ir_k(out, "label");
  fprintf(out, ",\"name\":\"zir_main\"");
  zasm_write_loc(out, line++);
  fprintf(out, "}\n");

  JsonValue* bodyv = zir_main->fields ? json_obj_get(zir_main->fields, "body") : NULL;
  int64_t body_id = 0;
  if (!parse_node_ref_id(bodyv, &body_id)) {
    fclose(out);
    free(strs);
    free(allocas);
    free(decls);
    errf(p, "sircc: zasm: fn zir_main missing body ref");
    return false;
  }
  NodeRec* body = get_node(p, body_id);
  if (!body || strcmp(body->tag, "block") != 0 || !body->fields) {
    fclose(out);
    free(strs);
    free(allocas);
    free(decls);
    errf(p, "sircc: zasm: zir_main body must be a block node");
    return false;
  }
  JsonValue* stmts = json_obj_get(body->fields, "stmts");
  if (!stmts || stmts->type != JSON_ARRAY) {
    fclose(out);
    free(strs);
    free(allocas);
    free(decls);
    errf(p, "sircc: zasm: zir_main body block missing stmts array");
    return false;
  }

  for (size_t si = 0; si < stmts->v.arr.len; si++) {
    int64_t sid = 0;
    if (!parse_node_ref_id(stmts->v.arr.items[si], &sid)) {
      fclose(out);
      free(strs);
      free(allocas);
      free(decls);
      errf(p, "sircc: zasm: block stmt[%zu] must be node ref", si);
      return false;
    }
    NodeRec* s = get_node(p, sid);
    if (!s) {
      fclose(out);
      free(strs);
      free(allocas);
      free(decls);
      errf(p, "sircc: zasm: unknown stmt node %lld", (long long)sid);
      return false;
    }

    if (strcmp(s->tag, "let") == 0) {
      JsonValue* v = s->fields ? json_obj_get(s->fields, "value") : NULL;
      int64_t vid = 0;
      if (!parse_node_ref_id(v, &vid)) {
        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        errf(p, "sircc: zasm: let node %lld missing fields.value ref", (long long)sid);
        return false;
      }
      NodeRec* vn = get_node(p, vid);
      if (!vn) {
        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        errf(p, "sircc: zasm: let node %lld value references unknown node", (long long)sid);
        return false;
      }
      if (strcmp(vn->tag, "call") == 0 || strcmp(vn->tag, "call.indirect") == 0) {
        if (!zasm_emit_call_stmt(out, p, strs, strs_len, allocas, allocas_len, vid, line++)) {
          fclose(out);
          free(strs);
          free(allocas);
          free(decls);
          return false;
        }
      }
      continue;
    }

    if (strcmp(s->tag, "mem.fill") == 0) {
      if (!zasm_emit_mem_fill_stmt(out, p, strs, strs_len, allocas, allocas_len, s, line)) {
        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        return false;
      }
      line += 4;
      continue;
    }

    if (strcmp(s->tag, "mem.copy") == 0) {
      if (!zasm_emit_mem_copy_stmt(out, p, strs, strs_len, allocas, allocas_len, s, line)) {
        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        return false;
      }
      line += 4;
      continue;
    }

    if (strncmp(s->tag, "store.", 6) == 0) {
      if (!zasm_emit_store_stmt(out, p, strs, strs_len, allocas, allocas_len, s, line)) {
        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        return false;
      }
      line += 2;
      continue;
    }

    if (strcmp(s->tag, "term.ret") == 0 || strcmp(s->tag, "return") == 0) {
      JsonValue* rv = s->fields ? json_obj_get(s->fields, "value") : NULL;
      int64_t rid = 0;
      if (rv && parse_node_ref_id(rv, &rid)) {
        if (!zasm_emit_ret_value_to_hl(out, p, strs, strs_len, allocas, allocas_len, rid, line++)) {
          fclose(out);
          free(strs);
          free(allocas);
          free(decls);
          return false;
        }
      } else {
        zasm_write_ir_k(out, "instr");
        fprintf(out, ",\"m\":\"LD\",\"ops\":[");
        zasm_write_op_reg(out, "HL");
        fprintf(out, ",");
        zasm_write_op_num(out, 0);
        fprintf(out, "]");
        zasm_write_loc(out, line++);
        fprintf(out, "}\n");
      }

      zasm_write_ir_k(out, "instr");
      fprintf(out, ",\"m\":\"RET\",\"ops\":[]");
      zasm_write_loc(out, line++);
      fprintf(out, "}\n");
      break;
    }

    fclose(out);
    free(strs);
    free(allocas);
    free(decls);
    errf(p, "sircc: zasm: unsupported stmt tag '%s' in zir_main", s->tag);
    return false;
  }

  if (strs_len) fprintf(out, "\n");
  for (size_t i = 0; i < strs_len; i++) {
    zasm_write_ir_k(out, "dir");
    fprintf(out, ",\"d\":\"STR\",\"name\":");
    json_write_escaped(out, strs[i].sym);
    fprintf(out, ",\"args\":[");
    zasm_write_op_str(out, strs[i].value);
    fprintf(out, "]");
    zasm_write_loc(out, line++);
    fprintf(out, "}\n");
  }

  if (allocas_len) fprintf(out, "\n");
  for (size_t i = 0; i < allocas_len; i++) {
    zasm_write_ir_k(out, "dir");
    fprintf(out, ",\"d\":\"RESB\",\"name\":");
    json_write_escaped(out, allocas[i].sym);
    fprintf(out, ",\"args\":[");
    zasm_write_op_num(out, allocas[i].size_bytes);
    fprintf(out, "]");
    zasm_write_loc(out, line++);
    fprintf(out, "}\n");
  }

  fclose(out);
  free(strs);
  free(allocas);
  free(decls);
  return true;
}

