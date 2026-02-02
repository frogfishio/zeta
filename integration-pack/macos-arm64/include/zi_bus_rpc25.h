#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// RPC-over-event/bus v1 helper utilities.
//
// This is an *application protocol* layered over event/bus@v1.
// Spec: src/zingcore/2.5/abi/EVENT_BUS_RPC_V1.md

// Topics (bytes, not NUL-terminated on the wire; helpers use C strings for convenience).
#define ZI_BUS_RPC_V1_TOPIC_REQ "rpc/v1/req"
#define ZI_BUS_RPC_V1_TOPIC_RESP "rpc/v1/resp"

// Message types.
enum {
  ZI_BUS_RPC_V1_CALL = 1,
  ZI_BUS_RPC_V1_OK = 2,
  ZI_BUS_RPC_V1_ERR = 3,

  ZI_BUS_RPC_V1_STREAM_CHUNK = 10,
  ZI_BUS_RPC_V1_STREAM_END = 11,

  ZI_BUS_RPC_V1_CANCEL = 20,
};

// Stream kinds.
enum {
  ZI_BUS_RPC_V1_STREAM_REQ_BODY = 0,
  ZI_BUS_RPC_V1_STREAM_RESP_BODY = 1,
};

// Computes required size for CALL message.
uint32_t zi_bus_rpc_v1_call_size(uint32_t selector_len, uint32_t payload_len);
// Encodes CALL into out (little-endian). Returns bytes written or 0.
uint32_t zi_bus_rpc_v1_write_call(uint8_t *out, uint32_t cap, uint64_t call_id,
                                  const uint8_t *selector, uint32_t selector_len,
                                  const uint8_t *payload, uint32_t payload_len);

uint32_t zi_bus_rpc_v1_ok_size(uint32_t payload_len);
uint32_t zi_bus_rpc_v1_write_ok(uint8_t *out, uint32_t cap, uint64_t call_id,
                                const uint8_t *payload, uint32_t payload_len);

uint32_t zi_bus_rpc_v1_err_size(uint32_t code_len, uint32_t msg_len);
uint32_t zi_bus_rpc_v1_write_err(uint8_t *out, uint32_t cap, uint64_t call_id,
                                 const uint8_t *code, uint32_t code_len,
                                 const uint8_t *msg, uint32_t msg_len);

uint32_t zi_bus_rpc_v1_stream_chunk_size(uint32_t bytes_len);
uint32_t zi_bus_rpc_v1_write_stream_chunk(uint8_t *out, uint32_t cap, uint64_t call_id,
                                          uint32_t stream_kind, uint32_t seq,
                                          const uint8_t *bytes, uint32_t bytes_len);

uint32_t zi_bus_rpc_v1_stream_end_size(void);
uint32_t zi_bus_rpc_v1_write_stream_end(uint8_t *out, uint32_t cap, uint64_t call_id,
                                        uint32_t stream_kind, uint32_t seq);

uint32_t zi_bus_rpc_v1_cancel_size(void);
uint32_t zi_bus_rpc_v1_write_cancel(uint8_t *out, uint32_t cap, uint64_t call_id);

// Parsed view of an RPC message. Pointers refer to the input buffer.
typedef struct zi_bus_rpc_v1_msg {
  uint32_t msg_type;
  uint64_t call_id;

  // CALL
  const uint8_t *selector;
  uint32_t selector_len;
  const uint8_t *payload;
  uint32_t payload_len;

  // ERR
  const uint8_t *code;
  uint32_t code_len;
  const uint8_t *err_msg;
  uint32_t err_msg_len;

  // STREAM_*
  uint32_t stream_kind;
  uint32_t seq;
  const uint8_t *chunk;
  uint32_t chunk_len;
} zi_bus_rpc_v1_msg;

// Parses a single RPC message. Returns 1 on success, 0 on failure.
int zi_bus_rpc_v1_parse(const uint8_t *msg, uint32_t msg_len, zi_bus_rpc_v1_msg *out);

#ifdef __cplusplus
} // extern "C"
#endif
