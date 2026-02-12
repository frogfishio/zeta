#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zi_zcl1_frame {
  const uint8_t *req;
  uint32_t req_len;
  uint16_t op;
  uint32_t rid;
  const uint8_t *payload;
  uint32_t payload_len;
} zi_zcl1_frame;

int zi_zcl1_parse(const uint8_t *req, uint32_t req_len, zi_zcl1_frame *out);

// Writes a ZCL1 frame with a payload (ok response). Returns bytes written or -1.
int zi_zcl1_write_ok(uint8_t *out, uint32_t cap, uint16_t op, uint32_t rid,
                     const uint8_t *payload, uint32_t payload_len);

// Writes a ZCL1 error frame. Returns bytes written or -1.
int zi_zcl1_write_error(uint8_t *out, uint32_t cap, uint16_t op, uint32_t rid,
                        const char *trace, const char *msg);

// Little-endian helpers.
uint16_t zi_zcl1_read_u16(const uint8_t *p);
uint32_t zi_zcl1_read_u32(const uint8_t *p);
void zi_zcl1_write_u16(uint8_t *p, uint16_t v);
void zi_zcl1_write_u32(uint8_t *p, uint32_t v);

#ifdef __cplusplus
} // extern "C"
#endif
