#include "guest_mem.h"

#include <stdlib.h>
#include <string.h>

static uint32_t align_up_u32(uint32_t x, uint32_t a) {
  if (a == 0) return x;
  const uint32_t mask = a - 1u;
  return (x + mask) & ~mask;
}

bool sem_guest_mem_init(sem_guest_mem_t* m, uint32_t cap, uint64_t base) {
  if (!m) return false;
  if (cap == 0) return false;
  if (base == 0) return false;
  memset(m, 0, sizeof(*m));

  uint8_t* buf = (uint8_t*)calloc(1, cap);
  if (!buf) return false;

  m->buf = buf;
  m->cap = cap;
  m->brk = 0;
  m->base = base;
  return true;
}

void sem_guest_mem_dispose(sem_guest_mem_t* m) {
  if (!m) return;
  free(m->buf);
  memset(m, 0, sizeof(*m));
}

static bool sem_guest_bounds(const sem_guest_mem_t* m, zi_ptr_t ptr, zi_size32_t len, uint32_t* out_off) {
  if (!m || !m->buf) return false;
  if (ptr == 0) return false;
  if (ptr < m->base) return false;
  const uint64_t off64 = ptr - m->base;
  if (off64 > 0xFFFFFFFFull) return false;
  const uint32_t off = (uint32_t)off64;
  const uint64_t end = (uint64_t)off + (uint64_t)len;
  if (end > (uint64_t)m->brk) return false;
  if (out_off) *out_off = off;
  return true;
}

bool sem_guest_mem_map_ro(const sem_guest_mem_t* m, zi_ptr_t ptr, zi_size32_t len, const uint8_t** out) {
  if (!out) return false;
  *out = NULL;
  uint32_t off = 0;
  if (len == 0) {
    *out = (m && m->buf) ? m->buf : NULL;
    return *out != NULL;
  }
  if (!sem_guest_bounds(m, ptr, len, &off)) return false;
  *out = m->buf + off;
  return true;
}

bool sem_guest_mem_map_rw(sem_guest_mem_t* m, zi_ptr_t ptr, zi_size32_t len, uint8_t** out) {
  if (!out) return false;
  *out = NULL;
  uint32_t off = 0;
  if (len == 0) {
    *out = (m && m->buf) ? m->buf : NULL;
    return *out != NULL;
  }
  if (!sem_guest_bounds(m, ptr, len, &off)) return false;
  *out = m->buf + off;
  return true;
}

zi_ptr_t sem_guest_alloc(sem_guest_mem_t* m, zi_size32_t size, zi_size32_t align) {
  if (!m || !m->buf) return 0;
  if (size == 0) return 0;
  uint32_t a = align ? align : 16u;
  if ((a & (a - 1u)) != 0) return 0;

  const uint32_t start = align_up_u32(m->brk, a);
  const uint64_t end = (uint64_t)start + (uint64_t)size;
  if (end > (uint64_t)m->cap) return 0;
  m->brk = (uint32_t)end;
  return (zi_ptr_t)(m->base + (uint64_t)start);
}

int32_t sem_guest_free(sem_guest_mem_t* m, zi_ptr_t ptr) {
  (void)m;
  // MVP: deterministic no-op free. We still validate the pointer shape lightly.
  if (ptr == 0) return -1;
  return 0;
}

