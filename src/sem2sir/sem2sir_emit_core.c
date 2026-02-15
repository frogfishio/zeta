#include "sem2sir_emit_internal.h"

// -----------------
// Utilities
// -----------------

void emit_json_string(FILE *out, const char *s) {
  fputc('"', out);
  if (!s) {
    fputc('"', out);
    return;
  }
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    unsigned char c = *p;
    switch (c) {
    case '"':
      fputs("\\\"", out);
      break;
    case '\\':
      fputs("\\\\", out);
      break;
    case '\b':
      fputs("\\b", out);
      break;
    case '\f':
      fputs("\\f", out);
      break;
    case '\n':
      fputs("\\n", out);
      break;
    case '\r':
      fputs("\\r", out);
      break;
    case '\t':
      fputs("\\t", out);
      break;
    default:
      if (c < 0x20) {
        fprintf(out, "\\u%04x", (unsigned int)c);
      } else {
        fputc((int)c, out);
      }
    }
  }
  fputc('"', out);
}

char *read_file(const char *path, size_t *len_out) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;

  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long n = ftell(f);
  if (n < 0) {
    fclose(f);
    return NULL;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }

  char *buf = (char *)malloc((size_t)n);
  if (!buf) {
    fclose(f);
    return NULL;
  }

  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  if (got != (size_t)n) {
    free(buf);
    return NULL;
  }

  if (len_out)
    *len_out = (size_t)n;
  return buf;
}

void err(const char *in_path, const char *msg) {
  fprintf(stderr, "sem2sir: %s: %s\n", in_path ? in_path : "<input>", msg ? msg : "error");
}

bool json_peek_non_ws(GritJsonCursor *c, char *out) {
  if (!grit_json_skip_ws(c))
    return false;
  if (c->p >= c->end)
    return false;
  if (out)
    *out = *c->p;
  return true;
}

bool json_expect_key(GritJsonCursor *c, char **out_key) {
  if (!grit_json_parse_string_alloc(c, out_key))
    return false;
  if (!grit_json_consume_char(c, ':')) {
    free(*out_key);
    *out_key = NULL;
    return false;
  }
  return true;
}

bool parse_tok_text_alloc_strict(GritJsonCursor *c, const char *in_path, char **out_text) {
  *out_text = NULL;
  if (!grit_json_consume_char(c, '{')) {
    err(in_path, "expected token object");
    return false;
  }

  char *key = NULL;
  if (!json_expect_key(c, &key)) {
    err(in_path, "invalid token object key");
    return false;
  }
  if (strcmp(key, "k") != 0) {
    err(in_path, "token object must start with key 'k'");
    free(key);
    return false;
  }
  free(key);

  char *k_str = NULL;
  if (!grit_json_parse_string_alloc(c, &k_str)) {
    err(in_path, "token field k must be string");
    return false;
  }
  bool ok = (strcmp(k_str, "tok") == 0);
  free(k_str);
  if (!ok) {
    err(in_path, "expected k='tok' for token leaf");
    return false;
  }

  bool seen_text = false;
  char ch = 0;
  for (;;) {
    if (!json_peek_non_ws(c, &ch)) {
      err(in_path, "unexpected EOF in token object");
      return false;
    }
    if (ch == '}') {
      c->p++;
      break;
    }
    if (ch != ',') {
      err(in_path, "expected ',' or '}' in token object");
      return false;
    }
    c->p++;

    char *tkey = NULL;
    if (!json_expect_key(c, &tkey)) {
      err(in_path, "invalid token object key");
      return false;
    }
    if (strcmp(tkey, "text") == 0) {
      seen_text = true;
      if (!grit_json_parse_string_alloc(c, out_text)) {
        err(in_path, "tok.text must be a string");
        free(tkey);
        return false;
      }
    } else {
      if (!grit_json_skip_value(c)) {
        err(in_path, "invalid token value");
        free(tkey);
        return false;
      }
    }
    free(tkey);
  }

  if (!seen_text) {
    err(in_path, "tok requires field: text");
    free(*out_text);
    *out_text = NULL;
    return false;
  }
  return true;
}


// Forward decls needed by proc table helpers.
char *new_node_id(EmitCtx *ctx);
const char *sir_type_id_for(sem2sir_type_id t);
bool emit_typeinfo_if_needed(EmitCtx *ctx, const SemTypeInfo *ti);
bool emit_type_if_needed(EmitCtx *ctx, sem2sir_type_id t);
int type_align_bytes(sem2sir_type_id t);
const char *type_store_tag(sem2sir_type_id t);
const char *type_load_tag(sem2sir_type_id t);

void proc_table_free(EmitCtx *ctx) {
  for (size_t i = 0; i < ctx->proc_count; i++) {
    free(ctx->procs[i].name);
    free(ctx->procs[i].fn_id);
    free(ctx->procs[i].fn_type_id);
    free(ctx->procs[i].params);
  }
  free(ctx->procs);
  ctx->procs = NULL;
  ctx->proc_count = 0;
  ctx->proc_cap = 0;

  for (size_t i = 0; i < ctx->emitted_fn_type_count; i++) {
    free(ctx->emitted_fn_type_ids[i]);
  }
  free(ctx->emitted_fn_type_ids);
  ctx->emitted_fn_type_ids = NULL;
  ctx->emitted_fn_type_count = 0;
  ctx->emitted_fn_type_cap = 0;
}

ProcInfo *proc_table_find(EmitCtx *ctx, const char *name) {
  if (!name) return NULL;
  for (size_t i = 0; i < ctx->proc_count; i++) {
    if (strcmp(ctx->procs[i].name, name) == 0) return &ctx->procs[i];
  }
  return NULL;
}

bool proc_table_add(EmitCtx *ctx, const char *name, const SemTypeInfo *params, size_t param_count, SemTypeInfo ret_ti) {
  if (!name) return false;
  if (proc_table_find(ctx, name)) {
    err(ctx->in_path, "duplicate Proc name in Unit (no implicit overloading)");
    return false;
  }
  if (ctx->proc_count == ctx->proc_cap) {
    size_t next_cap = ctx->proc_cap ? ctx->proc_cap * 2 : 8;
    ProcInfo *next = (ProcInfo *)realloc(ctx->procs, next_cap * sizeof(ProcInfo));
    if (!next) return false;
    ctx->procs = next;
    ctx->proc_cap = next_cap;
  }
  ProcInfo *p = &ctx->procs[ctx->proc_count++];
  memset(p, 0, sizeof(*p));
  p->name = strdup(name);
  p->fn_id = new_node_id(ctx);
  {
    size_t n = strlen(name);
    // t:fn:<name>
    size_t cap = 5 + 3 + n + 1;
    p->fn_type_id = (char *)malloc(cap);
    if (!p->name || !p->fn_id || !p->fn_type_id) return false;
    snprintf(p->fn_type_id, cap, "t:fn:%s", name);
  }
  if (param_count) {
    p->params = (SemTypeInfo *)calloc(param_count, sizeof(SemTypeInfo));
    if (!p->params) return false;
    for (size_t i = 0; i < param_count; i++) {
      p->params[i] = params[i];
    }
  }
  p->param_count = param_count;
  p->ret = ret_ti.base;
  p->ret_ti = ret_ti;
  return true;
}

bool emit_fn_type_if_needed(EmitCtx *ctx, const ProcInfo *p) {
  if (!p || !p->fn_type_id) return false;
  const char *fn_type_id = p->fn_type_id;
  for (size_t i = 0; i < ctx->emitted_fn_type_count; i++) {
    if (strcmp(ctx->emitted_fn_type_ids[i], fn_type_id) == 0) return true;
  }

  if (ctx->emitted_fn_type_count == ctx->emitted_fn_type_cap) {
    size_t next_cap = ctx->emitted_fn_type_cap ? ctx->emitted_fn_type_cap * 2 : 8;
    char **next = (char **)realloc(ctx->emitted_fn_type_ids, next_cap * sizeof(char *));
    if (!next) return false;
    ctx->emitted_fn_type_ids = next;
    ctx->emitted_fn_type_cap = next_cap;
  }
  ctx->emitted_fn_type_ids[ctx->emitted_fn_type_count++] = strdup(fn_type_id);

  const SemTypeInfo *ret_ti = &p->ret_ti;
  if (ret_ti->base == SEM2SIR_TYPE_INVALID || !ret_ti->sir_id) {
    err(ctx->in_path, "function return type not supported");
    return false;
  }
  // Type defs must appear before uses.
  if (!emit_typeinfo_if_needed(ctx, ret_ti)) {
    return false;
  }
  for (size_t i = 0; i < p->param_count; i++) {
    if (p->params[i].base == SEM2SIR_TYPE_INVALID || !p->params[i].sir_id) {
      err(ctx->in_path, "function param type not supported");
      return false;
    }
    if (!emit_typeinfo_if_needed(ctx, &p->params[i])) {
      return false;
    }
  }
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":");
  emit_json_string(ctx->out, fn_type_id);
  fprintf(ctx->out, ",\"kind\":\"fn\",\"params\":[");
  for (size_t i = 0; i < p->param_count; i++) {
    if (i) fprintf(ctx->out, ",");
    emit_json_string(ctx->out, p->params[i].sir_id);
  }
  fprintf(ctx->out, "],\"ret\":");
  emit_json_string(ctx->out, ret_ti->sir_id);
  fprintf(ctx->out, "}\n");
  return true;
}

char *new_node_id(EmitCtx *ctx);

bool fn_build_new_block(SirFnBuild *fn, EmitCtx *ctx, size_t *out_idx) {
  if (!fn || !ctx || !out_idx)
    return false;
  if (fn->block_count == fn->block_cap) {
    size_t next_cap = fn->block_cap ? fn->block_cap * 2 : 8;
    SirBlockBuild *next = (SirBlockBuild *)realloc(fn->blocks, next_cap * sizeof(SirBlockBuild));
    if (!next)
      return false;
    fn->blocks = next;
    fn->block_cap = next_cap;
  }
  size_t idx = fn->block_count++;
  fn->blocks[idx] = (SirBlockBuild){0};
  fn->blocks[idx].id = new_node_id(ctx);
  *out_idx = idx;
  return true;
}

bool fn_build_append_stmt(SirFnBuild *fn, EmitCtx *ctx, char *stmt_id, bool is_terminator) {
  if (!fn || !ctx || !stmt_id)
    return false;
  SirBlockBuild *b = &fn->blocks[fn->cur_block];
  if (b->terminated) {
    err(ctx->in_path, "statement after terminator (no implicit control flow)");
    free(stmt_id);
    return false;
  }
  if (b->stmt_count == b->stmt_cap) {
    size_t next_cap = b->stmt_cap ? b->stmt_cap * 2 : 8;
    char **next = (char **)realloc(b->stmt_ids, next_cap * sizeof(char *));
    if (!next) {
      free(stmt_id);
      return false;
    }
    b->stmt_ids = next;
    b->stmt_cap = next_cap;
  }
  b->stmt_ids[b->stmt_count++] = stmt_id;
  if (is_terminator)
    b->terminated = true;
  return true;
}

bool fn_build_append_effects(SirFnBuild *fn, EmitCtx *ctx, StmtList *effects) {
  if (!effects || effects->count == 0)
    return true;
  for (size_t i = 0; i < effects->count; i++) {
    if (!fn_build_append_stmt(fn, ctx, effects->ids[i], false))
      return false;
    effects->ids[i] = NULL;
  }
  free(effects->ids);
  effects->ids = NULL;
  effects->count = 0;
  return true;
}

bool emit_term_ret(EmitCtx *ctx, sem2sir_type_id fn_ret, const char *value_id, char **out_term_id) {
  *out_term_id = NULL;
  char *ret_id = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, ret_id);
  fprintf(ctx->out, ",\"tag\":\"term.ret\",\"fields\":{");
  if (fn_ret != SEM2SIR_TYPE_VOID) {
    fprintf(ctx->out, "\"value\":{\"t\":\"ref\",\"id\":");
    emit_json_string(ctx->out, value_id);
    fprintf(ctx->out, "}");
  }
  fprintf(ctx->out, "}}\n");
  *out_term_id = ret_id;
  return true;
}

bool emit_term_br(EmitCtx *ctx, const char *to_block_id, char **out_term_id) {
  *out_term_id = NULL;
  char *br_id = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, br_id);
  fprintf(ctx->out, ",\"tag\":\"term.br\",\"fields\":{\"to\":{\"t\":\"ref\",\"id\":");
  emit_json_string(ctx->out, to_block_id);
  fprintf(ctx->out, "}}}\n");
  *out_term_id = br_id;
  return true;
}

bool emit_term_condbr(EmitCtx *ctx, const char *cond_id, const char *then_block_id, const char *else_block_id,
                             char **out_term_id) {
  *out_term_id = NULL;
  char *t_id = new_node_id(ctx);
  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":");
  emit_json_string(ctx->out, t_id);
  fprintf(ctx->out, ",\"tag\":\"term.condbr\",\"fields\":{\"cond\":{\"t\":\"ref\",\"id\":");
  emit_json_string(ctx->out, cond_id);
  fprintf(ctx->out, "},\"then\":{\"to\":{\"t\":\"ref\",\"id\":");
  emit_json_string(ctx->out, then_block_id);
  fprintf(ctx->out, "}},\"else\":{\"to\":{\"t\":\"ref\",\"id\":");
  emit_json_string(ctx->out, else_block_id);
  fprintf(ctx->out, "}}}}\n");
  *out_term_id = t_id;
  return true;
}

char *new_node_id(EmitCtx *ctx) {
  char buf[64];
  snprintf(buf, sizeof(buf), "n:%u", ctx->next_node++);
  return strdup(buf);
}

const char *sir_type_id_for(sem2sir_type_id t) {
  switch (t) {
  case SEM2SIR_TYPE_I32:
    return "t:i32";
  case SEM2SIR_TYPE_I64:
    return "t:i64";
  case SEM2SIR_TYPE_BOOL:
    return "t:bool";
  case SEM2SIR_TYPE_U8:
    return "t:u8";
  case SEM2SIR_TYPE_U32:
    return "t:u32";
  case SEM2SIR_TYPE_U64:
    return "t:u64";
  case SEM2SIR_TYPE_F64:
    return "t:f64";
  case SEM2SIR_TYPE_PTR:
    return "t:ptr";
  case SEM2SIR_TYPE_SLICE:
    return "t:slice";
  case SEM2SIR_TYPE_STRING_UTF8:
    return "t:string.utf8";
  case SEM2SIR_TYPE_VOID:
    return "t:void";
  default:
    return NULL;
  }
}

const char *sem_type_sanitize_for_id(const char *s) {
  // Types are from a closed normalized vocabulary; currently only string.utf8 needs sanitizing.
  // Returns a pointer to a static buffer.
  static char buf[64];
  size_t n = 0;
  for (const char *p = s; p && *p && n + 1 < sizeof(buf); p++) {
    char ch = *p;
    if (ch == '.')
      ch = '_';
    buf[n++] = ch;
  }
  buf[n] = '\0';
  return buf;
}

const char *get_derived_ptr_type_id(EmitCtx *ctx, sem2sir_type_id pointee) {
  if (!ctx)
    return NULL;
  if (pointee <= SEM2SIR_TYPE_INVALID || (size_t)pointee >= SEM2SIR_TYPE_COUNT)
    return NULL;
  if (ctx->derived_ptr_type_id[pointee])
    return ctx->derived_ptr_type_id[pointee];

  const char *pname = sem2sir_type_to_string(pointee);
  if (!pname)
    return NULL;
  const char *san = sem_type_sanitize_for_id(pname);

  size_t cap = strlen("t:p_") + strlen(san) + 1;
  char *id = (char *)malloc(cap);
  if (!id)
    return NULL;
  snprintf(id, cap, "t:p_%s", san);
  ctx->derived_ptr_type_id[pointee] = id;
  return id;
}

bool emit_derived_ptr_type_if_needed(EmitCtx *ctx, sem2sir_type_id pointee) {
  if (!ctx)
    return false;
  if (pointee <= SEM2SIR_TYPE_INVALID || (size_t)pointee >= SEM2SIR_TYPE_COUNT) {
    err(ctx->in_path, "ptr(T) pointee type out of range");
    return false;
  }
  if (pointee == SEM2SIR_TYPE_PTR || pointee == SEM2SIR_TYPE_SLICE) {
    err(ctx->in_path, "ptr(T) does not support ptr/slice pointees in sem2sir MVP");
    return false;
  }
  // MVP representability:
  // - allow ptr(void) as an opaque pointer type (cannot be deref'd/stored-through)
  // - otherwise, require load/store-capable value types
  if (pointee != SEM2SIR_TYPE_VOID) {
    if (!type_store_tag(pointee) || !type_load_tag(pointee) || type_align_bytes(pointee) == 0) {
      err(ctx->in_path, "ptr(T) pointee type not representable in sem2sir MVP");
      return false;
    }
  }

  if (ctx->emitted_derived_ptr_type[pointee])
    return true;

  const char *ptr_id = get_derived_ptr_type_id(ctx, pointee);
  const char *of_id = sir_type_id_for(pointee);
  if (!ptr_id || !of_id) {
    err(ctx->in_path, "failed to allocate derived ptr type id");
    return false;
  }
  if (!emit_type_if_needed(ctx, pointee))
    return false;

  fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":");
  emit_json_string(ctx->out, ptr_id);
  fprintf(ctx->out, ",\"kind\":\"ptr\",\"of\":");
  emit_json_string(ctx->out, of_id);
  fprintf(ctx->out, "}\n");

  ctx->emitted_derived_ptr_type[pointee] = true;
  return true;
}

bool emit_typeinfo_if_needed(EmitCtx *ctx, const SemTypeInfo *ti) {
  if (!ti)
    return false;
  if (ti->base == SEM2SIR_TYPE_INVALID) {
    err(ctx->in_path, "invalid type");
    return false;
  }
  if (ti->base == SEM2SIR_TYPE_PTR && ti->ptr_of != SEM2SIR_TYPE_INVALID) {
    return emit_derived_ptr_type_if_needed(ctx, ti->ptr_of);
  }
  return emit_type_if_needed(ctx, ti->base);
}

bool emit_type_if_needed(EmitCtx *ctx, sem2sir_type_id t) {
  if (t == SEM2SIR_TYPE_I32) {
    if (ctx->emitted_i32)
      return true;
    ctx->emitted_i32 = true;
    fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":\"t:i32\",\"kind\":\"prim\",\"prim\":\"i32\"}\n");
    return true;
  }
  if (t == SEM2SIR_TYPE_I64) {
    if (ctx->emitted_i64)
      return true;
    ctx->emitted_i64 = true;
    fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":\"t:i64\",\"kind\":\"prim\",\"prim\":\"i64\"}\n");
    return true;
  }
  if (t == SEM2SIR_TYPE_BOOL) {
    if (ctx->emitted_bool)
      return true;
    ctx->emitted_bool = true;
    fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":\"t:bool\",\"kind\":\"prim\",\"prim\":\"bool\"}\n");
    return true;
  }
  if (t == SEM2SIR_TYPE_U8) {
    if (ctx->emitted_u8)
      return true;
    ctx->emitted_u8 = true;
    fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":\"t:u8\",\"kind\":\"prim\",\"prim\":\"u8\"}\n");
    return true;
  }
  if (t == SEM2SIR_TYPE_U32) {
    if (ctx->emitted_u32)
      return true;
    ctx->emitted_u32 = true;
    fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":\"t:u32\",\"kind\":\"prim\",\"prim\":\"u32\"}\n");
    return true;
  }
  if (t == SEM2SIR_TYPE_U64) {
    if (ctx->emitted_u64)
      return true;
    ctx->emitted_u64 = true;
    fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":\"t:u64\",\"kind\":\"prim\",\"prim\":\"u64\"}\n");
    return true;
  }
  if (t == SEM2SIR_TYPE_F64) {
    if (ctx->emitted_f64)
      return true;
    ctx->emitted_f64 = true;
    fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":\"t:f64\",\"kind\":\"prim\",\"prim\":\"f64\"}\n");
    return true;
  }
  if (t == SEM2SIR_TYPE_PTR) {
    if (ctx->emitted_ptr)
      return true;
    ctx->emitted_ptr = true;
    fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":\"t:ptr\",\"kind\":\"prim\",\"prim\":\"ptr\"}\n");
    return true;
  }
  if (t == SEM2SIR_TYPE_SLICE) {
    if (ctx->emitted_slice)
      return true;
    ctx->emitted_slice = true;
    fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":\"t:slice\",\"kind\":\"prim\",\"prim\":\"slice\"}\n");
    return true;
  }
  if (t == SEM2SIR_TYPE_STRING_UTF8) {
    if (ctx->emitted_string_utf8)
      return true;
    ctx->emitted_string_utf8 = true;
    fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":\"t:string.utf8\",\"kind\":\"prim\",\"prim\":\"string.utf8\"}\n");
    return true;
  }
  if (t == SEM2SIR_TYPE_VOID) {
    if (ctx->emitted_void)
      return true;
    ctx->emitted_void = true;
    fprintf(ctx->out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":\"t:void\",\"kind\":\"prim\",\"prim\":\"void\"}\n");
    return true;
  }

  err(ctx->in_path, "type not supported for SIR emission (no implicit mapping)");
  return false;
}

// emit_fn_type_if_needed lives above, parameterized by fn_type_id.

bool locals_lookup(EmitCtx *ctx, const char *name, sem2sir_type_id *out_type, sem2sir_type_id *out_ptr_of,
                          const char **out_sir_type_id, bool *out_is_slot) {
  for (size_t i = 0; i < ctx->local_count; i++) {
    if (strcmp(ctx->locals[i].name, name) == 0) {
      if (out_type)
        *out_type = ctx->locals[i].type;
      if (out_ptr_of)
        *out_ptr_of = ctx->locals[i].ptr_of;
      if (out_sir_type_id)
        *out_sir_type_id = ctx->locals[i].sir_type_id;
      if (out_is_slot)
        *out_is_slot = ctx->locals[i].is_slot;
      return true;
    }
  }
  return false;
}

bool locals_push_binding(EmitCtx *ctx, const char *name, SemTypeInfo ti, bool is_slot) {
  Local *next = (Local *)realloc(ctx->locals, (ctx->local_count + 1) * sizeof(Local));
  if (!next)
    return false;
  ctx->locals = next;
  ctx->locals[ctx->local_count].name = strdup(name);
  ctx->locals[ctx->local_count].type = ti.base;
  ctx->locals[ctx->local_count].ptr_of = ti.ptr_of;
  ctx->locals[ctx->local_count].sir_type_id = ti.sir_id;
  ctx->locals[ctx->local_count].is_slot = is_slot;
  ctx->local_count++;
  return true;
}

bool type_supports_slot_storage(sem2sir_type_id t) {
  switch (t) {
  case SEM2SIR_TYPE_I32:
  case SEM2SIR_TYPE_I64:
  case SEM2SIR_TYPE_U8:
  case SEM2SIR_TYPE_F64:
  case SEM2SIR_TYPE_PTR:
    return true;
  default:
    return false;
  }
}

int type_align_bytes(sem2sir_type_id t) {
  switch (t) {
  case SEM2SIR_TYPE_I32:
    return 4;
  case SEM2SIR_TYPE_I64:
    return 8;
  case SEM2SIR_TYPE_U8:
    return 1;
  case SEM2SIR_TYPE_F64:
    return 8;
  case SEM2SIR_TYPE_PTR:
    return 8;
  default:
    return 0;
  }
}

const char *type_store_tag(sem2sir_type_id t) {
  switch (t) {
  case SEM2SIR_TYPE_I32:
    return "store.i32";
  case SEM2SIR_TYPE_I64:
    return "store.i64";
  case SEM2SIR_TYPE_U8:
    return "store.i8";
  case SEM2SIR_TYPE_F64:
    return "store.f64";
  case SEM2SIR_TYPE_PTR:
    return "store.ptr";
  default:
    return NULL;
  }
}

const char *type_load_tag(sem2sir_type_id t) {
  switch (t) {
  case SEM2SIR_TYPE_I32:
    return "load.i32";
  case SEM2SIR_TYPE_I64:
    return "load.i64";
  case SEM2SIR_TYPE_U8:
    return "load.i8";
  case SEM2SIR_TYPE_F64:
    return "load.f64";
  case SEM2SIR_TYPE_PTR:
    return "load.ptr";
  default:
    return NULL;
  }
}

void locals_free(EmitCtx *ctx) {
  for (size_t i = 0; i < ctx->local_count; i++)
    free(ctx->locals[i].name);
  free(ctx->locals);
  ctx->locals = NULL;
  ctx->local_count = 0;
}

// Forward decl: used by a few helpers that parse node objects.
