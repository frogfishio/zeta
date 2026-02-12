#include "zi_handles25.h"

#include <stddef.h>

#ifndef ZI_HANDLES25_MAX
#define ZI_HANDLES25_MAX 256
#endif

typedef struct {
  int in_use;
  uint32_t hflags;
  const zi_handle_ops_v1 *ops;
  const zi_handle_poll_ops_v1 *poll_ops;
  void *ctx;
} zi_handle_entry;

static struct {
  int initialized;
  zi_handle_entry entries[ZI_HANDLES25_MAX];
  uint32_t next;
} g_h;

static uint32_t idx_from_handle(zi_handle_t h) {
  if (h < 0) return 0xFFFFFFFFu;
  // Reserve 0/1/2.
  if (h < 3) return 0xFFFFFFFFu;
  return (uint32_t)(h - 3);
}

int zi_handles25_init(void) {
  if (g_h.initialized) return 1;
  g_h.initialized = 1;
  g_h.next = 3;
  for (uint32_t i = 0; i < ZI_HANDLES25_MAX; i++) {
    g_h.entries[i].in_use = 0;
    g_h.entries[i].hflags = 0;
    g_h.entries[i].ops = NULL;
    g_h.entries[i].poll_ops = NULL;
    g_h.entries[i].ctx = NULL;
  }
  return 1;
}

void zi_handles25_reset_for_test(void) {
  g_h.initialized = 0;
  (void)zi_handles25_init();
}

zi_handle_t zi_handle25_alloc(const zi_handle_ops_v1 *ops, void *ctx, uint32_t hflags) {
  return zi_handle25_alloc_with_poll(ops, NULL, ctx, hflags);
}

zi_handle_t zi_handle25_alloc_with_poll(const zi_handle_ops_v1 *ops, const zi_handle_poll_ops_v1 *poll_ops, void *ctx, uint32_t hflags) {
  if (!g_h.initialized && !zi_handles25_init()) return 0;

  // Simple linear probe starting from next.
  for (uint32_t probe = 0; probe < ZI_HANDLES25_MAX; probe++) {
    uint32_t h = g_h.next + probe;
    if (h < 3) continue;
    uint32_t idx = idx_from_handle((zi_handle_t)h);
    if (idx >= ZI_HANDLES25_MAX) continue;
    if (g_h.entries[idx].in_use) continue;

    g_h.entries[idx].in_use = 1;
    g_h.entries[idx].hflags = hflags;
    g_h.entries[idx].ops = ops;
    g_h.entries[idx].poll_ops = poll_ops;
    g_h.entries[idx].ctx = ctx;

    g_h.next = h + 1;
    if (g_h.next < 3) g_h.next = 3;
    return (zi_handle_t)h;
  }
  return 0;
}

int zi_handle25_lookup(zi_handle_t h, const zi_handle_ops_v1 **out_ops, void **out_ctx, uint32_t *out_hflags) {
  if (!g_h.initialized) return 0;
  uint32_t idx = idx_from_handle(h);
  if (idx >= ZI_HANDLES25_MAX) return 0;
  const zi_handle_entry *e = &g_h.entries[idx];
  if (!e->in_use) return 0;
  if (out_ops) *out_ops = e->ops;
  if (out_ctx) *out_ctx = e->ctx;
  if (out_hflags) *out_hflags = e->hflags;
  return 1;
}

int zi_handle25_release(zi_handle_t h) {
  if (!g_h.initialized) return 0;
  uint32_t idx = idx_from_handle(h);
  if (idx >= ZI_HANDLES25_MAX) return 0;
  zi_handle_entry *e = &g_h.entries[idx];
  if (!e->in_use) return 0;
  e->in_use = 0;
  e->hflags = 0;
  e->ops = NULL;
  e->poll_ops = NULL;
  e->ctx = NULL;
  return 1;
}

uint32_t zi_handle25_hflags(zi_handle_t h) {
  uint32_t flags = 0;
  (void)zi_handle25_lookup(h, NULL, NULL, &flags);
  return flags;
}

int zi_handle25_poll_fd(zi_handle_t h, int *out_fd) {
  if (!g_h.initialized) return 0;
  uint32_t idx = idx_from_handle(h);
  if (idx >= ZI_HANDLES25_MAX) return 0;
  const zi_handle_entry *e = &g_h.entries[idx];
  if (!e->in_use) return 0;
  if (!e->poll_ops || !e->poll_ops->get_fd) return 0;
  int fd = -1;
  if (!e->poll_ops->get_fd(e->ctx, &fd)) return 0;
  if (fd < 0) return 0;
  if (out_fd) *out_fd = fd;
  return 1;
}

int zi_handle25_poll_ops(zi_handle_t h, const zi_handle_poll_ops_v1 **out_poll_ops, void **out_ctx) {
  if (!g_h.initialized) return 0;
  uint32_t idx = idx_from_handle(h);
  if (idx >= ZI_HANDLES25_MAX) return 0;
  const zi_handle_entry *e = &g_h.entries[idx];
  if (!e->in_use) return 0;
  if (!e->poll_ops) return 0;
  if (out_poll_ops) *out_poll_ops = e->poll_ops;
  if (out_ctx) *out_ctx = e->ctx;
  return 1;
}
