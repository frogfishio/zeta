#include "zi_caps.h"
#include "zi_file_fs25.h"
#include "zi_handles25.h"
#include "zi_runtime25.h"
#include "zi_sysabi25.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void build_open_req(uint8_t req[40], const char *kind, const char *name, const void *params, uint32_t params_len) {
  // Packed open request (see zi_syscalls_caps25.c):
  // u64 kind_ptr, u32 kind_len, u64 name_ptr, u32 name_len, u32 mode, u64 params_ptr, u32 params_len
  write_u64le(req + 0, (uint64_t)(uintptr_t)kind);
  write_u32le(req + 8, (uint32_t)strlen(kind));
  write_u64le(req + 12, (uint64_t)(uintptr_t)name);
  write_u32le(req + 20, (uint32_t)strlen(name));
  write_u32le(req + 24, 0);
  write_u64le(req + 28, (uint64_t)(uintptr_t)params);
  write_u32le(req + 36, params_len);
}

static void build_fs_params(uint8_t params[20], const char *path, uint32_t oflags, uint32_t create_mode) {
  // u64 path_ptr, u32 path_len, u32 oflags, u32 create_mode
  write_u64le(params + 0, (uint64_t)(uintptr_t)path);
  write_u32le(params + 8, (uint32_t)strlen(path));
  write_u32le(params + 12, oflags);
  write_u32le(params + 16, create_mode);
}

int main(void) {
  // Native memory mapping lets syscalls interpret pointers directly.
  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);

  zi_caps_reset_for_test();
  zi_handles25_reset_for_test();

  if (!zi_caps_init()) {
    fprintf(stderr, "zi_caps_init failed\n");
    return 1;
  }
  if (!zi_file_fs25_register()) {
    fprintf(stderr, "zi_file_fs25_register failed\n");
    return 1;
  }

  char root_template[] = "/tmp/zi_fs_root_XXXXXX";
  char *root = mkdtemp(root_template);
  if (!root) {
    perror("mkdtemp");
    return 1;
  }
  if (setenv("ZI_FS_ROOT", root, 1) != 0) {
    perror("setenv");
    return 1;
  }

  const char *guest_path = "/hello.txt";

  // Open for write (create+trunc).
  uint8_t params[20];
  build_fs_params(params, guest_path, ZI_FILE_O_WRITE | ZI_FILE_O_CREATE | ZI_FILE_O_TRUNC, 0644);

  uint8_t req[40];
  build_open_req(req, ZI_CAP_KIND_FILE, ZI_CAP_NAME_FS, params, (uint32_t)sizeof(params));

  zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)req);
  if (h < 3) {
    fprintf(stderr, "expected handle, got %d\n", h);
    return 1;
  }

  const char msg[] = "hello file cap\n";
  int32_t wn = zi_write(h, (zi_ptr_t)(uintptr_t)msg, (zi_size32_t)sizeof(msg) - 1);
  if (wn != (int32_t)(sizeof(msg) - 1)) {
    fprintf(stderr, "write failed: %d\n", wn);
    return 1;
  }

  if (zi_end(h) != 0) {
    fprintf(stderr, "end failed\n");
    return 1;
  }

  if (zi_write(h, (zi_ptr_t)(uintptr_t)msg, (zi_size32_t)sizeof(msg) - 1) != ZI_E_NOSYS) {
    fprintf(stderr, "expected ended handle to be invalid\n");
    return 1;
  }

  // Open for read.
  build_fs_params(params, guest_path, ZI_FILE_O_READ, 0);
  build_open_req(req, ZI_CAP_KIND_FILE, ZI_CAP_NAME_FS, params, (uint32_t)sizeof(params));

  zi_handle_t hr = zi_cap_open((zi_ptr_t)(uintptr_t)req);
  if (hr < 3) {
    fprintf(stderr, "expected read handle, got %d\n", hr);
    return 1;
  }

  char buf[64];
  memset(buf, 0, sizeof(buf));
  int32_t rn = zi_read(hr, (zi_ptr_t)(uintptr_t)buf, (zi_size32_t)sizeof(buf));
  if (rn <= 0) {
    fprintf(stderr, "read failed: %d\n", rn);
    return 1;
  }

  if ((size_t)rn != (sizeof(msg) - 1) || memcmp(buf, msg, (size_t)rn) != 0) {
    fprintf(stderr, "unexpected content\n");
    return 1;
  }

  if (zi_end(hr) != 0) {
    fprintf(stderr, "end(read) failed\n");
    return 1;
  }

  if (zi_read(hr, (zi_ptr_t)(uintptr_t)buf, (zi_size32_t)sizeof(buf)) != ZI_E_NOSYS) {
    fprintf(stderr, "expected ended read handle to be invalid\n");
    return 1;
  }

  // Root escape should be denied.
  const char *bad_path = "/../escape.txt";
  build_fs_params(params, bad_path, ZI_FILE_O_READ, 0);
  build_open_req(req, ZI_CAP_KIND_FILE, ZI_CAP_NAME_FS, params, (uint32_t)sizeof(params));
  zi_handle_t hb = zi_cap_open((zi_ptr_t)(uintptr_t)req);
  if (hb != ZI_E_DENIED) {
    fprintf(stderr, "expected denied for .. traversal, got %d\n", hb);
    return 1;
  }

  printf("ok\n");
  return 0;
}
