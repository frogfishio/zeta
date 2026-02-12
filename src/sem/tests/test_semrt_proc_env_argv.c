#include "hosted_zabi.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char* msg) {
  fprintf(stderr, "sem_unit_tests: %s\n", msg);
  return 1;
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

static uint32_t r_u32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_all(sir_hosted_zabi_t* rt, zi_handle_t h, uint8_t* out, uint32_t out_cap, uint32_t* out_len) {
  if (!rt || !out || !out_len) return fail("read_all bad args");
  *out_len = 0;

  const zi_ptr_t buf_ptr = sir_zi_alloc(rt, 64);
  if (!buf_ptr) return fail("alloc read buf failed");

  while (1) {
    const int32_t n = sir_zi_read(rt, h, buf_ptr, 64);
    if (n < 0) return fail("read failed");
    if (n == 0) break;
    if (*out_len + (uint32_t)n > out_cap) return fail("read_all overflow");

    const uint8_t* r = NULL;
    if (!sem_guest_mem_map_ro(rt->mem, buf_ptr, (zi_size32_t)n, &r) || !r) return fail("map read buf failed");
    memcpy(out + *out_len, r, (size_t)n);
    *out_len += (uint32_t)n;
  }
  return 0;
}

static zi_handle_t open_cap(sir_hosted_zabi_t* rt, const char* kind, const char* name) {
  const uint32_t kind_len = (uint32_t)strlen(kind);
  const uint32_t name_len = (uint32_t)strlen(name);

  zi_ptr_t kind_ptr = sir_zi_alloc(rt, kind_len);
  zi_ptr_t name_ptr = sir_zi_alloc(rt, name_len);
  if (!kind_ptr || !name_ptr) return (zi_handle_t)-10;

  uint8_t* w = NULL;
  if (!sem_guest_mem_map_rw(rt->mem, kind_ptr, kind_len, &w) || !w) return (zi_handle_t)-10;
  memcpy(w, kind, kind_len);
  if (!sem_guest_mem_map_rw(rt->mem, name_ptr, name_len, &w) || !w) return (zi_handle_t)-10;
  memcpy(w, name, name_len);

  uint8_t open_req[40];
  memset(open_req, 0, sizeof(open_req));
  u64le(open_req + 0, (uint64_t)kind_ptr);
  u32le(open_req + 8, kind_len);
  u64le(open_req + 12, (uint64_t)name_ptr);
  u32le(open_req + 20, name_len);
  u32le(open_req + 24, 0);
  u64le(open_req + 28, 0);
  u32le(open_req + 36, 0);

  const zi_ptr_t open_req_ptr = sir_zi_alloc(rt, (zi_size32_t)sizeof(open_req));
  if (!open_req_ptr) return (zi_handle_t)-10;
  if (!sem_guest_mem_map_rw(rt->mem, open_req_ptr, (zi_size32_t)sizeof(open_req), &w) || !w) return (zi_handle_t)-10;
  memcpy(w, open_req, sizeof(open_req));

  return sir_zi_cap_open(rt, open_req_ptr);
}

int main(void) {
  const char* argv0[] = {"a", "bcd"};
  const sem_env_kv_t env0[] = {{"K", "V"}};

  sem_cap_t caps[2];
  memset(caps, 0, sizeof(caps));
  caps[0].kind = "proc";
  caps[0].name = "argv";
  caps[0].flags = SEM_ZI_CAP_CAN_OPEN | SEM_ZI_CAP_PURE;
  caps[1].kind = "proc";
  caps[1].name = "env";
  caps[1].flags = SEM_ZI_CAP_CAN_OPEN | SEM_ZI_CAP_PURE;

  sir_hosted_zabi_t rt;
  if (!sir_hosted_zabi_init(&rt,
                            (sir_hosted_zabi_cfg_t){.guest_mem_cap = 1024 * 1024,
                                                   .guest_mem_base = 0x10000ull,
                                                   .caps = caps,
                                                   .cap_count = 2,
                                                   .argv_enabled = true,
                                                   .argv = argv0,
                                                   .argv_count = 2,
                                                   .env_enabled = true,
                                                   .env = env0,
                                                   .env_count = 1})) {
    return fail("sir_hosted_zabi_init failed");
  }

  // proc/argv
  const zi_handle_t h_argv = open_cap(&rt, "proc", "argv");
  if (h_argv < 3) return fail("cap_open proc/argv failed");

  uint8_t buf[256];
  uint32_t n = 0;
  if (read_all(&rt, h_argv, buf, (uint32_t)sizeof(buf), &n) != 0) return 1;

  if (n < 8) return fail("argv blob too small");
  uint32_t off = 0;
  const uint32_t ver = r_u32le(buf + off);
  off += 4;
  const uint32_t argc = r_u32le(buf + off);
  off += 4;
  if (ver != 1) return fail("argv version mismatch");
  if (argc != 2) return fail("argv argc mismatch");

  const char* want_argv[] = {"a", "bcd"};
  for (uint32_t i = 0; i < argc; i++) {
    if (off + 4 > n) return fail("argv parse oob");
    const uint32_t slen = r_u32le(buf + off);
    off += 4;
    if (off + slen > n) return fail("argv parse oob2");
    const char* s = (const char*)(buf + off);
    const uint32_t want_len = (uint32_t)strlen(want_argv[i]);
    if (slen != want_len) return fail("argv entry len mismatch");
    if (memcmp(s, want_argv[i], want_len) != 0) return fail("argv entry mismatch");
    off += slen;
  }
  if (off != n) return fail("argv trailing bytes");
  (void)sir_zi_end(&rt, h_argv);

  // proc/env
  const zi_handle_t h_env = open_cap(&rt, "proc", "env");
  if (h_env < 3) return fail("cap_open proc/env failed");

  uint32_t n2 = 0;
  if (read_all(&rt, h_env, buf, (uint32_t)sizeof(buf), &n2) != 0) return 1;
  if (n2 < 8) return fail("env blob too small");
  off = 0;
  const uint32_t ver2 = r_u32le(buf + off);
  off += 4;
  const uint32_t envc = r_u32le(buf + off);
  off += 4;
  if (ver2 != 1) return fail("env version mismatch");
  if (envc != 1) return fail("env count mismatch");

  if (off + 4 > n2) return fail("env parse oob");
  const uint32_t elen = r_u32le(buf + off);
  off += 4;
  if (off + elen > n2) return fail("env parse oob2");
  const char* e = (const char*)(buf + off);
  const char* want_env = "K=V";
  if (elen != (uint32_t)strlen(want_env)) return fail("env entry len mismatch");
  if (memcmp(e, want_env, (size_t)elen) != 0) return fail("env entry mismatch");
  off += elen;

  if (off != n2) return fail("env trailing bytes");
  (void)sir_zi_end(&rt, h_env);

  sir_hosted_zabi_dispose(&rt);
  return 0;
}
