#include "hosted_zabi.h"
#include "hosted_file_fs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail(const char* msg) {
  fprintf(stderr, "sem_unit_tests: %s\n", msg);
  return 1;
}

static bool write_all(int fd, const uint8_t* p, size_t n) {
  while (n) {
    const ssize_t w = write(fd, p, n);
    if (w < 0) return false;
    p += (size_t)w;
    n -= (size_t)w;
  }
  return true;
}

static void u32le(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xffu);
  p[1] = (uint8_t)((v >> 8) & 0xffu);
  p[2] = (uint8_t)((v >> 16) & 0xffu);
  p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void u64le(uint8_t* p, uint64_t v) {
  u32le(p + 0, (uint32_t)(v & 0xFFFFFFFFu));
  u32le(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

int main(void) {
  char root_tmpl[] = "/tmp/sem_fsroot.XXXXXX";
  char* root = mkdtemp(root_tmpl);
  if (!root) return fail("mkdtemp failed");

  // Create a file under root.
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", root, "a.txt");
  const int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0) return fail("open temp file failed");
  const uint8_t contents[] = "filefs ok\n";
  if (!write_all(fd, contents, sizeof(contents) - 1)) {
    (void)close(fd);
    return fail("write temp file failed");
  }
  (void)close(fd);

  sem_cap_t caps[1];
  memset(caps, 0, sizeof(caps));
  caps[0].kind = "file";
  caps[0].name = "fs";
  caps[0].flags = SEM_ZI_CAP_CAN_OPEN | SEM_ZI_CAP_MAY_BLOCK;

  sir_hosted_zabi_t rt;
  if (!sir_hosted_zabi_init(
          &rt, (sir_hosted_zabi_cfg_t){.guest_mem_cap = 1024 * 1024, .guest_mem_base = 0x10000ull, .caps = caps, .cap_count = 1, .fs_root = root})) {
    return fail("sir_hosted_zabi_init failed");
  }

  // Guest path is absolute within sandbox.
  const char* guest_path = "/a.txt";
  const zi_size32_t guest_path_len = (zi_size32_t)strlen(guest_path);

  const zi_ptr_t guest_path_ptr = sir_zi_alloc(&rt, guest_path_len);
  if (!guest_path_ptr) return fail("alloc guest_path failed");
  uint8_t* w = NULL;
  if (!sem_guest_mem_map_rw(rt.mem, guest_path_ptr, guest_path_len, &w) || !w) return fail("map path failed");
  memcpy(w, guest_path, guest_path_len);

  // file/fs params blob: u64 path_ptr, u32 path_len, u32 oflags, u32 create_mode
  uint8_t params[20];
  u64le(params + 0, (uint64_t)guest_path_ptr);
  u32le(params + 8, guest_path_len);
  u32le(params + 12, ZI_FILE_O_READ);
  u32le(params + 16, 0);

  const zi_ptr_t params_ptr = sir_zi_alloc(&rt, (zi_size32_t)sizeof(params));
  if (!params_ptr) return fail("alloc params failed");
  if (!sem_guest_mem_map_rw(rt.mem, params_ptr, (zi_size32_t)sizeof(params), &w) || !w) return fail("map params failed");
  memcpy(w, params, sizeof(params));

  // zi_cap_open request: u64 kind_ptr, u32 kind_len, u64 name_ptr, u32 name_len, u32 mode, u64 params_ptr, u32 params_len
  const char* kind = "file";
  const char* name = "fs";
  const uint32_t kind_len = (uint32_t)strlen(kind);
  const uint32_t name_len = (uint32_t)strlen(name);

  const zi_ptr_t kind_ptr = sir_zi_alloc(&rt, kind_len);
  const zi_ptr_t name_ptr = sir_zi_alloc(&rt, name_len);
  if (!kind_ptr || !name_ptr) return fail("alloc kind/name failed");
  if (!sem_guest_mem_map_rw(rt.mem, kind_ptr, kind_len, &w) || !w) return fail("map kind failed");
  memcpy(w, kind, kind_len);
  if (!sem_guest_mem_map_rw(rt.mem, name_ptr, name_len, &w) || !w) return fail("map name failed");
  memcpy(w, name, name_len);

  uint8_t open_req[40];
  memset(open_req, 0, sizeof(open_req));
  u64le(open_req + 0, (uint64_t)kind_ptr);
  u32le(open_req + 8, kind_len);
  u64le(open_req + 12, (uint64_t)name_ptr);
  u32le(open_req + 20, name_len);
  u32le(open_req + 24, 0);
  u64le(open_req + 28, (uint64_t)params_ptr);
  u32le(open_req + 36, (uint32_t)sizeof(params));

  const zi_ptr_t open_req_ptr = sir_zi_alloc(&rt, (zi_size32_t)sizeof(open_req));
  if (!open_req_ptr) return fail("alloc open req failed");
  if (!sem_guest_mem_map_rw(rt.mem, open_req_ptr, (zi_size32_t)sizeof(open_req), &w) || !w) return fail("map open req failed");
  memcpy(w, open_req, sizeof(open_req));

  const zi_handle_t h = sir_zi_cap_open(&rt, open_req_ptr);
  if (h < 3) return fail("cap_open failed");

  // Read back file content.
  const zi_ptr_t buf_ptr = sir_zi_alloc(&rt, 64);
  if (!buf_ptr) return fail("alloc read buf failed");
  const int32_t n = sir_zi_read(&rt, h, buf_ptr, 64);
  if (n <= 0) return fail("read failed");
  const uint8_t* r = NULL;
  if (!sem_guest_mem_map_ro(rt.mem, buf_ptr, (zi_size32_t)n, &r) || !r) return fail("map read buf failed");
  if ((size_t)n != sizeof(contents) - 1) return fail("read size mismatch");
  if (memcmp(r, contents, sizeof(contents) - 1) != 0) return fail("read contents mismatch");

  (void)sir_zi_end(&rt, h);
  sir_hosted_zabi_dispose(&rt);

  // Best effort cleanup.
  (void)unlink(path);
  (void)rmdir(root);
  return 0;
}
