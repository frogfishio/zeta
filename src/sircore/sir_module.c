#include "sir_module.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static bool is_pow2_u32(uint32_t x);

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
  struct sir_pool_block* head;
  struct sir_pool_block* cur;
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

typedef struct sir_dyn_globals {
  sir_global_t* p;
  uint32_t n;
  uint32_t cap;
} sir_dyn_globals_t;

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
  sir_dyn_globals_t globals;
  sir_dyn_funcs_t funcs;

  // Per-func instruction buffers (same order as funcs).
  sir_dyn_insts_t* func_insts;
  uint32_t func_insts_cap;

  // Owned string/bytes pool
  sir_dyn_bytes_t pool;

  sir_func_id_t entry;
  bool has_entry;

  // Current source context applied to emitted instructions.
  uint32_t cur_src_node_id;
  uint32_t cur_src_line;
};

typedef struct sir_module_impl {
  sir_module_t pub;
  struct sir_pool_block* pool_head;
} sir_module_impl_t;

static sir_module_impl_t* module_impl_from_pub(sir_module_t* m) {
  if (!m) return NULL;
  return (sir_module_impl_t*)((uint8_t*)m - offsetof(sir_module_impl_t, pub));
}

typedef struct sir_pool_block {
  struct sir_pool_block* next;
  uint32_t cap;
  uint32_t len;
  uint8_t data[];
} sir_pool_block_t;

static void* pool_alloc(sir_dyn_bytes_t* d, uint32_t size) {
  if (!d) return NULL;
  if (size == 0) size = 1;
  sir_pool_block_t* b = (sir_pool_block_t*)d->cur;
  if (!b) {
    uint32_t cap = 4096;
    while (cap < size) cap *= 2;
    b = (sir_pool_block_t*)malloc(sizeof(*b) + cap);
    if (!b) return NULL;
    b->next = NULL;
    b->cap = cap;
    b->len = 0;
    d->head = (struct sir_pool_block*)b;
    d->cur = (struct sir_pool_block*)b;
  }

  // Align to 4 bytes for u32 arrays stored in the pool.
  uint32_t aligned = (b->len + 3u) & ~3u;
  if (aligned + size > b->cap) {
    uint32_t cap = b->cap * 2u;
    while (cap < size) cap *= 2u;
    sir_pool_block_t* n = (sir_pool_block_t*)malloc(sizeof(*n) + cap);
    if (!n) return NULL;
    n->next = NULL;
    n->cap = cap;
    n->len = 0;
    b->next = n;
    d->cur = (struct sir_pool_block*)n;
    b = n;
    aligned = 0;
  }

  void* p = b->data + aligned;
  b->len = aligned + size;
  return p;
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

static bool grow_globals(sir_dyn_globals_t* d, uint32_t add) {
  if (!d) return false;
  const uint64_t need64 = (uint64_t)d->n + (uint64_t)add;
  if (need64 > 0xFFFFFFFFull) return false;
  uint32_t need = (uint32_t)need64;
  if (need <= d->cap) return true;
  uint32_t cap = d->cap ? d->cap : 16;
  while (cap < need) cap *= 2;
  sir_global_t* np = (sir_global_t*)realloc(d->p, cap * sizeof(sir_global_t));
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
  uint8_t* p = (uint8_t*)pool_alloc(&b->pool, len);
  if (!p && len) return NULL;
  if (len) memcpy(p, bytes, len);
  return p;
}

static const char* pool_copy_cstr(sir_module_builder_t* b, const char* s) {
  if (!b || !s) return NULL;
  const uint32_t len = (uint32_t)strlen(s);
  const uint32_t need = len + 1;
  char* p = (char*)pool_alloc(&b->pool, need);
  if (!p) return NULL;
  memcpy(p, s, need);
  return p;
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
  free(b->globals.p);
  free(b->funcs.p);
  sir_pool_block_t* pb = (sir_pool_block_t*)b->pool.head;
  while (pb) {
    sir_pool_block_t* next = pb->next;
    free(pb);
    pb = next;
  }
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

sir_global_id_t sir_mb_global(sir_module_builder_t* b, const char* name, uint32_t size, uint32_t align, const uint8_t* init_bytes,
                              uint32_t init_len) {
  if (!b || !name) return 0;
  if (size == 0) return 0;
  if (align == 0) align = 1;
  if (init_len > size) return 0;
  if (!grow_globals(&b->globals, 1)) return 0;
  const char* nm = pool_copy_cstr(b, name);
  if (!nm) return 0;
  const uint8_t* ib = NULL;
  if (init_len) {
    ib = pool_copy_bytes(b, init_bytes, init_len);
    if (!ib) return 0;
  }
  b->globals.p[b->globals.n++] =
      (sir_global_t){.name = nm, .size = size, .align = align, .init_bytes = ib, .init_len = init_len};
  return b->globals.n;
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

bool sir_mb_func_set_sig(sir_module_builder_t* b, sir_func_id_t f, sir_sig_t sig) {
  if (!b) return false;
  if (f == 0 || f > b->funcs.n) return false;

  const uint32_t params_bytes = sig.param_count * (uint32_t)sizeof(sir_type_id_t);
  const uint32_t results_bytes = sig.result_count * (uint32_t)sizeof(sir_type_id_t);
  const uint8_t* params_p = pool_copy_bytes(b, (const uint8_t*)sig.params, params_bytes);
  const uint8_t* results_p = pool_copy_bytes(b, (const uint8_t*)sig.results, results_bytes);
  if ((sig.param_count && !params_p) || (sig.result_count && !results_p)) return false;

  b->funcs.p[f - 1].sig.params = (const sir_type_id_t*)params_p;
  b->funcs.p[f - 1].sig.param_count = sig.param_count;
  b->funcs.p[f - 1].sig.results = (const sir_type_id_t*)results_p;
  b->funcs.p[f - 1].sig.result_count = sig.result_count;
  return true;
}

void sir_mb_set_src(sir_module_builder_t* b, uint32_t node_id, uint32_t line) {
  if (!b) return;
  b->cur_src_node_id = node_id;
  b->cur_src_line = line;
}

void sir_mb_clear_src(sir_module_builder_t* b) {
  if (!b) return;
  b->cur_src_node_id = 0;
  b->cur_src_line = 0;
}

static bool emit_inst(sir_module_builder_t* b, sir_func_id_t f, sir_inst_t inst) {
  if (!b) return false;
  if (f == 0 || f > b->funcs.n) return false;
  sir_dyn_insts_t* d = &b->func_insts[f - 1];
  if (!grow_insts(d, 1)) return false;
  inst.src_node_id = b->cur_src_node_id;
  inst.src_line = b->cur_src_line;
  d->p[d->n++] = inst;
  return true;
}

bool sir_mb_emit_const_i1(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, bool v) {
  sir_inst_t i = {0};
  i.k = SIR_INST_CONST_I1;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.const_i1.v = v ? 1 : 0;
  i.u.const_i1.dst = dst;
  return emit_inst(b, f, i);
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

bool sir_mb_emit_const_bool(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, bool v) {
  sir_inst_t i = {0};
  i.k = SIR_INST_CONST_BOOL;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.const_bool.v = v ? 1 : 0;
  i.u.const_bool.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_const_ptr(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, zi_ptr_t v) {
  sir_inst_t i = {0};
  i.k = SIR_INST_CONST_PTR;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.const_ptr.v = v;
  i.u.const_ptr.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_const_i8(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, uint8_t v) {
  sir_inst_t i = {0};
  i.k = SIR_INST_CONST_I8;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.const_i8.v = v;
  i.u.const_i8.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_const_i16(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, uint16_t v) {
  sir_inst_t i = {0};
  i.k = SIR_INST_CONST_I16;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.const_i16.v = v;
  i.u.const_i16.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_const_f32_bits(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, uint32_t bits) {
  sir_inst_t i = {0};
  i.k = SIR_INST_CONST_F32;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.const_f32.bits = bits;
  i.u.const_f32.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_const_f64_bits(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, uint64_t bits) {
  sir_inst_t i = {0};
  i.k = SIR_INST_CONST_F64;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.const_f64.bits = bits;
  i.u.const_f64.dst = dst;
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

bool sir_mb_emit_i32_add(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  sir_inst_t i = {0};
  i.k = SIR_INST_I32_ADD;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.i32_add.a = a;
  i.u.i32_add.b = b_;
  i.u.i32_add.dst = dst;
  return emit_inst(b, f, i);
}

static bool emit_i32_bin(sir_module_builder_t* b, sir_func_id_t f, sir_inst_kind_t k, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  sir_inst_t i = {0};
  i.k = k;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.i32_add.a = a;
  i.u.i32_add.b = b_;
  i.u.i32_add.dst = dst;
  return emit_inst(b, f, i);
}

static bool emit_i32_un(sir_module_builder_t* b, sir_func_id_t f, sir_inst_kind_t k, sir_val_id_t dst, sir_val_id_t x) {
  sir_inst_t i = {0};
  i.k = k;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.i32_un.x = x;
  i.u.i32_un.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_i32_sub(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_i32_bin(b, f, SIR_INST_I32_SUB, dst, a, b_);
}
bool sir_mb_emit_i32_mul(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_i32_bin(b, f, SIR_INST_I32_MUL, dst, a, b_);
}
bool sir_mb_emit_i32_and(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_i32_bin(b, f, SIR_INST_I32_AND, dst, a, b_);
}
bool sir_mb_emit_i32_or(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_i32_bin(b, f, SIR_INST_I32_OR, dst, a, b_);
}
bool sir_mb_emit_i32_xor(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_i32_bin(b, f, SIR_INST_I32_XOR, dst, a, b_);
}
bool sir_mb_emit_i32_not(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x) {
  return emit_i32_un(b, f, SIR_INST_I32_NOT, dst, x);
}
bool sir_mb_emit_i32_neg(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x) {
  return emit_i32_un(b, f, SIR_INST_I32_NEG, dst, x);
}
bool sir_mb_emit_i32_shl(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x, sir_val_id_t shift) {
  return emit_i32_bin(b, f, SIR_INST_I32_SHL, dst, x, shift);
}
bool sir_mb_emit_i32_shr_s(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x, sir_val_id_t shift) {
  return emit_i32_bin(b, f, SIR_INST_I32_SHR_S, dst, x, shift);
}
bool sir_mb_emit_i32_shr_u(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x, sir_val_id_t shift) {
  return emit_i32_bin(b, f, SIR_INST_I32_SHR_U, dst, x, shift);
}
bool sir_mb_emit_i32_div_s_sat(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_i32_bin(b, f, SIR_INST_I32_DIV_S_SAT, dst, a, b_);
}
bool sir_mb_emit_i32_div_s_trap(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_i32_bin(b, f, SIR_INST_I32_DIV_S_TRAP, dst, a, b_);
}
bool sir_mb_emit_i32_div_u_sat(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_i32_bin(b, f, SIR_INST_I32_DIV_U_SAT, dst, a, b_);
}
bool sir_mb_emit_i32_rem_s_sat(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_i32_bin(b, f, SIR_INST_I32_REM_S_SAT, dst, a, b_);
}
bool sir_mb_emit_i32_rem_u_sat(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_i32_bin(b, f, SIR_INST_I32_REM_U_SAT, dst, a, b_);
}

bool sir_mb_emit_i32_cmp_eq(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  sir_inst_t i = {0};
  i.k = SIR_INST_I32_CMP_EQ;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.i32_cmp_eq.a = a;
  i.u.i32_cmp_eq.b = b_;
  i.u.i32_cmp_eq.dst = dst;
  return emit_inst(b, f, i);
}

static bool emit_i32_cmp(sir_module_builder_t* b, sir_func_id_t f, sir_inst_kind_t k, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  sir_inst_t i = {0};
  i.k = k;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.i32_cmp_eq.a = a;
  i.u.i32_cmp_eq.b = b_;
  i.u.i32_cmp_eq.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_i32_cmp_ne(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_i32_cmp(b, f, SIR_INST_I32_CMP_NE, dst, a, b_);
}
bool sir_mb_emit_i32_cmp_slt(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_i32_cmp(b, f, SIR_INST_I32_CMP_SLT, dst, a, b_);
}
bool sir_mb_emit_i32_cmp_sle(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_i32_cmp(b, f, SIR_INST_I32_CMP_SLE, dst, a, b_);
}
bool sir_mb_emit_i32_cmp_sgt(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_i32_cmp(b, f, SIR_INST_I32_CMP_SGT, dst, a, b_);
}
bool sir_mb_emit_i32_cmp_sge(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_i32_cmp(b, f, SIR_INST_I32_CMP_SGE, dst, a, b_);
}
bool sir_mb_emit_i32_cmp_ult(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_i32_cmp(b, f, SIR_INST_I32_CMP_ULT, dst, a, b_);
}
bool sir_mb_emit_i32_cmp_ule(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_i32_cmp(b, f, SIR_INST_I32_CMP_ULE, dst, a, b_);
}
bool sir_mb_emit_i32_cmp_ugt(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_i32_cmp(b, f, SIR_INST_I32_CMP_UGT, dst, a, b_);
}
bool sir_mb_emit_i32_cmp_uge(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_i32_cmp(b, f, SIR_INST_I32_CMP_UGE, dst, a, b_);
}

static bool emit_f_cmp(sir_module_builder_t* b, sir_func_id_t f, sir_inst_kind_t k, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  sir_inst_t i = {0};
  i.k = k;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.f_cmp.a = a;
  i.u.f_cmp.b = b_;
  i.u.f_cmp.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_f32_cmp_ueq(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_f_cmp(b, f, SIR_INST_F32_CMP_UEQ, dst, a, b_);
}

bool sir_mb_emit_f64_cmp_olt(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_f_cmp(b, f, SIR_INST_F64_CMP_OLT, dst, a, b_);
}

bool sir_mb_emit_i32_trunc_i64(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x) {
  sir_inst_t i = {0};
  i.k = SIR_INST_I32_TRUNC_I64;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.i32_trunc_i64.x = x;
  i.u.i32_trunc_i64.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_i32_zext_i8(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x) {
  sir_inst_t i = {0};
  i.k = SIR_INST_I32_ZEXT_I8;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.i32_zext_i8.x = x;
  i.u.i32_zext_i8.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_i32_zext_i16(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x) {
  sir_inst_t i = {0};
  i.k = SIR_INST_I32_ZEXT_I16;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.i32_zext_i16.x = x;
  i.u.i32_zext_i16.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_i64_zext_i32(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x) {
  sir_inst_t i = {0};
  i.k = SIR_INST_I64_ZEXT_I32;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.i64_zext_i32.x = x;
  i.u.i64_zext_i32.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_global_addr(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_global_id_t gid) {
  sir_inst_t i = {0};
  i.k = SIR_INST_GLOBAL_ADDR;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.global_addr.gid = gid;
  i.u.global_addr.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_ptr_offset(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t base, sir_val_id_t index, uint32_t scale) {
  sir_inst_t i = {0};
  i.k = SIR_INST_PTR_OFFSET;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.ptr_offset.base = base;
  i.u.ptr_offset.index = index;
  i.u.ptr_offset.scale = scale;
  i.u.ptr_offset.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_ptr_add(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t base, sir_val_id_t off) {
  sir_inst_t i = {0};
  i.k = SIR_INST_PTR_ADD;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.ptr_add.base = base;
  i.u.ptr_add.off = off;
  i.u.ptr_add.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_ptr_sub(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t base, sir_val_id_t off) {
  sir_inst_t i = {0};
  i.k = SIR_INST_PTR_SUB;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.ptr_sub.base = base;
  i.u.ptr_sub.off = off;
  i.u.ptr_sub.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_ptr_cmp_eq(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  sir_inst_t i = {0};
  i.k = SIR_INST_PTR_CMP_EQ;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.ptr_cmp.a = a;
  i.u.ptr_cmp.b = b_;
  i.u.ptr_cmp.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_ptr_cmp_ne(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  sir_inst_t i = {0};
  i.k = SIR_INST_PTR_CMP_NE;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.ptr_cmp.a = a;
  i.u.ptr_cmp.b = b_;
  i.u.ptr_cmp.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_ptr_to_i64(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x) {
  sir_inst_t i = {0};
  i.k = SIR_INST_PTR_TO_I64;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.ptr_to_i64.x = x;
  i.u.ptr_to_i64.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_ptr_from_i64(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x) {
  sir_inst_t i = {0};
  i.k = SIR_INST_PTR_FROM_I64;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.ptr_from_i64.x = x;
  i.u.ptr_from_i64.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_bool_not(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t x) {
  sir_inst_t i = {0};
  i.k = SIR_INST_BOOL_NOT;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.bool_not.x = x;
  i.u.bool_not.dst = dst;
  return emit_inst(b, f, i);
}

static bool emit_bool_bin(sir_module_builder_t* b, sir_func_id_t f, sir_inst_kind_t k, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  sir_inst_t i = {0};
  i.k = k;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.bool_bin.a = a;
  i.u.bool_bin.b = b_;
  i.u.bool_bin.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_bool_and(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_bool_bin(b, f, SIR_INST_BOOL_AND, dst, a, b_);
}
bool sir_mb_emit_bool_or(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_bool_bin(b, f, SIR_INST_BOOL_OR, dst, a, b_);
}
bool sir_mb_emit_bool_xor(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t a, sir_val_id_t b_) {
  return emit_bool_bin(b, f, SIR_INST_BOOL_XOR, dst, a, b_);
}

bool sir_mb_emit_select(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t cond, sir_val_id_t a, sir_val_id_t b_) {
  sir_inst_t i = {0};
  i.k = SIR_INST_SELECT;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.select.cond = cond;
  i.u.select.a = a;
  i.u.select.b = b_;
  i.u.select.dst = dst;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_br_args(sir_module_builder_t* b, sir_func_id_t f, uint32_t target_ip, const sir_val_id_t* src_slots, const sir_val_id_t* dst_slots,
                         uint32_t arg_count, uint32_t* out_ip) {
  if (!b) return false;
  if (f == 0 || f > b->funcs.n) return false;
  sir_dyn_insts_t* d = &b->func_insts[f - 1];
  const uint32_t ip = d->n;

  const uint32_t bytes = arg_count * (uint32_t)sizeof(sir_val_id_t);
  const uint8_t* sp = NULL;
  const uint8_t* dp = NULL;
  if (arg_count) {
    if (!src_slots || !dst_slots) return false;
    sp = pool_copy_bytes(b, (const uint8_t*)src_slots, bytes);
    if (!sp) return false;
    dp = pool_copy_bytes(b, (const uint8_t*)dst_slots, bytes);
    if (!dp) return false;
  }

  sir_inst_t i = {0};
  i.k = SIR_INST_BR;
  i.result_count = 0;
  i.u.br.target_ip = target_ip;
  i.u.br.src_slots = (const sir_val_id_t*)sp;
  i.u.br.dst_slots = (const sir_val_id_t*)dp;
  i.u.br.arg_count = arg_count;
  if (!emit_inst(b, f, i)) return false;
  if (out_ip) *out_ip = ip;
  return true;
}

bool sir_mb_emit_br(sir_module_builder_t* b, sir_func_id_t f, uint32_t target_ip, uint32_t* out_ip) {
  return sir_mb_emit_br_args(b, f, target_ip, NULL, NULL, 0, out_ip);
}

bool sir_mb_emit_cbr(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t cond, uint32_t then_ip, uint32_t else_ip, uint32_t* out_ip) {
  if (!b) return false;
  if (f == 0 || f > b->funcs.n) return false;
  sir_dyn_insts_t* d = &b->func_insts[f - 1];
  const uint32_t ip = d->n;
  sir_inst_t i = {0};
  i.k = SIR_INST_CBR;
  i.result_count = 0;
  i.u.cbr.cond = cond;
  i.u.cbr.then_ip = then_ip;
  i.u.cbr.else_ip = else_ip;
  if (!emit_inst(b, f, i)) return false;
  if (out_ip) *out_ip = ip;
  return true;
}

bool sir_mb_emit_switch(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t scrut, const int32_t* case_lits, const uint32_t* case_target,
                        uint32_t case_count, uint32_t default_ip, uint32_t* out_ip) {
  if (!b) return false;
  if (f == 0 || f > b->funcs.n) return false;
  sir_dyn_insts_t* d = &b->func_insts[f - 1];
  const uint32_t ip = d->n;

  if (case_count && (!case_lits || !case_target)) return false;
  const uint32_t lits_bytes = case_count * (uint32_t)sizeof(int32_t);
  const uint32_t tgt_bytes = case_count * (uint32_t)sizeof(uint32_t);
  const uint8_t* lp = NULL;
  const uint8_t* tp = NULL;
  if (case_count) {
    lp = pool_copy_bytes(b, (const uint8_t*)case_lits, lits_bytes);
    if (!lp) return false;
    tp = pool_copy_bytes(b, (const uint8_t*)case_target, tgt_bytes);
    if (!tp) return false;
  }

  sir_inst_t i = {0};
  i.k = SIR_INST_SWITCH;
  i.result_count = 0;
  i.u.sw.scrut = scrut;
  i.u.sw.case_lits = (const int32_t*)lp;
  i.u.sw.case_target = (const uint32_t*)tp;
  i.u.sw.case_count = case_count;
  i.u.sw.default_ip = default_ip;
  if (!emit_inst(b, f, i)) return false;
  if (out_ip) *out_ip = ip;
  return true;
}

bool sir_mb_emit_mem_copy(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t src, sir_val_id_t len, bool overlap_allow) {
  sir_inst_t i = {0};
  i.k = SIR_INST_MEM_COPY;
  i.result_count = 0;
  i.u.mem_copy.dst = dst;
  i.u.mem_copy.src = src;
  i.u.mem_copy.len = len;
  i.u.mem_copy.overlap_allow = overlap_allow ? 1 : 0;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_mem_fill(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t byte, sir_val_id_t len) {
  sir_inst_t i = {0};
  i.k = SIR_INST_MEM_FILL;
  i.result_count = 0;
  i.u.mem_fill.dst = dst;
  i.u.mem_fill.byte = byte;
  i.u.mem_fill.len = len;
  return emit_inst(b, f, i);
}

static bool emit_atomic_rmw(sir_module_builder_t* b, sir_func_id_t f, sir_inst_kind_t k, sir_val_id_t dst_old, sir_val_id_t addr, sir_val_id_t value,
                            sir_atomic_rmw_op_t op, uint32_t align) {
  if (!b) return false;
  sir_inst_t i = {0};
  i.k = k;
  i.result_count = 1;
  i.results[0] = dst_old;
  i.u.atomic_rmw.dst_old = dst_old;
  i.u.atomic_rmw.addr = addr;
  i.u.atomic_rmw.value = value;
  i.u.atomic_rmw.op = op;
  i.u.atomic_rmw.align = align ? align : 1u;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_atomic_rmw_i8(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst_old, sir_val_id_t addr, sir_val_id_t value,
                               sir_atomic_rmw_op_t op, uint32_t align) {
  return emit_atomic_rmw(b, f, SIR_INST_ATOMIC_RMW_I8, dst_old, addr, value, op, align);
}

bool sir_mb_emit_atomic_rmw_i16(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst_old, sir_val_id_t addr, sir_val_id_t value,
                                sir_atomic_rmw_op_t op, uint32_t align) {
  return emit_atomic_rmw(b, f, SIR_INST_ATOMIC_RMW_I16, dst_old, addr, value, op, align);
}

bool sir_mb_emit_atomic_rmw_i32(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst_old, sir_val_id_t addr, sir_val_id_t value,
                                sir_atomic_rmw_op_t op, uint32_t align) {
  return emit_atomic_rmw(b, f, SIR_INST_ATOMIC_RMW_I32, dst_old, addr, value, op, align);
}

bool sir_mb_emit_atomic_rmw_i64(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst_old, sir_val_id_t addr, sir_val_id_t value,
                                sir_atomic_rmw_op_t op, uint32_t align) {
  return emit_atomic_rmw(b, f, SIR_INST_ATOMIC_RMW_I64, dst_old, addr, value, op, align);
}

bool sir_mb_emit_atomic_cmpxchg_i64(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst_old, sir_val_id_t addr, sir_val_id_t expected,
                                    sir_val_id_t desired, uint32_t align) {
  if (!b) return false;
  sir_inst_t i = {0};
  i.k = SIR_INST_ATOMIC_CMPXCHG_I64;
  i.result_count = 1;
  i.results[0] = dst_old;
  i.u.atomic_cmpxchg_i64.dst_old = dst_old;
  i.u.atomic_cmpxchg_i64.addr = addr;
  i.u.atomic_cmpxchg_i64.expected = expected;
  i.u.atomic_cmpxchg_i64.desired = desired;
  i.u.atomic_cmpxchg_i64.align = align ? align : 1u;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_alloca(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, uint32_t size, uint32_t align) {
  sir_inst_t i = {0};
  i.k = SIR_INST_ALLOCA;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.alloca_.dst = dst;
  i.u.alloca_.size = size;
  i.u.alloca_.align = align ? align : 1;
  return emit_inst(b, f, i);
}

static bool emit_store(sir_module_builder_t* b, sir_func_id_t f, sir_inst_kind_t k, sir_val_id_t addr, sir_val_id_t value, uint32_t align) {
  sir_inst_t i = {0};
  i.k = k;
  i.result_count = 0;
  i.u.store.addr = addr;
  i.u.store.value = value;
  i.u.store.align = align ? align : 1;
  return emit_inst(b, f, i);
}

static bool emit_load(sir_module_builder_t* b, sir_func_id_t f, sir_inst_kind_t k, sir_val_id_t dst, sir_val_id_t addr, uint32_t align) {
  sir_inst_t i = {0};
  i.k = k;
  i.result_count = 1;
  i.results[0] = dst;
  i.u.load.addr = addr;
  i.u.load.dst = dst;
  i.u.load.align = align ? align : 1;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_store_i8(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t addr, sir_val_id_t value, uint32_t align) {
  return emit_store(b, f, SIR_INST_STORE_I8, addr, value, align);
}
bool sir_mb_emit_store_i16(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t addr, sir_val_id_t value, uint32_t align) {
  return emit_store(b, f, SIR_INST_STORE_I16, addr, value, align);
}
bool sir_mb_emit_store_i32(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t addr, sir_val_id_t value, uint32_t align) {
  return emit_store(b, f, SIR_INST_STORE_I32, addr, value, align);
}
bool sir_mb_emit_store_i64(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t addr, sir_val_id_t value, uint32_t align) {
  return emit_store(b, f, SIR_INST_STORE_I64, addr, value, align);
}
bool sir_mb_emit_store_ptr(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t addr, sir_val_id_t value, uint32_t align) {
  return emit_store(b, f, SIR_INST_STORE_PTR, addr, value, align);
}
bool sir_mb_emit_store_f32(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t addr, sir_val_id_t value, uint32_t align) {
  return emit_store(b, f, SIR_INST_STORE_F32, addr, value, align);
}
bool sir_mb_emit_store_f64(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t addr, sir_val_id_t value, uint32_t align) {
  return emit_store(b, f, SIR_INST_STORE_F64, addr, value, align);
}
bool sir_mb_emit_load_i8(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t addr, uint32_t align) {
  return emit_load(b, f, SIR_INST_LOAD_I8, dst, addr, align);
}
bool sir_mb_emit_load_i16(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t addr, uint32_t align) {
  return emit_load(b, f, SIR_INST_LOAD_I16, dst, addr, align);
}
bool sir_mb_emit_load_i32(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t addr, uint32_t align) {
  return emit_load(b, f, SIR_INST_LOAD_I32, dst, addr, align);
}
bool sir_mb_emit_load_i64(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t addr, uint32_t align) {
  return emit_load(b, f, SIR_INST_LOAD_I64, dst, addr, align);
}
bool sir_mb_emit_load_ptr(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t addr, uint32_t align) {
  return emit_load(b, f, SIR_INST_LOAD_PTR, dst, addr, align);
}
bool sir_mb_emit_load_f32(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t addr, uint32_t align) {
  return emit_load(b, f, SIR_INST_LOAD_F32, dst, addr, align);
}
bool sir_mb_emit_load_f64(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t dst, sir_val_id_t addr, uint32_t align) {
  return emit_load(b, f, SIR_INST_LOAD_F64, dst, addr, align);
}

bool sir_mb_emit_call_extern(sir_module_builder_t* b, sir_func_id_t f, sir_sym_id_t callee, const sir_val_id_t* args, uint32_t arg_count) {
  return sir_mb_emit_call_extern_res(b, f, callee, args, arg_count, NULL, 0);
}

bool sir_mb_emit_call_extern_res(sir_module_builder_t* b, sir_func_id_t f, sir_sym_id_t callee, const sir_val_id_t* args, uint32_t arg_count,
                                 const sir_val_id_t* results, uint8_t result_count) {
  if (!b) return false;
  if (result_count > 2) return false;
  const uint32_t args_bytes = arg_count * (uint32_t)sizeof(sir_val_id_t);
  const uint8_t* ap = pool_copy_bytes(b, (const uint8_t*)args, args_bytes);
  if (arg_count && !ap) return false;
  sir_inst_t i = {0};
  i.k = SIR_INST_CALL_EXTERN;
  i.result_count = result_count;
  if (result_count > 0 && results) {
    for (uint8_t ri = 0; ri < result_count; ri++) i.results[ri] = results[ri];
  }
  i.u.call_extern.callee = callee;
  i.u.call_extern.args = (const sir_val_id_t*)ap;
  i.u.call_extern.arg_count = arg_count;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_call_func_res(sir_module_builder_t* b, sir_func_id_t f, sir_func_id_t callee, const sir_val_id_t* args, uint32_t arg_count,
                               const sir_val_id_t* results, uint8_t result_count) {
  if (!b) return false;
  if (result_count > 2) return false;
  const uint32_t args_bytes = arg_count * (uint32_t)sizeof(sir_val_id_t);
  const uint8_t* ap = pool_copy_bytes(b, (const uint8_t*)args, args_bytes);
  if (arg_count && !ap) return false;
  sir_inst_t i = {0};
  i.k = SIR_INST_CALL_FUNC;
  i.result_count = result_count;
  if (result_count > 0 && results) {
    for (uint8_t ri = 0; ri < result_count; ri++) i.results[ri] = results[ri];
  }
  i.u.call_func.callee = callee;
  i.u.call_func.args = (const sir_val_id_t*)ap;
  i.u.call_func.arg_count = arg_count;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_call_func_ptr_res(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t callee_ptr, const sir_val_id_t* args, uint32_t arg_count,
                                   const sir_val_id_t* results, uint8_t result_count) {
  if (!b) return false;
  if (result_count > 2) return false;
  const uint32_t args_bytes = arg_count * (uint32_t)sizeof(sir_val_id_t);
  const uint8_t* ap = pool_copy_bytes(b, (const uint8_t*)args, args_bytes);
  if (arg_count && !ap) return false;
  sir_inst_t i = {0};
  i.k = SIR_INST_CALL_FUNC_PTR;
  i.result_count = result_count;
  if (result_count > 0 && results) {
    for (uint8_t ri = 0; ri < result_count; ri++) i.results[ri] = results[ri];
  }
  i.u.call_func_ptr.callee_ptr = callee_ptr;
  i.u.call_func_ptr.args = (const sir_val_id_t*)ap;
  i.u.call_func_ptr.arg_count = arg_count;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_exit(sir_module_builder_t* b, sir_func_id_t f, int32_t code) {
  sir_inst_t i = {0};
  i.k = SIR_INST_EXIT;
  i.result_count = 0;
  i.u.exit_.code = code;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_exit_val(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t code) {
  sir_inst_t i = {0};
  i.k = SIR_INST_EXIT_VAL;
  i.result_count = 0;
  i.u.exit_val.code = code;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_ret(sir_module_builder_t* b, sir_func_id_t f) {
  sir_inst_t i = {0};
  i.k = SIR_INST_RET;
  i.result_count = 0;
  i.u.ret_._unused = 0;
  return emit_inst(b, f, i);
}

bool sir_mb_emit_ret_val(sir_module_builder_t* b, sir_func_id_t f, sir_val_id_t value) {
  sir_inst_t i = {0};
  i.k = SIR_INST_RET_VAL;
  i.result_count = 0;
  i.u.ret_val.value = value;
  return emit_inst(b, f, i);
}

uint32_t sir_mb_func_ip(const sir_module_builder_t* b, sir_func_id_t f) {
  if (!b) return 0;
  if (f == 0 || f > b->funcs.n) return 0;
  const sir_dyn_insts_t* d = &b->func_insts[f - 1];
  return d->n;
}

bool sir_mb_patch_br(sir_module_builder_t* b, sir_func_id_t f, uint32_t ip, uint32_t target_ip) {
  if (!b) return false;
  if (f == 0 || f > b->funcs.n) return false;
  sir_dyn_insts_t* d = &b->func_insts[f - 1];
  if (ip >= d->n) return false;
  if (d->p[ip].k != SIR_INST_BR) return false;
  d->p[ip].u.br.target_ip = target_ip;
  return true;
}

bool sir_mb_patch_cbr(sir_module_builder_t* b, sir_func_id_t f, uint32_t ip, uint32_t then_ip, uint32_t else_ip) {
  if (!b) return false;
  if (f == 0 || f > b->funcs.n) return false;
  sir_dyn_insts_t* d = &b->func_insts[f - 1];
  if (ip >= d->n) return false;
  if (d->p[ip].k != SIR_INST_CBR) return false;
  d->p[ip].u.cbr.then_ip = then_ip;
  d->p[ip].u.cbr.else_ip = else_ip;
  return true;
}

bool sir_mb_patch_switch(sir_module_builder_t* b, sir_func_id_t f, uint32_t ip, const uint32_t* case_target, uint32_t case_count,
                         uint32_t default_ip) {
  if (!b) return false;
  if (f == 0 || f > b->funcs.n) return false;
  sir_dyn_insts_t* d = &b->func_insts[f - 1];
  if (ip >= d->n) return false;
  if (d->p[ip].k != SIR_INST_SWITCH) return false;
  if (d->p[ip].u.sw.case_count != case_count) return false;
  if (case_count && !case_target) return false;

  d->p[ip].u.sw.default_ip = default_ip;
  if (case_count) {
    uint32_t* tp = (uint32_t*)(uintptr_t)d->p[ip].u.sw.case_target;
    if (!tp) return false;
    memcpy(tp, case_target, (size_t)case_count * sizeof(uint32_t));
  }
  return true;
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
  sir_global_t* globals = NULL;
  if (b->globals.n) {
    globals = (sir_global_t*)dup_mem(b->globals.p, (size_t)b->globals.n * sizeof(sir_global_t));
    if (!globals) {
      free(syms);
      free(types);
      for (uint32_t fi = 0; fi < b->funcs.n; fi++) free((void*)funcs[fi].insts);
      free(funcs);
      return NULL;
    }
  }

  // Package module.
  sir_module_impl_t* impl = (sir_module_impl_t*)calloc(1, sizeof(*impl));
  if (!impl) {
    free(globals);
    free(syms);
    free(types);
    for (uint32_t fi = 0; fi < b->funcs.n; fi++) free((void*)funcs[fi].insts);
    free(funcs);
    return NULL;
  }

  // Steal pool blocks from the builder so pointers remain valid even though
  // the pool grows via multiple allocations.
  impl->pool_head = b->pool.head;
  b->pool.head = NULL;
  b->pool.cur = NULL;
  impl->pub = (sir_module_t){
      .types = types,
      .type_count = b->types.n,
      .syms = syms,
      .sym_count = b->syms.n,
      .globals = globals,
      .global_count = b->globals.n,
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
  free((void*)pub->globals);
  free((void*)pub->syms);
  free((void*)pub->types);
  sir_pool_block_t* pb = (sir_pool_block_t*)impl->pool_head;
  while (pb) {
    sir_pool_block_t* next = pb->next;
    free(pb);
    pb = next;
  }
  free(impl);
}

// Validator context for filling sir_module_validate_ex diagnostics.
typedef struct sir__validate_ctx {
  const char* code;
  sir_func_id_t fid;
  uint32_t ip;
  const sir_inst_t* inst;
} sir__validate_ctx_t;

static sir_validate_diag_t* sir__validate_out_diag = NULL;
static sir__validate_ctx_t sir__validate_ctx = {0};

static void sir__validate_note(const char* code, sir_func_id_t fid, uint32_t ip, const sir_inst_t* inst) {
  sir__validate_ctx.code = code;
  sir__validate_ctx.fid = fid;
  sir__validate_ctx.ip = ip;
  sir__validate_ctx.inst = inst;
}

static bool set_err(char* err, size_t cap, const char* msg) {
  char tmp[256];
  (void)snprintf(tmp, sizeof(tmp), "%s", msg ? msg : "invalid");

  bool wrote = false;
  if (err && cap) {
    (void)snprintf(err, cap, "%s", tmp);
    wrote = true;
  }

  if (sir__validate_out_diag) {
    sir_validate_diag_t* d = sir__validate_out_diag;
    memset(d, 0, sizeof(*d));
    d->code = sir__validate_ctx.code ? sir__validate_ctx.code : "sir.validate";
    (void)snprintf(d->message, sizeof(d->message), "%s", tmp);
    d->fid = sir__validate_ctx.fid;
    d->ip = sir__validate_ctx.ip;
    d->op = sir__validate_ctx.inst ? sir__validate_ctx.inst->k : SIR_INST_INVALID;
    d->src_node_id = sir__validate_ctx.inst ? sir__validate_ctx.inst->src_node_id : 0;
    d->src_line = sir__validate_ctx.inst ? sir__validate_ctx.inst->src_line : 0;
  }

  return wrote;
}

static bool set_errf(char* err, size_t cap, const char* fmt, uint32_t a, uint32_t b) {
  char tmp[256];
  (void)snprintf(tmp, sizeof(tmp), fmt ? fmt : "invalid", (unsigned)a, (unsigned)b);

  bool wrote = false;
  if (err && cap) {
    (void)snprintf(err, cap, "%s", tmp);
    wrote = true;
  }

  if (sir__validate_out_diag) {
    sir_validate_diag_t* d = sir__validate_out_diag;
    memset(d, 0, sizeof(*d));
    d->code = sir__validate_ctx.code ? sir__validate_ctx.code : "sir.validate";
    (void)snprintf(d->message, sizeof(d->message), "%s", tmp);
    d->fid = sir__validate_ctx.fid;
    d->ip = sir__validate_ctx.ip;
    d->op = sir__validate_ctx.inst ? sir__validate_ctx.inst->k : SIR_INST_INVALID;
    d->src_node_id = sir__validate_ctx.inst ? sir__validate_ctx.inst->src_node_id : 0;
    d->src_line = sir__validate_ctx.inst ? sir__validate_ctx.inst->src_line : 0;
  }

  return wrote;
}

bool sir_module_validate(const sir_module_t* m, char* err, size_t err_cap) {
  sir__validate_note("sir.validate.module", 0, 0, NULL);
  if (!m) {
    set_err(err, err_cap, "module is null");
    return false;
  }
  if (m->func_count == 0 || !m->funcs) {
    set_err(err, err_cap, "module has no funcs");
    return false;
  }
  if (m->entry == 0 || m->entry > m->func_count) {
    set_errf(err, err_cap, "entry out of range (%u > %u)", m->entry, m->func_count);
    return false;
  }

  sir__validate_note("sir.validate.type", 0, 0, NULL);
  if (m->type_count && !m->types) {
    set_err(err, err_cap, "type_count set but types is null");
    return false;
  }
  for (uint32_t ti = 0; ti < m->type_count; ti++) {
    if (m->types[ti].prim == SIR_PRIM_INVALID) {
      set_errf(err, err_cap, "invalid prim type at index %u of %u", ti + 1, m->type_count);
      return false;
    }
  }

  sir__validate_note("sir.validate.sym", 0, 0, NULL);
  if (m->sym_count && !m->syms) {
    set_err(err, err_cap, "sym_count set but syms is null");
    return false;
  }
  for (uint32_t si = 0; si < m->sym_count; si++) {
    const sir_sym_t* s = &m->syms[si];
    if (s->kind != SIR_SYM_EXTERN_FN) {
      set_errf(err, err_cap, "invalid sym kind at index %u of %u", si + 1, m->sym_count);
      return false;
    }
    if (!s->name || s->name[0] == '\0') {
      set_errf(err, err_cap, "sym name missing at index %u of %u", si + 1, m->sym_count);
      return false;
    }
    if (s->sig.param_count && !s->sig.params) {
      set_errf(err, err_cap, "sym params missing at index %u of %u", si + 1, m->sym_count);
      return false;
    }
    if (s->sig.result_count && !s->sig.results) {
      set_errf(err, err_cap, "sym results missing at index %u of %u", si + 1, m->sym_count);
      return false;
    }
    for (uint32_t pi = 0; pi < s->sig.param_count; pi++) {
      const sir_type_id_t tid = s->sig.params[pi];
      if (tid == 0 || tid > m->type_count) {
        set_errf(err, err_cap, "sym param type out of range (%u > %u)", (uint32_t)tid, m->type_count);
        return false;
      }
    }
    for (uint32_t ri = 0; ri < s->sig.result_count; ri++) {
      const sir_type_id_t tid = s->sig.results[ri];
      if (tid == 0 || tid > m->type_count) {
        set_errf(err, err_cap, "sym result type out of range (%u > %u)", (uint32_t)tid, m->type_count);
        return false;
      }
    }
  }

  sir__validate_note("sir.validate.global", 0, 0, NULL);
  if (m->global_count && !m->globals) {
    set_err(err, err_cap, "global_count set but globals is null");
    return false;
  }
  for (uint32_t gi = 0; gi < m->global_count; gi++) {
    const sir_global_t* g = &m->globals[gi];
    if (!g->name || g->name[0] == '\0') {
      set_errf(err, err_cap, "global name missing at index %u of %u", gi + 1, m->global_count);
      return false;
    }
    if (g->size == 0) {
      set_errf(err, err_cap, "global size must be >0 at index %u of %u", gi + 1, m->global_count);
      return false;
    }
    if (g->align == 0) {
      set_errf(err, err_cap, "global align must be >0 at index %u of %u", gi + 1, m->global_count);
      return false;
    }
    if (g->init_len > g->size) {
      set_errf(err, err_cap, "global init_len out of range at index %u of %u", gi + 1, m->global_count);
      return false;
    }
    if (g->init_len && !g->init_bytes) {
      set_errf(err, err_cap, "global init_bytes missing at index %u of %u", gi + 1, m->global_count);
      return false;
    }
  }

  for (uint32_t fi = 0; fi < m->func_count; fi++) {
    const sir_func_t* f = &m->funcs[fi];
    const sir_func_id_t fid = (sir_func_id_t)(fi + 1);
    sir__validate_note("sir.validate.func", fid, 0, NULL);
    if (!f->name || f->name[0] == '\0') {
      set_errf(err, err_cap, "func name missing at index %u of %u", fi + 1, m->func_count);
      return false;
    }
    if (f->inst_count && !f->insts) {
      set_errf(err, err_cap, "func insts missing at index %u of %u", fi + 1, m->func_count);
      return false;
    }
    const uint32_t vc = f->value_count;
    for (uint32_t ii = 0; ii < f->inst_count; ii++) {
      const sir_inst_t* inst = &f->insts[ii];
      sir__validate_note("sir.validate.inst", fid, ii, inst);
      switch (inst->k) {
        case SIR_INST_CONST_I1:
          if (inst->u.const_i1.dst >= vc) {
            set_errf(err, err_cap, "const_i1 dst out of range (%u >= %u)", inst->u.const_i1.dst, vc);
            return false;
          }
          if (inst->u.const_i1.v > 1) {
            set_err(err, err_cap, "const_i1 value must be 0 or 1");
            return false;
          }
          break;
        case SIR_INST_CONST_I8:
          if (inst->u.const_i8.dst >= vc) {
            set_errf(err, err_cap, "const_i8 dst out of range (%u >= %u)", inst->u.const_i8.dst, vc);
            return false;
          }
          break;
        case SIR_INST_CONST_I16:
          if (inst->u.const_i16.dst >= vc) {
            set_errf(err, err_cap, "const_i16 dst out of range (%u >= %u)", inst->u.const_i16.dst, vc);
            return false;
          }
          break;
        case SIR_INST_CONST_I32:
          if (inst->u.const_i32.dst >= vc) {
            set_errf(err, err_cap, "const_i32 dst out of range (%u >= %u)", inst->u.const_i32.dst, vc);
            return false;
          }
          break;
        case SIR_INST_CONST_I64:
          if (inst->u.const_i64.dst >= vc) {
            set_errf(err, err_cap, "const_i64 dst out of range (%u >= %u)", inst->u.const_i64.dst, vc);
            return false;
          }
          break;
        case SIR_INST_CONST_BOOL:
          if (inst->u.const_bool.dst >= vc) {
            set_errf(err, err_cap, "const_bool dst out of range (%u >= %u)", inst->u.const_bool.dst, vc);
            return false;
          }
          if (inst->u.const_bool.v > 1) {
            set_err(err, err_cap, "const_bool value must be 0 or 1");
            return false;
          }
          break;
        case SIR_INST_CONST_F32:
          if (inst->u.const_f32.dst >= vc) {
            set_errf(err, err_cap, "const_f32 dst out of range (%u >= %u)", inst->u.const_f32.dst, vc);
            return false;
          }
          break;
        case SIR_INST_CONST_F64:
          if (inst->u.const_f64.dst >= vc) {
            set_errf(err, err_cap, "const_f64 dst out of range (%u >= %u)", inst->u.const_f64.dst, vc);
            return false;
          }
          break;
        case SIR_INST_CONST_PTR:
          if (inst->u.const_ptr.dst >= vc) {
            set_errf(err, err_cap, "const_ptr dst out of range (%u >= %u)", inst->u.const_ptr.dst, vc);
            return false;
          }
          break;
        case SIR_INST_CONST_PTR_NULL:
          if (inst->u.const_null.dst >= vc) {
            set_errf(err, err_cap, "const_null dst out of range (%u >= %u)", inst->u.const_null.dst, vc);
            return false;
          }
          break;
        case SIR_INST_CONST_BYTES:
          if (inst->u.const_bytes.dst_ptr >= vc || inst->u.const_bytes.dst_len >= vc) {
            set_err(err, err_cap, "const_bytes dst out of range");
            return false;
          }
          if (inst->u.const_bytes.len && !inst->u.const_bytes.bytes) {
            set_err(err, err_cap, "const_bytes has len but no bytes");
            return false;
          }
          break;
        case SIR_INST_I32_ADD:
          if (inst->u.i32_add.dst >= vc || inst->u.i32_add.a >= vc || inst->u.i32_add.b >= vc) {
            set_err(err, err_cap, "i32_add operand out of range");
            return false;
          }
          break;
        case SIR_INST_I32_SUB:
        case SIR_INST_I32_MUL:
        case SIR_INST_I32_AND:
        case SIR_INST_I32_OR:
        case SIR_INST_I32_XOR:
        case SIR_INST_I32_SHL:
        case SIR_INST_I32_SHR_S:
        case SIR_INST_I32_SHR_U:
        case SIR_INST_I32_DIV_S_SAT:
        case SIR_INST_I32_DIV_S_TRAP:
        case SIR_INST_I32_DIV_U_SAT:
        case SIR_INST_I32_REM_S_SAT:
        case SIR_INST_I32_REM_U_SAT:
          if (inst->u.i32_add.dst >= vc || inst->u.i32_add.a >= vc || inst->u.i32_add.b >= vc) {
            set_err(err, err_cap, "i32_bin operand out of range");
            return false;
          }
          break;
        case SIR_INST_I32_NOT:
        case SIR_INST_I32_NEG:
          if (inst->u.i32_un.dst >= vc || inst->u.i32_un.x >= vc) {
            set_err(err, err_cap, "i32_un operand out of range");
            return false;
          }
          break;
        case SIR_INST_I32_CMP_EQ:
          if (inst->u.i32_cmp_eq.dst >= vc || inst->u.i32_cmp_eq.a >= vc || inst->u.i32_cmp_eq.b >= vc) {
            set_err(err, err_cap, "i32_cmp_eq operand out of range");
            return false;
          }
          break;
        case SIR_INST_I32_CMP_NE:
        case SIR_INST_I32_CMP_SLT:
        case SIR_INST_I32_CMP_SLE:
        case SIR_INST_I32_CMP_SGT:
        case SIR_INST_I32_CMP_SGE:
        case SIR_INST_I32_CMP_ULT:
        case SIR_INST_I32_CMP_ULE:
        case SIR_INST_I32_CMP_UGT:
        case SIR_INST_I32_CMP_UGE:
          if (inst->u.i32_cmp_eq.dst >= vc || inst->u.i32_cmp_eq.a >= vc || inst->u.i32_cmp_eq.b >= vc) {
            set_err(err, err_cap, "i32_cmp operand out of range");
            return false;
          }
          break;
        case SIR_INST_F32_CMP_UEQ:
        case SIR_INST_F64_CMP_OLT:
          if (inst->u.f_cmp.dst >= vc || inst->u.f_cmp.a >= vc || inst->u.f_cmp.b >= vc) {
            set_err(err, err_cap, "f_cmp operand out of range");
            return false;
          }
          break;
        case SIR_INST_GLOBAL_ADDR:
          if (inst->u.global_addr.dst >= vc) {
            set_err(err, err_cap, "global_addr dst out of range");
            return false;
          }
          if (inst->u.global_addr.gid == 0 || inst->u.global_addr.gid > m->global_count) {
            set_err(err, err_cap, "global_addr gid out of range");
            return false;
          }
          break;
        case SIR_INST_PTR_OFFSET:
          if (inst->u.ptr_offset.dst >= vc || inst->u.ptr_offset.base >= vc || inst->u.ptr_offset.index >= vc) {
            set_err(err, err_cap, "ptr_offset operand out of range");
            return false;
          }
          if (inst->u.ptr_offset.scale == 0) {
            set_err(err, err_cap, "ptr_offset scale must be >0");
            return false;
          }
          break;
        case SIR_INST_PTR_ADD:
          if (inst->u.ptr_add.dst >= vc || inst->u.ptr_add.base >= vc || inst->u.ptr_add.off >= vc) {
            set_err(err, err_cap, "ptr_add operand out of range");
            return false;
          }
          break;
        case SIR_INST_PTR_SUB:
          if (inst->u.ptr_sub.dst >= vc || inst->u.ptr_sub.base >= vc || inst->u.ptr_sub.off >= vc) {
            set_err(err, err_cap, "ptr_sub operand out of range");
            return false;
          }
          break;
        case SIR_INST_PTR_CMP_EQ:
        case SIR_INST_PTR_CMP_NE:
          if (inst->u.ptr_cmp.dst >= vc || inst->u.ptr_cmp.a >= vc || inst->u.ptr_cmp.b >= vc) {
            set_err(err, err_cap, "ptr_cmp operand out of range");
            return false;
          }
          break;
        case SIR_INST_PTR_TO_I64:
          if (inst->u.ptr_to_i64.dst >= vc || inst->u.ptr_to_i64.x >= vc) {
            set_err(err, err_cap, "ptr_to_i64 operand out of range");
            return false;
          }
          break;
        case SIR_INST_PTR_FROM_I64:
          if (inst->u.ptr_from_i64.dst >= vc || inst->u.ptr_from_i64.x >= vc) {
            set_err(err, err_cap, "ptr_from_i64 operand out of range");
            return false;
          }
          break;
        case SIR_INST_BOOL_NOT:
          if (inst->u.bool_not.dst >= vc || inst->u.bool_not.x >= vc) {
            set_err(err, err_cap, "bool_not operand out of range");
            return false;
          }
          break;
        case SIR_INST_BOOL_AND:
        case SIR_INST_BOOL_OR:
        case SIR_INST_BOOL_XOR:
          if (inst->u.bool_bin.dst >= vc || inst->u.bool_bin.a >= vc || inst->u.bool_bin.b >= vc) {
            set_err(err, err_cap, "bool_bin operand out of range");
            return false;
          }
          break;
        case SIR_INST_I32_TRUNC_I64:
          if (inst->u.i32_trunc_i64.dst >= vc || inst->u.i32_trunc_i64.x >= vc) {
            set_err(err, err_cap, "i32_trunc_i64 operand out of range");
            return false;
          }
          break;
        case SIR_INST_I32_ZEXT_I8:
          if (inst->u.i32_zext_i8.dst >= vc || inst->u.i32_zext_i8.x >= vc) {
            set_err(err, err_cap, "i32_zext_i8 operand out of range");
            return false;
          }
          break;
        case SIR_INST_I32_ZEXT_I16:
          if (inst->u.i32_zext_i16.dst >= vc || inst->u.i32_zext_i16.x >= vc) {
            set_err(err, err_cap, "i32_zext_i16 operand out of range");
            return false;
          }
          break;
        case SIR_INST_I64_ZEXT_I32:
          if (inst->u.i64_zext_i32.dst >= vc || inst->u.i64_zext_i32.x >= vc) {
            set_err(err, err_cap, "i64_zext_i32 operand out of range");
            return false;
          }
          break;
        case SIR_INST_SELECT:
          if (inst->u.select.dst >= vc || inst->u.select.cond >= vc || inst->u.select.a >= vc || inst->u.select.b >= vc) {
            set_err(err, err_cap, "select operand out of range");
            return false;
          }
          break;
        case SIR_INST_BR:
          if (inst->u.br.target_ip >= f->inst_count) {
            set_err(err, err_cap, "br target_ip out of range");
            return false;
          }
          if (inst->u.br.arg_count) {
            if (!inst->u.br.src_slots || !inst->u.br.dst_slots) {
              set_err(err, err_cap, "br arg_count set but slot arrays are null");
              return false;
            }
            for (uint32_t ai = 0; ai < inst->u.br.arg_count; ai++) {
              if (inst->u.br.src_slots[ai] >= vc || inst->u.br.dst_slots[ai] >= vc) {
                set_err(err, err_cap, "br arg slot out of range");
                return false;
              }
            }
          }
          break;
        case SIR_INST_CBR:
          if (inst->u.cbr.cond >= vc) {
            set_err(err, err_cap, "cbr cond out of range");
            return false;
          }
          if (inst->u.cbr.then_ip >= f->inst_count || inst->u.cbr.else_ip >= f->inst_count) {
            set_err(err, err_cap, "cbr target_ip out of range");
            return false;
          }
          break;
        case SIR_INST_SWITCH:
          if (inst->u.sw.scrut >= vc) {
            set_err(err, err_cap, "switch scrut out of range");
            return false;
          }
          if (inst->u.sw.case_count) {
            if (!inst->u.sw.case_lits || !inst->u.sw.case_target) {
              set_err(err, err_cap, "switch case_count set but arrays are null");
              return false;
            }
            for (uint32_t ci = 0; ci < inst->u.sw.case_count; ci++) {
              if (inst->u.sw.case_target[ci] >= f->inst_count) {
                set_err(err, err_cap, "switch case target_ip out of range");
                return false;
              }
            }
          }
          if (inst->u.sw.default_ip >= f->inst_count) {
            set_err(err, err_cap, "switch default_ip out of range");
            return false;
          }
          break;
        case SIR_INST_MEM_COPY:
          if (inst->u.mem_copy.dst >= vc || inst->u.mem_copy.src >= vc || inst->u.mem_copy.len >= vc) {
            set_err(err, err_cap, "mem.copy operand out of range");
            return false;
          }
          break;
        case SIR_INST_MEM_FILL:
          if (inst->u.mem_fill.dst >= vc || inst->u.mem_fill.byte >= vc || inst->u.mem_fill.len >= vc) {
            set_err(err, err_cap, "mem.fill operand out of range");
            return false;
          }
          break;
        case SIR_INST_ATOMIC_RMW_I8:
        case SIR_INST_ATOMIC_RMW_I16:
        case SIR_INST_ATOMIC_RMW_I32:
        case SIR_INST_ATOMIC_RMW_I64:
          if (inst->u.atomic_rmw.dst_old >= vc || inst->u.atomic_rmw.addr >= vc || inst->u.atomic_rmw.value >= vc) {
            set_err(err, err_cap, "atomic.rmw operand out of range");
            return false;
          }
          if (!is_pow2_u32(inst->u.atomic_rmw.align)) {
            set_err(err, err_cap, "atomic.rmw align must be a power of two");
            return false;
          }
          if (!(inst->u.atomic_rmw.op == SIR_ATOMIC_RMW_ADD || inst->u.atomic_rmw.op == SIR_ATOMIC_RMW_AND || inst->u.atomic_rmw.op == SIR_ATOMIC_RMW_OR ||
                inst->u.atomic_rmw.op == SIR_ATOMIC_RMW_XOR || inst->u.atomic_rmw.op == SIR_ATOMIC_RMW_XCHG)) {
            set_err(err, err_cap, "atomic.rmw op invalid");
            return false;
          }
          break;
        case SIR_INST_ATOMIC_CMPXCHG_I64:
          if (inst->u.atomic_cmpxchg_i64.dst_old >= vc || inst->u.atomic_cmpxchg_i64.addr >= vc || inst->u.atomic_cmpxchg_i64.expected >= vc ||
              inst->u.atomic_cmpxchg_i64.desired >= vc) {
            set_err(err, err_cap, "atomic.cmpxchg.i64 operand out of range");
            return false;
          }
          if (!is_pow2_u32(inst->u.atomic_cmpxchg_i64.align)) {
            set_err(err, err_cap, "atomic.cmpxchg.i64 align must be a power of two");
            return false;
          }
          break;
        case SIR_INST_ALLOCA:
          if (inst->u.alloca_.dst >= vc) {
            set_err(err, err_cap, "alloca dst out of range");
            return false;
          }
          if (inst->u.alloca_.size == 0) {
            set_err(err, err_cap, "alloca size must be >0");
            return false;
          }
          break;
        case SIR_INST_STORE_I8:
        case SIR_INST_STORE_I16:
        case SIR_INST_STORE_I32:
        case SIR_INST_STORE_I64:
        case SIR_INST_STORE_PTR:
        case SIR_INST_STORE_F32:
        case SIR_INST_STORE_F64:
          if (inst->u.store.addr >= vc || inst->u.store.value >= vc) {
            set_err(err, err_cap, "store operand out of range");
            return false;
          }
          if (!is_pow2_u32(inst->u.store.align)) {
            set_err(err, err_cap, "store align must be a power of two");
            return false;
          }
          break;
        case SIR_INST_LOAD_I8:
        case SIR_INST_LOAD_I16:
        case SIR_INST_LOAD_I32:
        case SIR_INST_LOAD_I64:
        case SIR_INST_LOAD_PTR:
        case SIR_INST_LOAD_F32:
        case SIR_INST_LOAD_F64:
          if (inst->u.load.addr >= vc || inst->u.load.dst >= vc) {
            set_err(err, err_cap, "load operand out of range");
            return false;
          }
          if (!is_pow2_u32(inst->u.load.align)) {
            set_err(err, err_cap, "load align must be a power of two");
            return false;
          }
          break;
        case SIR_INST_CALL_EXTERN: {
          const sir_sym_id_t callee = inst->u.call_extern.callee;
          if (callee == 0 || callee > m->sym_count) {
            set_err(err, err_cap, "call_extern callee out of range");
            return false;
          }
          const sir_sym_t* s = &m->syms[callee - 1];
          if (inst->u.call_extern.arg_count && !inst->u.call_extern.args) {
            set_err(err, err_cap, "call_extern arg_count set but args is null");
            return false;
          }
          if (inst->u.call_extern.arg_count != s->sig.param_count) {
            set_err(err, err_cap, "call_extern arg_count does not match signature");
            return false;
          }
          if (inst->result_count != s->sig.result_count) {
            set_err(err, err_cap, "call_extern result_count does not match signature");
            return false;
          }
          for (uint32_t ai = 0; ai < inst->u.call_extern.arg_count; ai++) {
            if (inst->u.call_extern.args[ai] >= vc) {
              set_err(err, err_cap, "call_extern arg out of range");
              return false;
            }
          }
          for (uint8_t ri = 0; ri < inst->result_count; ri++) {
            if (inst->results[ri] >= vc) {
              set_err(err, err_cap, "call_extern result out of range");
              return false;
            }
          }
          break;
        }
        case SIR_INST_CALL_FUNC: {
          const sir_func_id_t callee = inst->u.call_func.callee;
          if (callee == 0 || callee > m->func_count) {
            set_err(err, err_cap, "call_func callee out of range");
            return false;
          }
          const sir_func_t* cf = &m->funcs[callee - 1];
          if (inst->u.call_func.arg_count && !inst->u.call_func.args) {
            set_err(err, err_cap, "call_func arg_count set but args is null");
            return false;
          }
          if (inst->u.call_func.arg_count != cf->sig.param_count) {
            set_err(err, err_cap, "call_func arg_count does not match callee signature");
            return false;
          }
          if (inst->result_count != cf->sig.result_count) {
            set_err(err, err_cap, "call_func result_count does not match callee signature");
            return false;
          }
          for (uint32_t ai = 0; ai < inst->u.call_func.arg_count; ai++) {
            if (inst->u.call_func.args[ai] >= vc) {
              set_err(err, err_cap, "call_func arg out of range");
              return false;
            }
          }
          for (uint8_t ri = 0; ri < inst->result_count; ri++) {
            if (inst->results[ri] >= vc) {
              set_err(err, err_cap, "call_func result out of range");
              return false;
            }
          }
          break;
        }
        case SIR_INST_CALL_FUNC_PTR: {
          if (inst->u.call_func_ptr.callee_ptr >= vc) {
            set_err(err, err_cap, "call_func_ptr callee_ptr out of range");
            return false;
          }
          if (inst->u.call_func_ptr.arg_count && !inst->u.call_func_ptr.args) {
            set_err(err, err_cap, "call_func_ptr arg_count set but args is null");
            return false;
          }
          for (uint32_t ai = 0; ai < inst->u.call_func_ptr.arg_count; ai++) {
            if (inst->u.call_func_ptr.args[ai] >= vc) {
              set_err(err, err_cap, "call_func_ptr arg out of range");
              return false;
            }
          }
          for (uint8_t ri = 0; ri < inst->result_count; ri++) {
            if (inst->results[ri] >= vc) {
              set_err(err, err_cap, "call_func_ptr result out of range");
              return false;
            }
          }
          break;
        }
        case SIR_INST_RET:
          break;
        case SIR_INST_RET_VAL:
          if (inst->u.ret_val.value >= vc) {
            set_err(err, err_cap, "ret_val value out of range");
            return false;
          }
          break;
        case SIR_INST_EXIT:
          break;
        case SIR_INST_EXIT_VAL:
          if (inst->u.exit_val.code >= vc) {
            set_err(err, err_cap, "exit_val code out of range");
            return false;
          }
          break;
        default:
          set_err(err, err_cap, "unknown instruction kind");
          return false;
      }
    }
  }

  if (err && err_cap) err[0] = '\0';
  return true;
}

bool sir_module_validate_ex(const sir_module_t* m, sir_validate_diag_t* out) {
  sir_validate_diag_t* prev = sir__validate_out_diag;
  sir__validate_out_diag = out;
  if (out) memset(out, 0, sizeof(*out));
  const bool ok = sir_module_validate(m, NULL, 0);
  sir__validate_out_diag = prev;
  if (ok && out) memset(out, 0, sizeof(*out));
  return ok;
}

static const sir_sym_t* sym_at(const sir_module_t* m, sir_sym_id_t id) {
  if (!m) return NULL;
  if (id == 0 || id > m->sym_count) return NULL;
  return &m->syms[id - 1];
}

static int32_t exec_call_extern(const sir_module_t* m, sem_guest_mem_t* mem, sir_host_t host, sir_func_id_t fid, uint32_t ip,
                                const sir_exec_event_sink_t* sink, const sir_inst_t* inst, sir_value_t* vals, uint32_t val_count) {
  (void)mem;
  if (!m || !inst || !vals) return ZI_E_INTERNAL;
  const sir_sym_t* s = sym_at(m, inst->u.call_extern.callee);
  if (!s || s->kind != SIR_SYM_EXTERN_FN || !s->name) return ZI_E_NOENT;

  // MVP: dispatch by name to zABI primitives.
  const char* nm = s->name;
  const sir_val_id_t* args = inst->u.call_extern.args;
  const uint32_t n = inst->u.call_extern.arg_count;

  sir_val_id_t r0 = 0;
  if (inst->result_count > 0) {
    r0 = inst->results[0];
    if (r0 >= val_count) return ZI_E_BOUNDS;
  }

  if (strcmp(nm, "zi_write") == 0) {
    if (!host.v.zi_write) return ZI_E_NOSYS;
    if (n != 3) return ZI_E_INVALID;
    const sir_val_id_t a0 = args[0], a1 = args[1], a2 = args[2];
    if (a0 >= val_count || a1 >= val_count || a2 >= val_count) return ZI_E_BOUNDS;
    const sir_value_t h = vals[a0];
    const sir_value_t p = vals[a1];
    const sir_value_t l = vals[a2];
    if (h.kind != SIR_VAL_I32) return ZI_E_INVALID;
    const zi_ptr_t pp = (p.kind == SIR_VAL_PTR) ? p.u.ptr : (p.kind == SIR_VAL_I64) ? (zi_ptr_t)p.u.i64 : (zi_ptr_t)0;
    if (p.kind != SIR_VAL_PTR && p.kind != SIR_VAL_I64) return ZI_E_INVALID;
    const int64_t ll = (l.kind == SIR_VAL_I64) ? l.u.i64 : (l.kind == SIR_VAL_I32) ? (int64_t)l.u.i32 : (int64_t)-1;
    if (l.kind != SIR_VAL_I64 && l.kind != SIR_VAL_I32) return ZI_E_INVALID;
    if (ll < 0 || ll > 0x7FFFFFFFll) return ZI_E_INVALID;
    const int32_t rc = host.v.zi_write(host.user, (zi_handle_t)h.u.i32, pp, (zi_size32_t)ll);
    if (sink && sink->on_hostcall) sink->on_hostcall(sink->user, m, fid, ip, nm, rc);
    if (inst->result_count == 1) {
      vals[r0] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = rc};
    }
    return 0;
  }

  if (strcmp(nm, "zi_end") == 0) {
    if (!host.v.zi_end) return ZI_E_NOSYS;
    if (n != 1) return ZI_E_INVALID;
    const sir_val_id_t a0 = args[0];
    if (a0 >= val_count) return ZI_E_BOUNDS;
    const sir_value_t h = vals[a0];
    if (h.kind != SIR_VAL_I32) return ZI_E_INVALID;
    const int32_t rc = host.v.zi_end(host.user, (zi_handle_t)h.u.i32);
    if (sink && sink->on_hostcall) sink->on_hostcall(sink->user, m, fid, ip, nm, rc);
    if (inst->result_count == 1) {
      vals[r0] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = rc};
    }
    return 0;
  }

  if (strcmp(nm, "zi_read") == 0) {
    if (!host.v.zi_read) return ZI_E_NOSYS;
    if (n != 3) return ZI_E_INVALID;
    const sir_val_id_t a0 = args[0], a1 = args[1], a2 = args[2];
    if (a0 >= val_count || a1 >= val_count || a2 >= val_count) return ZI_E_BOUNDS;
    const sir_value_t h = vals[a0];
    const sir_value_t p = vals[a1];
    const sir_value_t l = vals[a2];
    if (h.kind != SIR_VAL_I32) return ZI_E_INVALID;
    const zi_ptr_t pp = (p.kind == SIR_VAL_PTR) ? p.u.ptr : (p.kind == SIR_VAL_I64) ? (zi_ptr_t)p.u.i64 : (zi_ptr_t)0;
    if (p.kind != SIR_VAL_PTR && p.kind != SIR_VAL_I64) return ZI_E_INVALID;
    const int64_t ll = (l.kind == SIR_VAL_I64) ? l.u.i64 : (l.kind == SIR_VAL_I32) ? (int64_t)l.u.i32 : (int64_t)-1;
    if (l.kind != SIR_VAL_I64 && l.kind != SIR_VAL_I32) return ZI_E_INVALID;
    if (ll < 0 || ll > 0x7FFFFFFFll) return ZI_E_INVALID;
    const int32_t rc = host.v.zi_read(host.user, (zi_handle_t)h.u.i32, pp, (zi_size32_t)ll);
    if (sink && sink->on_hostcall) sink->on_hostcall(sink->user, m, fid, ip, nm, rc);
    if (inst->result_count == 1) {
      vals[r0] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = rc};
    }
    return 0;
  }

  if (strcmp(nm, "zi_alloc") == 0) {
    if (!host.v.zi_alloc) return ZI_E_NOSYS;
    if (n != 1) return ZI_E_INVALID;
    const sir_val_id_t a0 = args[0];
    if (a0 >= val_count) return ZI_E_BOUNDS;
    const sir_value_t sz = vals[a0];
    if (sz.kind != SIR_VAL_I32) return ZI_E_INVALID;
    const zi_ptr_t p = host.v.zi_alloc(host.user, (zi_size32_t)sz.u.i32);
    if (sink && sink->on_hostcall) sink->on_hostcall(sink->user, m, fid, ip, nm, p ? 0 : ZI_E_OOM);
    if (!p && sz.u.i32 != 0) return ZI_E_OOM;
    if (inst->result_count == 1) {
      vals[r0] = (sir_value_t){.kind = SIR_VAL_PTR, .u.ptr = p};
    }
    return 0;
  }

  if (strcmp(nm, "zi_free") == 0) {
    if (!host.v.zi_free) return ZI_E_NOSYS;
    if (n != 1) return ZI_E_INVALID;
    const sir_val_id_t a0 = args[0];
    if (a0 >= val_count) return ZI_E_BOUNDS;
    const sir_value_t p = vals[a0];
    if (p.kind != SIR_VAL_PTR) return ZI_E_INVALID;
    const int32_t rc = host.v.zi_free(host.user, p.u.ptr);
    if (sink && sink->on_hostcall) sink->on_hostcall(sink->user, m, fid, ip, nm, rc);
    if (inst->result_count == 1) {
      vals[r0] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = rc};
    }
    return 0;
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
    const zi_ptr_t tpp = (tp.kind == SIR_VAL_PTR) ? tp.u.ptr : (tp.kind == SIR_VAL_I64) ? (zi_ptr_t)tp.u.i64 : (zi_ptr_t)0;
    const zi_ptr_t mpp = (mp.kind == SIR_VAL_PTR) ? mp.u.ptr : (mp.kind == SIR_VAL_I64) ? (zi_ptr_t)mp.u.i64 : (zi_ptr_t)0;
    if (tp.kind != SIR_VAL_PTR && tp.kind != SIR_VAL_I64) return ZI_E_INVALID;
    if (mp.kind != SIR_VAL_PTR && mp.kind != SIR_VAL_I64) return ZI_E_INVALID;
    if (tl.kind != SIR_VAL_I32 || ml.kind != SIR_VAL_I32) return ZI_E_INVALID;
    const int32_t rc = host.v.zi_telemetry(host.user, tpp, (zi_size32_t)tl.u.i32, mpp, (zi_size32_t)ml.u.i32);
    if (sink && sink->on_hostcall) sink->on_hostcall(sink->user, m, fid, ip, nm, rc);
    if (inst->result_count == 1) {
      vals[r0] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = rc};
    }
    return 0;
  }

  if (strcmp(nm, "zi_abi_version") == 0) {
    if (!host.v.zi_abi_version) return ZI_E_NOSYS;
    if (n != 0) return ZI_E_INVALID;
    const uint32_t v = host.v.zi_abi_version(host.user);
    if (sink && sink->on_hostcall) sink->on_hostcall(sink->user, m, fid, ip, nm, (int32_t)v);
    if (inst->result_count == 1) {
      vals[r0] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = (int32_t)v};
    }
    return 0;
  }

  if (strcmp(nm, "zi_ctl") == 0) {
    if (!host.v.zi_ctl) return ZI_E_NOSYS;
    if (n != 4) return ZI_E_INVALID;
    const sir_val_id_t a0 = args[0], a1 = args[1], a2 = args[2], a3 = args[3];
    if (a0 >= val_count || a1 >= val_count || a2 >= val_count || a3 >= val_count) return ZI_E_BOUNDS;
    const sir_value_t rp = vals[a0];
    const sir_value_t rl = vals[a1];
    const sir_value_t sp = vals[a2];
    const sir_value_t sl = vals[a3];

    const zi_ptr_t req_ptr = (rp.kind == SIR_VAL_PTR) ? rp.u.ptr : (rp.kind == SIR_VAL_I64) ? (zi_ptr_t)rp.u.i64 : (zi_ptr_t)0;
    const zi_ptr_t resp_ptr = (sp.kind == SIR_VAL_PTR) ? sp.u.ptr : (sp.kind == SIR_VAL_I64) ? (zi_ptr_t)sp.u.i64 : (zi_ptr_t)0;
    if (rp.kind != SIR_VAL_PTR && rp.kind != SIR_VAL_I64) return ZI_E_INVALID;
    if (sp.kind != SIR_VAL_PTR && sp.kind != SIR_VAL_I64) return ZI_E_INVALID;

    const int64_t req_len64 = (rl.kind == SIR_VAL_I32) ? (int64_t)rl.u.i32 : (rl.kind == SIR_VAL_I64) ? rl.u.i64 : (int64_t)-1;
    const int64_t resp_cap64 = (sl.kind == SIR_VAL_I32) ? (int64_t)sl.u.i32 : (sl.kind == SIR_VAL_I64) ? sl.u.i64 : (int64_t)-1;
    if (rl.kind != SIR_VAL_I32 && rl.kind != SIR_VAL_I64) return ZI_E_INVALID;
    if (sl.kind != SIR_VAL_I32 && sl.kind != SIR_VAL_I64) return ZI_E_INVALID;
    if (req_len64 < 0 || req_len64 > 0x7FFFFFFFll) return ZI_E_INVALID;
    if (resp_cap64 < 0 || resp_cap64 > 0x7FFFFFFFll) return ZI_E_INVALID;

    const int32_t rc = host.v.zi_ctl(host.user, req_ptr, (zi_size32_t)req_len64, resp_ptr, (zi_size32_t)resp_cap64);
    if (sink && sink->on_hostcall) sink->on_hostcall(sink->user, m, fid, ip, nm, rc);
    if (inst->result_count == 1) {
      vals[r0] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = rc};
    }
    return 0;
  }

  if (strcmp(nm, "zi_cap_count") == 0) {
    if (!host.v.zi_cap_count) return ZI_E_NOSYS;
    if (n != 0) return ZI_E_INVALID;
    const int32_t rc = host.v.zi_cap_count(host.user);
    if (sink && sink->on_hostcall) sink->on_hostcall(sink->user, m, fid, ip, nm, rc);
    if (inst->result_count == 1) {
      vals[r0] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = rc};
    }
    return 0;
  }

  if (strcmp(nm, "zi_cap_get_size") == 0) {
    if (!host.v.zi_cap_get_size) return ZI_E_NOSYS;
    if (n != 1) return ZI_E_INVALID;
    const sir_val_id_t a0 = args[0];
    if (a0 >= val_count) return ZI_E_BOUNDS;
    const sir_value_t idx = vals[a0];
    if (idx.kind != SIR_VAL_I32) return ZI_E_INVALID;
    const int32_t rc = host.v.zi_cap_get_size(host.user, idx.u.i32);
    if (sink && sink->on_hostcall) sink->on_hostcall(sink->user, m, fid, ip, nm, rc);
    if (inst->result_count == 1) {
      vals[r0] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = rc};
    }
    return 0;
  }

  if (strcmp(nm, "zi_cap_get") == 0) {
    if (!host.v.zi_cap_get) return ZI_E_NOSYS;
    if (n != 3) return ZI_E_INVALID;
    const sir_val_id_t a0 = args[0], a1 = args[1], a2 = args[2];
    if (a0 >= val_count || a1 >= val_count || a2 >= val_count) return ZI_E_BOUNDS;
    const sir_value_t idx = vals[a0];
    const sir_value_t outp = vals[a1];
    const sir_value_t capv = vals[a2];
    if (idx.kind != SIR_VAL_I32) return ZI_E_INVALID;
    const zi_ptr_t out_ptr = (outp.kind == SIR_VAL_PTR) ? outp.u.ptr : (outp.kind == SIR_VAL_I64) ? (zi_ptr_t)outp.u.i64 : (zi_ptr_t)0;
    if (outp.kind != SIR_VAL_PTR && outp.kind != SIR_VAL_I64) return ZI_E_INVALID;
    const int64_t out_cap64 = (capv.kind == SIR_VAL_I32) ? (int64_t)capv.u.i32 : (capv.kind == SIR_VAL_I64) ? capv.u.i64 : (int64_t)-1;
    if (capv.kind != SIR_VAL_I32 && capv.kind != SIR_VAL_I64) return ZI_E_INVALID;
    if (out_cap64 < 0 || out_cap64 > 0x7FFFFFFFll) return ZI_E_INVALID;
    const int32_t rc = host.v.zi_cap_get(host.user, idx.u.i32, out_ptr, (zi_size32_t)out_cap64);
    if (sink && sink->on_hostcall) sink->on_hostcall(sink->user, m, fid, ip, nm, rc);
    if (inst->result_count == 1) {
      vals[r0] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = rc};
    }
    return 0;
  }

  if (strcmp(nm, "zi_cap_open") == 0) {
    if (!host.v.zi_cap_open) return ZI_E_NOSYS;
    if (n != 1) return ZI_E_INVALID;
    const sir_val_id_t a0 = args[0];
    if (a0 >= val_count) return ZI_E_BOUNDS;
    const sir_value_t rp = vals[a0];
    const zi_ptr_t req_ptr = (rp.kind == SIR_VAL_PTR) ? rp.u.ptr : (rp.kind == SIR_VAL_I64) ? (zi_ptr_t)rp.u.i64 : (zi_ptr_t)0;
    if (rp.kind != SIR_VAL_PTR && rp.kind != SIR_VAL_I64) return ZI_E_INVALID;
    const zi_handle_t h = host.v.zi_cap_open(host.user, req_ptr);
    if (sink && sink->on_hostcall) sink->on_hostcall(sink->user, m, fid, ip, nm, (int32_t)h);
    if (inst->result_count == 1) {
      vals[r0] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = (int32_t)h};
    }
    return 0;
  }

  if (strcmp(nm, "zi_handle_hflags") == 0) {
    if (!host.v.zi_handle_hflags) return ZI_E_NOSYS;
    if (n != 1) return ZI_E_INVALID;
    const sir_val_id_t a0 = args[0];
    if (a0 >= val_count) return ZI_E_BOUNDS;
    const sir_value_t hv = vals[a0];
    if (hv.kind != SIR_VAL_I32) return ZI_E_INVALID;
    const uint32_t hf = host.v.zi_handle_hflags(host.user, (zi_handle_t)hv.u.i32);
    if (sink && sink->on_hostcall) sink->on_hostcall(sink->user, m, fid, ip, nm, (int32_t)hf);
    if (inst->result_count == 1) {
      vals[r0] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = (int32_t)hf};
    }
    return 0;
  }

  return ZI_E_NOSYS;
}

typedef struct sir_frame {
  const sir_func_t* f;
  sir_value_t* vals;
} sir_frame_t;

static bool f32_is_nan_bits(uint32_t bits) {
  const uint32_t exp = bits & 0x7F800000u;
  const uint32_t frac = bits & 0x007FFFFFu;
  return exp == 0x7F800000u && frac != 0;
}

static uint32_t f32_canon_bits(uint32_t bits) {
  return f32_is_nan_bits(bits) ? 0x7FC00000u : bits;
}

static bool f64_is_nan_bits(uint64_t bits) {
  const uint64_t exp = bits & 0x7FF0000000000000ull;
  const uint64_t frac = bits & 0x000FFFFFFFFFFFFFull;
  return exp == 0x7FF0000000000000ull && frac != 0;
}

static uint64_t f64_canon_bits(uint64_t bits) {
  return f64_is_nan_bits(bits) ? 0x7FF8000000000000ull : bits;
}

static bool is_pow2_u32(uint32_t x) {
  return x != 0u && (x & (x - 1u)) == 0u;
}

static int32_t exec_func(const sir_module_t* m, sem_guest_mem_t* mem, sir_host_t host, const zi_ptr_t* globals, uint32_t global_count,
                         sir_func_id_t fid, const sir_value_t* args, uint32_t arg_count, sir_value_t* out_results, uint32_t out_result_count,
                         uint32_t depth, const sir_exec_event_sink_t* sink);

static int32_t exec_call_func(const sir_module_t* m, sem_guest_mem_t* mem, sir_host_t host, const zi_ptr_t* globals, uint32_t global_count,
                              const sir_inst_t* inst, sir_value_t* vals, uint32_t val_count, uint32_t depth, const sir_exec_event_sink_t* sink) {
  if (!m || !inst || !vals) return ZI_E_INTERNAL;
  const sir_func_id_t fid = inst->u.call_func.callee;
  if (fid == 0 || fid > m->func_count) return ZI_E_NOENT;

  const sir_func_t* cf = &m->funcs[fid - 1];
  if (inst->u.call_func.arg_count != cf->sig.param_count) return ZI_E_INVALID;
  if (inst->result_count != cf->sig.result_count) return ZI_E_INVALID;

  if (inst->u.call_func.arg_count > 16) return ZI_E_INVALID;
  sir_value_t argv[16];
  for (uint32_t i = 0; i < inst->u.call_func.arg_count; i++) {
    const sir_val_id_t a = inst->u.call_func.args[i];
    if (a >= val_count) return ZI_E_BOUNDS;
    argv[i] = vals[a];
  }

  sir_value_t resv[2];
  memset(resv, 0, sizeof(resv));
  const int32_t rc =
      exec_func(m, mem, host, globals, global_count, fid, argv, inst->u.call_func.arg_count, resv, inst->result_count, depth + 1, sink);
  // Propagate errors and process-exit requests.
  if (rc != 0) return rc;
  for (uint8_t ri = 0; ri < inst->result_count; ri++) {
    const sir_val_id_t dst = inst->results[ri];
    if (dst >= val_count) return ZI_E_BOUNDS;
    vals[dst] = resv[ri];
  }
  return 0;
}

static bool decode_tagged_fid(zi_ptr_t p, sir_func_id_t* out_fid) {
  if (!out_fid) return false;
  // Encoding contract: ptr = 0xF000... | fid
  const uint64_t tag = UINT64_C(0xF000000000000000);
  const uint64_t v = (uint64_t)p;
  if ((v & tag) != tag) return false;
  const uint64_t fid64 = v & ~tag;
  if (fid64 == 0 || fid64 > 0xFFFFFFFFull) return false;
  *out_fid = (sir_func_id_t)fid64;
  return true;
}

static int32_t exec_call_func_ptr(const sir_module_t* m, sem_guest_mem_t* mem, sir_host_t host, const zi_ptr_t* globals, uint32_t global_count,
                                  const sir_inst_t* inst, sir_value_t* vals, uint32_t val_count, uint32_t depth, const sir_exec_event_sink_t* sink) {
  if (!m || !inst || !vals) return ZI_E_INTERNAL;
  const sir_val_id_t callee_slot = inst->u.call_func_ptr.callee_ptr;
  if (callee_slot >= val_count) return ZI_E_BOUNDS;
  const sir_value_t cv = vals[callee_slot];
  if (cv.kind != SIR_VAL_PTR) return ZI_E_INVALID;

  sir_func_id_t fid = 0;
  if (!decode_tagged_fid(cv.u.ptr, &fid)) return ZI_E_INVALID;
  if (fid == 0 || fid > m->func_count) return ZI_E_NOENT;

  const sir_func_t* cf = &m->funcs[fid - 1];
  if (inst->u.call_func_ptr.arg_count != cf->sig.param_count) return ZI_E_INVALID;
  if (inst->result_count != cf->sig.result_count) return ZI_E_INVALID;

  if (inst->u.call_func_ptr.arg_count > 16) return ZI_E_INVALID;
  sir_value_t argv[16];
  for (uint32_t i = 0; i < inst->u.call_func_ptr.arg_count; i++) {
    const sir_val_id_t a = inst->u.call_func_ptr.args[i];
    if (a >= val_count) return ZI_E_BOUNDS;
    argv[i] = vals[a];
  }

  sir_value_t resv[2];
  memset(resv, 0, sizeof(resv));
  const int32_t rc =
      exec_func(m, mem, host, globals, global_count, fid, argv, inst->u.call_func_ptr.arg_count, resv, inst->result_count, depth + 1, sink);
  if (rc != 0) return rc;
  for (uint8_t ri = 0; ri < inst->result_count; ri++) {
    const sir_val_id_t dst = inst->results[ri];
    if (dst >= val_count) return ZI_E_BOUNDS;
    vals[dst] = resv[ri];
  }
  return 0;
}

static int32_t exec_func(const sir_module_t* m, sem_guest_mem_t* mem, sir_host_t host, const zi_ptr_t* globals, uint32_t global_count,
                         sir_func_id_t fid, const sir_value_t* args, uint32_t arg_count, sir_value_t* out_results, uint32_t out_result_count,
                         uint32_t depth, const sir_exec_event_sink_t* sink) {
  if (!m) return ZI_E_INTERNAL;
  if (depth > 1024) return ZI_E_INTERNAL;
  if (fid == 0 || fid > m->func_count) return ZI_E_NOENT;

  const sir_func_t* f = &m->funcs[fid - 1];
  if (!(args == NULL && arg_count == 0 && fid == m->entry) && arg_count != f->sig.param_count) return ZI_E_INVALID;
  if (out_result_count != f->sig.result_count) return ZI_E_INVALID;

  if (f->value_count > 1u << 20) return ZI_E_INVALID;
  sir_value_t* vals = (sir_value_t*)calloc(f->value_count, sizeof(*vals));
  if (!vals) return ZI_E_OOM;

  if (args == NULL && arg_count == 0 && fid == m->entry) {
    // Default-initialize entry params to zero (DX convenience).
    for (uint32_t i = 0; i < f->sig.param_count; i++) {
      if (i >= f->value_count) {
        free(vals);
        return ZI_E_BOUNDS;
      }
      const sir_type_id_t tid = f->sig.params ? f->sig.params[i] : 0;
      if (tid == 0 || tid > m->type_count) {
        free(vals);
        return ZI_E_INVALID;
      }
      const sir_prim_type_t prim = m->types[tid - 1].prim;
      switch (prim) {
        case SIR_PRIM_VOID:
          free(vals);
          return ZI_E_INVALID;
        case SIR_PRIM_I1:
          vals[i] = (sir_value_t){.kind = SIR_VAL_I1, .u.u1 = 0};
          break;
        case SIR_PRIM_I8:
          vals[i] = (sir_value_t){.kind = SIR_VAL_I8, .u.u8 = 0};
          break;
        case SIR_PRIM_I16:
          vals[i] = (sir_value_t){.kind = SIR_VAL_I16, .u.u16 = 0};
          break;
        case SIR_PRIM_I32:
          vals[i] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = 0};
          break;
        case SIR_PRIM_I64:
          vals[i] = (sir_value_t){.kind = SIR_VAL_I64, .u.i64 = 0};
          break;
        case SIR_PRIM_PTR:
          vals[i] = (sir_value_t){.kind = SIR_VAL_PTR, .u.ptr = 0};
          break;
        case SIR_PRIM_BOOL:
          vals[i] = (sir_value_t){.kind = SIR_VAL_BOOL, .u.b = 0};
          break;
        case SIR_PRIM_F32:
          vals[i] = (sir_value_t){.kind = SIR_VAL_F32, .u.f32_bits = 0};
          break;
        case SIR_PRIM_F64:
          vals[i] = (sir_value_t){.kind = SIR_VAL_F64, .u.f64_bits = 0};
          break;
        default:
          free(vals);
          return ZI_E_INVALID;
      }
    }
  } else {
    for (uint32_t i = 0; i < arg_count; i++) {
      if (i >= f->value_count) {
        free(vals);
        return ZI_E_BOUNDS;
      }
      vals[i] = args[i];
    }
  }

  for (uint32_t ip = 0; ip < f->inst_count;) {
    const sir_inst_t* i = &f->insts[ip];
    if (sink && sink->on_step) sink->on_step(sink->user, m, fid, ip, i->k);
    switch (i->k) {
      case SIR_INST_CONST_I1:
        if (i->u.const_i1.dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        if (i->u.const_i1.v > 1) {
          free(vals);
          return ZI_E_INVALID;
        }
        vals[i->u.const_i1.dst] = (sir_value_t){.kind = SIR_VAL_I1, .u.u1 = i->u.const_i1.v};
        ip++;
        break;
      case SIR_INST_CONST_I8:
        if (i->u.const_i8.dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        vals[i->u.const_i8.dst] = (sir_value_t){.kind = SIR_VAL_I8, .u.u8 = i->u.const_i8.v};
        ip++;
        break;
      case SIR_INST_CONST_I16:
        if (i->u.const_i16.dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        vals[i->u.const_i16.dst] = (sir_value_t){.kind = SIR_VAL_I16, .u.u16 = i->u.const_i16.v};
        ip++;
        break;
      case SIR_INST_CONST_I32:
        if (i->u.const_i32.dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        vals[i->u.const_i32.dst] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = i->u.const_i32.v};
        ip++;
        break;
      case SIR_INST_CONST_I64:
        if (i->u.const_i64.dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        vals[i->u.const_i64.dst] = (sir_value_t){.kind = SIR_VAL_I64, .u.i64 = i->u.const_i64.v};
        ip++;
        break;
      case SIR_INST_CONST_BOOL:
        if (i->u.const_bool.dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        if (i->u.const_bool.v > 1) {
          free(vals);
          return ZI_E_INVALID;
        }
        vals[i->u.const_bool.dst] = (sir_value_t){.kind = SIR_VAL_BOOL, .u.b = i->u.const_bool.v};
        ip++;
        break;
      case SIR_INST_CONST_F32:
        if (i->u.const_f32.dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        vals[i->u.const_f32.dst] = (sir_value_t){.kind = SIR_VAL_F32, .u.f32_bits = f32_canon_bits(i->u.const_f32.bits)};
        ip++;
        break;
      case SIR_INST_CONST_F64:
        if (i->u.const_f64.dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        vals[i->u.const_f64.dst] = (sir_value_t){.kind = SIR_VAL_F64, .u.f64_bits = f64_canon_bits(i->u.const_f64.bits)};
        ip++;
        break;
      case SIR_INST_CONST_PTR:
        if (i->u.const_ptr.dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        vals[i->u.const_ptr.dst] = (sir_value_t){.kind = SIR_VAL_PTR, .u.ptr = i->u.const_ptr.v};
        ip++;
        break;
      case SIR_INST_CONST_PTR_NULL:
        if (i->u.const_null.dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        vals[i->u.const_null.dst] = (sir_value_t){.kind = SIR_VAL_PTR, .u.ptr = 0};
        ip++;
        break;
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
        ip++;
        break;
      }
      case SIR_INST_I32_ADD: {
        const sir_val_id_t a = i->u.i32_add.a;
        const sir_val_id_t b = i->u.i32_add.b;
        const sir_val_id_t dst = i->u.i32_add.dst;
        if (a >= f->value_count || b >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t av = vals[a];
        const sir_value_t bv = vals[b];
        if (av.kind != SIR_VAL_I32 || bv.kind != SIR_VAL_I32) {
          free(vals);
          return ZI_E_INVALID;
        }
        vals[dst] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = (int32_t)(av.u.i32 + bv.u.i32)};
        ip++;
        break;
      }
      case SIR_INST_I32_SUB:
      case SIR_INST_I32_MUL:
      case SIR_INST_I32_AND:
      case SIR_INST_I32_OR:
      case SIR_INST_I32_XOR:
      case SIR_INST_I32_NOT:
      case SIR_INST_I32_NEG:
      case SIR_INST_I32_SHL:
      case SIR_INST_I32_SHR_S:
      case SIR_INST_I32_SHR_U:
      case SIR_INST_I32_DIV_S_SAT:
      case SIR_INST_I32_DIV_S_TRAP:
      case SIR_INST_I32_DIV_U_SAT:
      case SIR_INST_I32_REM_S_SAT:
      case SIR_INST_I32_REM_U_SAT: {
        sir_val_id_t dst = 0;
        int32_t x = 0;
        int32_t y = 0;
        if (i->k == SIR_INST_I32_NOT || i->k == SIR_INST_I32_NEG) {
          const sir_val_id_t xv = i->u.i32_un.x;
          dst = i->u.i32_un.dst;
          if (xv >= f->value_count || dst >= f->value_count) {
            free(vals);
            return ZI_E_BOUNDS;
          }
          const sir_value_t av = vals[xv];
          if (av.kind != SIR_VAL_I32) {
            free(vals);
            return ZI_E_INVALID;
          }
          x = av.u.i32;
          y = 0;
        } else {
          const sir_val_id_t a = i->u.i32_add.a;
          const sir_val_id_t b = i->u.i32_add.b;
          dst = i->u.i32_add.dst;
          if (a >= f->value_count || b >= f->value_count || dst >= f->value_count) {
            free(vals);
            return ZI_E_BOUNDS;
          }
          const sir_value_t av = vals[a];
          const sir_value_t bv = vals[b];
          if (av.kind != SIR_VAL_I32 || bv.kind != SIR_VAL_I32) {
            free(vals);
            return ZI_E_INVALID;
          }
          x = av.u.i32;
          y = bv.u.i32;
        }
        int32_t r = 0;
        switch (i->k) {
          case SIR_INST_I32_SUB:
            r = (int32_t)(x - y);
            break;
          case SIR_INST_I32_MUL:
            r = (int32_t)((int64_t)x * (int64_t)y);
            break;
          case SIR_INST_I32_AND:
            r = (int32_t)((uint32_t)x & (uint32_t)y);
            break;
          case SIR_INST_I32_OR:
            r = (int32_t)((uint32_t)x | (uint32_t)y);
            break;
          case SIR_INST_I32_XOR:
            r = (int32_t)((uint32_t)x ^ (uint32_t)y);
            break;
          case SIR_INST_I32_NOT:
            r = (int32_t)(~(uint32_t)x);
            break;
          case SIR_INST_I32_NEG:
            r = (int32_t)(0 - x);
            break;
          case SIR_INST_I32_SHL: {
            const uint32_t sh = ((uint32_t)y) & 31u;
            r = (int32_t)((uint32_t)x << sh);
            break;
          }
          case SIR_INST_I32_SHR_S: {
            const uint32_t sh = ((uint32_t)y) & 31u;
            r = (int32_t)(x >> sh);
            break;
          }
          case SIR_INST_I32_SHR_U: {
            const uint32_t sh = ((uint32_t)y) & 31u;
            r = (int32_t)((uint32_t)x >> sh);
            break;
          }
          case SIR_INST_I32_DIV_S_SAT:
            if (y == 0) r = 0;
            else if (x == INT32_MIN && y == -1) r = INT32_MIN;
            else r = (int32_t)(x / y);
            break;
          case SIR_INST_I32_DIV_S_TRAP:
            if (y == 0 || (x == INT32_MIN && y == -1)) {
              free(vals);
              return 255 + 1;
            }
            r = (int32_t)(x / y);
            break;
          case SIR_INST_I32_DIV_U_SAT:
            if (y == 0) r = 0;
            else r = (int32_t)((uint32_t)x / (uint32_t)y);
            break;
          case SIR_INST_I32_REM_S_SAT:
            if (y == 0) r = 0;
            else if (x == INT32_MIN && y == -1) r = 0;
            else r = (int32_t)(x % y);
            break;
          case SIR_INST_I32_REM_U_SAT:
            if (y == 0) r = 0;
            else r = (int32_t)((uint32_t)x % (uint32_t)y);
            break;
          default:
            free(vals);
            return ZI_E_INTERNAL;
        }
        vals[dst] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = r};
        ip++;
        break;
      }
      case SIR_INST_I32_CMP_EQ: {
        const sir_val_id_t a = i->u.i32_cmp_eq.a;
        const sir_val_id_t b = i->u.i32_cmp_eq.b;
        const sir_val_id_t dst = i->u.i32_cmp_eq.dst;
        if (a >= f->value_count || b >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t av = vals[a];
        const sir_value_t bv = vals[b];
        if (av.kind != SIR_VAL_I32 || bv.kind != SIR_VAL_I32) {
          free(vals);
          return ZI_E_INVALID;
        }
        vals[dst] = (sir_value_t){.kind = SIR_VAL_BOOL, .u.b = (uint8_t)(av.u.i32 == bv.u.i32)};
        ip++;
        break;
      }
      case SIR_INST_I32_CMP_NE:
      case SIR_INST_I32_CMP_SLT:
      case SIR_INST_I32_CMP_SLE:
      case SIR_INST_I32_CMP_SGT:
      case SIR_INST_I32_CMP_SGE:
      case SIR_INST_I32_CMP_ULT:
      case SIR_INST_I32_CMP_ULE:
      case SIR_INST_I32_CMP_UGT:
      case SIR_INST_I32_CMP_UGE: {
        const sir_val_id_t a = i->u.i32_cmp_eq.a;
        const sir_val_id_t b = i->u.i32_cmp_eq.b;
        const sir_val_id_t dst = i->u.i32_cmp_eq.dst;
        if (a >= f->value_count || b >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t av = vals[a];
        const sir_value_t bv = vals[b];
        if (av.kind != SIR_VAL_I32 || bv.kind != SIR_VAL_I32) {
          free(vals);
          return ZI_E_INVALID;
        }
        const int32_t x = av.u.i32;
        const int32_t y = bv.u.i32;
        bool r = false;
        switch (i->k) {
          case SIR_INST_I32_CMP_NE:
            r = (x != y);
            break;
          case SIR_INST_I32_CMP_SLT:
            r = (x < y);
            break;
          case SIR_INST_I32_CMP_SLE:
            r = (x <= y);
            break;
          case SIR_INST_I32_CMP_SGT:
            r = (x > y);
            break;
          case SIR_INST_I32_CMP_SGE:
            r = (x >= y);
            break;
          case SIR_INST_I32_CMP_ULT:
            r = ((uint32_t)x < (uint32_t)y);
            break;
          case SIR_INST_I32_CMP_ULE:
            r = ((uint32_t)x <= (uint32_t)y);
            break;
          case SIR_INST_I32_CMP_UGT:
            r = ((uint32_t)x > (uint32_t)y);
            break;
          case SIR_INST_I32_CMP_UGE:
            r = ((uint32_t)x >= (uint32_t)y);
            break;
          default:
            free(vals);
            return ZI_E_INTERNAL;
        }
        vals[dst] = (sir_value_t){.kind = SIR_VAL_BOOL, .u.b = (uint8_t)(r ? 1 : 0)};
        ip++;
        break;
      }
      case SIR_INST_F32_CMP_UEQ: {
        const sir_val_id_t a = i->u.f_cmp.a;
        const sir_val_id_t b = i->u.f_cmp.b;
        const sir_val_id_t dst = i->u.f_cmp.dst;
        if (a >= f->value_count || b >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t av = vals[a];
        const sir_value_t bv = vals[b];
        if (av.kind != SIR_VAL_F32 || bv.kind != SIR_VAL_F32) {
          free(vals);
          return ZI_E_INVALID;
        }
        const bool nan_a = f32_is_nan_bits(av.u.f32_bits);
        const bool nan_b = f32_is_nan_bits(bv.u.f32_bits);
        float af = 0.0f, bf = 0.0f;
        memcpy(&af, &av.u.f32_bits, 4);
        memcpy(&bf, &bv.u.f32_bits, 4);
        const bool r = nan_a || nan_b || (af == bf);
        vals[dst] = (sir_value_t){.kind = SIR_VAL_BOOL, .u.b = (uint8_t)(r ? 1 : 0)};
        ip++;
        break;
      }
      case SIR_INST_F64_CMP_OLT: {
        const sir_val_id_t a = i->u.f_cmp.a;
        const sir_val_id_t b = i->u.f_cmp.b;
        const sir_val_id_t dst = i->u.f_cmp.dst;
        if (a >= f->value_count || b >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t av = vals[a];
        const sir_value_t bv = vals[b];
        if (av.kind != SIR_VAL_F64 || bv.kind != SIR_VAL_F64) {
          free(vals);
          return ZI_E_INVALID;
        }
        const bool nan_a = f64_is_nan_bits(av.u.f64_bits);
        const bool nan_b = f64_is_nan_bits(bv.u.f64_bits);
        double ad = 0.0, bd = 0.0;
        memcpy(&ad, &av.u.f64_bits, 8);
        memcpy(&bd, &bv.u.f64_bits, 8);
        const bool r = (!nan_a && !nan_b) && (ad < bd);
        vals[dst] = (sir_value_t){.kind = SIR_VAL_BOOL, .u.b = (uint8_t)(r ? 1 : 0)};
        ip++;
        break;
      }
      case SIR_INST_GLOBAL_ADDR: {
        const sir_global_id_t gid = i->u.global_addr.gid;
        const sir_val_id_t dst = i->u.global_addr.dst;
        if (dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        if (!globals || gid == 0 || gid > global_count) {
          free(vals);
          return ZI_E_NOENT;
        }
        vals[dst] = (sir_value_t){.kind = SIR_VAL_PTR, .u.ptr = globals[gid - 1]};
        ip++;
        break;
      }
      case SIR_INST_PTR_OFFSET: {
        const sir_val_id_t base_id = i->u.ptr_offset.base;
        const sir_val_id_t index_id = i->u.ptr_offset.index;
        const sir_val_id_t dst = i->u.ptr_offset.dst;
        if (base_id >= f->value_count || index_id >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t bv = vals[base_id];
        const sir_value_t iv = vals[index_id];
        if (bv.kind != SIR_VAL_PTR) {
          free(vals);
          return ZI_E_INVALID;
        }
        int64_t idx = 0;
        if (iv.kind == SIR_VAL_I64) idx = iv.u.i64;
        else if (iv.kind == SIR_VAL_I32) idx = iv.u.i32;
        else {
          free(vals);
          return ZI_E_INVALID;
        }
        const uint64_t base = (uint64_t)bv.u.ptr;
        const uint64_t off = (uint64_t)idx * (uint64_t)i->u.ptr_offset.scale;
        vals[dst] = (sir_value_t){.kind = SIR_VAL_PTR, .u.ptr = (zi_ptr_t)(base + off)};
        ip++;
        break;
      }
      case SIR_INST_PTR_ADD: {
        const sir_val_id_t base_id = i->u.ptr_add.base;
        const sir_val_id_t off_id = i->u.ptr_add.off;
        const sir_val_id_t dst = i->u.ptr_add.dst;
        if (base_id >= f->value_count || off_id >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t bv = vals[base_id];
        const sir_value_t ov = vals[off_id];
        if (bv.kind != SIR_VAL_PTR) {
          free(vals);
          return ZI_E_INVALID;
        }
        int64_t off = 0;
        if (ov.kind == SIR_VAL_I64) off = ov.u.i64;
        else if (ov.kind == SIR_VAL_I32) off = ov.u.i32;
        else {
          free(vals);
          return ZI_E_INVALID;
        }
        const uint64_t base = (uint64_t)bv.u.ptr;
        vals[dst] = (sir_value_t){.kind = SIR_VAL_PTR, .u.ptr = (zi_ptr_t)(base + (uint64_t)off)};
        ip++;
        break;
      }
      case SIR_INST_PTR_SUB: {
        const sir_val_id_t base_id = i->u.ptr_sub.base;
        const sir_val_id_t off_id = i->u.ptr_sub.off;
        const sir_val_id_t dst = i->u.ptr_sub.dst;
        if (base_id >= f->value_count || off_id >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t bv = vals[base_id];
        const sir_value_t ov = vals[off_id];
        if (bv.kind != SIR_VAL_PTR) {
          free(vals);
          return ZI_E_INVALID;
        }
        int64_t off = 0;
        if (ov.kind == SIR_VAL_I64) off = ov.u.i64;
        else if (ov.kind == SIR_VAL_I32) off = ov.u.i32;
        else {
          free(vals);
          return ZI_E_INVALID;
        }
        const uint64_t base = (uint64_t)bv.u.ptr;
        vals[dst] = (sir_value_t){.kind = SIR_VAL_PTR, .u.ptr = (zi_ptr_t)(base - (uint64_t)off)};
        ip++;
        break;
      }
      case SIR_INST_PTR_CMP_EQ:
      case SIR_INST_PTR_CMP_NE: {
        const sir_val_id_t a = i->u.ptr_cmp.a;
        const sir_val_id_t b = i->u.ptr_cmp.b;
        const sir_val_id_t dst = i->u.ptr_cmp.dst;
        if (a >= f->value_count || b >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t av = vals[a];
        const sir_value_t bv = vals[b];
        if (av.kind != SIR_VAL_PTR || bv.kind != SIR_VAL_PTR) {
          free(vals);
          return ZI_E_INVALID;
        }
        const bool eq = (av.u.ptr == bv.u.ptr);
        const uint8_t r = (i->k == SIR_INST_PTR_CMP_EQ) ? (uint8_t)eq : (uint8_t)(!eq);
        vals[dst] = (sir_value_t){.kind = SIR_VAL_BOOL, .u.b = r};
        ip++;
        break;
      }
      case SIR_INST_PTR_TO_I64: {
        const sir_val_id_t x = i->u.ptr_to_i64.x;
        const sir_val_id_t dst = i->u.ptr_to_i64.dst;
        if (x >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t xv = vals[x];
        if (xv.kind != SIR_VAL_PTR) {
          free(vals);
          return ZI_E_INVALID;
        }
        vals[dst] = (sir_value_t){.kind = SIR_VAL_I64, .u.i64 = (int64_t)(uint64_t)xv.u.ptr};
        ip++;
        break;
      }
      case SIR_INST_PTR_FROM_I64: {
        const sir_val_id_t x = i->u.ptr_from_i64.x;
        const sir_val_id_t dst = i->u.ptr_from_i64.dst;
        if (x >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t xv = vals[x];
        uint64_t bits = 0;
        if (xv.kind == SIR_VAL_I64) bits = (uint64_t)xv.u.i64;
        else if (xv.kind == SIR_VAL_I32) bits = (uint64_t)(uint32_t)xv.u.i32;
        else {
          free(vals);
          return ZI_E_INVALID;
        }
        vals[dst] = (sir_value_t){.kind = SIR_VAL_PTR, .u.ptr = (zi_ptr_t)bits};
        ip++;
        break;
      }
      case SIR_INST_BOOL_NOT: {
        const sir_val_id_t x = i->u.bool_not.x;
        const sir_val_id_t dst = i->u.bool_not.dst;
        if (x >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t xv = vals[x];
        if (xv.kind != SIR_VAL_BOOL) {
          free(vals);
          return ZI_E_INVALID;
        }
        vals[dst] = (sir_value_t){.kind = SIR_VAL_BOOL, .u.b = (uint8_t)(xv.u.b ? 0 : 1)};
        ip++;
        break;
      }
      case SIR_INST_BOOL_AND:
      case SIR_INST_BOOL_OR:
      case SIR_INST_BOOL_XOR: {
        const sir_val_id_t a = i->u.bool_bin.a;
        const sir_val_id_t b = i->u.bool_bin.b;
        const sir_val_id_t dst = i->u.bool_bin.dst;
        if (a >= f->value_count || b >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t av = vals[a];
        const sir_value_t bv = vals[b];
        if (av.kind != SIR_VAL_BOOL || bv.kind != SIR_VAL_BOOL) {
          free(vals);
          return ZI_E_INVALID;
        }
        const uint8_t ax = (uint8_t)(av.u.b ? 1 : 0);
        const uint8_t bx = (uint8_t)(bv.u.b ? 1 : 0);
        uint8_t r = 0;
        if (i->k == SIR_INST_BOOL_AND) r = (uint8_t)(ax & bx);
        else if (i->k == SIR_INST_BOOL_OR) r = (uint8_t)(ax | bx);
        else r = (uint8_t)(ax ^ bx);
        vals[dst] = (sir_value_t){.kind = SIR_VAL_BOOL, .u.b = r};
        ip++;
        break;
      }
      case SIR_INST_I32_TRUNC_I64: {
        const sir_val_id_t x = i->u.i32_trunc_i64.x;
        const sir_val_id_t dst = i->u.i32_trunc_i64.dst;
        if (x >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t xv = vals[x];
        if (xv.kind != SIR_VAL_I64) {
          free(vals);
          return ZI_E_INVALID;
        }
        vals[dst] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = (int32_t)(uint32_t)xv.u.i64};
        ip++;
        break;
      }
      case SIR_INST_I32_ZEXT_I8: {
        const sir_val_id_t x = i->u.i32_zext_i8.x;
        const sir_val_id_t dst = i->u.i32_zext_i8.dst;
        if (x >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t xv = vals[x];
        if (xv.kind != SIR_VAL_I8) {
          free(vals);
          return ZI_E_INVALID;
        }
        vals[dst] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = (int32_t)(uint32_t)xv.u.u8};
        ip++;
        break;
      }
      case SIR_INST_I32_ZEXT_I16: {
        const sir_val_id_t x = i->u.i32_zext_i16.x;
        const sir_val_id_t dst = i->u.i32_zext_i16.dst;
        if (x >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t xv = vals[x];
        if (xv.kind != SIR_VAL_I16) {
          free(vals);
          return ZI_E_INVALID;
        }
        vals[dst] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = (int32_t)(uint32_t)xv.u.u16};
        ip++;
        break;
      }
      case SIR_INST_I64_ZEXT_I32: {
        const sir_val_id_t x = i->u.i64_zext_i32.x;
        const sir_val_id_t dst = i->u.i64_zext_i32.dst;
        if (x >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t xv = vals[x];
        if (xv.kind != SIR_VAL_I32) {
          free(vals);
          return ZI_E_INVALID;
        }
        vals[dst] = (sir_value_t){.kind = SIR_VAL_I64, .u.i64 = (int64_t)(uint64_t)(uint32_t)xv.u.i32};
        ip++;
        break;
      }
      case SIR_INST_SELECT: {
        const sir_val_id_t cond = i->u.select.cond;
        const sir_val_id_t a = i->u.select.a;
        const sir_val_id_t b = i->u.select.b;
        const sir_val_id_t dst = i->u.select.dst;
        if (cond >= f->value_count || a >= f->value_count || b >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t cv = vals[cond];
        if (cv.kind != SIR_VAL_BOOL) {
          free(vals);
          return ZI_E_INVALID;
        }
        vals[dst] = cv.u.b ? vals[a] : vals[b];
        ip++;
        break;
      }
      case SIR_INST_BR:
        if (i->u.br.arg_count) {
          const uint32_t n = i->u.br.arg_count;
          const sir_val_id_t* src = i->u.br.src_slots;
          const sir_val_id_t* dst = i->u.br.dst_slots;
          if (!src || !dst) {
            free(vals);
            return ZI_E_INVALID;
          }

          sir_value_t tmp_small[16];
          sir_value_t* tmp = tmp_small;
          if (n > (uint32_t)(sizeof(tmp_small) / sizeof(tmp_small[0]))) {
            tmp = (sir_value_t*)malloc((size_t)n * sizeof(*tmp));
            if (!tmp) {
              free(vals);
              return ZI_E_OOM;
            }
          }

          for (uint32_t ai = 0; ai < n; ai++) {
            const sir_val_id_t s = src[ai];
            if (s >= f->value_count) {
              if (tmp != tmp_small) free(tmp);
              free(vals);
              return ZI_E_BOUNDS;
            }
            tmp[ai] = vals[s];
          }
          for (uint32_t ai = 0; ai < n; ai++) {
            const sir_val_id_t d = dst[ai];
            if (d >= f->value_count) {
              if (tmp != tmp_small) free(tmp);
              free(vals);
              return ZI_E_BOUNDS;
            }
            vals[d] = tmp[ai];
          }

          if (tmp != tmp_small) free(tmp);
        }
        ip = i->u.br.target_ip;
        break;
      case SIR_INST_CBR: {
        const sir_val_id_t cvid = i->u.cbr.cond;
        if (cvid >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t cv = vals[cvid];
        if (cv.kind != SIR_VAL_BOOL) {
          free(vals);
          return ZI_E_INVALID;
        }
        ip = cv.u.b ? i->u.cbr.then_ip : i->u.cbr.else_ip;
        break;
      }
      case SIR_INST_SWITCH: {
        const sir_val_id_t sid = i->u.sw.scrut;
        if (sid >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t sv = vals[sid];
        if (sv.kind != SIR_VAL_I32) {
          free(vals);
          return ZI_E_INVALID;
        }
        const uint32_t n = i->u.sw.case_count;
        const int32_t* lits = i->u.sw.case_lits;
        const uint32_t* tgt = i->u.sw.case_target;
        if (n && (!lits || !tgt)) {
          free(vals);
          return ZI_E_INVALID;
        }
        uint32_t next_ip = i->u.sw.default_ip;
        for (uint32_t ci = 0; ci < n; ci++) {
          if (sv.u.i32 == lits[ci]) {
            next_ip = tgt[ci];
            break;
          }
        }
        ip = next_ip;
        break;
      }
      case SIR_INST_MEM_COPY: {
        const sir_val_id_t dst_id = i->u.mem_copy.dst;
        const sir_val_id_t src_id = i->u.mem_copy.src;
        const sir_val_id_t len_id = i->u.mem_copy.len;
        if (dst_id >= f->value_count || src_id >= f->value_count || len_id >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t dv = vals[dst_id];
        const sir_value_t sv = vals[src_id];
        const sir_value_t lv = vals[len_id];
        if (dv.kind != SIR_VAL_PTR || sv.kind != SIR_VAL_PTR) {
          free(vals);
          return ZI_E_INVALID;
        }
        int64_t ll = 0;
        if (lv.kind == SIR_VAL_I64) ll = lv.u.i64;
        else if (lv.kind == SIR_VAL_I32) ll = lv.u.i32;
        else {
          free(vals);
          return ZI_E_INVALID;
        }
        if (ll < 0 || ll > 0x7FFFFFFFll) {
          free(vals);
          return ZI_E_INVALID;
        }
        const uint32_t n = (uint32_t)ll;
        if (n == 0) {
          ip++;
          break;
        }

        if (!i->u.mem_copy.overlap_allow) {
          const zi_ptr_t da = dv.u.ptr;
          const zi_ptr_t sa = sv.u.ptr;
          const zi_ptr_t da_end = (zi_ptr_t)(da + (zi_ptr_t)n);
          const zi_ptr_t sa_end = (zi_ptr_t)(sa + (zi_ptr_t)n);
          const bool overlap = (da < sa_end) && (sa < da_end);
          if (overlap) {
            free(vals);
            // deterministic trap (align with term.trap in SEM: exit code 255)
            return 256;
          }
        }

        const uint8_t* r = NULL;
        uint8_t* w = NULL;
        if (!sem_guest_mem_map_ro(mem, sv.u.ptr, (zi_size32_t)n, &r) || !r) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        if (!sem_guest_mem_map_rw(mem, dv.u.ptr, (zi_size32_t)n, &w) || !w) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        memmove(w, r, n);
        ip++;
        break;
      }
      case SIR_INST_MEM_FILL: {
        const sir_val_id_t dst_id = i->u.mem_fill.dst;
        const sir_val_id_t byte_id = i->u.mem_fill.byte;
        const sir_val_id_t len_id = i->u.mem_fill.len;
        if (dst_id >= f->value_count || byte_id >= f->value_count || len_id >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t dv = vals[dst_id];
        const sir_value_t bv = vals[byte_id];
        const sir_value_t lv = vals[len_id];
        if (dv.kind != SIR_VAL_PTR) {
          free(vals);
          return ZI_E_INVALID;
        }
        uint8_t byte = 0;
        if (bv.kind == SIR_VAL_I8) byte = bv.u.u8;
        else if (bv.kind == SIR_VAL_I32) byte = (uint8_t)bv.u.i32;
        else {
          free(vals);
          return ZI_E_INVALID;
        }
        int64_t ll = 0;
        if (lv.kind == SIR_VAL_I64) ll = lv.u.i64;
        else if (lv.kind == SIR_VAL_I32) ll = lv.u.i32;
        else {
          free(vals);
          return ZI_E_INVALID;
        }
        if (ll < 0 || ll > 0x7FFFFFFFll) {
          free(vals);
          return ZI_E_INVALID;
        }
        const uint32_t n = (uint32_t)ll;
        if (n == 0) {
          ip++;
          break;
        }
        uint8_t* w = NULL;
        if (!sem_guest_mem_map_rw(mem, dv.u.ptr, (zi_size32_t)n, &w) || !w) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        memset(w, (int)byte, n);
        ip++;
        break;
      }
      case SIR_INST_ATOMIC_RMW_I8:
      case SIR_INST_ATOMIC_RMW_I16:
      case SIR_INST_ATOMIC_RMW_I32:
      case SIR_INST_ATOMIC_RMW_I64: {
        const sir_val_id_t a = i->u.atomic_rmw.addr;
        const sir_val_id_t v = i->u.atomic_rmw.value;
        const sir_val_id_t dst = i->u.atomic_rmw.dst_old;
        if (a >= f->value_count || v >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t av = vals[a];
        if (av.kind != SIR_VAL_PTR) {
          free(vals);
          return ZI_E_INVALID;
        }
        const uint32_t align = i->u.atomic_rmw.align ? i->u.atomic_rmw.align : 1u;
        if (!is_pow2_u32(align)) {
          free(vals);
          return ZI_E_INVALID;
        }
        if (align > 1u) {
          const uint64_t addr = (uint64_t)av.u.ptr;
          if ((addr & (uint64_t)(align - 1u)) != 0ull) {
            free(vals);
            return 256;
          }
        }

        uint32_t size = 0;
        sir_val_kind_t want = SIR_VAL_INVALID;
        if (i->k == SIR_INST_ATOMIC_RMW_I8) {
          size = 1;
          want = SIR_VAL_I8;
        } else if (i->k == SIR_INST_ATOMIC_RMW_I16) {
          size = 2;
          want = SIR_VAL_I16;
        } else if (i->k == SIR_INST_ATOMIC_RMW_I32) {
          size = 4;
          want = SIR_VAL_I32;
        } else {
          size = 8;
          want = SIR_VAL_I64;
        }
        const sir_value_t vv = vals[v];
        if (vv.kind != want) {
          free(vals);
          return ZI_E_INVALID;
        }

        uint8_t* w = NULL;
        if (!sem_guest_mem_map_rw(mem, av.u.ptr, (zi_size32_t)size, &w) || !w) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        if (sink && sink->on_mem) sink->on_mem(sink->user, m, fid, ip, SIR_MEM_READ, av.u.ptr, size);

        sir_value_t old = {0};
        old.kind = want;
        if (size == 1) {
          uint8_t x = 0;
          memcpy(&x, w, 1);
          old.u.u8 = x;
          uint8_t nv = x;
          switch (i->u.atomic_rmw.op) {
            case SIR_ATOMIC_RMW_ADD: nv = (uint8_t)(x + vv.u.u8); break;
            case SIR_ATOMIC_RMW_AND: nv = (uint8_t)(x & vv.u.u8); break;
            case SIR_ATOMIC_RMW_OR: nv = (uint8_t)(x | vv.u.u8); break;
            case SIR_ATOMIC_RMW_XOR: nv = (uint8_t)(x ^ vv.u.u8); break;
            case SIR_ATOMIC_RMW_XCHG: nv = vv.u.u8; break;
            default:
              free(vals);
              return ZI_E_INVALID;
          }
          if (sink && sink->on_mem) sink->on_mem(sink->user, m, fid, ip, SIR_MEM_WRITE, av.u.ptr, 1);
          memcpy(w, &nv, 1);
        } else if (size == 2) {
          uint16_t x = 0;
          memcpy(&x, w, 2);
          old.u.u16 = x;
          uint16_t nv = x;
          switch (i->u.atomic_rmw.op) {
            case SIR_ATOMIC_RMW_ADD: nv = (uint16_t)(x + vv.u.u16); break;
            case SIR_ATOMIC_RMW_AND: nv = (uint16_t)(x & vv.u.u16); break;
            case SIR_ATOMIC_RMW_OR: nv = (uint16_t)(x | vv.u.u16); break;
            case SIR_ATOMIC_RMW_XOR: nv = (uint16_t)(x ^ vv.u.u16); break;
            case SIR_ATOMIC_RMW_XCHG: nv = vv.u.u16; break;
            default:
              free(vals);
              return ZI_E_INVALID;
          }
          if (sink && sink->on_mem) sink->on_mem(sink->user, m, fid, ip, SIR_MEM_WRITE, av.u.ptr, 2);
          memcpy(w, &nv, 2);
        } else if (size == 4) {
          int32_t x = 0;
          memcpy(&x, w, 4);
          old.u.i32 = x;
          uint32_t xo = (uint32_t)x;
          uint32_t xv = (uint32_t)vv.u.i32;
          uint32_t nv = xo;
          switch (i->u.atomic_rmw.op) {
            case SIR_ATOMIC_RMW_ADD: nv = xo + xv; break;
            case SIR_ATOMIC_RMW_AND: nv = xo & xv; break;
            case SIR_ATOMIC_RMW_OR: nv = xo | xv; break;
            case SIR_ATOMIC_RMW_XOR: nv = xo ^ xv; break;
            case SIR_ATOMIC_RMW_XCHG: nv = xv; break;
            default:
              free(vals);
              return ZI_E_INVALID;
          }
          const int32_t out = (int32_t)nv;
          if (sink && sink->on_mem) sink->on_mem(sink->user, m, fid, ip, SIR_MEM_WRITE, av.u.ptr, 4);
          memcpy(w, &out, 4);
        } else {
          int64_t x = 0;
          memcpy(&x, w, 8);
          old.u.i64 = x;
          uint64_t xo = (uint64_t)x;
          uint64_t xv = (uint64_t)vv.u.i64;
          uint64_t nv = xo;
          switch (i->u.atomic_rmw.op) {
            case SIR_ATOMIC_RMW_ADD: nv = xo + xv; break;
            case SIR_ATOMIC_RMW_AND: nv = xo & xv; break;
            case SIR_ATOMIC_RMW_OR: nv = xo | xv; break;
            case SIR_ATOMIC_RMW_XOR: nv = xo ^ xv; break;
            case SIR_ATOMIC_RMW_XCHG: nv = xv; break;
            default:
              free(vals);
              return ZI_E_INVALID;
          }
          const int64_t out = (int64_t)nv;
          if (sink && sink->on_mem) sink->on_mem(sink->user, m, fid, ip, SIR_MEM_WRITE, av.u.ptr, 8);
          memcpy(w, &out, 8);
        }

        vals[dst] = old;
        ip++;
        break;
      }
      case SIR_INST_ATOMIC_CMPXCHG_I64: {
        const sir_val_id_t a = i->u.atomic_cmpxchg_i64.addr;
        const sir_val_id_t e = i->u.atomic_cmpxchg_i64.expected;
        const sir_val_id_t d = i->u.atomic_cmpxchg_i64.desired;
        const sir_val_id_t dst = i->u.atomic_cmpxchg_i64.dst_old;
        if (a >= f->value_count || e >= f->value_count || d >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t av = vals[a];
        const sir_value_t ev = vals[e];
        const sir_value_t dv = vals[d];
        if (av.kind != SIR_VAL_PTR || ev.kind != SIR_VAL_I64 || dv.kind != SIR_VAL_I64) {
          free(vals);
          return ZI_E_INVALID;
        }
        const uint32_t align = i->u.atomic_cmpxchg_i64.align ? i->u.atomic_cmpxchg_i64.align : 1u;
        if (!is_pow2_u32(align)) {
          free(vals);
          return ZI_E_INVALID;
        }
        if (align > 1u) {
          const uint64_t addr = (uint64_t)av.u.ptr;
          if ((addr & (uint64_t)(align - 1u)) != 0ull) {
            free(vals);
            return 256;
          }
        }
        uint8_t* w = NULL;
        if (!sem_guest_mem_map_rw(mem, av.u.ptr, 8, &w) || !w) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        if (sink && sink->on_mem) sink->on_mem(sink->user, m, fid, ip, SIR_MEM_READ, av.u.ptr, 8);
        int64_t old = 0;
        memcpy(&old, w, 8);
        if (old == ev.u.i64) {
          if (sink && sink->on_mem) sink->on_mem(sink->user, m, fid, ip, SIR_MEM_WRITE, av.u.ptr, 8);
          memcpy(w, &dv.u.i64, 8);
        }
        vals[dst] = (sir_value_t){.kind = SIR_VAL_I64, .u.i64 = old};
        ip++;
        break;
      }
      case SIR_INST_ALLOCA: {
        const sir_val_id_t dst = i->u.alloca_.dst;
        if (dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const zi_ptr_t p = sem_guest_alloc(mem, (zi_size32_t)i->u.alloca_.size, (zi_size32_t)i->u.alloca_.align);
        if (!p) {
          free(vals);
          return ZI_E_OOM;
        }
        vals[dst] = (sir_value_t){.kind = SIR_VAL_PTR, .u.ptr = p};
        ip++;
        break;
      }
      case SIR_INST_STORE_I8:
      case SIR_INST_STORE_I16:
      case SIR_INST_STORE_I32:
      case SIR_INST_STORE_I64:
      case SIR_INST_STORE_PTR: {
        const sir_val_id_t a = i->u.store.addr;
        const sir_val_id_t v = i->u.store.value;
        if (a >= f->value_count || v >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t av = vals[a];
        if (av.kind != SIR_VAL_PTR) {
          free(vals);
          return ZI_E_INVALID;
        }
        const uint32_t align = i->u.store.align ? i->u.store.align : 1u;
        if (!is_pow2_u32(align)) {
          free(vals);
          return ZI_E_INVALID;
        }
        if (align > 1u) {
          const uint64_t addr = (uint64_t)av.u.ptr;
          if ((addr & (uint64_t)(align - 1u)) != 0ull) {
            free(vals);
            return 256;
          }
        }
        uint32_t size = 0;
        if (i->k == SIR_INST_STORE_I8) size = 1;
        else if (i->k == SIR_INST_STORE_I16) size = 2;
        else if (i->k == SIR_INST_STORE_I32) size = 4;
        else if (i->k == SIR_INST_STORE_I64) size = 8;
        else size = (uint32_t)sizeof(zi_ptr_t);
        uint8_t* w = NULL;
        if (!sem_guest_mem_map_rw(mem, av.u.ptr, (zi_size32_t)size, &w) || !w) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        if (sink && sink->on_mem) sink->on_mem(sink->user, m, fid, ip, SIR_MEM_WRITE, av.u.ptr, size);
        if (i->k == SIR_INST_STORE_I8) {
          const sir_value_t vv = vals[v];
          const uint8_t b = (vv.kind == SIR_VAL_I8) ? vv.u.u8 : (vv.kind == SIR_VAL_I32) ? (uint8_t)vv.u.i32 : 0;
          if (vv.kind != SIR_VAL_I8 && vv.kind != SIR_VAL_I32) {
            free(vals);
            return ZI_E_INVALID;
          }
          memcpy(w, &b, 1);
        } else if (i->k == SIR_INST_STORE_I16) {
          const sir_value_t vv = vals[v];
          uint16_t x = 0;
          if (vv.kind == SIR_VAL_I16) x = vv.u.u16;
          else if (vv.kind == SIR_VAL_I8) x = (uint16_t)vv.u.u8;
          else if (vv.kind == SIR_VAL_I32) x = (uint16_t)(uint32_t)vv.u.i32;
          else if (vv.kind == SIR_VAL_I64) x = (uint16_t)(uint64_t)vv.u.i64;
          else {
            free(vals);
            return ZI_E_INVALID;
          }
          memcpy(w, &x, 2);
        } else if (i->k == SIR_INST_STORE_I32) {
          const sir_value_t vv = vals[v];
          if (vv.kind != SIR_VAL_I32) {
            free(vals);
            return ZI_E_INVALID;
          }
          memcpy(w, &vv.u.i32, 4);
        } else if (i->k == SIR_INST_STORE_I64) {
          const sir_value_t vv = vals[v];
          if (vv.kind != SIR_VAL_I64) {
            free(vals);
            return ZI_E_INVALID;
          }
          memcpy(w, &vv.u.i64, 8);
        } else {
          const sir_value_t vv = vals[v];
          if (vv.kind != SIR_VAL_PTR) {
            free(vals);
            return ZI_E_INVALID;
          }
          memcpy(w, &vv.u.ptr, sizeof(vv.u.ptr));
        }
        ip++;
        break;
      }
      case SIR_INST_STORE_F32:
      case SIR_INST_STORE_F64: {
        const sir_val_id_t a = i->u.store.addr;
        const sir_val_id_t v = i->u.store.value;
        if (a >= f->value_count || v >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t av = vals[a];
        if (av.kind != SIR_VAL_PTR) {
          free(vals);
          return ZI_E_INVALID;
        }
        const uint32_t align = i->u.store.align ? i->u.store.align : 1u;
        if (!is_pow2_u32(align)) {
          free(vals);
          return ZI_E_INVALID;
        }
        if (align > 1u) {
          const uint64_t addr = (uint64_t)av.u.ptr;
          if ((addr & (uint64_t)(align - 1u)) != 0ull) {
            free(vals);
            return 256;
          }
        }
        const uint32_t size = (i->k == SIR_INST_STORE_F32) ? 4u : 8u;
        uint8_t* w = NULL;
        if (!sem_guest_mem_map_rw(mem, av.u.ptr, (zi_size32_t)size, &w) || !w) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        if (sink && sink->on_mem) sink->on_mem(sink->user, m, fid, ip, SIR_MEM_WRITE, av.u.ptr, size);
        const sir_value_t vv = vals[v];
        if (i->k == SIR_INST_STORE_F32) {
          if (vv.kind != SIR_VAL_F32) {
            free(vals);
            return ZI_E_INVALID;
          }
          const uint32_t bits = f32_canon_bits(vv.u.f32_bits);
          memcpy(w, &bits, 4);
        } else {
          if (vv.kind != SIR_VAL_F64) {
            free(vals);
            return ZI_E_INVALID;
          }
          const uint64_t bits = f64_canon_bits(vv.u.f64_bits);
          memcpy(w, &bits, 8);
        }
        ip++;
        break;
      }
      case SIR_INST_LOAD_I8:
      case SIR_INST_LOAD_I16:
      case SIR_INST_LOAD_I32:
      case SIR_INST_LOAD_I64:
      case SIR_INST_LOAD_PTR: {
        const sir_val_id_t a = i->u.load.addr;
        const sir_val_id_t dst = i->u.load.dst;
        if (a >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t av = vals[a];
        if (av.kind != SIR_VAL_PTR) {
          free(vals);
          return ZI_E_INVALID;
        }
        const uint32_t align = i->u.load.align ? i->u.load.align : 1u;
        if (!is_pow2_u32(align)) {
          free(vals);
          return ZI_E_INVALID;
        }
        if (align > 1u) {
          const uint64_t addr = (uint64_t)av.u.ptr;
          if ((addr & (uint64_t)(align - 1u)) != 0ull) {
            free(vals);
            return 256;
          }
        }
        uint32_t size = 0;
        if (i->k == SIR_INST_LOAD_I8) size = 1;
        else if (i->k == SIR_INST_LOAD_I16) size = 2;
        else if (i->k == SIR_INST_LOAD_I32) size = 4;
        else if (i->k == SIR_INST_LOAD_I64) size = 8;
        else size = (uint32_t)sizeof(zi_ptr_t);
        const uint8_t* r = NULL;
        if (!sem_guest_mem_map_ro(mem, av.u.ptr, (zi_size32_t)size, &r) || !r) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        if (sink && sink->on_mem) sink->on_mem(sink->user, m, fid, ip, SIR_MEM_READ, av.u.ptr, size);
        if (i->k == SIR_INST_LOAD_I8) {
          uint8_t b = 0;
          memcpy(&b, r, 1);
          vals[dst] = (sir_value_t){.kind = SIR_VAL_I8, .u.u8 = b};
        } else if (i->k == SIR_INST_LOAD_I16) {
          uint16_t x = 0;
          memcpy(&x, r, 2);
          vals[dst] = (sir_value_t){.kind = SIR_VAL_I16, .u.u16 = x};
        } else if (i->k == SIR_INST_LOAD_I32) {
          int32_t x = 0;
          memcpy(&x, r, 4);
          vals[dst] = (sir_value_t){.kind = SIR_VAL_I32, .u.i32 = x};
        } else if (i->k == SIR_INST_LOAD_I64) {
          int64_t x = 0;
          memcpy(&x, r, 8);
          vals[dst] = (sir_value_t){.kind = SIR_VAL_I64, .u.i64 = x};
        } else {
          zi_ptr_t x = 0;
          memcpy(&x, r, sizeof(x));
          vals[dst] = (sir_value_t){.kind = SIR_VAL_PTR, .u.ptr = x};
        }
        ip++;
        break;
      }
      case SIR_INST_LOAD_F32:
      case SIR_INST_LOAD_F64: {
        const sir_val_id_t a = i->u.load.addr;
        const sir_val_id_t dst = i->u.load.dst;
        if (a >= f->value_count || dst >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t av = vals[a];
        if (av.kind != SIR_VAL_PTR) {
          free(vals);
          return ZI_E_INVALID;
        }
        const uint32_t align = i->u.load.align ? i->u.load.align : 1u;
        if (!is_pow2_u32(align)) {
          free(vals);
          return ZI_E_INVALID;
        }
        if (align > 1u) {
          const uint64_t addr = (uint64_t)av.u.ptr;
          if ((addr & (uint64_t)(align - 1u)) != 0ull) {
            free(vals);
            return 256;
          }
        }
        const uint32_t size = (i->k == SIR_INST_LOAD_F32) ? 4u : 8u;
        const uint8_t* r = NULL;
        if (!sem_guest_mem_map_ro(mem, av.u.ptr, (zi_size32_t)size, &r) || !r) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        if (sink && sink->on_mem) sink->on_mem(sink->user, m, fid, ip, SIR_MEM_READ, av.u.ptr, size);
        if (i->k == SIR_INST_LOAD_F32) {
          uint32_t bits = 0;
          memcpy(&bits, r, 4);
          vals[dst] = (sir_value_t){.kind = SIR_VAL_F32, .u.f32_bits = f32_canon_bits(bits)};
        } else {
          uint64_t bits = 0;
          memcpy(&bits, r, 8);
          vals[dst] = (sir_value_t){.kind = SIR_VAL_F64, .u.f64_bits = f64_canon_bits(bits)};
        }
        ip++;
        break;
      }
      case SIR_INST_CALL_EXTERN: {
        const int32_t r = exec_call_extern(m, mem, host, fid, ip, sink, i, vals, f->value_count);
        if (r < 0) {
          free(vals);
          return r;
        }
        ip++;
        break;
      }
      case SIR_INST_CALL_FUNC: {
        const int32_t r = exec_call_func(m, mem, host, globals, global_count, i, vals, f->value_count, depth, sink);
        if (r < 0) {
          free(vals);
          return r;
        }
        ip++;
        break;
      }
      case SIR_INST_CALL_FUNC_PTR: {
        const int32_t r = exec_call_func_ptr(m, mem, host, globals, global_count, i, vals, f->value_count, depth, sink);
        if (r < 0) {
          free(vals);
          return r;
        }
        ip++;
        break;
      }
      case SIR_INST_RET:
        if (out_results && out_result_count) {
          free(vals);
          return ZI_E_INVALID;
        }
        free(vals);
        return 0;
      case SIR_INST_RET_VAL:
        if (out_result_count != 1 || !out_results) {
          free(vals);
          return ZI_E_INVALID;
        }
        if (i->u.ret_val.value >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        out_results[0] = vals[i->u.ret_val.value];
        free(vals);
        return 0;
      case SIR_INST_EXIT:
        free(vals);
        if (i->u.exit_.code < 0) return ZI_E_INVALID;
        if (i->u.exit_.code == INT32_MAX) return ZI_E_INVALID;
        // Encode "process exit requested" as rc+1 so callers can distinguish from
        // a normal `RET` (which returns 0).
        return i->u.exit_.code + 1;
      case SIR_INST_EXIT_VAL: {
        const sir_val_id_t cv = i->u.exit_val.code;
        if (cv >= f->value_count) {
          free(vals);
          return ZI_E_BOUNDS;
        }
        const sir_value_t v = vals[cv];
        if (v.kind == SIR_VAL_I32) {
          free(vals);
          if (v.u.i32 < 0) return ZI_E_INVALID;
          if (v.u.i32 == INT32_MAX) return ZI_E_INVALID;
          return v.u.i32 + 1;
        }
        if (v.kind == SIR_VAL_I64) {
          if (v.u.i64 < INT32_MIN || v.u.i64 > INT32_MAX) {
            free(vals);
            return ZI_E_INVALID;
          }
          free(vals);
          if (v.u.i64 < 0 || v.u.i64 == INT32_MAX) return ZI_E_INVALID;
          return (int32_t)v.u.i64 + 1;
        }
        {
          free(vals);
          return ZI_E_INVALID;
        }
      }
      default:
        free(vals);
        return ZI_E_INVALID;
    }
  }

  free(vals);
  return 0;
}

const char* sir_inst_kind_name(sir_inst_kind_t k) {
  switch (k) {
    case SIR_INST_INVALID:
      return "invalid";
    case SIR_INST_CONST_I1:
      return "const.i1";
    case SIR_INST_CONST_I8:
      return "const.i8";
    case SIR_INST_CONST_I16:
      return "const.i16";
    case SIR_INST_CONST_I32:
      return "const.i32";
    case SIR_INST_CONST_I64:
      return "const.i64";
    case SIR_INST_CONST_BOOL:
      return "const.bool";
    case SIR_INST_CONST_F32:
      return "const.f32";
    case SIR_INST_CONST_F64:
      return "const.f64";
    case SIR_INST_CONST_PTR:
      return "const.ptr";
    case SIR_INST_CONST_PTR_NULL:
      return "const.null";
    case SIR_INST_CONST_BYTES:
      return "const.bytes";
    case SIR_INST_I32_ADD:
      return "i32.add";
    case SIR_INST_I32_SUB:
      return "i32.sub";
    case SIR_INST_I32_MUL:
      return "i32.mul";
    case SIR_INST_I32_AND:
      return "i32.and";
    case SIR_INST_I32_OR:
      return "i32.or";
    case SIR_INST_I32_XOR:
      return "i32.xor";
    case SIR_INST_I32_NOT:
      return "i32.not";
    case SIR_INST_I32_NEG:
      return "i32.neg";
    case SIR_INST_I32_SHL:
      return "i32.shl";
    case SIR_INST_I32_SHR_S:
      return "i32.shr.s";
    case SIR_INST_I32_SHR_U:
      return "i32.shr.u";
    case SIR_INST_I32_DIV_S_SAT:
      return "i32.div.s.sat";
    case SIR_INST_I32_DIV_S_TRAP:
      return "i32.div.s.trap";
    case SIR_INST_I32_DIV_U_SAT:
      return "i32.div.u.sat";
    case SIR_INST_I32_REM_S_SAT:
      return "i32.rem.s.sat";
    case SIR_INST_I32_REM_U_SAT:
      return "i32.rem.u.sat";
    case SIR_INST_I32_CMP_EQ:
      return "i32.cmp.eq";
    case SIR_INST_I32_CMP_NE:
      return "i32.cmp.ne";
    case SIR_INST_I32_CMP_SLT:
      return "i32.cmp.slt";
    case SIR_INST_I32_CMP_SLE:
      return "i32.cmp.sle";
    case SIR_INST_I32_CMP_SGT:
      return "i32.cmp.sgt";
    case SIR_INST_I32_CMP_SGE:
      return "i32.cmp.sge";
    case SIR_INST_I32_CMP_ULT:
      return "i32.cmp.ult";
    case SIR_INST_I32_CMP_ULE:
      return "i32.cmp.ule";
    case SIR_INST_I32_CMP_UGT:
      return "i32.cmp.ugt";
    case SIR_INST_I32_CMP_UGE:
      return "i32.cmp.uge";
    case SIR_INST_F32_CMP_UEQ:
      return "f32.cmp.ueq";
    case SIR_INST_F64_CMP_OLT:
      return "f64.cmp.olt";
    case SIR_INST_GLOBAL_ADDR:
      return "global.addr";
    case SIR_INST_PTR_OFFSET:
      return "ptr.offset";
    case SIR_INST_PTR_ADD:
      return "ptr.add";
    case SIR_INST_PTR_SUB:
      return "ptr.sub";
    case SIR_INST_PTR_CMP_EQ:
      return "ptr.cmp.eq";
    case SIR_INST_PTR_CMP_NE:
      return "ptr.cmp.ne";
    case SIR_INST_PTR_TO_I64:
      return "ptr.to_i64";
    case SIR_INST_PTR_FROM_I64:
      return "ptr.from_i64";
    case SIR_INST_BOOL_NOT:
      return "bool.not";
    case SIR_INST_BOOL_AND:
      return "bool.and";
    case SIR_INST_BOOL_OR:
      return "bool.or";
    case SIR_INST_BOOL_XOR:
      return "bool.xor";
    case SIR_INST_I32_ZEXT_I8:
      return "i32.zext.i8";
    case SIR_INST_I32_ZEXT_I16:
      return "i32.zext.i16";
    case SIR_INST_I64_ZEXT_I32:
      return "i64.zext.i32";
    case SIR_INST_I32_TRUNC_I64:
      return "i32.trunc.i64";
    case SIR_INST_SELECT:
      return "select";
    case SIR_INST_BR:
      return "term.br";
    case SIR_INST_CBR:
      return "term.cbr";
    case SIR_INST_SWITCH:
      return "term.switch";
    case SIR_INST_MEM_COPY:
      return "mem.copy";
    case SIR_INST_MEM_FILL:
      return "mem.fill";
    case SIR_INST_ATOMIC_RMW_I8:
      return "atomic.rmw.i8";
    case SIR_INST_ATOMIC_RMW_I16:
      return "atomic.rmw.i16";
    case SIR_INST_ATOMIC_RMW_I32:
      return "atomic.rmw.i32";
    case SIR_INST_ATOMIC_RMW_I64:
      return "atomic.rmw.i64";
    case SIR_INST_ATOMIC_CMPXCHG_I64:
      return "atomic.cmpxchg.i64";
    case SIR_INST_ALLOCA:
      return "alloca";
    case SIR_INST_STORE_I8:
      return "store.i8";
    case SIR_INST_STORE_I16:
      return "store.i16";
    case SIR_INST_STORE_I32:
      return "store.i32";
    case SIR_INST_STORE_I64:
      return "store.i64";
    case SIR_INST_STORE_PTR:
      return "store.ptr";
    case SIR_INST_LOAD_I8:
      return "load.i8";
    case SIR_INST_LOAD_I16:
      return "load.i16";
    case SIR_INST_LOAD_I32:
      return "load.i32";
    case SIR_INST_LOAD_I64:
      return "load.i64";
    case SIR_INST_LOAD_PTR:
      return "load.ptr";
    case SIR_INST_STORE_F32:
      return "store.f32";
    case SIR_INST_STORE_F64:
      return "store.f64";
    case SIR_INST_LOAD_F32:
      return "load.f32";
    case SIR_INST_LOAD_F64:
      return "load.f64";
    case SIR_INST_CALL_EXTERN:
      return "call.extern";
    case SIR_INST_CALL_FUNC:
      return "call.func";
    case SIR_INST_CALL_FUNC_PTR:
      return "call.func_ptr";
    case SIR_INST_RET:
      return "term.ret";
    case SIR_INST_RET_VAL:
      return "term.ret_val";
    case SIR_INST_EXIT:
      return "term.exit";
    case SIR_INST_EXIT_VAL:
      return "term.exit_val";
    default:
      return "unknown";
  }
}

int32_t sir_module_run(const sir_module_t* m, sem_guest_mem_t* mem, sir_host_t host) {
  return sir_module_run_ex(m, mem, host, NULL);
}

int32_t sir_module_run_ex(const sir_module_t* m, sem_guest_mem_t* mem, sir_host_t host, const sir_exec_event_sink_t* sink) {
  if (!m || !mem) return ZI_E_INTERNAL;
  char err[160];
  if (!sir_module_validate(m, err, sizeof(err))) return ZI_E_INVALID;

  zi_ptr_t* globals = NULL;
  if (m->global_count) {
    globals = (zi_ptr_t*)calloc(m->global_count, sizeof(*globals));
    if (!globals) return ZI_E_OOM;
    for (uint32_t i = 0; i < m->global_count; i++) {
      const sir_global_t* g = &m->globals[i];
      const zi_ptr_t p = sem_guest_alloc(mem, (zi_size32_t)g->size, (zi_size32_t)g->align);
      if (!p) {
        free(globals);
        return ZI_E_OOM;
      }
      globals[i] = p;

      uint8_t* w = NULL;
      if (!sem_guest_mem_map_rw(mem, p, (zi_size32_t)g->size, &w) || !w) {
        free(globals);
        return ZI_E_BOUNDS;
      }
      memset(w, 0, g->size);
      if (g->init_len) {
        memcpy(w, g->init_bytes, g->init_len);
      }
    }
  }

  const int32_t r = exec_func(m, mem, host, globals, m->global_count, m->entry, NULL, 0, NULL, 0, 0, sink);
  free(globals);
  if (r > 0) return r - 1;
  return r;
}
