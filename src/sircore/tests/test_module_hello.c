#include "hosted_zabi.h"
#include "sir_module.h"
#include "sircore_vm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint8_t buf[128];
  uint32_t len;
} sink_t;

static int32_t sink_write(void* ctx, sem_guest_mem_t* mem, zi_ptr_t src_ptr, zi_size32_t len) {
  sink_t* s = (sink_t*)ctx;
  if (!s) return -10;
  if (len > (zi_size32_t)(sizeof(s->buf) - s->len)) return -2;
  const uint8_t* src = NULL;
  if (!sem_guest_mem_map_ro(mem, src_ptr, len, &src) || !src) return -2;
  memcpy(s->buf + s->len, src, len);
  s->len += len;
  return (int32_t)len;
}

static const sem_handle_ops_t sink_ops = {
    .read = NULL,
    .write = sink_write,
    .end = NULL,
};

static uint32_t hz_abi_version(void* u) { return sir_zi_abi_version((sir_hosted_zabi_t*)u); }
static int32_t hz_ctl(void* u, zi_ptr_t a, zi_size32_t b, zi_ptr_t c, zi_size32_t d) { return sir_zi_ctl((sir_hosted_zabi_t*)u, a, b, c, d); }
static int32_t hz_read(void* u, zi_handle_t h, zi_ptr_t p, zi_size32_t n) { return sir_zi_read((sir_hosted_zabi_t*)u, h, p, n); }
static int32_t hz_write(void* u, zi_handle_t h, zi_ptr_t p, zi_size32_t n) { return sir_zi_write((sir_hosted_zabi_t*)u, h, p, n); }
static int32_t hz_end(void* u, zi_handle_t h) { return sir_zi_end((sir_hosted_zabi_t*)u, h); }
static zi_ptr_t hz_alloc(void* u, zi_size32_t n) { return sir_zi_alloc((sir_hosted_zabi_t*)u, n); }
static int32_t hz_free(void* u, zi_ptr_t p) { return sir_zi_free((sir_hosted_zabi_t*)u, p); }
static int32_t hz_telemetry(void* u, zi_ptr_t a, zi_size32_t b, zi_ptr_t c, zi_size32_t d) { return sir_zi_telemetry((sir_hosted_zabi_t*)u, a, b, c, d); }
static int32_t hz_cap_count(void* u) { return sir_zi_cap_count((sir_hosted_zabi_t*)u); }
static int32_t hz_cap_get_size(void* u, int32_t i) { return sir_zi_cap_get_size((sir_hosted_zabi_t*)u, i); }
static int32_t hz_cap_get(void* u, int32_t i, zi_ptr_t p, zi_size32_t n) { return sir_zi_cap_get((sir_hosted_zabi_t*)u, i, p, n); }
static zi_handle_t hz_cap_open(void* u, zi_ptr_t p) { return sir_zi_cap_open((sir_hosted_zabi_t*)u, p); }
static uint32_t hz_handle_hflags(void* u, zi_handle_t h) { return sir_zi_handle_hflags((sir_hosted_zabi_t*)u, h); }

static int fail(const char* msg) {
  fprintf(stderr, "sircore_unit: %s\n", msg);
  return 1;
}

int main(void) {
  sem_guest_mem_t mem;
  if (!sem_guest_mem_init(&mem, 1024 * 1024, 0x10000ull)) {
    return fail("sem_guest_mem_init failed");
  }

  sir_hosted_zabi_t hz;
  if (!sir_hosted_zabi_init_with_mem(&hz, &mem, (sir_hosted_zabi_cfg_t){.abi_version = 0x00020005u})) {
    sem_guest_mem_dispose(&mem);
    return fail("sir_hosted_zabi_init_with_mem failed");
  }

  sink_t sink = {0};
  const zi_handle_t sink_h =
      sem_handle_alloc(&hz.handles, (sem_handle_entry_t){.ops = &sink_ops, .ctx = &sink, .hflags = ZI_H_WRITABLE | ZI_H_ENDABLE});
  if (sink_h < 3) {
    sir_hosted_zabi_dispose(&hz);
    sem_guest_mem_dispose(&mem);
    return fail("failed to alloc sink handle");
  }

  sir_host_t host = {0};
  host.user = &hz;
  host.v = (sir_host_vtable_t){
      .zi_abi_version = hz_abi_version,
      .zi_ctl = hz_ctl,
      .zi_read = hz_read,
      .zi_write = hz_write,
      .zi_end = hz_end,
      .zi_alloc = hz_alloc,
      .zi_free = hz_free,
      .zi_telemetry = hz_telemetry,
      .zi_cap_count = hz_cap_count,
      .zi_cap_get_size = hz_cap_get_size,
      .zi_cap_get = hz_cap_get,
      .zi_cap_open = hz_cap_open,
      .zi_handle_hflags = hz_handle_hflags,
  };

  sir_module_builder_t* b = sir_mb_new();
  if (!b) {
    sir_hosted_zabi_dispose(&hz);
    sem_guest_mem_dispose(&mem);
    return fail("sir_mb_new failed");
  }

  const sir_type_id_t ty_i32 = sir_mb_type_prim(b, SIR_PRIM_I32);
  const sir_type_id_t ty_i64 = sir_mb_type_prim(b, SIR_PRIM_I64);
  const sir_type_id_t ty_ptr = sir_mb_type_prim(b, SIR_PRIM_PTR);
  if (!ty_i32 || !ty_i64 || !ty_ptr) {
    sir_mb_free(b);
    sir_hosted_zabi_dispose(&hz);
    sem_guest_mem_dispose(&mem);
    return fail("sir_mb_type_prim failed");
  }

  const sir_type_id_t zi_write_params[] = {ty_i32, ty_ptr, ty_i64};
  sir_sig_t zi_write_sig = {
      .params = zi_write_params,
      .param_count = (uint32_t)(sizeof(zi_write_params) / sizeof(zi_write_params[0])),
      .results = NULL,
      .result_count = 0,
  };
  const sir_sym_id_t sym_zi_write = sir_mb_sym_extern_fn(b, "zi_write", zi_write_sig);
  if (!sym_zi_write) {
    sir_mb_free(b);
    sir_hosted_zabi_dispose(&hz);
    sem_guest_mem_dispose(&mem);
    return fail("sir_mb_sym_extern_fn failed");
  }

  const sir_func_id_t f = sir_mb_func_begin(b, "main");
  if (!f || !sir_mb_func_set_entry(b, f) || !sir_mb_func_set_value_count(b, f, 4)) {
    sir_mb_free(b);
    sir_hosted_zabi_dispose(&hz);
    sem_guest_mem_dispose(&mem);
    return fail("sir_mb_func_begin/set_entry/set_value_count failed");
  }

  // value slots:
  // 0 = handle (i32)
  // 1 = msg ptr (ptr)
  // 2 = msg len (i64)
  // 3 = unused
  static const uint8_t msg[] = "hello from sir_module\n";
  if (!sir_mb_emit_const_i32(b, f, 0, (int32_t)sink_h)) {
    sir_mb_free(b);
    sir_hosted_zabi_dispose(&hz);
    sem_guest_mem_dispose(&mem);
    return fail("sir_mb_emit_const_i32 failed");
  }
  if (!sir_mb_emit_const_bytes(b, f, 1, 2, msg, (uint32_t)(sizeof(msg) - 1))) {
    sir_mb_free(b);
    sir_hosted_zabi_dispose(&hz);
    sem_guest_mem_dispose(&mem);
    return fail("sir_mb_emit_const_bytes failed");
  }

  const sir_val_id_t args[] = {0, 1, 2};
  if (!sir_mb_emit_call_extern(b, f, sym_zi_write, args, (uint32_t)(sizeof(args) / sizeof(args[0])))) {
    sir_mb_free(b);
    sir_hosted_zabi_dispose(&hz);
    sem_guest_mem_dispose(&mem);
    return fail("sir_mb_emit_call_extern failed");
  }
  if (!sir_mb_emit_exit(b, f, 0)) {
    sir_mb_free(b);
    sir_hosted_zabi_dispose(&hz);
    sem_guest_mem_dispose(&mem);
    return fail("sir_mb_emit_exit failed");
  }

  sir_module_t* m = sir_mb_finalize(b);
  sir_mb_free(b);
  if (!m) {
    sir_hosted_zabi_dispose(&hz);
    sem_guest_mem_dispose(&mem);
    return fail("sir_mb_finalize failed");
  }

  const int32_t rc = sir_module_run(m, &mem, host);
  if (rc != 0) {
    sir_module_free(m);
    sir_hosted_zabi_dispose(&hz);
    sem_guest_mem_dispose(&mem);
    return fail("sir_module_run returned non-zero");
  }

  if (sink.len != (uint32_t)(sizeof(msg) - 1)) {
    sir_module_free(m);
    sir_hosted_zabi_dispose(&hz);
    sem_guest_mem_dispose(&mem);
    return fail("sink length mismatch");
  }
  if (memcmp(sink.buf, msg, sizeof(msg) - 1) != 0) {
    sir_module_free(m);
    sir_hosted_zabi_dispose(&hz);
    sem_guest_mem_dispose(&mem);
    return fail("sink contents mismatch");
  }

  sir_module_free(m);
  sir_hosted_zabi_dispose(&hz);
  sem_guest_mem_dispose(&mem);
  return 0;
}

