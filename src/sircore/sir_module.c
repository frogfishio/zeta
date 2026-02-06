#include "sir_module.h"

#include <stdlib.h>
#include <string.h>

enum {
  ZI_E_INVALID = -1,
  ZI_E_BOUNDS = -2,
  ZI_E_NOENT = -3,
  ZI_E_DENIED = -4,
  ZI_E_CLOSED = -5,
  ZI_E_AGAIN = -6,
  ZI_E_NOSYS = -7,
  ZI_E_OOM = -8,
  ZI_E_IO = -9,
  ZI_E_INTERNAL = -10,
};

typedef struct sir_dyn_bytes {
  uint8_t* p;
  uint32_t n;
  uint32_t cap;
} sir_dyn_bytes_t;

typedef struct sir_dyn_types {
  sir_type_t* p;
  uint32_t n;
  uint32_t cap;
} sir_dyn_types_t;

typedef struct sir_dyn_syms {
  sir_sym_t* p;
  uint32_t n;
  uint32_t cap;
} sir_dyn_syms_t;

typedef struct sir_dyn_funcs {
  sir_func_t* p;
  uint32_t n;
  uint32_t cap;
} sir_dyn_funcs_t;

typedef struct sir_dyn_insts {
  sir_inst_t* p;
  uint32_t n;
  uint32_t cap;
} sir_dyn_insts_t;

struct sir_module_builder {
  sir_dyn_types_t types;
  sir_dyn_syms_t syms;
  sir_dyn_funcs_t funcs;

  // Per-func instruction buffers (same order as funcs).
  sir_dyn_insts_t* func_insts;
  uint32_t func_insts_cap;

  // Owned string/bytes pool
  sir_dyn_bytes_t pool;

  sir_func_id_t entry;
  bool has_entry;
};

typedef struct sir_module_impl {
  sir_module_t pub;
  uint8_t* pool;
} sir_module_impl_t;

static sir_module_impl_t* module_impl_from_pub(sir_module_t* m) {
  if (!m) return NULL;
  return (sir_module_impl_t*)((uint8_t*)m - offsetof(sir_module_impl_t, pub));
}

static bool grow_bytes(sir_dyn_bytes_t* d, uint32_t add) {
  if (!d) return false;
  const uint64_t need64 = (uint64_t)d->n + (uint64_t)add;
  if (need64 > 0xFFFFFFFFull) return false;
  uint32_t need = (uint32_t)need64;
  if (need <= d->cap) return true;
  uint32_t cap = d->cap ? d->cap : 256;
  while (cap < need) cap *= 2;
  uint8_t* np = (uint8_t*)realloc(d->p, cap);
  if (!np) return false;
  d->p = np;
  d->cap = cap;
  return true;
}

static bool grow_types(sir_dyn_types_t* d, uint32_t add) {
  if (!d) return false;
  const uint64_t need64 = (uint64_t)d->n + (uint64_t)add;
  if (need64 > 0xFFFFFFFFull) return false;
  uint32_t need = (uint32_t)need64;
  if (need <= d->cap) return true;
  uint32_t cap = d->cap ? d->cap : 16;
  while (cap < need) cap *= 2;
  sir_type_t* np = (sir_type_t*)realloc(d->p, cap * sizeof(sir_type_t));
  if (!np) return false;
  d->p = np;
  d->cap = cap;
  return true;
}

static bool grow_syms(sir_dyn_syms_t* d, uint32_t add) {
  if (!d) return false;
  const uint64_t need64 = (uint64_t)d->n + (uint64_t)add;
  if (need64 > 0xFFFFFFFFull) return false;
  uint32_t need = (uint32_t)need64;
  if (need <= d->cap) return true;
  uint32_t cap = d->cap ? d->cap : 16;
  while (cap < need) cap *= 2;
  sir_sym_t* np = (sir_sym_t*)realloc(d->p, cap * sizeof(sir_sym_t));
  if (!np) return false;
  d->p = np;
  d->cap = cap;
  return true;
}

static bool grow_funcs(sir_dyn_funcs_t* d, uint32_t add) {
  if (!d) return false;
  const uint64_t need64 = (uint64_t)d->n + (uint64_t)add;
  if (need64 > 0xFFFFFFFFull) return false;
  uint32_t need = (uint32_t)need64;
  if (need <= d->cap) return true;
  uint32_t cap = d->cap ? d->cap : 8;
  while (cap < need) cap *= 2;
  sir_func_t* np = (sir_func_t*)realloc(d->p, cap * sizeof(sir_func_t));
  if (!np) return false;
  d->p = np;
  d->cap = cap;
  return true;
}

static bool grow_insts(sir_dyn_insts_t* d, uint32_t add) {
  if (!d) return false;
  const uint64_t need64 = (uint64_t)d->n + (uint64_t)add;
  if (need64 > 0xFFFFFFFFull) return false;
  uint32_t need = (uint32_t)need64;
  if (need <= d->cap) return true;
  uint32_t cap = d->cap ? d->cap : 32;
  while (cap < need) cap *= 2;
  sir_inst_t* np = (sir_inst_t*)realloc(d->p, cap * sizeof(sir_inst_t));
  if (!np) return false;
  d->p = np;
  d->cap = cap;
  return true;
}

static const uint8_t* pool_copy_bytes(sir_module_builder_t* b, const uint8_t* bytes, uint32_t len) {
  if (!b) return NULL;
  if (len && !bytes) return NULL;
  if (!grow_bytes(&b->pool, len)) return NULL;
  const uint32_t off = b->pool.n;
  if (len) memcpy(b->pool.p + off, bytes, len);
  b->pool.n += len;
  return b->pool.p + off;
}

static const char* pool_copy_cstr(sir_module_builder_t* b, const char* s) {
  if (!b || !s) return NULL;
  const uint32_t len = (uint32_t)strlen(s);
  const uint32_t need = len + 1;
  if (!grow_bytes(&b->pool, need)) return NULL;
  const uint32_t off = b->pool.n;
  memcpy(b->pool.p + off, s, need);
  b->pool.n += need;
  return (const char*)(b->pool.p + off);
}

sir_module_builder_t* sir_mb_new(void) {
  sir_module_builder_t* b = (sir_module_builder_t*)calloc(1, sizeof(*b));
  if (!b) return NULL;
  b->entry = 0;
  b->has_entry = false;
  return b;
}

void sir_mb_free(sir_module_builder_t* b) {
  if (!b) return;
  for (uint32_t i = 0; i < b->funcs.n; i++) {
    free(b->func_insts[i].p);
  }
  free(b->func_insts);
  free(b->types.p);
  free(b->syms.p);
  free(b->funcs.p);
  free(b->pool.p);
  free(b);
}

sir_type_id_t sir_mb_type_prim(sir_module_builder_t* b, sir_prim_type_t prim) {
  if (!b) return 0;
  if (prim == SIR_PRIM_INVALID) return 0;
  // Dedup: return existing prim type id if present.
  for (uint32_t i = 0; i < b->types.n; i++) {
    if (b->types.p[i].prim == prim) return i + 1;
  }
  if (!grow_types(&b->types, 1)) return 0;
  b->types.p[b->types.n++] = (sir_type_t){.prim = prim};
  return b->types.n;
}

sir_sym_id_t sir_mb_sym_extern_fn(sir_module_builder_t* b, const char* name, sir_sig_t sig) {
  if (!b || !name) return 0;
  if (!grow_syms(&b->syms, 1)) return 0;
  const char* nm = pool_copy_cstr(b, name);
  if (!nm) return 0;

  // Copy params/results arrays into pool as u32s (packed as bytes to keep a single pool).
  const uint32_t params_bytes = sig.param_count * (uint32_t)sizeof(sir_type_id_t);
  const uint32_t results_bytes = sig.result_count * (uint32_t)sizeof(sir_type_id_t);
  const uint8_t* params_p = pool_copy_bytes(b, (const uint8_t*)sig.params, params_bytes);
  const uint8_t* results_p = pool_copy_bytes(b, (const uint8_t*)sig.results, results_bytes);
  if ((sig.param_count && !params_p) || (sig.result_count && !results_p)) return 0;

  sir_sym_t s = {0};
  s.kind = SIR_SYM_EXTERN_FN;
  s.name = nm;
  s.sig.params = (const sir_type_id_t*)params_p;
  s.sig.param_count = sig.param_count;
  s.sig.results = (const sir_type_id_t*)results_p;
  s.sig.result_count = sig.result_count;

  b->syms.p[b->syms.n++] = s;
  return b->syms.n;
}

sir_func_id_t sir_mb_func_begin(sir_module_builder_t* b, const char* name) {
  if (!b || !name) return 0;
  if (!grow_funcs(&b->funcs, 1)) return 0;
  const char* nm = pool_copy_cstr(b, name);
  if (!nm) return 0;

  const uint32_t idx = b->funcs.n;
  b->funcs.p[b->funcs.n++] = (sir_func_t){.name = nm, .insts = NULL, .inst_count = 0, .value_count = 0};

  if (idx + 1 > b->func_insts_cap) {
    uint32_t cap = b->func_insts_cap ? b->func_insts_cap : 4;
    while (cap < idx + 1) cap *= 2;
    sir_dyn_insts_t* np = (sir_dyn_insts_t*)realloc(b->func_insts, cap * sizeof(sir_dyn_insts_t));
    if (!np) return 0;
    // zero new range
    for (uint32_t i = b->func_insts_cap; i < cap; i++) np[i] = (sir_dyn_insts_t){0};
    b->func_insts = np;
    b->func_insts_cap = cap;
  }

  return idx + 1;
}

bool sir_mb_func_set_entry(sir_module_builder_t* b, sir_func_id_t f) {
  if (!b) return false;
  if (f == 0 || f > b->funcs.n) return false;
  b->entry = f;
  b->has_entry = true;
  return true;
}

bool sir_mb_func_set_value_count(sir_module_builder_t* b, sir_func_id_t f, uint32_t value_count) {
  if (!b) return false;
  if (f == 0 || f > b->funcs.n) return false;
  b->funcs.p[f - 1].value_count = value_count;
  return true;
}

static bool emit_inst(sir_module_builder_t* b, sir_func_id_t f, sir_inst_t inst) {
  if (!b) return false;
  if (f == 0 || f > b->funcs.n) return false;
  sir_dyn_insts_t* d = &b->func_insts[f - 1];
  if (!grow_insts(d, 1)) return false;
  d->p[d->n++] = inst;
  return true;
}

bool sir_mb_emit_const_i32(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, int32_t v) {
  sir_inst_t i = {0};
  i.k = SIR_INST_CONST_I32;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.const_i32.v = v;
  i.u.const_i32.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_const_i64(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, int64_t v) {
  sir_inst_t i = {0};
  i.k = SIR_INST_CONST_I64;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.const_i64.v = v;
  i.u.const_i64.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_const_null_ptr(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst) {
  sir_inst_t i = {0};
  i.k = SIR_INST_CONST_PTR_NULL;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.const_null.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_const_bytes(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst_ptr, sir_val_id_t dst_len, const uint8_t* bytes,
                             uint32_t len) {
  if (!b) return false;
  const uint8_t* p = pool_copy_bytes(b, bytes, len);
  if (len && !p) return false;
  sir_inst_t i = {0};
  i.k = SIR_INST_CONST_BYTES;
  i.result_count = 2;
  i.results[0] = dst_ptr;
  i.results[1] = dst_len;
  i.u.const_bytes.bytes = p;
  i.u.const_bytes.len = len;
  i.u.const_bytes.dst_ptr = dst_ptr;
  i.u.const_bytes.dst_len = dst_len;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_call_extern(sir_module_builder_t* b, sir_func_id_t f, sir_sym_id_t callee, const sir_val_id_t* args, uint32_t arg_count) {
  if (!b) return false;
  const uint32_t args_bytes = arg_count * (uint32_t)sizeof(sir_val_id_t);
  const uint8_t* ap = pool_copy_bytes(b, (const uint8_t*)args, args_bytes);
  if (arg_count && !ap) return false;
  sir_inst_t i = {0};
  i.k = SIR_INST_CALL_EXTERN;
  i.result_count = 0;
  i.u.call_extern.callee = callee;
  i.u.call_extern.args = (const sir_val_id_t*)ap;
  i.u.call_extern.arg_count = arg_count;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_exit(sir_module_builder_t* b, sir_func_id_t f, int32_t code) {
  sir_inst_t i = {0};
  i.k = SIR_INST_EXIT;
  i.result_count = 0;
  i.u.exit_.code = code;
  return emit_inst(b, f, i);
}

static void* dup_mem(const void* p, size_t n) {
  if (n == 0) return NULL;
  void* r = malloc(n);
  if (!r) return NULL;
  memcpy(r, p, n);
  return r;
}

sir_module_t* sir_mb_finalize(sir_module_builder_t* b) {
  if (!b) return NULL;
  if (b->funcs.n == 0) return NULL;
  if (!b->has_entry) return NULL;
  if (b->entry == 0 || b->entry > b->funcs.n) return NULL;

  // Compute inst counts and duplicate per-func inst arrays.
  sir_func_t* funcs = (sir_func_t*)calloc(b->funcs.n, sizeof(sir_func_t));
  if (!funcs) return NULL;
  for (uint32_t fi = 0; fi < b->funcs.n; fi++) {
    funcs[fi] = b->funcs.p[fi];
    sir_dyn_insts_t* d = &b->func_insts[fi];
    if (d->n) {
      sir_inst_t* insts = (sir_inst_t*)dup_mem(d->p, (size_t)d->n * sizeof(sir_inst_t));
      if (!insts) {
        free(funcs);
        return NULL;
      }
      funcs[fi].insts = insts;
      funcs[fi].inst_count = d->n;
    } else {
      funcs[fi].insts = NULL;
      funcs[fi].inst_count = 0;
    }
  }

  // Duplicate types and syms.
  sir_type_t* types = NULL;
  if (b->types.n) {
    types = (sir_type_t*)dup_mem(b->types.p, (size_t)b->types.n * sizeof(sir_type_t));
    if (!types) {
      for (uint32_t fi = 0; fi < b->funcs.n; fi++) free((void*)funcs[fi].insts);
      free(funcs);
      return NULL;
    }
  }
  sir_sym_t* syms = NULL;
  if (b->syms.n) {
    syms = (sir_sym_t*)dup_mem(b->syms.p, (size_t)b->syms.n * sizeof(sir_sym_t));
    if (!syms) {
      free(types);
      for (uint32_t fi = 0; fi < b->funcs.n; fi++) free((void*)funcs[fi].insts);
      free(funcs);
      return NULL;
    }
  }

  // Copy pool.
  uint8_t* pool = NULL;
  if (b->pool.n) {
    pool = (uint8_t*)dup_mem(b->pool.p, b->pool.n);
    if (!pool) {
      free(syms);
      free(types);
      for (uint32_t fi = 0; fi < b->funcs.n; fi++) free((void*)funcs[fi].insts);
      free(funcs);
      return NULL;
    }
  }

  // Fix pointers in funcs/syms/insts into the copied pool.
  const intptr_t delta = (intptr_t)pool - (intptr_t)b->pool.p;
  for (uint32_t si = 0; si < b->syms.n; si++) {
    if (syms[si].name) syms[si].name = (const char*)((intptr_t)syms[si].name + delta);
    if (syms[si].sig.params) syms[si].sig.params = (const sir_type_id_t*)((intptr_t)syms[si].sig.params + delta);
    if (syms[si].sig.results) syms[si].sig.results = (const sir_type_id_t*)((intptr_t)syms[si].sig.results + delta);
  }
  for (uint32_t fi = 0; fi < b->funcs.n; fi++) {
    if (funcs[fi].name) funcs[fi].name = (const char*)((intptr_t)funcs[fi].name + delta);
    sir_inst_t* insts = (sir_inst_t*)funcs[fi].insts;
    for (uint32_t ii = 0; ii < funcs[fi].inst_count; ii++) {
      if (insts[ii].k == SIR_INST_CONST_BYTES) {
        if (insts[ii].u.const_bytes.bytes) insts[ii].u.const_bytes.bytes = (const uint8_t*)((intptr_t)insts[ii].u.const_bytes.bytes + delta);
      } else if (insts[ii].k == SIR_INST_CALL_EXTERN) {
        if (insts[ii].u.call_extern.args) insts[ii].u.call_extern.args = (const sir_val_id_t*)((intptr_t)insts[ii].u.call_extern.args + delta);
      }
    }
  }

  // Package module.
  sir_module_impl_t* impl = (sir_module_impl_t*)calloc(1, sizeof(*impl));
  if (!impl) {
    free(pool);
    free(syms);
    free(types);
    for (uint32_t fi = 0; fi < b->funcs.n; fi++) free((void*)funcs[fi].insts);
    free(funcs);
    return NULL;
  }

  impl->pool = pool;
  impl->pub = (sir_module_t){
      .types = types,
      .type_count = b->types.n,
      .syms = syms,
      .sym_count = b->syms.n,
      .funcs = funcs,
      .func_count = b->funcs.n,
      .entry = b->entry,
  };

  // free builder now? caller owns builder lifetime; leave it as-is.
  return &impl->pub;
}

void sir_module_free(sir_module_t* m) {
  if (!m) return;
  sir_module_impl_t* impl = module_impl_from_pub(m);
  if (!impl) return;

  const sir_module_t* pub = &impl->pub;
  if (pub->funcs) {
    for (uint32_t fi = 0; fi < pub->func_count; fi++) {
      free((void*)pub->funcs[fi].insts);
    }
  }
  free((void*)pub->funcs);
  free((void*)pub->syms);
  free((void*)pub->types);
  free(impl->pool);
  free(impl);
}

static const sir_sym_t* sym_at(const sir_module_t* m, sir_sym_id_t id) {
  if (!m) return NULL;
  if (id == 0 || id > m->sym_count) return NULL;
  return &m->syms[id - 1];
}

static int32_t exec_call_extern(const sir_module_t* m, sem_guest_mem_t* mem, sir_host_t host, const sir_inst_t* inst,
                                sir_value_t* vals, uint32_t val_count) {
  (void)mem;
  if (!m || !inst || !vals) return ZI_E_INTERNAL;
  const sir_sym_t* s = sym_at(m, inst->u.call_extern.callee);
  if (!s || s->kind != SIR_SYM_EXTERN_FN || !s->name) return ZI_E_NOENT;

  // MVP: dispatch by name to zABI primitives.
  const char* nm = s->name;
  const sir_val_id_t* args = inst->u.call_extern.args;
  const uint32_t n = inst->u.call_extern.arg_count;

  if (strcmp(nm, "zi_write") == 0) {
    if (!host.v.zi_write) return ZI_E_NOSYS;
    if (n != 3) return ZI_E_INVALID;
    const sir_val_id_t a0 = args[0], a1 = args[1], a2 = args[2];
    if (a0 >= val_count || a1 >= val_count || a2 >= val_count) return ZI_E_BOUNDS;
    const sir_value_t h = vals[a0];
    const sir_value_t p = vals[a1];
    const sir_value_t l = vals[a2];
    if (h.kind != SIR_VAL_I32) return ZI_E_INVALID;
    if (p.kind != SIR_VAL_PTR) return ZI_E_INVALID;
    if (l.kind != SIR_VAL_I64) return ZI_E_INVALID;
    if (l.u.i64 < 0 || l.u.i64 > 0x7FFFFFFFll) return ZI_E_INVALID;
    return host.v.zi_write(host.user, (zi_handle_t)h.u.i32, p.u.ptr, (zi_size32_t)l.u.i64);
  }

  if (strcmp(nm, "zi_end") == 0) {
    if (!host.v.zi_end) return ZI_E_NOSYS;
    if (n != 1) return ZI_E_INVALID;
    const sir_val_id_t a0 = args[0];
    if (a0 >= val_count) return ZI_E_BOUNDS;
    const sir_value_t h = vals[a0];
    if (h.kind != SIR_VAL_I32) return ZI_E_INVALID;
    return host.v.zi_end(host.user, (zi_handle_t)h.u.i32);
  }

  if (strcmp(nm, "zi_alloc") == 0) {
    if (!host.v.zi_alloc) return ZI_E_NOSYS;
    if (n != 1) return ZI_E_INVALID;
    const sir_val_id_t a0 = args[0];
    if (a0 >= val_count) return ZI_E_BOUNDS;
    const sir_value_t sz = vals[a0];
    if (sz.kind != SIR_VAL_I32) return ZI_E_INVALID;
    const zi_ptr_t p = host.v.zi_alloc(host.user, (zi_size32_t)sz.u.i32);
    return p ? 0 : ZI_E_OOM;
  }

  if (strcmp(nm, "zi_free") == 0) {
    if (!host.v.zi_free) return ZI_E_NOSYS;
    if (n != 1) return ZI_E_INVALID;
    const sir_val_id_t a0 = args[0];
    if (a0 >= val_count) return ZI_E_BOUNDS;
    const sir_value_t p = vals[a0];
    if (p.kind != SIR_VAL_PTR) return ZI_E_INVALID;
    return host.v.zi_free(host.user, p.u.ptr);
  }

  if (strcmp(nm, "zi_telemetry") == 0) {
    if (!host.v.zi_telemetry) return ZI_E_NOSYS;
    if (n != 4) return ZI_E_INVALID;
    const sir_val_id_t a0 = args[0], a1 = args[1], a2 = args[2], a3 = args[3];
    if (a0 >= val_count || a1 >= val_count || a2 >= val_count || a3 >= val_count) return ZI_E_BOUNDS;
    const sir_value_t tp = vals[a0];
    const sir_value_t tl = vals[a1];
    const sir_value_t mp = vals[a2];
    const sir_value_t ml = vals[a3];
    if (tp.kind != SIR_VAL_PTR || mp.kind != SIR_VAL_PTR) return ZI_E_INVALID;
    if (tl.kind != SIR_VAL_I32 || ml.kind != SIR_VAL_I32) return ZI_E_INVALID;
    return host.v.zi_telemetry(host.user, tp.u.ptr, (zi_size32_t)tl.u.i32, mp.u.ptr, (zi_size32_t)ml.u.i32);
  }

  return ZI_E_NOSYS;
}

int32_t sir_module_run(const sir_module_t* m, sem_guest_mem_t* mem, sir_host_t host) {
  if (!m || !mem) return ZI_E_INTERNAL;
  if (m->entry == 0 || m->entry > m->func_count) return ZI_E_INVALID;
  const sir_func_t* f = &m->funcs[m->entry - 1];

  if (f->value_count > 1u << 20) return ZI_E_INVALID;
  sir_value_t* vals = (sir_value_t*)calloc(f->value_count, sizeof(*vals));
  if (!vals) return ZI_E_OOM;

  int32_t exit_code = 0;
  for (uint32_t ip = 0; ip < f->inst_count; ip++) {
    const sir_inst_t* i = &f->insts[ip];
    switch (i->k) {
      case SIR_INST_CONST_I32: {
        if (i->u.const_i32.dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        vals[i->u.const_i32.dst] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = i->u.const_i32.v};
        break;
      }
      case SIR_INST_CONST_I64: {
        if (i->u.const_i64.dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        vals[i->u.const_i64.dst] = (sir_value_t){.kind = SIR_VAL_I64, .u.i64 = i->u.const_i64.v};
        break;
      }
      case SIR_INST_CONST_PTR_NULL: {
        if (i->u.const_null.dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        vals[i->u.const_null.dst] = (sir_value_t){.kind = SIR_VAL_PTR, .u.ptr = 0};
        break;
      }
      case SIR_INST_CONST_BYTES: {
        if (!host.v.zi_alloc) {
          free(vals);
          return ZI_E_NOSYS;
        }
        if (i->u.const_bytes.dst_ptr >= f->value_count || i->u.const_bytes.dst_len >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const zi_ptr_t p = host.v.zi_alloc(host.user, (zi_size32_t)i->u.const_bytes.len);
        if (!p && i->u.const_bytes.len) {
          free(vals);
          return ZI_E_OOM;
        }
        if (i->u.const_bytes.len) {
          uint8_t* w = NULL;
          if (!sem_guest_mem_map_rw(mem, p, (zi_size32_t)i->u.const_bytes.len, &w) || !w) {
            free(vals);
            return ZI_E_BOUNDS;
          }
          memcpy(w, i->u.const_bytes.bytes, i->u.const_bytes.len);
        }
        vals[i->u.const_bytes.dst_ptr] = (sir_value_t){.kind = SIR_VAL_PTR, .u.ptr = p};
        vals[i->u.const_bytes.dst_len] = (sir_value_t){.kind = SIR_VAL_I64, .u.i64 = (int64_t)i->u.const_bytes.len};
        break;
      }
      case SIR_INST_CALL_EXTERN: {
        const int32_t r = exec_call_extern(m, mem, host, i, vals, f->value_count);
        if (r < 0) {
          free(vals);
          return r;
        }
        break;
      }
      case SIR_INST_EXIT:
        exit_code = i->u.exit_.code;
        free(vals);
        return exit_code;
      default:
        free(vals);
        return ZI_E_INVALID;
    }
  }

  free(vals);
  return exit_code;
}
