#include "sem_host.h"
#include "zcl1.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fail(const char* msg) {
  fprintf(stderr, "sem_unit_tests: %s\n", msg);
  return 1;
}

static bool req(uint8_t* out, uint32_t cap, uint16_t op, uint32_t rid, const uint8_t* payload, uint32_t payload_len, uint32_t* out_len) {
  return zcl1_write(out, cap, op, rid, 0, payload, payload_len, out_len);
}

int main(void) {
  // Disabled by default => denied.
  sem_host_t host0;
  sem_host_init(&host0, (sem_host_cfg_t){.caps = NULL, .cap_count = 0, .argv_enabled = false, .env_enabled = false});

  uint8_t q[ZCL1_HDR_SIZE + 8];
  uint32_t qn = 0;
  if (!req(q, (uint32_t)sizeof(q), SEM_ZI_CTL_OP_SEM_ARGV_COUNT, 7, NULL, 0, &qn)) return fail("failed to build ARGV_COUNT req");

  uint8_t r[256];
  const int32_t rn = sem_zi_ctl(&host0, q, qn, r, (uint32_t)sizeof(r));
  if (rn < 0) return fail("sem_zi_ctl transport error");

  zcl1_hdr_t h = {0};
  const uint8_t* p = NULL;
  if (!zcl1_parse(r, (uint32_t)rn, &h, &p)) return fail("parse denied response");
  if (h.op != SEM_ZI_CTL_OP_SEM_ARGV_COUNT || h.rid != 7) return fail("bad denied hdr");
  if (h.status != 0) return fail("expected denied status=0");

  // Enabled argv.
  const char* const argv1[] = {"a", "b"};
  sem_host_t host1;
  sem_host_init(&host1,
                (sem_host_cfg_t){.caps = NULL, .cap_count = 0, .argv_enabled = true, .argv = argv1, .argv_count = 2, .env_enabled = false});

  if (!req(q, (uint32_t)sizeof(q), SEM_ZI_CTL_OP_SEM_ARGV_COUNT, 1, NULL, 0, &qn)) return fail("failed to build ARGV_COUNT req2");
  const int32_t rn2 = sem_zi_ctl(&host1, q, qn, r, (uint32_t)sizeof(r));
  if (rn2 < 0) return fail("sem_zi_ctl transport error2");
  if (!zcl1_parse(r, (uint32_t)rn2, &h, &p)) return fail("parse ok response");
  if (h.status != 1 || h.payload_len != 4) return fail("bad ARGV_COUNT response");
  if (zcl1_read_u32le(p) != 2) return fail("bad argv count");

  uint8_t idx[4];
  zcl1_write_u32le(idx, 1);
  if (!req(q, (uint32_t)sizeof(q), SEM_ZI_CTL_OP_SEM_ARGV_GET, 2, idx, (uint32_t)sizeof(idx), &qn)) return fail("failed to build ARGV_GET");
  const int32_t rn3 = sem_zi_ctl(&host1, q, qn, r, (uint32_t)sizeof(r));
  if (rn3 < 0) return fail("sem_zi_ctl transport error3");
  if (!zcl1_parse(r, (uint32_t)rn3, &h, &p)) return fail("parse argv_get");
  if (h.status != 1) return fail("expected ok argv_get");
  if (h.payload_len < 4) return fail("argv_get payload too short");
  const uint32_t sl = zcl1_read_u32le(p);
  if (sl != 1) return fail("argv_get bad len");
  if (h.payload_len != 4 + sl) return fail("argv_get bad payload_len");
  if (memcmp(p + 4, "b", 1) != 0) return fail("argv_get bad bytes");

  // Enabled env.
  const sem_env_kv_t env1[] = {{.key = "K", .val = "V"}};
  sem_host_t host2;
  sem_host_init(&host2,
                (sem_host_cfg_t){.caps = NULL, .cap_count = 0, .argv_enabled = false, .env_enabled = true, .env = env1, .env_count = 1});

  if (!req(q, (uint32_t)sizeof(q), SEM_ZI_CTL_OP_SEM_ENV_COUNT, 3, NULL, 0, &qn)) return fail("failed to build ENV_COUNT");
  const int32_t rn4 = sem_zi_ctl(&host2, q, qn, r, (uint32_t)sizeof(r));
  if (rn4 < 0) return fail("sem_zi_ctl transport error4");
  if (!zcl1_parse(r, (uint32_t)rn4, &h, &p)) return fail("parse env_count");
  if (h.status != 1 || h.payload_len != 4) return fail("bad ENV_COUNT response");
  if (zcl1_read_u32le(p) != 1) return fail("bad env count");

  zcl1_write_u32le(idx, 0);
  if (!req(q, (uint32_t)sizeof(q), SEM_ZI_CTL_OP_SEM_ENV_GET, 4, idx, (uint32_t)sizeof(idx), &qn)) return fail("failed to build ENV_GET");
  const int32_t rn5 = sem_zi_ctl(&host2, q, qn, r, (uint32_t)sizeof(r));
  if (rn5 < 0) return fail("sem_zi_ctl transport error5");
  if (!zcl1_parse(r, (uint32_t)rn5, &h, &p)) return fail("parse env_get");
  if (h.status != 1) return fail("expected ok env_get");
  if (h.payload_len != 4 + 1 + 4 + 1) return fail("env_get bad payload_len");
  const uint32_t kl = zcl1_read_u32le(p + 0);
  if (kl != 1 || memcmp(p + 4, "K", 1) != 0) return fail("env_get bad key");
  const uint32_t vl = zcl1_read_u32le(p + 4 + kl);
  if (vl != 1 || memcmp(p + 4 + kl + 4, "V", 1) != 0) return fail("env_get bad val");

  return 0;
}
