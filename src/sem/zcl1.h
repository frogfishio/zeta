#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  ZCL1_HDR_SIZE = 24,
  ZCL1_VERSION = 1,
};

typedef struct zcl1_hdr {
  uint16_t version;
  uint16_t op;
  uint32_t rid;
  uint32_t status;
  uint32_t reserved;
  uint32_t payload_len;
} zcl1_hdr_t;

bool zcl1_parse(const uint8_t* buf, uint32_t len, zcl1_hdr_t* out_hdr, const uint8_t** out_payload);
bool zcl1_write(uint8_t* buf, uint32_t cap, uint16_t op, uint32_t rid, uint32_t status, const uint8_t* payload,
                uint32_t payload_len, uint32_t* out_len);

bool zcl1_write_error_payload(uint8_t* buf, uint32_t cap, const char* trace, const char* msg, const char* detail,
                              uint32_t* out_len);

// Little-endian helpers (stable across host endianness)
uint16_t zcl1_read_u16le(const uint8_t* p);
uint32_t zcl1_read_u32le(const uint8_t* p);
void zcl1_write_u16le(uint8_t* p, uint16_t v);
void zcl1_write_u32le(uint8_t* p, uint32_t v);
