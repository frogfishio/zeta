// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_zasm_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char* sym;
  int64_t size_bytes;
} ZasmTempSlot;

static bool add_temp_slot(
    SirProgram* p,
    ZasmTempSlot** slots,
    size_t* slots_len,
    size_t* slots_cap,
    int64_t id_hint,
    int64_t size_bytes,
    const char** out_sym) {
  if (!p || !slots || !slots_len || !slots_cap || !out_sym) return false;
  *out_sym = NULL;

  if (*slots_len == *slots_cap) {
    size_t next = *slots_cap ? (*slots_cap * 2) : 16;
    ZasmTempSlot* bigger = (ZasmTempSlot*)realloc(*slots, next * sizeof(ZasmTempSlot));
    if (!bigger) return false;
    *slots = bigger;
    *slots_cap = next;
  }

  char name_buf[96];
  snprintf(name_buf, sizeof(name_buf), "tmp_%lld", (long long)id_hint);
  char* sym = arena_strdup(&p->arena, name_buf);
  if (!sym) return false;

  (*slots)[(*slots_len)++] = (ZasmTempSlot){.sym = sym, .size_bytes = size_bytes};
  *out_sym = sym;
  return true;
}

static bool emit_st64_slot_from_hl(FILE* out, const char* slot_sym, int64_t line_no) {
  if (!out || !slot_sym) return false;
  ZasmOp base = {.k = ZOP_SYM, .s = slot_sym};
  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"ST64\",\"ops\":[");
  zasm_write_op_mem(out, &base, 0, 8);
  fprintf(out, ",");
  zasm_write_op_reg(out, "HL");
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
  return true;
}

static bool emit_store_reg_to_slot(FILE* out, const char* slot_sym, int64_t size_bytes, const char* reg, int64_t line_no) {
  if (!out || !slot_sym || !reg) return false;
  const char* m = NULL;
  int64_t hint = 0;
  if (size_bytes == 1) {
    m = "ST8";
    hint = 1;
  } else if (size_bytes == 2) {
    m = "ST16";
    hint = 2;
  } else if (size_bytes == 4) {
    m = "ST32";
    hint = 4;
  } else if (size_bytes == 8) {
    m = "ST64";
    hint = 8;
  } else {
    return false;
  }

  ZasmOp base = {.k = ZOP_SYM, .s = slot_sym};
  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":");
  json_write_escaped(out, m);
  fprintf(out, ",\"ops\":[");
  zasm_write_op_mem(out, &base, 0, hint);
  fprintf(out, ",");
  zasm_write_op_reg(out, reg);
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
  return true;
}

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
  size_t name_cap = 0;
  size_t name_len = 0;
  ZasmNameBinding* names = NULL;
  size_t tmp_cap = 0;
  size_t tmp_len = 0;
  ZasmTempSlot* tmps = NULL;

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
      free(names);
      errf(p, "sircc: zasm: block stmt[%zu] must be node ref", si);
      return false;
    }
    NodeRec* s = get_node(p, sid);
    if (!s) {
      fclose(out);
      free(strs);
      free(allocas);
      free(decls);
      free(names);
      errf(p, "sircc: zasm: unknown stmt node %lld", (long long)sid);
      return false;
    }

    if (strcmp(s->tag, "let") == 0) {
      const char* bind_name = s->fields ? json_get_string(json_obj_get(s->fields, "name")) : NULL;
      JsonValue* v = s->fields ? json_obj_get(s->fields, "value") : NULL;
      int64_t vid = 0;
      if (!parse_node_ref_id(v, &vid)) {
        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        free(names);
        errf(p, "sircc: zasm: let node %lld missing fields.value ref", (long long)sid);
        return false;
      }
      NodeRec* vn = get_node(p, vid);
      if (!vn) {
        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        free(names);
        errf(p, "sircc: zasm: let node %lld value references unknown node", (long long)sid);
        return false;
      }
      if (strcmp(vn->tag, "call") == 0 || strcmp(vn->tag, "call.indirect") == 0) {
        if (!zasm_emit_call_stmt(out, p, strs, strs_len, allocas, allocas_len, names, name_len, vid, line++)) {
          fclose(out);
          free(strs);
          free(allocas);
          free(decls);
          free(names);
          free(tmps);
          return false;
        }

        if (bind_name && strcmp(bind_name, "_") != 0) {
          const char* slot_sym = NULL;
          if (!add_temp_slot(p, &tmps, &tmp_len, &tmp_cap, sid, 8, &slot_sym)) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            errf(p, "sircc: zasm: out of memory");
            return false;
          }

          if (!emit_st64_slot_from_hl(out, slot_sym, line++)) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            return false;
          }

          if (name_len == name_cap) {
            size_t next = name_cap ? name_cap * 2 : 16;
            ZasmNameBinding* bigger = (ZasmNameBinding*)realloc(names, next * sizeof(ZasmNameBinding));
            if (!bigger) {
              fclose(out);
              free(strs);
              free(allocas);
              free(decls);
              free(names);
              free(tmps);
              errf(p, "sircc: zasm: out of memory");
              return false;
            }
            names = bigger;
            name_cap = next;
          }

          names[name_len++] = (ZasmNameBinding){
              .name = bind_name,
              .is_slot = true,
              .op = {.k = ZOP_SYM, .s = slot_sym},
              .slot_size_bytes = 8,
          };
        }

        continue;
      }

      if (strncmp(vn->tag, "load.", 5) == 0) {
        int64_t width = 0;
        const char* m = NULL;
        const char* dst_reg = NULL;
        if (strcmp(vn->tag, "load.i8") == 0) {
          width = 1;
          m = "LD8U";
          dst_reg = "A";
        } else if (strcmp(vn->tag, "load.i16") == 0) {
          width = 2;
          m = "LD16U";
          dst_reg = "HL";
        } else if (strcmp(vn->tag, "load.i32") == 0) {
          width = 4;
          m = "LD32U64";
          dst_reg = "HL";
        } else if (strcmp(vn->tag, "load.i64") == 0 || strcmp(vn->tag, "load.ptr") == 0) {
          width = 8;
          m = "LD64";
          dst_reg = "HL";
        } else {
          fclose(out);
          free(strs);
          free(allocas);
          free(decls);
          free(names);
          free(tmps);
          errf(p, "sircc: zasm: unsupported load '%s'", vn->tag);
          return false;
        }

        int64_t addr_id = 0;
        JsonValue* av = vn->fields ? json_obj_get(vn->fields, "addr") : NULL;
        if (!parse_node_ref_id(av, &addr_id)) {
          fclose(out);
          free(strs);
          free(allocas);
          free(decls);
          free(names);
          free(tmps);
          errf(p, "sircc: zasm: %s node %lld requires fields.addr node ref", vn->tag, (long long)vn->id);
          return false;
        }
        ZasmOp addr = {0};
        if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, name_len, addr_id, &addr) || addr.k != ZOP_SYM) {
          fclose(out);
          free(strs);
          free(allocas);
          free(decls);
          free(names);
          free(tmps);
          errf(p, "sircc: zasm: %s addr must be an alloca symbol (node %lld)", vn->tag, (long long)addr_id);
          return false;
        }

        zasm_write_ir_k(out, "instr");
        fprintf(out, ",\"m\":");
        json_write_escaped(out, m);
        fprintf(out, ",\"ops\":[");
        zasm_write_op_reg(out, dst_reg);
        fprintf(out, ",");
        zasm_write_op_mem(out, &addr, 0, width);
        fprintf(out, "]");
        zasm_write_loc(out, line++);
        fprintf(out, "}\n");

        if (bind_name && strcmp(bind_name, "_") != 0) {
          const char* slot_sym = NULL;
          if (!add_temp_slot(p, &tmps, &tmp_len, &tmp_cap, sid, width, &slot_sym)) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            errf(p, "sircc: zasm: out of memory");
            return false;
          }

          if (!emit_store_reg_to_slot(out, slot_sym, width, dst_reg, line++)) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            return false;
          }

          if (name_len == name_cap) {
            size_t next = name_cap ? name_cap * 2 : 16;
            ZasmNameBinding* bigger = (ZasmNameBinding*)realloc(names, next * sizeof(ZasmNameBinding));
            if (!bigger) {
              fclose(out);
              free(strs);
              free(allocas);
              free(decls);
              free(names);
              free(tmps);
              errf(p, "sircc: zasm: out of memory");
              return false;
            }
            names = bigger;
            name_cap = next;
          }

          names[name_len++] = (ZasmNameBinding){
              .name = bind_name,
              .is_slot = true,
              .op = {.k = ZOP_SYM, .s = slot_sym},
              .slot_size_bytes = width,
          };
        }

        continue;
      }

      // Pure-ish binding of stable values (consts/symbols); no code emitted.
      if (bind_name && strcmp(bind_name, "_") != 0) {
        ZasmOp op = {0};
        if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, name_len, vid, &op)) {
          fclose(out);
          free(strs);
          free(allocas);
          free(decls);
          free(names);
          free(tmps);
          return false;
        }

        if (name_len == name_cap) {
          size_t next = name_cap ? name_cap * 2 : 16;
          ZasmNameBinding* bigger = (ZasmNameBinding*)realloc(names, next * sizeof(ZasmNameBinding));
          if (!bigger) {
            fclose(out);
            free(strs);
            free(allocas);
            free(decls);
            free(names);
            free(tmps);
            errf(p, "sircc: zasm: out of memory");
            return false;
          }
          names = bigger;
          name_cap = next;
        }

        // Shadowing allowed; last binding wins during lookup.
        names[name_len++] = (ZasmNameBinding){.name = bind_name, .is_slot = false, .op = op, .slot_size_bytes = 0};
      }
      continue;
    }

    if (strcmp(s->tag, "mem.fill") == 0) {
      if (!zasm_emit_mem_fill_stmt(out, p, strs, strs_len, allocas, allocas_len, names, name_len, s, line)) {
        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        free(names);
        free(tmps);
        return false;
      }
      line += 4;
      continue;
    }

    if (strcmp(s->tag, "mem.copy") == 0) {
      if (!zasm_emit_mem_copy_stmt(out, p, strs, strs_len, allocas, allocas_len, names, name_len, s, line)) {
        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        free(names);
        free(tmps);
        return false;
      }
      line += 4;
      continue;
    }

    if (strncmp(s->tag, "store.", 6) == 0) {
      if (!zasm_emit_store_stmt(out, p, strs, strs_len, allocas, allocas_len, names, name_len, s, line)) {
        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        free(names);
        free(tmps);
        return false;
      }
      line += 2;
      continue;
    }

    if (strcmp(s->tag, "term.ret") == 0 || strcmp(s->tag, "return") == 0) {
      JsonValue* rv = s->fields ? json_obj_get(s->fields, "value") : NULL;
      int64_t rid = 0;
      if (rv && parse_node_ref_id(rv, &rid)) {
        if (!zasm_emit_ret_value_to_hl(out, p, strs, strs_len, allocas, allocas_len, names, name_len, rid, line++)) {
          fclose(out);
          free(strs);
          free(allocas);
          free(decls);
          free(names);
          free(tmps);
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
    free(names);
    free(tmps);
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

  if (tmp_len) fprintf(out, "\n");
  for (size_t i = 0; i < tmp_len; i++) {
    zasm_write_ir_k(out, "dir");
    fprintf(out, ",\"d\":\"RESB\",\"name\":");
    json_write_escaped(out, tmps[i].sym);
    fprintf(out, ",\"args\":[");
    zasm_write_op_num(out, tmps[i].size_bytes);
    fprintf(out, "]");
    zasm_write_loc(out, line++);
    fprintf(out, "}\n");
  }

  fclose(out);
  free(strs);
  free(allocas);
  free(decls);
  free(names);
  free(tmps);
  return true;
}
