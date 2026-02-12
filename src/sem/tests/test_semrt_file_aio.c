#include "hosted_zabi.h"

#include "zcl1.h"

#ifdef SIR_HAVE_ZINGCORE25
#include "zi_file_aio25.h"
#include "zi_sys_loop25.h"
#include "zi_sysabi25.h"
#endif

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

static void u64le(uint8_t* p, uint64_t v) {
  zcl1_write_u32le(p + 0, (uint32_t)(v & 0xFFFFFFFFu));
  zcl1_write_u32le(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static zi_ptr_t alloc_and_copy(sir_hosted_zabi_t* rt, const void* bytes, zi_size32_t n) {
  if (!rt) return 0;
  const zi_ptr_t p = sir_zi_alloc(rt, n);
  if (!p) return 0;
  uint8_t* w = NULL;
  if (!sem_guest_mem_map_rw(rt->mem, p, n, &w) || !w) return 0;
  if (n && bytes) memcpy(w, bytes, (size_t)n);
  return p;
}

static zi_handle_t cap_open_empty_params(sir_hosted_zabi_t* rt, const char* kind, const char* name) {
  if (!rt || !kind || !name) return -1;

  const uint32_t kind_len = (uint32_t)strlen(kind);
  const uint32_t name_len = (uint32_t)strlen(name);
  const zi_ptr_t kind_ptr = alloc_and_copy(rt, kind, (zi_size32_t)kind_len);
  const zi_ptr_t name_ptr = alloc_and_copy(rt, name, (zi_size32_t)name_len);
  if (!kind_ptr || !name_ptr) return -1;

  uint8_t req[40];
  memset(req, 0, sizeof(req));
  u64le(req + 0, (uint64_t)kind_ptr);
  zcl1_write_u32le(req + 8, kind_len);
  u64le(req + 12, (uint64_t)name_ptr);
  zcl1_write_u32le(req + 20, name_len);
  zcl1_write_u32le(req + 24, 0); // mode
  u64le(req + 28, 0);            // params_ptr
  zcl1_write_u32le(req + 36, 0); // params_len

  const zi_ptr_t req_ptr = alloc_and_copy(rt, req, (zi_size32_t)sizeof(req));
  if (!req_ptr) return -1;
  return sir_zi_cap_open(rt, req_ptr);
}

#ifdef SIR_HAVE_ZINGCORE25
static bool sys_loop_poll_until_ready(sir_hosted_zabi_t* rt, zi_handle_t loop_h, zi_handle_t watched_h, zi_ptr_t io_ptr,
                                     zi_size32_t io_cap) {
  if (!rt) return false;

  uint8_t poll_pl[8];
  zcl1_write_u32le(poll_pl + 0, 1u);
  zcl1_write_u32le(poll_pl + 4, 1000u); // 1s

  uint8_t poll_fr[ZCL1_HDR_SIZE + sizeof(poll_pl)];
  uint32_t poll_fr_len = 0;
  if (!zcl1_write(poll_fr, (uint32_t)sizeof(poll_fr), (uint16_t)ZI_SYS_LOOP_OP_POLL, 99 /*rid*/, 0, poll_pl,
                  (uint32_t)sizeof(poll_pl), &poll_fr_len)) {
    return false;
  }

  const zi_ptr_t poll_ptr = alloc_and_copy(rt, poll_fr, (zi_size32_t)poll_fr_len);
  if (!poll_ptr) return false;

  if (sir_zi_write(rt, loop_h, poll_ptr, (zi_size32_t)poll_fr_len) < 0) return false;

  for (int i = 0; i < 1000; i++) {
    const int32_t n = sir_zi_read(rt, loop_h, io_ptr, io_cap);
    if (n == ZI_E_AGAIN) continue;
    if (n <= 0) return false;

    uint8_t fr[65536];
    if ((uint32_t)n > sizeof(fr)) return false;

    const uint8_t* r = NULL;
    if (!sem_guest_mem_map_ro(rt->mem, io_ptr, (zi_size32_t)n, &r) || !r) return false;
    memcpy(fr, r, (size_t)n);

    zcl1_hdr_t h = {0};
    const uint8_t* pl = NULL;
    if (!zcl1_parse(fr, (uint32_t)n, &h, &pl)) return false;
    if (h.op != (uint16_t)ZI_SYS_LOOP_OP_POLL) return false;
    if (h.status == 0) return false;
    if (h.payload_len < 16) return false;

    const uint32_t ver = zcl1_read_u32le(pl + 0);
    const uint32_t count = zcl1_read_u32le(pl + 8);
    if (ver != 1u) return false;
    if (h.payload_len < 16u + count * 32u) return false;

    for (uint32_t ei = 0; ei < count; ei++) {
      const uint8_t* e = pl + 16u + ei * 32u;
      const uint32_t type = zcl1_read_u32le(e + 0);
      const uint32_t events = zcl1_read_u32le(e + 4);
      const uint32_t handle = zcl1_read_u32le(e + 8);
      if (type != 1u) continue; // READY
      if (handle != (uint32_t)watched_h) continue;
      if (events & 0x1u) return true; // READABLE
    }
    return false;
  }

  return false;
}

static bool read_zcl1_frame_wait(sir_hosted_zabi_t* rt, zi_handle_t loop_h, zi_handle_t watched_h, zi_ptr_t io_ptr,
                                zi_size32_t io_cap, uint8_t* out_fr, uint32_t out_cap, uint32_t* out_len) {
  if (!rt || !out_fr || !out_len) return false;

  for (int attempts = 0; attempts < 200; attempts++) {
    const int32_t n = sir_zi_read(rt, watched_h, io_ptr, io_cap);
    if (n == ZI_E_AGAIN) {
      if (!sys_loop_poll_until_ready(rt, loop_h, watched_h, io_ptr, io_cap)) return false;
      continue;
    }
    if (n <= 0) return false;
    if ((uint32_t)n > out_cap) return false;

    const uint8_t* r = NULL;
    if (!sem_guest_mem_map_ro(rt->mem, io_ptr, (zi_size32_t)n, &r) || !r) return false;
    memcpy(out_fr, r, (size_t)n);
    *out_len = (uint32_t)n;
    return true;
  }
  return false;
}
#endif

int main(void) {
#ifndef SIR_HAVE_ZINGCORE25
  // This test exercises file/aio + sys/loop, which are provided by zingcore25.
  return 0;
#else
  char root_tmpl[] = "/tmp/sem_aioroot.XXXXXX";
  char* root = mkdtemp(root_tmpl);
  if (!root) return fail("mkdtemp failed");

  // Create a file under root.
  char host_path[1024];
  snprintf(host_path, sizeof(host_path), "%s/%s", root, "a.txt");
  const int fd = open(host_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0) return fail("open temp file failed");
  const uint8_t contents[] = "fileaio ok\n";
  if (!write_all(fd, contents, sizeof(contents) - 1)) {
    (void)close(fd);
    return fail("write temp file failed");
  }
  (void)close(fd);

  sem_cap_t caps[2];
  memset(caps, 0, sizeof(caps));
  caps[0].kind = "sys";
  caps[0].name = "loop";
  caps[0].flags = SEM_ZI_CAP_CAN_OPEN | SEM_ZI_CAP_MAY_BLOCK;
  caps[1].kind = "file";
  caps[1].name = "aio";
  caps[1].flags = SEM_ZI_CAP_CAN_OPEN | SEM_ZI_CAP_MAY_BLOCK;

  sir_hosted_zabi_t rt;
  if (!sir_hosted_zabi_init(&rt, (sir_hosted_zabi_cfg_t){
                                     .guest_mem_cap = 1024 * 1024,
                                     .guest_mem_base = 0x10000ull,
                                     .caps = caps,
                                     .cap_count = 2,
                                     .fs_root = root,
                                 })) {
    return fail("sir_hosted_zabi_init failed");
  }

  const zi_handle_t loop_h = cap_open_empty_params(&rt, "sys", "loop");
  if (loop_h < 3) {
    sir_hosted_zabi_dispose(&rt);
    return fail("cap_open sys:loop failed");
  }

  const zi_handle_t aio_h = cap_open_empty_params(&rt, "file", "aio");
  if (aio_h < 3) {
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("cap_open file:aio failed");
  }

  // WATCH aio for readability.
  const uint64_t watch_id = 1;
  uint8_t watch_pl[20];
  zcl1_write_u32le(watch_pl + 0, (uint32_t)aio_h);
  zcl1_write_u32le(watch_pl + 4, 0x1u);
  u64le(watch_pl + 8, watch_id);
  zcl1_write_u32le(watch_pl + 16, 0u);

  uint8_t watch_fr[ZCL1_HDR_SIZE + sizeof(watch_pl)];
  uint32_t watch_fr_len = 0;
  if (!zcl1_write(watch_fr, (uint32_t)sizeof(watch_fr), (uint16_t)ZI_SYS_LOOP_OP_WATCH, 1 /*rid*/, 0, watch_pl,
                  (uint32_t)sizeof(watch_pl), &watch_fr_len)) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("build WATCH frame failed");
  }
  const zi_ptr_t watch_ptr = alloc_and_copy(&rt, watch_fr, (zi_size32_t)watch_fr_len);
  if (!watch_ptr) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("alloc WATCH failed");
  }

  const zi_ptr_t io_ptr = sir_zi_alloc(&rt, 65536);
  if (!io_ptr) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("alloc io buffer failed");
  }

  if (sir_zi_write(&rt, loop_h, watch_ptr, (zi_size32_t)watch_fr_len) < 0) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("WATCH write failed");
  }

  // Drain WATCH ack.
  uint8_t fr[65536];
  uint32_t fr_len = 0;
  if (!read_zcl1_frame_wait(&rt, loop_h, loop_h, io_ptr, 65536, fr, (uint32_t)sizeof(fr), &fr_len)) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("WATCH ack read failed");
  }

  // Build OPEN request for guest path.
  const char* guest_path = "/a.txt";
  const zi_size32_t guest_path_len = (zi_size32_t)strlen(guest_path);
  const zi_ptr_t guest_path_ptr = alloc_and_copy(&rt, guest_path, guest_path_len);
  if (!guest_path_ptr) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("alloc guest_path failed");
  }

  uint8_t open_pl[20];
  u64le(open_pl + 0, (uint64_t)guest_path_ptr);
  zcl1_write_u32le(open_pl + 8, (uint32_t)guest_path_len);
  zcl1_write_u32le(open_pl + 12, (uint32_t)ZI_FILE_O_READ);
  zcl1_write_u32le(open_pl + 16, 0u);

  uint8_t open_fr[ZCL1_HDR_SIZE + sizeof(open_pl)];
  uint32_t open_fr_len = 0;
  if (!zcl1_write(open_fr, (uint32_t)sizeof(open_fr), (uint16_t)ZI_FILE_AIO_OP_OPEN, 1 /*rid*/, 0, open_pl,
                  (uint32_t)sizeof(open_pl), &open_fr_len)) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("build OPEN frame failed");
  }
  const zi_ptr_t open_ptr = alloc_and_copy(&rt, open_fr, (zi_size32_t)open_fr_len);
  if (!open_ptr) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("alloc OPEN frame failed");
  }
  if (sir_zi_write(&rt, aio_h, open_ptr, (zi_size32_t)open_fr_len) < 0) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("OPEN write failed");
  }

  // Read OPEN ack then DONE.
  if (!read_zcl1_frame_wait(&rt, loop_h, aio_h, io_ptr, 65536, fr, (uint32_t)sizeof(fr), &fr_len)) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("OPEN ack read failed");
  }

  zcl1_hdr_t ah = {0};
  const uint8_t* apl = NULL;
  if (!zcl1_parse(fr, fr_len, &ah, &apl) || ah.op != (uint16_t)ZI_FILE_AIO_OP_OPEN || ah.rid != 1 || ah.status == 0) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("OPEN ack malformed");
  }

  if (!read_zcl1_frame_wait(&rt, loop_h, aio_h, io_ptr, 65536, fr, (uint32_t)sizeof(fr), &fr_len)) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("OPEN done read failed");
  }

  zcl1_hdr_t dh = {0};
  const uint8_t* dpl = NULL;
  if (!zcl1_parse(fr, fr_len, &dh, &dpl) || dh.op != (uint16_t)ZI_FILE_AIO_EV_DONE || dh.rid != 1 || dh.status == 0 ||
      dh.payload_len < 16u) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("OPEN done malformed");
  }
  const uint16_t orig_op = zcl1_read_u16le(dpl + 0);
  if (orig_op != (uint16_t)ZI_FILE_AIO_OP_OPEN) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("OPEN done orig_op mismatch");
  }
  const uint64_t file_id = (uint64_t)zcl1_read_u32le(dpl + 8) | ((uint64_t)zcl1_read_u32le(dpl + 12) << 32);
  if (file_id == 0) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("OPEN done file_id=0");
  }

  // READ request.
  uint8_t read_pl[24];
  u64le(read_pl + 0, file_id);
  u64le(read_pl + 8, 0ull);
  zcl1_write_u32le(read_pl + 16, 64u);
  zcl1_write_u32le(read_pl + 20, 0u);

  uint8_t read_fr[ZCL1_HDR_SIZE + sizeof(read_pl)];
  uint32_t read_fr_len = 0;
  if (!zcl1_write(read_fr, (uint32_t)sizeof(read_fr), (uint16_t)ZI_FILE_AIO_OP_READ, 2 /*rid*/, 0, read_pl,
                  (uint32_t)sizeof(read_pl), &read_fr_len)) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("build READ frame failed");
  }

  const zi_ptr_t read_ptr = alloc_and_copy(&rt, read_fr, (zi_size32_t)read_fr_len);
  if (!read_ptr) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("alloc READ frame failed");
  }
  if (sir_zi_write(&rt, aio_h, read_ptr, (zi_size32_t)read_fr_len) < 0) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("READ write failed");
  }

  // READ ack then DONE.
  if (!read_zcl1_frame_wait(&rt, loop_h, aio_h, io_ptr, 65536, fr, (uint32_t)sizeof(fr), &fr_len)) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("READ ack read failed");
  }
  if (!read_zcl1_frame_wait(&rt, loop_h, aio_h, io_ptr, 65536, fr, (uint32_t)sizeof(fr), &fr_len)) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("READ done read failed");
  }

  zcl1_hdr_t rd = {0};
  const uint8_t* rpl = NULL;
  if (!zcl1_parse(fr, fr_len, &rd, &rpl) || rd.op != (uint16_t)ZI_FILE_AIO_EV_DONE || rd.rid != 2 || rd.status == 0 ||
      rd.payload_len < 8u) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("READ done malformed");
  }
  const uint16_t read_orig = zcl1_read_u16le(rpl + 0);
  const uint32_t nbytes = zcl1_read_u32le(rpl + 4);
  if (read_orig != (uint16_t)ZI_FILE_AIO_OP_READ) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("READ done orig_op mismatch");
  }
  if (rd.payload_len < 8u + nbytes) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("READ done truncated");
  }
  if (nbytes != (uint32_t)(sizeof(contents) - 1u)) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("READ size mismatch");
  }
  if (memcmp(rpl + 8, contents, sizeof(contents) - 1u) != 0) {
    (void)sir_zi_end(&rt, aio_h);
    (void)sir_zi_end(&rt, loop_h);
    sir_hosted_zabi_dispose(&rt);
    return fail("READ contents mismatch");
  }

  (void)sir_zi_end(&rt, aio_h);
  (void)sir_zi_end(&rt, loop_h);
  sir_hosted_zabi_dispose(&rt);

  // Best-effort cleanup.
  (void)unlink(host_path);
  (void)rmdir(root);
  return 0;
#endif
}
