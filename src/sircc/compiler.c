// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler.h"
#include "json.h"
#include "sircc.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum TypeKind {
  TYPE_INVALID = 0,
  TYPE_PRIM,
  TYPE_PTR,
  TYPE_FN,
} TypeKind;

typedef struct TypeRec {
  int64_t id;
  TypeKind kind;

  const char* prim;
  int64_t of;

  int64_t* params;
  size_t param_len;
  int64_t ret;
  bool varargs;

  LLVMTypeRef llvm;
  bool resolving;
} TypeRec;

typedef struct NodeRec {
  int64_t id;
  const char* tag;
  int64_t type_ref;  // 0 means absent
  JsonValue* fields; // JSON object (or NULL)

  LLVMValueRef llvm_value; // cached when lowered (expressions); for fn nodes this is the LLVM function
  bool resolving;
} NodeRec;

typedef struct SirProgram {
  Arena arena;

  TypeRec** types;
  size_t types_cap;

  NodeRec** nodes;
  size_t nodes_cap;
} SirProgram;

static void fatalf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

static bool ensure_type_slot(SirProgram* p, int64_t id) {
  if (id < 0) return false;
  size_t want = (size_t)id + 1;
  if (want <= p->types_cap) return true;
  size_t new_cap = p->types_cap ? p->types_cap * 2 : 64;
  while (new_cap < want) new_cap *= 2;
  TypeRec** next = (TypeRec**)realloc(p->types, new_cap * sizeof(TypeRec*));
  if (!next) return false;
  memset(next + p->types_cap, 0, (new_cap - p->types_cap) * sizeof(TypeRec*));
  p->types = next;
  p->types_cap = new_cap;
  return true;
}

static bool ensure_node_slot(SirProgram* p, int64_t id) {
  if (id < 0) return false;
  size_t want = (size_t)id + 1;
  if (want <= p->nodes_cap) return true;
  size_t new_cap = p->nodes_cap ? p->nodes_cap * 2 : 128;
  while (new_cap < want) new_cap *= 2;
  NodeRec** next = (NodeRec**)realloc(p->nodes, new_cap * sizeof(NodeRec*));
  if (!next) return false;
  memset(next + p->nodes_cap, 0, (new_cap - p->nodes_cap) * sizeof(NodeRec*));
  p->nodes = next;
  p->nodes_cap = new_cap;
  return true;
}

static JsonValue* must_obj(JsonValue* v, const char* ctx) {
  if (!v || v->type != JSON_OBJECT) {
    fatalf("sircc: expected object for %s", ctx);
    return NULL;
  }
  return v;
}

static const char* must_string(JsonValue* v, const char* ctx) {
  const char* s = json_get_string(v);
  if (!s) fatalf("sircc: expected string for %s", ctx);
  return s;
}

static bool must_i64(JsonValue* v, int64_t* out, const char* ctx) {
  if (!json_get_i64(v, out)) {
    fatalf("sircc: expected integer for %s", ctx);
    return false;
  }
  return true;
}

static bool parse_node_ref_id(const JsonValue* v, int64_t* out_id) {
  if (!v || v->type != JSON_OBJECT) return false;
  JsonValue* t = json_obj_get(v, "t");
  const char* ts = json_get_string(t);
  if (!ts || strcmp(ts, "ref") != 0) return false;
  JsonValue* idv = json_obj_get(v, "id");
  return json_get_i64(idv, out_id);
}

static bool read_line(FILE* f, char** buf, size_t* cap, size_t* out_len) {
  if (!*buf || *cap == 0) {
    *cap = 4096;
    *buf = (char*)malloc(*cap);
    if (!*buf) return false;
  }
  (*buf)[0] = 0;

  size_t len = 0;
  while (fgets(*buf + len, (int)(*cap - len), f)) {
    len += strlen(*buf + len);
    if (len && (*buf)[len - 1] == '\n') break;
    if (*cap - len < 2) {
      size_t next = (*cap) * 2;
      char* bigger = (char*)realloc(*buf, next);
      if (!bigger) return false;
      *buf = bigger;
      *cap = next;
    }
  }

  if (len == 0 && feof(f)) return false;
  while (len && ((*buf)[len - 1] == '\n' || (*buf)[len - 1] == '\r')) {
    (*buf)[--len] = 0;
  }
  *out_len = len;
  return true;
}

static bool is_blank_line(const char* s) {
  for (; *s; s++) {
    if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') return false;
  }
  return true;
}

static bool parse_type_record(SirProgram* p, JsonValue* obj) {
  int64_t id = 0;
  if (!must_i64(json_obj_get(obj, "id"), &id, "type.id")) return false;
  const char* kind = must_string(json_obj_get(obj, "kind"), "type.kind");
  if (!kind) return false;
  if (!ensure_type_slot(p, id)) return false;
  if (p->types[id]) {
    fatalf("sircc: duplicate type id %lld", (long long)id);
    return false;
  }

  TypeRec* tr = (TypeRec*)arena_alloc(&p->arena, sizeof(TypeRec));
  if (!tr) return false;
  tr->id = id;
  tr->kind = TYPE_INVALID;
  tr->of = 0;
  tr->ret = 0;
  tr->params = NULL;
  tr->param_len = 0;
  tr->varargs = false;

  if (strcmp(kind, "prim") == 0) {
    tr->kind = TYPE_PRIM;
    tr->prim = must_string(json_obj_get(obj, "prim"), "type.prim");
    if (!tr->prim) return false;
  } else if (strcmp(kind, "ptr") == 0) {
    tr->kind = TYPE_PTR;
    if (!must_i64(json_obj_get(obj, "of"), &tr->of, "type.of")) return false;
  } else if (strcmp(kind, "fn") == 0) {
    tr->kind = TYPE_FN;
    JsonValue* params = json_obj_get(obj, "params");
    if (!params || params->type != JSON_ARRAY) {
      fatalf("sircc: expected array for type.params");
      return false;
    }
    tr->param_len = params->v.arr.len;
    tr->params = (int64_t*)arena_alloc(&p->arena, tr->param_len * sizeof(int64_t));
    if (!tr->params) return false;
    for (size_t i = 0; i < tr->param_len; i++) {
      int64_t pid = 0;
      if (!must_i64(params->v.arr.items[i], &pid, "type.params[i]")) return false;
      tr->params[i] = pid;
    }
    if (!must_i64(json_obj_get(obj, "ret"), &tr->ret, "type.ret")) return false;
    JsonValue* va = json_obj_get(obj, "varargs");
    if (va && va->type == JSON_BOOL) tr->varargs = va->v.b;
  } else {
    fatalf("sircc: unsupported type kind '%s' (v1 subset)", kind);
    return false;
  }

  p->types[id] = tr;
  return true;
}

static bool parse_node_record(SirProgram* p, JsonValue* obj) {
  int64_t id = 0;
  if (!must_i64(json_obj_get(obj, "id"), &id, "node.id")) return false;
  const char* tag = must_string(json_obj_get(obj, "tag"), "node.tag");
  if (!tag) return false;

  int64_t type_ref = 0;
  JsonValue* tr = json_obj_get(obj, "type_ref");
  if (tr) {
    if (!must_i64(tr, &type_ref, "node.type_ref")) return false;
  }

  JsonValue* fields = json_obj_get(obj, "fields");
  if (fields && fields->type != JSON_OBJECT) {
    fatalf("sircc: expected object for node.fields");
    return false;
  }

  if (!ensure_node_slot(p, id)) return false;
  if (p->nodes[id]) {
    fatalf("sircc: duplicate node id %lld", (long long)id);
    return false;
  }

  NodeRec* nr = (NodeRec*)arena_alloc(&p->arena, sizeof(NodeRec));
  if (!nr) return false;
  nr->id = id;
  nr->tag = tag;
  nr->type_ref = type_ref;
  nr->fields = fields;
  nr->llvm_value = NULL;
  nr->resolving = false;

  p->nodes[id] = nr;
  return true;
}

static bool parse_program(SirProgram* p, const char* input_path) {
  FILE* f = fopen(input_path, "rb");
  if (!f) {
    fatalf("sircc: failed to open %s: %s", input_path, strerror(errno));
    return false;
  }

  char* line = NULL;
  size_t cap = 0;
  size_t len = 0;
  size_t line_no = 0;

  while (read_line(f, &line, &cap, &len)) {
    line_no++;
    if (len == 0 || is_blank_line(line)) continue;

    JsonError jerr = {0};
    JsonValue* root = NULL;
    if (!json_parse(&p->arena, line, &root, &jerr)) {
      fatalf("sircc: JSON parse error at %s:%zu:%zu: %s", input_path, line_no, jerr.offset + 1,
             jerr.msg ? jerr.msg : "unknown");
      free(line);
      fclose(f);
      return false;
    }
    if (!must_obj(root, "record")) {
      free(line);
      fclose(f);
      return false;
    }

    const char* ir = must_string(json_obj_get(root, "ir"), "record.ir");
    const char* k = must_string(json_obj_get(root, "k"), "record.k");
    if (!ir || !k) {
      free(line);
      fclose(f);
      return false;
    }
    if (strcmp(ir, "sir-v1.0") != 0) {
      fatalf("sircc: unsupported ir '%s' at %s:%zu (expected sir-v1.0)", ir, input_path, line_no);
      free(line);
      fclose(f);
      return false;
    }

    if (strcmp(k, "meta") == 0) {
      continue;
    }
    if (strcmp(k, "type") == 0) {
      if (!parse_type_record(p, root)) {
        free(line);
        fclose(f);
        return false;
      }
      continue;
    }
    if (strcmp(k, "node") == 0) {
      if (!parse_node_record(p, root)) {
        free(line);
        fclose(f);
        return false;
      }
      continue;
    }

    // Non-core records are currently ignored by sircc.
  }

  free(line);
  fclose(f);
  return true;
}

static TypeRec* get_type(SirProgram* p, int64_t id) {
  if (id < 0 || (size_t)id >= p->types_cap) return NULL;
  return p->types[id];
}

static NodeRec* get_node(SirProgram* p, int64_t id) {
  if (id < 0 || (size_t)id >= p->nodes_cap) return NULL;
  return p->nodes[id];
}

static LLVMTypeRef lower_type(SirProgram* p, LLVMContextRef ctx, int64_t id);

static LLVMTypeRef lower_type_prim(LLVMContextRef ctx, const char* prim) {
  if (strcmp(prim, "i1") == 0 || strcmp(prim, "bool") == 0) return LLVMInt1TypeInContext(ctx);
  if (strcmp(prim, "i8") == 0) return LLVMInt8TypeInContext(ctx);
  if (strcmp(prim, "i16") == 0) return LLVMInt16TypeInContext(ctx);
  if (strcmp(prim, "i32") == 0) return LLVMInt32TypeInContext(ctx);
  if (strcmp(prim, "i64") == 0) return LLVMInt64TypeInContext(ctx);
  if (strcmp(prim, "void") == 0) return LLVMVoidTypeInContext(ctx);
  return NULL;
}

static LLVMTypeRef lower_type(SirProgram* p, LLVMContextRef ctx, int64_t id) {
  TypeRec* tr = get_type(p, id);
  if (!tr) return NULL;
  if (tr->llvm) return tr->llvm;
  if (tr->resolving) return NULL;
  tr->resolving = true;

  LLVMTypeRef out = NULL;
  switch (tr->kind) {
    case TYPE_PRIM:
      out = lower_type_prim(ctx, tr->prim);
      break;
    case TYPE_PTR: {
      LLVMTypeRef of = lower_type(p, ctx, tr->of);
      if (of) out = LLVMPointerType(of, 0);
      break;
    }
    case TYPE_FN: {
      LLVMTypeRef ret = lower_type(p, ctx, tr->ret);
      if (!ret) break;
      LLVMTypeRef* params = NULL;
      if (tr->param_len) {
        params = (LLVMTypeRef*)malloc(tr->param_len * sizeof(LLVMTypeRef));
        if (!params) break;
        for (size_t i = 0; i < tr->param_len; i++) {
          params[i] = lower_type(p, ctx, tr->params[i]);
          if (!params[i]) {
            free(params);
            params = NULL;
            break;
          }
        }
      }
      if (tr->param_len == 0 || params) {
        out = LLVMFunctionType(ret, params, (unsigned)tr->param_len, tr->varargs ? 1 : 0);
      }
      free(params);
      break;
    }
    default:
      break;
  }

  tr->llvm = out;
  tr->resolving = false;
  return out;
}

typedef struct Binding {
  const char* name;
  LLVMValueRef value;
} Binding;

typedef struct FunctionCtx {
  SirProgram* p;
  LLVMContextRef ctx;
  LLVMModuleRef mod;
  LLVMBuilderRef builder;
  LLVMValueRef fn;

  Binding* binds;
  size_t bind_len;
  size_t bind_cap;
} FunctionCtx;

static bool bind_add(FunctionCtx* f, const char* name, LLVMValueRef v) {
  if (!name) return false;
  for (size_t i = 0; i < f->bind_len; i++) {
    if (strcmp(f->binds[i].name, name) == 0) return false;
  }
  if (f->bind_len == f->bind_cap) {
    size_t next = f->bind_cap ? f->bind_cap * 2 : 16;
    Binding* bigger = (Binding*)realloc(f->binds, next * sizeof(Binding));
    if (!bigger) return false;
    f->binds = bigger;
    f->bind_cap = next;
  }
  f->binds[f->bind_len++] = (Binding){.name = name, .value = v};
  return true;
}

static LLVMValueRef bind_get(FunctionCtx* f, const char* name) {
  for (size_t i = 0; i < f->bind_len; i++) {
    if (strcmp(f->binds[i].name, name) == 0) return f->binds[i].value;
  }
  return NULL;
}

static LLVMValueRef lower_expr(FunctionCtx* f, int64_t node_id);
static bool lower_stmt(FunctionCtx* f, int64_t node_id);

static LLVMValueRef lower_expr(FunctionCtx* f, int64_t node_id) {
  NodeRec* n = get_node(f->p, node_id);
  if (!n) {
    fatalf("sircc: unknown node id %lld", (long long)node_id);
    return NULL;
  }
  if (n->llvm_value) return n->llvm_value;
  if (n->resolving) {
    fatalf("sircc: cyclic node reference at %lld", (long long)node_id);
    return NULL;
  }
  n->resolving = true;

  LLVMValueRef out = NULL;

  if (strcmp(n->tag, "name") == 0) {
    const char* name = NULL;
    if (n->fields) name = json_get_string(json_obj_get(n->fields, "name"));
    if (!name) {
      fatalf("sircc: name node %lld missing fields.name", (long long)node_id);
      goto done;
    }
    out = bind_get(f, name);
    if (!out) fatalf("sircc: unknown name '%s' in node %lld", name, (long long)node_id);
    goto done;
  }

  if (strcmp(n->tag, "binop.add") == 0) {
    JsonValue* lhs = n->fields ? json_obj_get(n->fields, "lhs") : NULL;
    JsonValue* rhs = n->fields ? json_obj_get(n->fields, "rhs") : NULL;
    int64_t lhs_id = 0, rhs_id = 0;
    if (!parse_node_ref_id(lhs, &lhs_id) || !parse_node_ref_id(rhs, &rhs_id)) {
      fatalf("sircc: binop.add node %lld missing lhs/rhs refs", (long long)node_id);
      goto done;
    }
    LLVMValueRef a = lower_expr(f, lhs_id);
    LLVMValueRef b = lower_expr(f, rhs_id);
    if (!a || !b) goto done;
    LLVMTypeRef ty = LLVMTypeOf(a);
    if (LLVMGetTypeKind(ty) == LLVMIntegerTypeKind) {
      out = LLVMBuildAdd(f->builder, a, b, "add");
    } else {
      out = LLVMBuildFAdd(f->builder, a, b, "addf");
    }
    goto done;
  }

  if (strncmp(n->tag, "const.", 6) == 0) {
    const char* tyname = n->tag + 6;
    int64_t value = 0;
    if (!n->fields || !must_i64(json_obj_get(n->fields, "value"), &value, "const.value")) goto done;
    LLVMTypeRef ty = lower_type_prim(f->ctx, tyname);
    if (!ty) {
      fatalf("sircc: unsupported const type '%s'", tyname);
      goto done;
    }
    out = LLVMConstInt(ty, (unsigned long long)value, 1);
    goto done;
  }

  fatalf("sircc: unsupported expr tag '%s' (node %lld)", n->tag, (long long)node_id);

done:
  n->llvm_value = out;
  n->resolving = false;
  return out;
}

static bool lower_stmt(FunctionCtx* f, int64_t node_id) {
  NodeRec* n = get_node(f->p, node_id);
  if (!n) {
    fatalf("sircc: unknown stmt node %lld", (long long)node_id);
    return false;
  }

  if (strcmp(n->tag, "return") == 0) {
    JsonValue* v = n->fields ? json_obj_get(n->fields, "value") : NULL;
    int64_t vid = 0;
    if (!parse_node_ref_id(v, &vid)) {
      fatalf("sircc: return node %lld missing value ref", (long long)node_id);
      return false;
    }
    LLVMValueRef rv = lower_expr(f, vid);
    if (!rv) return false;
    LLVMBuildRet(f->builder, rv);
    return true;
  }

  if (strcmp(n->tag, "block") == 0) {
    JsonValue* stmts = n->fields ? json_obj_get(n->fields, "stmts") : NULL;
    if (!stmts || stmts->type != JSON_ARRAY) {
      fatalf("sircc: block node %lld missing stmts array", (long long)node_id);
      return false;
    }
    for (size_t i = 0; i < stmts->v.arr.len; i++) {
      int64_t sid = 0;
      if (!parse_node_ref_id(stmts->v.arr.items[i], &sid)) {
        fatalf("sircc: block node %lld has non-ref stmt", (long long)node_id);
        return false;
      }
      if (!lower_stmt(f, sid)) return false;
      if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(f->builder))) break;
    }
    return true;
  }

  fatalf("sircc: unsupported stmt tag '%s' (node %lld)", n->tag, (long long)node_id);
  return false;
}

static bool emit_module_ir(LLVMModuleRef mod, const char* out_path) {
  char* err = NULL;
  if (LLVMPrintModuleToFile(mod, out_path, &err) != 0) {
    fatalf("sircc: failed to write LLVM IR: %s", err ? err : "(unknown)");
    LLVMDisposeMessage(err);
    return false;
  }
  return true;
}

static bool emit_module_obj(LLVMModuleRef mod, const char* triple, const char* out_path) {
  if (LLVMInitializeNativeTarget() != 0) {
    fatalf("sircc: LLVMInitializeNativeTarget failed");
    return false;
  }
  LLVMInitializeNativeAsmPrinter();

  char* err = NULL;
  const char* use_triple = triple ? triple : LLVMGetDefaultTargetTriple();
  LLVMTargetRef target = NULL;
  if (LLVMGetTargetFromTriple(use_triple, &target, &err) != 0) {
    fatalf("sircc: target triple '%s' unsupported: %s", use_triple, err ? err : "(unknown)");
    LLVMDisposeMessage(err);
    if (!triple) LLVMDisposeMessage((char*)use_triple);
    return false;
  }

  LLVMTargetMachineRef tm =
      LLVMCreateTargetMachine(target, use_triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocDefault,
                              LLVMCodeModelDefault);
  if (!tm) {
    fatalf("sircc: failed to create target machine");
    if (!triple) LLVMDisposeMessage((char*)use_triple);
    return false;
  }

  LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
  char* dl_str = LLVMCopyStringRepOfTargetData(td);
  LLVMSetTarget(mod, use_triple);
  LLVMSetDataLayout(mod, dl_str);
  LLVMDisposeMessage(dl_str);
  LLVMDisposeTargetData(td);

  if (LLVMTargetMachineEmitToFile(tm, mod, (char*)out_path, LLVMObjectFile, &err) != 0) {
    fatalf("sircc: failed to emit object: %s", err ? err : "(unknown)");
    LLVMDisposeMessage(err);
    LLVMDisposeTargetMachine(tm);
    if (!triple) LLVMDisposeMessage((char*)use_triple);
    return false;
  }

  LLVMDisposeTargetMachine(tm);
  if (!triple) LLVMDisposeMessage((char*)use_triple);
  return true;
}

static bool run_clang_link(const char* clang_path, const char* obj_path, const char* out_path) {
  const char* clang = clang_path ? clang_path : "clang";

  pid_t pid = fork();
  if (pid < 0) {
    fatalf("sircc: fork failed: %s", strerror(errno));
    return false;
  }
  if (pid == 0) {
    execlp(clang, clang, "-o", out_path, obj_path, (char*)NULL);
    _exit(127);
  }
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) {
    fatalf("sircc: waitpid failed: %s", strerror(errno));
    return false;
  }
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    fatalf("sircc: clang failed (exit=%d)", WIFEXITED(st) ? WEXITSTATUS(st) : 1);
    return false;
  }
  return true;
}

static bool make_tmp_obj(char* out, size_t out_cap) {
  const char* dir = getenv("TMPDIR");
  if (!dir) dir = "/tmp";
  if (snprintf(out, out_cap, "%s/sircc-XXXXXX.o", dir) >= (int)out_cap) return false;
  int fd = mkstemps(out, 2);
  if (fd < 0) return false;
  close(fd);
  return true;
}

static bool lower_functions(SirProgram* p, LLVMContextRef ctx, LLVMModuleRef mod) {
  // Pass 1: create prototypes
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if (strcmp(n->tag, "fn") != 0) continue;

    const char* name = n->fields ? json_get_string(json_obj_get(n->fields, "name")) : NULL;
    if (!name) {
      fatalf("sircc: fn node %lld missing fields.name", (long long)n->id);
      return false;
    }
    if (n->type_ref == 0) {
      fatalf("sircc: fn node %lld missing type_ref", (long long)n->id);
      return false;
    }
    LLVMTypeRef fnty = lower_type(p, ctx, n->type_ref);
    if (!fnty || LLVMGetTypeKind(fnty) != LLVMFunctionTypeKind) {
      fatalf("sircc: fn node %lld has invalid function type_ref %lld", (long long)n->id, (long long)n->type_ref);
      return false;
    }
    LLVMValueRef fn = LLVMAddFunction(mod, name, fnty);
    n->llvm_value = fn;
  }

  // Pass 2: lower bodies
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if (strcmp(n->tag, "fn") != 0) continue;
    LLVMValueRef fn = n->llvm_value;
    if (!fn) continue;

    // Expression nodes are currently lowered relative to a specific function's builder. Clear any
    // previous per-node cached values before lowering a new function (constants + fn prototypes are safe).
    for (size_t j = 0; j < p->nodes_cap; j++) {
      NodeRec* x = p->nodes[j];
      if (!x) continue;
      if (strcmp(x->tag, "fn") == 0) continue;
      if (strncmp(x->tag, "const.", 6) == 0) continue;
      x->llvm_value = NULL;
      x->resolving = false;
    }

    JsonValue* paramsv = n->fields ? json_obj_get(n->fields, "params") : NULL;
    JsonValue* bodyv = n->fields ? json_obj_get(n->fields, "body") : NULL;
    if (!paramsv || paramsv->type != JSON_ARRAY) {
      fatalf("sircc: fn node %lld missing params array", (long long)n->id);
      return false;
    }
    int64_t body_id = 0;
    if (!parse_node_ref_id(bodyv, &body_id)) {
      fatalf("sircc: fn node %lld missing body ref", (long long)n->id);
      return false;
    }

    // Entry block + builder
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx, fn, "entry");
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(ctx);
    LLVMPositionBuilderAtEnd(builder, entry);

    FunctionCtx f = {.p = p, .ctx = ctx, .mod = mod, .builder = builder, .fn = fn};

    unsigned param_count = LLVMCountParams(fn);
    if (paramsv->v.arr.len != (size_t)param_count) {
      fatalf("sircc: fn node %lld param count mismatch: node has %zu, type has %u", (long long)n->id,
             paramsv->v.arr.len, param_count);
      LLVMDisposeBuilder(builder);
      free(f.binds);
      return false;
    }

    for (unsigned pi = 0; pi < param_count; pi++) {
      int64_t pid = 0;
      if (!parse_node_ref_id(paramsv->v.arr.items[pi], &pid)) {
        fatalf("sircc: fn node %lld has non-ref param", (long long)n->id);
        LLVMDisposeBuilder(builder);
        free(f.binds);
        return false;
      }
      NodeRec* pn = get_node(p, pid);
      if (!pn || strcmp(pn->tag, "param") != 0) {
        fatalf("sircc: fn node %lld param ref %lld is not a param node", (long long)n->id, (long long)pid);
        LLVMDisposeBuilder(builder);
        free(f.binds);
        return false;
      }
      const char* pname = pn->fields ? json_get_string(json_obj_get(pn->fields, "name")) : NULL;
      if (!pname) {
        fatalf("sircc: param node %lld missing fields.name", (long long)pid);
        LLVMDisposeBuilder(builder);
        free(f.binds);
        return false;
      }
      LLVMValueRef pv = LLVMGetParam(fn, pi);
      LLVMSetValueName2(pv, pname, strlen(pname));
      if (!bind_add(&f, pname, pv)) {
        fatalf("sircc: duplicate binding for '%s' in fn %lld", pname, (long long)n->id);
        LLVMDisposeBuilder(builder);
        free(f.binds);
        return false;
      }
    }

    if (!lower_stmt(&f, body_id)) {
      LLVMDisposeBuilder(builder);
      free(f.binds);
      return false;
    }

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
      // Conservative default: fallthrough returns 0 for integer returns, otherwise void.
      LLVMTypeRef rty = LLVMGetReturnType(LLVMGetElementType(LLVMTypeOf(fn)));
      if (LLVMGetTypeKind(rty) == LLVMVoidTypeKind) {
        LLVMBuildRetVoid(builder);
      } else if (LLVMGetTypeKind(rty) == LLVMIntegerTypeKind) {
        LLVMBuildRet(builder, LLVMConstInt(rty, 0, 0));
      } else {
        fatalf("sircc: fn %lld has implicit fallthrough with unsupported return type", (long long)n->id);
        LLVMDisposeBuilder(builder);
        free(f.binds);
        return false;
      }
    }

    LLVMDisposeBuilder(builder);
    free(f.binds);
  }

  return true;
}

bool sircc_compile(const SirccOptions* opt) {
  if (!opt || !opt->input_path || !opt->output_path) return false;

  SirProgram p = {0};
  arena_init(&p.arena);

  bool ok = parse_program(&p, opt->input_path);
  if (!ok) goto done;

  LLVMContextRef ctx = LLVMContextCreate();
  LLVMModuleRef mod = LLVMModuleCreateWithNameInContext("sir", ctx);

  if (!lower_functions(&p, ctx, mod)) {
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    ok = false;
    goto done;
  }

  char* verr = NULL;
  if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &verr) != 0) {
    fatalf("sircc: LLVM verification failed: %s", verr ? verr : "(unknown)");
    LLVMDisposeMessage(verr);
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    ok = false;
    goto done;
  }

  if (opt->emit == SIRCC_EMIT_LLVM_IR) {
    ok = emit_module_ir(mod, opt->output_path);
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    goto done;
  }

  if (opt->emit == SIRCC_EMIT_OBJ) {
    ok = emit_module_obj(mod, opt->target_triple, opt->output_path);
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    goto done;
  }

  char tmp_obj[4096];
  if (!make_tmp_obj(tmp_obj, sizeof(tmp_obj))) {
    fatalf("sircc: failed to create temporary object path");
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    ok = false;
    goto done;
  }

  ok = emit_module_obj(mod, opt->target_triple, tmp_obj);
  LLVMDisposeModule(mod);
  LLVMContextDispose(ctx);
  if (!ok) {
    unlink(tmp_obj);
    goto done;
  }

  ok = run_clang_link(opt->clang_path, tmp_obj, opt->output_path);
  unlink(tmp_obj);

done:
  free(p.types);
  free(p.nodes);
  arena_free(&p.arena);
  return ok;
}
