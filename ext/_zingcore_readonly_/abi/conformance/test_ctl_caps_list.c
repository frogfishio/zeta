#include "zi_caps.h"
#include "zi_handles25.h"
#include "zi_runtime25.h"
#include "zi_sysabi25.h"
#include "zi_zcl1.h"

#include <stdio.h>
#include <string.h>

// Conformance test: zi_ctl CAPS_LIST returns valid ZCL1 frame

static void write_u16le(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void write_u32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int main(void) {
  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);
  
  if (!zi_caps_init()) {
    fprintf(stderr, "FAIL: zi_caps_init failed\n");
    return 1;
  }
  
  // Build CAPS_LIST request
  uint8_t req[24];
  memcpy(req + 0, "ZCL1", 4);
  write_u16le(req + 4, 1);        // version
  write_u16le(req + 6, 1);        // op=CAPS_LIST
  write_u32le(req + 8, 42);       // rid
  write_u32le(req + 12, 0);       // status (request)
  write_u32le(req + 16, 0);       // reserved
  write_u32le(req + 20, 0);       // payload_len
  
  uint8_t res[4096];
  int32_t n = zi_ctl((zi_ptr_t)(uintptr_t)req, 24, 
                     (zi_ptr_t)(uintptr_t)res, sizeof(res));
  
  if (n < 0) {
    fprintf(stderr, "FAIL: zi_ctl returned error %d\n", n);
    return 1;
  }
  
  if (n < 24) {
    fprintf(stderr, "FAIL: zi_ctl returned %d bytes, expected >=24\n", n);
    return 1;
  }
  
  // Parse response frame
  zi_zcl1_frame fr;
  if (!zi_zcl1_parse(res, (uint32_t)n, &fr)) {
    fprintf(stderr, "FAIL: zi_zcl1_parse failed\n");
    return 1;
  }
  
  if (fr.op != 1) {
    fprintf(stderr, "FAIL: response op=%u, expected 1\n", fr.op);
    return 1;
  }
  
  if (fr.rid != 42) {
    fprintf(stderr, "FAIL: response rid=%u, expected 42\n", fr.rid);
    return 1;
  }
  
  uint32_t status = zi_zcl1_read_u32(res + 12);
  if (status != 1) {
    fprintf(stderr, "FAIL: response status=%u, expected 1 (ok)\n", status);
    return 1;
  }
  
  if (fr.payload_len < 8) {
    fprintf(stderr, "FAIL: payload too small (%u bytes)\n", fr.payload_len);
    return 1;
  }
  
  uint32_t version = zi_zcl1_read_u32(fr.payload + 0);
  uint32_t cap_count = zi_zcl1_read_u32(fr.payload + 4);
  
  if (version != 1) {
    fprintf(stderr, "FAIL: caps list version=%u, expected 1\n", version);
    return 1;
  }
  
  printf("PASS: zi_ctl CAPS_LIST returned %u caps\n", cap_count);
  return 0;
}
