#include "zcl1.h"

#include <string.h>

static const uint8_t ZCL1_MAGIC[4] = {'Z', 'C', 'L', '1'};

uint16_t zcl1_read_u16le(const uint8_t* p) {
  return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

uint32_t zcl1_read_u32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void zcl1_write_u16le(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xffu);
  p[1] = (uint8_t)((v >> 8) & 0xffu);
}

void zcl1_write_u32le(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xffu);
  p[1] = (uint8_t)((v >> 8) & 0xffu);
  p[2] = (uint8_t)((v >> 16) & 0xffu);
  p[3] = (uint8_t)((v >> 24) & 0xffu);
}

bool zcl1_parse(const uint8_t* buf, uint32_t len, zcl1_hdr_t* out_hdr, const uint8_t** out_payload) {
  if (!buf || !out_hdr || !out_payload) return false;
  if (len < ZCL1_HDR_SIZE) return false;
  if (memcmp(buf, ZCL1_MAGIC, 4) != 0) return false;

  zcl1_hdr_t h = {0};
  h.version = zcl1_read_u16le(buf + 4);
  h.op = zcl1_read_u16le(buf + 6);
  h.rid = zcl1_read_u32le(buf + 8);
  h.status = zcl1_read_u32le(buf + 12);
  h.reserved = zcl1_read_u32le(buf + 16);
  h.payload_len = zcl1_read_u32le(buf + 20);

  if (h.version != ZCL1_VERSION) return false;
  if (h.reserved != 0) return false;
  if ((uint64_t)ZCL1_HDR_SIZE + (uint64_t)h.payload_len > (uint64_t)len) return false;

  *out_hdr = h;
  *out_payload = buf + ZCL1_HDR_SIZE;
  return true;
}

bool zcl1_write(uint8_t* buf, uint32_t cap, uint16_t op, uint32_t rid, uint32_t status, const uint8_t* payload,
                uint32_t payload_len, uint32_t* out_len) {
  if (!buf || !out_len) return false;
  if ((uint64_t)ZCL1_HDR_SIZE + (uint64_t)payload_len > (uint64_t)cap) return false;
  if (payload_len != 0 && payload == NULL) return false;

  memcpy(buf, ZCL1_MAGIC, 4);
  zcl1_write_u16le(buf + 4, ZCL1_VERSION);
  zcl1_write_u16le(buf + 6, op);
  zcl1_write_u32le(buf + 8, rid);
  zcl1_write_u32le(buf + 12, status);
  zcl1_write_u32le(buf + 16, 0);
  zcl1_write_u32le(buf + 20, payload_len);
  if (payload_len) memcpy(buf + ZCL1_HDR_SIZE, payload, payload_len);

  *out_len = ZCL1_HDR_SIZE + payload_len;
  return true;
}

static bool zcl1_pack_u32_str(uint8_t* buf, uint32_t cap, uint32_t* inout_off, const char* s) {
  if (!buf || !inout_off) return false;
  const uint32_t off = *inout_off;
  const uint32_t n = s ? (uint32_t)strlen(s) : 0;
  if ((uint64_t)off + 4ull + (uint64_t)n > (uint64_t)cap) return false;
  zcl1_write_u32le(buf + off, n);
  if (n) memcpy(buf + off + 4, s, n);
  *inout_off = off + 4 + n;
  return true;
}

bool zcl1_write_error_payload(uint8_t* buf, uint32_t cap, const char* trace, const char* msg, const char* detail,
                              uint32_t* out_len) {
  if (!buf || !out_len) return false;
  uint32_t off = 0;
  if (!zcl1_pack_u32_str(buf, cap, &off, trace ? trace : "")) return false;
  if (!zcl1_pack_u32_str(buf, cap, &off, msg ? msg : "")) return false;
  if (!zcl1_pack_u32_str(buf, cap, &off, detail ? detail : "")) return false;
  *out_len = off;
  return true;
}
