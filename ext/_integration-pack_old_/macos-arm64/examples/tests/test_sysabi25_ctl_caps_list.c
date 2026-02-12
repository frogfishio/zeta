#include "zi_caps.h"
#include "zi_runtime25.h"
#include "zi_sysabi25.h"

#include <stdio.h>
#include <string.h>

static void u16le_write(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void u32le_write(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint16_t u16le_read(const uint8_t *p) {
  return (uint16_t)p[0] | (uint16_t)(p[1] << 8);
}

static uint32_t u32le_read(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void build_req(uint8_t *req, uint16_t op, uint32_t rid, const uint8_t *payload, uint32_t payload_len) {
  memcpy(req + 0, "ZCL1", 4);
  u16le_write(req + 4, 1);
  u16le_write(req + 6, op);
  u32le_write(req + 8, rid);
  u32le_write(req + 12, 0);
  u32le_write(req + 16, 0);
  u32le_write(req + 20, payload_len);
  if (payload_len && payload) memcpy(req + 24, payload, payload_len);
}

static int parse_resp_header(const uint8_t *resp, uint32_t n, uint16_t *op, uint32_t *rid, uint32_t *status,
                             const uint8_t **payload, uint32_t *payload_len) {
  if (n < 24) return 0;
  if (!(resp[0] == 'Z' && resp[1] == 'C' && resp[2] == 'L' && resp[3] == '1')) return 0;
  if (u16le_read(resp + 4) != 1) return 0;
  if (op) *op = u16le_read(resp + 6);
  if (rid) *rid = u32le_read(resp + 8);
  if (status) *status = u32le_read(resp + 12);
  uint32_t plen = u32le_read(resp + 20);
  if (24u + plen > n) return 0;
  if (payload) *payload = resp + 24;
  if (payload_len) *payload_len = plen;
  return 1;
}

static zi_cap_v1 cap_exec_run_v1 = {
    .kind = "exec",
    .name = "run",
    .version = 1,
    .cap_flags = ZI_CAP_CAN_OPEN,
    .meta = (const uint8_t *)"meta",
    .meta_len = 4,
};

static zi_cap_v1 cap_async_default_v1 = {
    .kind = "async",
    .name = "default",
    .version = 1,
    .cap_flags = ZI_CAP_PURE,
    .meta = NULL,
    .meta_len = 0,
};

int main(void) {
  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);

  if (!zi_caps_init()) {
    fprintf(stderr, "zi_caps_init failed\n");
    return 1;
  }
  zi_caps_reset_for_test();

  // Register out-of-order; registry should sort deterministically.
  if (!zi_cap_register(&cap_exec_run_v1) || !zi_cap_register(&cap_async_default_v1)) {
    fprintf(stderr, "zi_cap_register failed\n");
    return 1;
  }

  // Typed cap list.
  int32_t n = zi_cap_count();
  if (n != 2) {
    fprintf(stderr, "expected 2 caps, got %d\n", n);
    return 1;
  }

  uint8_t outbuf[256];
  int32_t need0 = zi_cap_get_size(0);
  int32_t wrote0 = zi_cap_get(0, (zi_ptr_t)(uintptr_t)outbuf, (zi_size32_t)sizeof(outbuf));
  if (need0 <= 0 || wrote0 != need0) {
    fprintf(stderr, "cap_get(0) size mismatch\n");
    return 1;
  }

  // CTL caps list.
  uint8_t req[24];
  uint8_t resp[4096];
  build_req(req, (uint16_t)ZI_CTL_OP_CAPS_LIST, 42, NULL, 0);

  int32_t r = zi_ctl((zi_ptr_t)(uintptr_t)req, (zi_size32_t)sizeof(req), (zi_ptr_t)(uintptr_t)resp,
                     (zi_size32_t)sizeof(resp));
  if (r <= 0) {
    fprintf(stderr, "zi_ctl returned %d\n", r);
    return 1;
  }

  uint16_t op = 0;
  uint32_t rid = 0;
  uint32_t status = 0;
  const uint8_t *payload = NULL;
  uint32_t plen = 0;
  if (!parse_resp_header(resp, (uint32_t)r, &op, &rid, &status, &payload, &plen)) {
    fprintf(stderr, "failed to parse resp header\n");
    return 1;
  }
  if (op != (uint16_t)ZI_CTL_OP_CAPS_LIST || rid != 42 || status != 1) {
    fprintf(stderr, "unexpected resp header fields\n");
    return 1;
  }
  if (plen < 8) {
    fprintf(stderr, "payload too small\n");
    return 1;
  }

  uint32_t ver = u32le_read(payload + 0);
  uint32_t count = u32le_read(payload + 4);
  if (ver != 1 || count != 2) {
    fprintf(stderr, "unexpected caps list header (ver=%u count=%u)\n", ver, count);
    return 1;
  }

  // Verify first entry is async/default (sorted) and second is exec/run.
  uint32_t off = 8;
  for (uint32_t i = 0; i < count; i++) {
    if (off + 4 > plen) return 1;
    uint32_t klen = u32le_read(payload + off);
    off += 4;
    if (off + klen + 4 > plen) return 1;
    const uint8_t *k = payload + off;
    off += klen;

    uint32_t nlen = u32le_read(payload + off);
    off += 4;
    if (off + nlen + 4 > plen) return 1;
    const uint8_t *nm = payload + off;
    off += nlen;

    uint32_t flags = u32le_read(payload + off);
    off += 4;

    uint32_t mlen = u32le_read(payload + off);
    off += 4;
    if (off + mlen > plen) return 1;
    const uint8_t *meta = payload + off;
    off += mlen;

    if (i == 0) {
      if (!(klen == 5 && memcmp(k, "async", 5) == 0 && nlen == 7 && memcmp(nm, "default", 7) == 0)) {
        fprintf(stderr, "unexpected first cap identity\n");
        return 1;
      }
      if (flags != ZI_CAP_PURE) {
        fprintf(stderr, "unexpected first cap flags\n");
        return 1;
      }
      if (mlen != 0) {
        fprintf(stderr, "unexpected first cap meta\n");
        return 1;
      }
    } else {
      if (!(klen == 4 && memcmp(k, "exec", 4) == 0 && nlen == 3 && memcmp(nm, "run", 3) == 0)) {
        fprintf(stderr, "unexpected second cap identity\n");
        return 1;
      }
      if (flags != ZI_CAP_CAN_OPEN) {
        fprintf(stderr, "unexpected second cap flags\n");
        return 1;
      }
      if (!(mlen == 4 && memcmp(meta, "meta", 4) == 0)) {
        fprintf(stderr, "unexpected second cap meta\n");
        return 1;
      }
    }
  }

  if (off != plen) {
    fprintf(stderr, "payload size mismatch\n");
    return 1;
  }

  printf("ok\n");
  return 0;
}
