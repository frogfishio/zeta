#include "zi_hostlib25.h"

#include "zingcore25.h"

#include "zi_caps.h"
#include "zi_event_bus25.h"
#include "zi_file_aio25.h"
#include "zi_handles25.h"
#include "zi_net_http25.h"
#include "zi_net_tcp25.h"
#include "zi_proc_argv25.h"
#include "zi_proc_env25.h"
#include "zi_proc_hopper25.h"
#include "zi_sys_info25.h"
#include "zi_sys_loop25.h"
#include "zi_telemetry.h"

#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct {
  zi_mem_v1 mem;
  zi_host_v1 host;
} hostlib_state;

static hostlib_state g_state;

static int count_env(const char *const *envp) {
  if (!envp) return 0;
  int n = 0;
  while (envp[n]) n++;
  return n;
}

static int32_t host_read(void *ctx, zi_handle_t h, zi_ptr_t dst_ptr, zi_size32_t cap) {
  (void)ctx;
  if (cap == 0) return 0;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_rw) return ZI_E_NOSYS;
  if (dst_ptr == 0) return ZI_E_BOUNDS;

  uint8_t *dst = NULL;
  if (!mem->map_rw(mem->ctx, dst_ptr, cap, &dst) || !dst) return ZI_E_BOUNDS;

  if (h == 0) {
    ssize_t n = read(0, dst, (size_t)cap);
    if (n < 0) return ZI_E_IO;
    return (int32_t)n;
  }

  const zi_handle_ops_v1 *ops = NULL;
  void *hctx = NULL;
  if (zi_handle25_lookup(h, &ops, &hctx, NULL) && ops && ops->read) {
    return ops->read(hctx, dst_ptr, cap);
  }
  return ZI_E_NOSYS;
}

static int32_t host_write(void *ctx, zi_handle_t h, zi_ptr_t src_ptr, zi_size32_t len) {
  (void)ctx;
  if (len == 0) return 0;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_ro) return ZI_E_NOSYS;
  if (src_ptr == 0) return ZI_E_BOUNDS;

  const uint8_t *src = NULL;
  if (!mem->map_ro(mem->ctx, src_ptr, len, &src) || !src) return ZI_E_BOUNDS;

  if (h == 1 || h == 2) {
    int fd = (h == 2) ? 2 : 1;
    ssize_t n = write(fd, src, (size_t)len);
    if (n < 0) return ZI_E_IO;
    return (int32_t)n;
  }

  const zi_handle_ops_v1 *ops = NULL;
  void *hctx = NULL;
  if (zi_handle25_lookup(h, &ops, &hctx, NULL) && ops && ops->write) {
    return ops->write(hctx, src_ptr, len);
  }
  return ZI_E_NOSYS;
}

static int32_t host_end(void *ctx, zi_handle_t h) {
  (void)ctx;
  if (h == 0 || h == 1 || h == 2) return 0;

  const zi_handle_ops_v1 *ops = NULL;
  void *hctx = NULL;
  if (!zi_handle25_lookup(h, &ops, &hctx, NULL) || !ops) return ZI_E_NOSYS;

  int32_t r = 0;
  if (ops->end) r = ops->end(hctx);
  (void)zi_handle25_release(h);
  return r;
}

static zi_ptr_t host_alloc(void *ctx, zi_size32_t size) {
  (void)ctx;
  if (size == 0) return 0;
  void *p = malloc((size_t)size);
  return (zi_ptr_t)(uintptr_t)p;
}

static int32_t host_free(void *ctx, zi_ptr_t ptr) {
  (void)ctx;
  if (ptr == 0) return ZI_E_INVALID;
  free((void *)(uintptr_t)ptr);
  return 0;
}

static int32_t host_telemetry(void *ctx, zi_ptr_t topic_ptr, zi_size32_t topic_len, zi_ptr_t msg_ptr,
                             zi_size32_t msg_len) {
  (void)ctx;
  const uint8_t *topic = (const uint8_t *)(uintptr_t)topic_ptr;
  const uint8_t *msg = (const uint8_t *)(uintptr_t)msg_ptr;
  (void)zi_telemetry_stderr_jsonl(NULL, topic, (uint32_t)topic_len, msg, (uint32_t)msg_len);
  return 0;
}

static int register_all_caps(void) {
  if (!zi_event_bus25_register()) return 0;
  if (!zi_file_aio25_register()) return 0;
  if (!zi_net_tcp25_register()) return 0;
  // net/http is intentionally opt-in (experimental / convenience only).
  // Enable explicitly via environment for dev/testing.
  const char *http_enable = getenv("ZI_ENABLE_HTTP_CAP");
  if (http_enable && http_enable[0] && http_enable[0] != '0') {
    if (!zi_net_http25_register()) return 0;
  }
  if (!zi_proc_argv25_register()) return 0;
  if (!zi_proc_env25_register()) return 0;
  if (!zi_proc_hopper25_register()) return 0;
  if (!zi_sys_info25_register()) return 0;
  if (!zi_sys_loop25_register()) return 0;
  return 1;
}

int zi_hostlib25_init_all(int argc, const char *const *argv, const char *const *envp) {
  if (!zingcore25_init()) return 0;
  if (!zi_handles25_init()) return 0;

  zi_mem_v1_native_init(&g_state.mem);
  zi_runtime25_set_mem(&g_state.mem);

  zi_runtime25_set_argv(argc, argv);
  zi_runtime25_set_env(count_env(envp), envp);

  g_state.host.ctx = NULL;
  g_state.host.abi_version = NULL;
  g_state.host.ctl = NULL;
  g_state.host.read = host_read;
  g_state.host.write = host_write;
  g_state.host.end = host_end;
  g_state.host.alloc = host_alloc;
  g_state.host.free = host_free;
  g_state.host.telemetry = host_telemetry;
  zi_runtime25_set_host(&g_state.host);

  return register_all_caps();
}
