#include "zi_zcl1.h"

#include <string.h>

uint16_t zi_zcl1_read_u16(const uint8_t *p) {
  return (uint16_t)p[0] | (uint16_t)(p[1] << 8);
}

uint32_t zi_zcl1_read_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void zi_zcl1_write_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

void zi_zcl1_write_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int zi_zcl1_parse(const uint8_t *req, uint32_t req_len, zi_zcl1_frame *out) {
  if (!req || !out) return 0;
  if (req_len < 24) return 0;
  if (req[0] != 'Z' || req[1] != 'C' || req[2] != 'L' || req[3] != '1') return 0;
  uint16_t v = zi_zcl1_read_u16(req + 4);
  uint16_t op = zi_zcl1_read_u16(req + 6);
  uint32_t rid = zi_zcl1_read_u32(req + 8);
  uint32_t payload_len = zi_zcl1_read_u32(req + 20);
  if (v != 1) return 0;
  if (24u + payload_len > req_len) return 0;
  out->req = req;
  out->req_len = req_len;
  out->op = op;
  out->rid = rid;
  out->payload = req + 24;
  out->payload_len = payload_len;
  return 1;
}

int zi_zcl1_write_ok(uint8_t *out, uint32_t cap, uint16_t op, uint32_t rid,
                     const uint8_t *payload, uint32_t payload_len) {
  uint32_t frame_len = 24 + payload_len;
  if (!out) return -1;
  if (cap < frame_len) return -1;
  memcpy(out + 0, "ZCL1", 4);
  zi_zcl1_write_u16(out + 4, 1);
  zi_zcl1_write_u16(out + 6, op);
  zi_zcl1_write_u32(out + 8, rid);
  zi_zcl1_write_u32(out + 12, 1); /* status: ok */
  zi_zcl1_write_u32(out + 16, 0);
  zi_zcl1_write_u32(out + 20, payload_len);
  if (payload_len && payload) memcpy(out + 24, payload, payload_len);
  return (int)frame_len;
}

int zi_zcl1_write_error(uint8_t *out, uint32_t cap, uint16_t op, uint32_t rid,
                        const char *trace, const char *msg) {
  if (!out || !trace || !msg) return -1;
  uint32_t tlen = (uint32_t)strlen(trace);
  uint32_t mlen = (uint32_t)strlen(msg);
  uint32_t clen = 0;
  uint32_t payload_len = 4 + 4 + tlen + 4 + mlen + 4 + clen;
  uint32_t frame_len = 24 + payload_len;
  if (cap < frame_len) return -1;
  memcpy(out + 0, "ZCL1", 4);
  zi_zcl1_write_u16(out + 4, 1);
  zi_zcl1_write_u16(out + 6, op);
  zi_zcl1_write_u32(out + 8, rid);
  zi_zcl1_write_u32(out + 12, 0); /* status: error */
  zi_zcl1_write_u32(out + 16, 0);
  zi_zcl1_write_u32(out + 20, payload_len);

  zi_zcl1_write_u32(out + 24, tlen);
  memcpy(out + 28, trace, tlen);
  zi_zcl1_write_u32(out + 28 + tlen, mlen);
  memcpy(out + 32 + tlen, msg, mlen);
  zi_zcl1_write_u32(out + 32 + tlen + mlen, clen);
  return (int)frame_len;
}
