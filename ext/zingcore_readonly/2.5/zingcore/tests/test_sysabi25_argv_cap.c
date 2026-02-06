#include "zi_caps.h"
#include "zi_proc_argv25.h"
#include "zi_runtime25.h"
#include "zi_sysabi25.h"

#include <stdio.h>
#include <string.h>

static void write_u32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void write_u64le(uint8_t *p, uint64_t v) {
  write_u32le(p + 0, (uint32_t)(v & 0xFFFFFFFFu));
  write_u32le(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static uint32_t read_u32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void build_open_req(uint8_t req[40], const char *kind, const char *name) {
  // Packed open request (see zi_syscalls_caps25.c):
  // u64 kind_ptr, u32 kind_len, u64 name_ptr, u32 name_len, u32 mode, u64 params_ptr, u32 params_len
  write_u64le(req + 0, (uint64_t)(uintptr_t)kind);
  write_u32le(req + 8, (uint32_t)strlen(kind));
  write_u64le(req + 12, (uint64_t)(uintptr_t)name);
  write_u32le(req + 20, (uint32_t)strlen(name));
  write_u32le(req + 24, 0);
  write_u64le(req + 28, 0);
  write_u32le(req + 36, 0);
}

int main(void) {
  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);

  zi_caps_reset_for_test();
  if (!zi_caps_init()) {
    fprintf(stderr, "zi_caps_init failed\n");
    return 1;
  }
  if (!zi_proc_argv25_register()) {
    fprintf(stderr, "zi_proc_argv25_register failed\n");
    return 1;
  }

  // Provide argv snapshot.
  const char *av[] = {"prog", "-x", "hello", NULL};
  zi_runtime25_set_argv(3, av);

  uint8_t req[40];
  build_open_req(req, ZI_CAP_KIND_PROC, ZI_CAP_NAME_ARGV);

  zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)req);
  if (h < 3) {
    fprintf(stderr, "expected handle, got %d\n", h);
    return 1;
  }

  // Read the whole blob.
  uint8_t buf[512];
  uint32_t off = 0;
  for (;;) {
    int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(buf + off), (zi_size32_t)(sizeof(buf) - off));
    if (n < 0) {
      fprintf(stderr, "read failed: %d\n", n);
      return 1;
    }
    if (n == 0) break;
    off += (uint32_t)n;
    if (off == sizeof(buf)) break;
  }

  if (off < 8) {
    fprintf(stderr, "short argv blob\n");
    return 1;
  }

  uint32_t version = read_u32le(buf + 0);
  uint32_t argc = read_u32le(buf + 4);
  if (version != 1 || argc != 3) {
    fprintf(stderr, "bad header: version=%u argc=%u\n", version, argc);
    return 1;
  }

  uint32_t p = 8;
  const char *expect[] = {"prog", "-x", "hello"};
  for (uint32_t i = 0; i < argc; i++) {
    if (p + 4 > off) {
      fprintf(stderr, "truncated len\n");
      return 1;
    }
    uint32_t sl = read_u32le(buf + p);
    p += 4;
    if (p + sl > off) {
      fprintf(stderr, "truncated str\n");
      return 1;
    }
    if (sl != (uint32_t)strlen(expect[i]) || memcmp(buf + p, expect[i], sl) != 0) {
      fprintf(stderr, "argv mismatch at %u\n", i);
      return 1;
    }
    p += sl;
  }

  if (p != off) {
    fprintf(stderr, "unexpected trailing bytes\n");
    return 1;
  }

  if (zi_end(h) != 0) {
    fprintf(stderr, "end failed\n");
    return 1;
  }

  printf("ok\n");
  return 0;
}
