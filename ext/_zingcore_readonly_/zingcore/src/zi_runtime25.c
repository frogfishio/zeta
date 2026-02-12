#include "zi_runtime25.h"

#include <stddef.h>

static const zi_host_v1 *g_host;
static const zi_mem_v1 *g_mem;
static int g_argc;
static const char *const *g_argv;
static int g_envc;
static const char *const *g_envp;

void zi_runtime25_set_host(const zi_host_v1 *host) { g_host = host; }
void zi_runtime25_set_mem(const zi_mem_v1 *mem) { g_mem = mem; }

const zi_host_v1 *zi_runtime25_host(void) { return g_host; }
const zi_mem_v1 *zi_runtime25_mem(void) { return g_mem; }

void zi_runtime25_set_argv(int argc, const char *const *argv) {
  g_argc = (argc < 0) ? 0 : argc;
  g_argv = argv;
}

void zi_runtime25_get_argv(int *out_argc, const char *const **out_argv) {
  if (out_argc) *out_argc = g_argc;
  if (out_argv) *out_argv = g_argv;
}

void zi_runtime25_set_env(int envc, const char *const *envp) {
  g_envc = (envc < 0) ? 0 : envc;
  g_envp = envp;
}

void zi_runtime25_get_env(int *out_envc, const char *const **out_envp) {
  if (out_envc) *out_envc = g_envc;
  if (out_envp) *out_envp = g_envp;
}

static int native_map_ro(void *ctx, zi_ptr_t ptr, zi_size32_t len, const uint8_t **out) {
  (void)ctx;
  if (!out) return 0;
  if (len == 0) {
    *out = (const uint8_t *)(uintptr_t)ptr;
    return 1;
  }
  if (ptr == 0) return 0;
  *out = (const uint8_t *)(uintptr_t)ptr;
  return 1;
}

static int native_map_rw(void *ctx, zi_ptr_t ptr, zi_size32_t len, uint8_t **out) {
  (void)ctx;
  if (!out) return 0;
  if (len == 0) {
    *out = (uint8_t *)(uintptr_t)ptr;
    return 1;
  }
  if (ptr == 0) return 0;
  *out = (uint8_t *)(uintptr_t)ptr;
  return 1;
}

void zi_mem_v1_native_init(zi_mem_v1 *out) {
  if (!out) return;
  out->ctx = NULL;
  out->map_ro = native_map_ro;
  out->map_rw = native_map_rw;
}
