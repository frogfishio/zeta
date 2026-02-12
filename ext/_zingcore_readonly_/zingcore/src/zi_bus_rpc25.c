#include "zi_bus_rpc25.h"

#include <string.h>

static void write_u32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void write_u64le(uint8_t *p, uint64_t v) {
  write_u32le(p + 0, (uint32_t)(v & 0xFFFFFFFFu));
  write_u32le(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static uint32_t read_u32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64le(const uint8_t *p) {
  uint64_t lo = (uint64_t)read_u32le(p + 0);
  uint64_t hi = (uint64_t)read_u32le(p + 4);
  return lo | (hi << 32);
}

static uint32_t base_size(void) { return 4u + 8u; }

uint32_t zi_bus_rpc_v1_call_size(uint32_t selector_len, uint32_t payload_len) {
  return base_size() + 4u + selector_len + 4u + payload_len;
}

uint32_t zi_bus_rpc_v1_write_call(uint8_t *out, uint32_t cap, uint64_t call_id,
                                  const uint8_t *selector, uint32_t selector_len,
                                  const uint8_t *payload, uint32_t payload_len) {
  uint32_t need = zi_bus_rpc_v1_call_size(selector_len, payload_len);
  if (!out || cap < need) return 0;
  if (call_id == 0) return 0;
  if (selector_len && !selector) return 0;
  if (payload_len && !payload) return 0;

  uint32_t off = 0;
  write_u32le(out + off, (uint32_t)ZI_BUS_RPC_V1_CALL);
  off += 4;
  write_u64le(out + off, call_id);
  off += 8;
  write_u32le(out + off, selector_len);
  off += 4;
  if (selector_len) memcpy(out + off, selector, selector_len);
  off += selector_len;
  write_u32le(out + off, payload_len);
  off += 4;
  if (payload_len) memcpy(out + off, payload, payload_len);
  off += payload_len;
  return off;
}

uint32_t zi_bus_rpc_v1_ok_size(uint32_t payload_len) { return base_size() + 4u + payload_len; }

uint32_t zi_bus_rpc_v1_write_ok(uint8_t *out, uint32_t cap, uint64_t call_id,
                                const uint8_t *payload, uint32_t payload_len) {
  uint32_t need = zi_bus_rpc_v1_ok_size(payload_len);
  if (!out || cap < need) return 0;
  if (call_id == 0) return 0;
  if (payload_len && !payload) return 0;

  uint32_t off = 0;
  write_u32le(out + off, (uint32_t)ZI_BUS_RPC_V1_OK);
  off += 4;
  write_u64le(out + off, call_id);
  off += 8;
  write_u32le(out + off, payload_len);
  off += 4;
  if (payload_len) memcpy(out + off, payload, payload_len);
  off += payload_len;
  return off;
}

uint32_t zi_bus_rpc_v1_err_size(uint32_t code_len, uint32_t msg_len) {
  return base_size() + 4u + code_len + 4u + msg_len;
}

uint32_t zi_bus_rpc_v1_write_err(uint8_t *out, uint32_t cap, uint64_t call_id,
                                 const uint8_t *code, uint32_t code_len,
                                 const uint8_t *msg, uint32_t msg_len) {
  uint32_t need = zi_bus_rpc_v1_err_size(code_len, msg_len);
  if (!out || cap < need) return 0;
  if (call_id == 0) return 0;
  if (code_len && !code) return 0;
  if (msg_len && !msg) return 0;

  uint32_t off = 0;
  write_u32le(out + off, (uint32_t)ZI_BUS_RPC_V1_ERR);
  off += 4;
  write_u64le(out + off, call_id);
  off += 8;
  write_u32le(out + off, code_len);
  off += 4;
  if (code_len) memcpy(out + off, code, code_len);
  off += code_len;
  write_u32le(out + off, msg_len);
  off += 4;
  if (msg_len) memcpy(out + off, msg, msg_len);
  off += msg_len;
  return off;
}

uint32_t zi_bus_rpc_v1_stream_chunk_size(uint32_t bytes_len) {
  return base_size() + 4u + 4u + 4u + bytes_len;
}

uint32_t zi_bus_rpc_v1_write_stream_chunk(uint8_t *out, uint32_t cap, uint64_t call_id,
                                          uint32_t stream_kind, uint32_t seq,
                                          const uint8_t *bytes, uint32_t bytes_len) {
  uint32_t need = zi_bus_rpc_v1_stream_chunk_size(bytes_len);
  if (!out || cap < need) return 0;
  if (call_id == 0) return 0;
  if (bytes_len && !bytes) return 0;

  uint32_t off = 0;
  write_u32le(out + off, (uint32_t)ZI_BUS_RPC_V1_STREAM_CHUNK);
  off += 4;
  write_u64le(out + off, call_id);
  off += 8;
  write_u32le(out + off, stream_kind);
  off += 4;
  write_u32le(out + off, seq);
  off += 4;
  write_u32le(out + off, bytes_len);
  off += 4;
  if (bytes_len) memcpy(out + off, bytes, bytes_len);
  off += bytes_len;
  return off;
}

uint32_t zi_bus_rpc_v1_stream_end_size(void) { return base_size() + 4u + 4u; }

uint32_t zi_bus_rpc_v1_write_stream_end(uint8_t *out, uint32_t cap, uint64_t call_id,
                                        uint32_t stream_kind, uint32_t seq) {
  uint32_t need = zi_bus_rpc_v1_stream_end_size();
  if (!out || cap < need) return 0;
  if (call_id == 0) return 0;

  uint32_t off = 0;
  write_u32le(out + off, (uint32_t)ZI_BUS_RPC_V1_STREAM_END);
  off += 4;
  write_u64le(out + off, call_id);
  off += 8;
  write_u32le(out + off, stream_kind);
  off += 4;
  write_u32le(out + off, seq);
  off += 4;
  return off;
}

uint32_t zi_bus_rpc_v1_cancel_size(void) { return base_size(); }

uint32_t zi_bus_rpc_v1_write_cancel(uint8_t *out, uint32_t cap, uint64_t call_id) {
  uint32_t need = zi_bus_rpc_v1_cancel_size();
  if (!out || cap < need) return 0;
  if (call_id == 0) return 0;

  write_u32le(out + 0, (uint32_t)ZI_BUS_RPC_V1_CANCEL);
  write_u64le(out + 4, call_id);
  return need;
}

int zi_bus_rpc_v1_parse(const uint8_t *msg, uint32_t msg_len, zi_bus_rpc_v1_msg *out) {
  if (!msg || !out) return 0;
  if (msg_len < base_size()) return 0;

  memset(out, 0, sizeof(*out));
  out->msg_type = read_u32le(msg + 0);
  out->call_id = read_u64le(msg + 4);
  if (out->call_id == 0) return 0;

  uint32_t off = base_size();

  switch (out->msg_type) {
    case ZI_BUS_RPC_V1_CALL: {
      if (off + 4 > msg_len) return 0;
      uint32_t selector_len = read_u32le(msg + off);
      off += 4;
      if (off + selector_len + 4 > msg_len) return 0;
      out->selector_len = selector_len;
      out->selector = selector_len ? (msg + off) : NULL;
      off += selector_len;
      uint32_t payload_len = read_u32le(msg + off);
      off += 4;
      if (off + payload_len != msg_len) return 0;
      out->payload_len = payload_len;
      out->payload = payload_len ? (msg + off) : NULL;
      return 1;
    }

    case ZI_BUS_RPC_V1_OK: {
      if (off + 4 > msg_len) return 0;
      uint32_t payload_len = read_u32le(msg + off);
      off += 4;
      if (off + payload_len != msg_len) return 0;
      out->payload_len = payload_len;
      out->payload = payload_len ? (msg + off) : NULL;
      return 1;
    }

    case ZI_BUS_RPC_V1_ERR: {
      if (off + 4 > msg_len) return 0;
      uint32_t code_len = read_u32le(msg + off);
      off += 4;
      if (off + code_len + 4 > msg_len) return 0;
      out->code_len = code_len;
      out->code = code_len ? (msg + off) : NULL;
      off += code_len;
      uint32_t err_len = read_u32le(msg + off);
      off += 4;
      if (off + err_len != msg_len) return 0;
      out->err_msg_len = err_len;
      out->err_msg = err_len ? (msg + off) : NULL;
      return 1;
    }

    case ZI_BUS_RPC_V1_STREAM_CHUNK: {
      if (off + 4 + 4 + 4 > msg_len) return 0;
      out->stream_kind = read_u32le(msg + off);
      off += 4;
      out->seq = read_u32le(msg + off);
      off += 4;
      uint32_t bytes_len = read_u32le(msg + off);
      off += 4;
      if (off + bytes_len != msg_len) return 0;
      out->chunk_len = bytes_len;
      out->chunk = bytes_len ? (msg + off) : NULL;
      return 1;
    }

    case ZI_BUS_RPC_V1_STREAM_END: {
      if (off + 4 + 4 != msg_len) return 0;
      out->stream_kind = read_u32le(msg + off);
      off += 4;
      out->seq = read_u32le(msg + off);
      return 1;
    }

    case ZI_BUS_RPC_V1_CANCEL: {
      if (off != msg_len) return 0;
      return 1;
    }

    default:
      return 0;
  }
}
