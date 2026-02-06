#include "../sem_host.h"
#include "../zcl1.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fail(const char* msg) {
  fprintf(stderr, "sem_unit_tests: %s\n", msg);
  return 1;
}

int main(void) {
  sem_host_t host;
  sem_host_init(&host, (sem_host_cfg_t){.caps = NULL, .cap_count = 0});

  uint8_t req[ZCL1_HDR_SIZE];
  uint32_t req_len = 0;
  if (!sem_build_caps_list_req(42, req, (uint32_t)sizeof(req), &req_len)) return fail("failed to build request");

  uint8_t resp[1024];
  const int32_t rc = sem_zi_ctl(&host, req, req_len, resp, (uint32_t)sizeof(resp));
  if (rc < 0) return fail("sem_zi_ctl returned transport error");

  zcl1_hdr_t rh = {0};
  const uint8_t* payload = NULL;
  if (!zcl1_parse(resp, (uint32_t)rc, &rh, &payload)) return fail("failed to parse ZCL1 response");
  if (rh.op != SEM_ZI_CTL_OP_CAPS_LIST) return fail("bad op");
  if (rh.rid != 42) return fail("bad rid echo");
  if (rh.status != 1) return fail("expected ok status");
  if (rh.payload_len != 8) return fail("expected version+count payload");

  const uint32_t ver = zcl1_read_u32le(payload + 0);
  const uint32_t n = zcl1_read_u32le(payload + 4);
  if (ver != 1) return fail("bad caps payload version");
  if (n != 0) return fail("expected empty caps list");

  return 0;
}
