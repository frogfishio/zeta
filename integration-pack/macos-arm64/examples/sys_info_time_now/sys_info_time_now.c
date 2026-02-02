#include <zi_hostlib25.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zi_sys_info25.h>
#include <zi_sysabi25.h>
#include <zi_zcl1.h>

static void write_u16le(uint8_t *p, uint16_t v) { zi_zcl1_write_u16(p, v); }
static void write_u32le(uint8_t *p, uint32_t v) { zi_zcl1_write_u32(p, v); }

static void write_u64le(uint8_t *p, uint64_t v) {
  write_u32le(p + 0, (uint32_t)(v & 0xFFFFFFFFu));
  write_u32le(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static uint64_t read_u64le(const uint8_t *p) {
  uint64_t lo = (uint64_t)zi_zcl1_read_u32(p + 0);
  uint64_t hi = (uint64_t)zi_zcl1_read_u32(p + 4);
  return lo | (hi << 32);
}

static void build_open_req(uint8_t req[40], const char *kind, const char *name) {
  // Matches zi_cap_open packed request format (little-endian).
  write_u64le(req + 0, (uint64_t)(uintptr_t)kind);
  write_u32le(req + 8, (uint32_t)strlen(kind));
  write_u64le(req + 12, (uint64_t)(uintptr_t)name);
  write_u32le(req + 20, (uint32_t)strlen(name));
  write_u32le(req + 24, 0);
  write_u64le(req + 28, 0);
  write_u32le(req + 36, 0);
}

static void build_zcl1_req(uint8_t out[24], uint16_t op, uint32_t rid) {
  memcpy(out + 0, "ZCL1", 4);
  write_u16le(out + 4, 1);
  write_u16le(out + 6, op);
  write_u32le(out + 8, rid);
  write_u32le(out + 12, 0);
  write_u32le(out + 16, 0);
  write_u32le(out + 20, 0);
}

static int read_frame(zi_handle_t h, uint8_t *buf, uint32_t cap, uint32_t *out_len) {
  uint32_t off = 0;
  for (;;) {
    int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(buf + off), (zi_size32_t)(cap - off));
    if (n == ZI_E_AGAIN) break;
    if (n < 0) return 0;
    if (n == 0) break;
    off += (uint32_t)n;
    if (off == cap) break;
  }
  *out_len = off;
  return 1;
}

int main(int argc, char **argv, char **envp) {
  if (!zi_hostlib25_init_all(argc, (const char *const *)argv, (const char *const *)envp)) {
    fprintf(stderr, "host init failed\n");
    return 111;
  }

  uint8_t open_req[40];
  build_open_req(open_req, ZI_CAP_KIND_SYS, ZI_CAP_NAME_INFO);

  zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)open_req);
  if (h < 3) {
    fprintf(stderr, "open sys/info@v1 failed: %d\n", h);
    return 1;
  }

  uint8_t req[24];
  build_zcl1_req(req, (uint16_t)ZI_SYS_INFO_OP_TIME_NOW, 1);

  if (zi_write(h, (zi_ptr_t)(uintptr_t)req, 24) != 24) {
    fprintf(stderr, "write failed\n");
    (void)zi_end(h);
    return 1;
  }

  uint8_t buf[256];
  uint32_t got = 0;
  if (!read_frame(h, buf, (uint32_t)sizeof(buf), &got)) {
    fprintf(stderr, "read failed\n");
    (void)zi_end(h);
    return 1;
  }

  zi_zcl1_frame z;
  if (!zi_zcl1_parse(buf, got, &z)) {
    fprintf(stderr, "bad ZCL1 frame\n");
    (void)zi_end(h);
    return 1;
  }

  // ZCL1 status: ok
  if (zi_zcl1_read_u32(buf + 12) != 1u) {
    fprintf(stderr, "TIME_NOW not ok\n");
    (void)zi_end(h);
    return 1;
  }

  if (z.op != (uint16_t)ZI_SYS_INFO_OP_TIME_NOW || z.rid != 1) {
    fprintf(stderr, "TIME_NOW response mismatch\n");
    (void)zi_end(h);
    return 1;
  }

  if (z.payload_len != 20u) {
    fprintf(stderr, "TIME_NOW payload_len=%u (want 20)\n", (unsigned)z.payload_len);
    (void)zi_end(h);
    return 1;
  }

  uint32_t v = zi_zcl1_read_u32(z.payload + 0);
  uint64_t realtime_ns = read_u64le(z.payload + 4);
  uint64_t monotonic_ns = read_u64le(z.payload + 12);

  printf("sys/info@v%u TIME_NOW realtime_ns=%llu monotonic_ns=%llu\n",
         (unsigned)v,
         (unsigned long long)realtime_ns,
         (unsigned long long)monotonic_ns);

  (void)zi_end(h);
  return 0;
}
