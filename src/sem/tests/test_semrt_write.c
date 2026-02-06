#include "hosted_zabi.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  uint8_t buf[128];
  uint32_t len;
} buf_sink_t;

static int32_t sink_write(void* ctx, sem_guest_mem_t* mem, zi_ptr_t src_ptr, zi_size32_t len) {
  buf_sink_t* s = (buf_sink_t*)ctx;
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

static int fail(const char* msg) {
  fprintf(stderr, "sem_unit_tests: %s\n", msg);
  return 1;
}

int main(void) {
  sir_hosted_zabi_t rt;
  if (!sir_hosted_zabi_init(&rt, (sir_hosted_zabi_cfg_t){.guest_mem_cap = 1024 * 1024, .guest_mem_base = 0x10000ull})) {
    return fail("sir_hosted_zabi_init failed");
  }

  buf_sink_t sink = {0};
  const zi_handle_t h =
      sem_handle_alloc(&rt.handles, (sem_handle_entry_t){.ops = &sink_ops, .ctx = &sink, .hflags = ZI_H_WRITABLE});
  if (h < 3) {
    sir_hosted_zabi_dispose(&rt);
    return fail("failed to alloc handle");
  }

  const char* msg = "hello";
  const zi_size32_t msg_len = (zi_size32_t)strlen(msg);
  const zi_ptr_t p = sir_zi_alloc(&rt, msg_len);
  if (p == 0) {
    sir_hosted_zabi_dispose(&rt);
    return fail("alloc failed");
  }

  uint8_t* w = NULL;
  if (!sem_guest_mem_map_rw(rt.mem, p, msg_len, &w) || !w) {
    sir_hosted_zabi_dispose(&rt);
    return fail("map_rw failed");
  }
  memcpy(w, msg, msg_len);

  const int32_t n = sir_zi_write(&rt, h, p, msg_len);
  if (n != (int32_t)msg_len) {
    sir_hosted_zabi_dispose(&rt);
    return fail("zi_write bad count");
  }
  if (sink.len != msg_len) {
    sir_hosted_zabi_dispose(&rt);
    return fail("sink len mismatch");
  }
  if (memcmp(sink.buf, msg, msg_len) != 0) {
    sir_hosted_zabi_dispose(&rt);
    return fail("sink contents mismatch");
  }

  sir_hosted_zabi_dispose(&rt);
  return 0;
}
