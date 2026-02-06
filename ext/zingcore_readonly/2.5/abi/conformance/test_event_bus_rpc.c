#include "zi_bus_rpc25.h"
#include "zi_caps.h"
#include "zi_event_bus25.h"
#include "zi_handles25.h"
#include "zi_runtime25.h"
#include "zi_sysabi25.h"
#include "zi_zcl1.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void write_u64le(uint8_t *p, uint64_t v) {
  write_u32le(p + 0, (uint32_t)(v & 0xFFFFFFFFu));
  write_u32le(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static uint32_t read_u32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t build_fetch_req_v1(uint8_t *out, uint32_t cap, const char *method, const char *url, const char *headers) {
  uint32_t method_len = method ? (uint32_t)strlen(method) : 0;
  uint32_t url_len = url ? (uint32_t)strlen(url) : 0;
  uint32_t headers_len = headers ? (uint32_t)strlen(headers) : 0;
  uint32_t need = 4u + 4u + method_len + 4u + url_len + 4u + headers_len;
  if (!out || cap < need) return 0;
  uint32_t off = 0;
  write_u32le(out + off, 1u);
  off += 4;
  write_u32le(out + off, method_len);
  off += 4;
  if (method_len) memcpy(out + off, method, method_len);
  off += method_len;
  write_u32le(out + off, url_len);
  off += 4;
  if (url_len) memcpy(out + off, url, url_len);
  off += url_len;
  write_u32le(out + off, headers_len);
  off += 4;
  if (headers_len) memcpy(out + off, headers, headers_len);
  off += headers_len;
  return off;
}

static int validate_fetch_req_v1(const uint8_t *p, uint32_t n) {
  if (!p) return 0;
  uint32_t off = 0;
  if (n < 4u) return 0;
  uint32_t version = read_u32le(p + off);
  off += 4;
  if (version != 1u) return 0;

  if (n < off + 4u) return 0;
  uint32_t method_len = read_u32le(p + off);
  off += 4;
  if (n < off + method_len) return 0;
  off += method_len;

  if (n < off + 4u) return 0;
  uint32_t url_len = read_u32le(p + off);
  off += 4;
  if (n < off + url_len) return 0;
  off += url_len;

  if (n < off + 4u) return 0;
  uint32_t headers_len = read_u32le(p + off);
  off += 4;
  if (n < off + headers_len) return 0;
  off += headers_len;

  return off == n;
}

static int parse_fetch_ok_v1(const uint8_t *p, uint32_t n, uint32_t *out_status) {
  if (!p || n < 12u) return 0;
  uint32_t version = read_u32le(p + 0);
  uint32_t status = read_u32le(p + 4);
  uint32_t headers_len = read_u32le(p + 8);
  if (version != 1u) return 0;
  if (12u + headers_len != n) return 0;
  if (out_status) *out_status = status;
  return 1;
}

static int parse_fetch_ok_v1_headers(const uint8_t *p, uint32_t n, uint32_t expected_status, const char *expected_headers) {
  if (!p || n < 12u) return 0;
  if (read_u32le(p + 0) != 1u) return 0;
  if (read_u32le(p + 4) != expected_status) return 0;
  uint32_t headers_len = read_u32le(p + 8);
  if (12u + headers_len != n) return 0;
  uint32_t exp_len = expected_headers ? (uint32_t)strlen(expected_headers) : 0;
  if (headers_len != exp_len) return 0;
  if (headers_len && memcmp(p + 12, expected_headers, headers_len) != 0) return 0;
  return 1;
}

static void build_open_req(uint8_t req[40], const char *kind, const char *name) {
  write_u64le(req + 0, (uint64_t)(uintptr_t)kind);
  write_u32le(req + 8, (uint32_t)strlen(kind));
  write_u64le(req + 12, (uint64_t)(uintptr_t)name);
  write_u32le(req + 20, (uint32_t)strlen(name));
  write_u32le(req + 24, 0);
  write_u64le(req + 28, 0);
  write_u32le(req + 36, 0);
}

static void build_zcl1_req(uint8_t *out, uint16_t op, uint32_t rid, const uint8_t *payload, uint32_t payload_len) {
  memcpy(out + 0, "ZCL1", 4);
  write_u16le(out + 4, 1);
  write_u16le(out + 6, op);
  write_u32le(out + 8, rid);
  write_u32le(out + 12, 0);
  write_u32le(out + 16, 0);
  write_u32le(out + 20, payload_len);
  if (payload_len && payload) memcpy(out + 24, payload, payload_len);
}

static int read_frame_exact(zi_handle_t h, uint8_t *buf, uint32_t cap, zi_zcl1_frame *out) {
  uint32_t have = 0;

  for (uint32_t spins = 0; spins < 100000u && have < 24u; spins++) {
    int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(buf + have), (zi_size32_t)(24u - have));
    if (n == ZI_E_AGAIN) continue;
    if (n <= 0) return 0;
    have += (uint32_t)n;
  }
  if (have < 24u) return 0;
  if (buf[0] != 'Z' || buf[1] != 'C' || buf[2] != 'L' || buf[3] != '1') return 0;

  uint32_t payload_len = read_u32le(buf + 20);
  uint32_t frame_len = 24u + payload_len;
  if (frame_len > cap) return 0;

  for (uint32_t spins = 0; spins < 100000u && have < frame_len; spins++) {
    int32_t n = zi_read(h, (zi_ptr_t)(uintptr_t)(buf + have), (zi_size32_t)(frame_len - have));
    if (n == ZI_E_AGAIN) continue;
    if (n <= 0) return 0;
    have += (uint32_t)n;
  }
  if (have < frame_len) return 0;
  return zi_zcl1_parse(buf, frame_len, out);
}

static int publish(zi_handle_t h, const char *topic, const uint8_t *data, uint32_t data_len, uint32_t rid) {
  uint32_t topic_len = (uint32_t)strlen(topic);
  uint8_t payload[4096];
  if (8u + topic_len + data_len > sizeof(payload)) return 0;

  write_u32le(payload + 0, topic_len);
  memcpy(payload + 4, topic, topic_len);
  write_u32le(payload + 4 + topic_len, data_len);
  if (data_len) memcpy(payload + 8 + topic_len, data, data_len);

  uint8_t fr[24 + sizeof(payload)];
  uint32_t pl_len = 8u + topic_len + data_len;
  build_zcl1_req(fr, (uint16_t)ZI_EVENT_BUS_OP_PUBLISH, rid, payload, pl_len);
  if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)(24u + pl_len)) != (int32_t)(24u + pl_len)) return 0;

  uint8_t resp[256];
  zi_zcl1_frame z;
  if (!read_frame_exact(h, resp, (uint32_t)sizeof(resp), &z)) return 0;
  if (z.op != ZI_EVENT_BUS_OP_PUBLISH || z.rid != rid || z.payload_len != 4) return 0;
  if (read_u32le(resp + 12) != 1) return 0;
  return 1;
}

static int subscribe(zi_handle_t h, const char *topic, uint32_t rid) {
  uint32_t topic_len = (uint32_t)strlen(topic);
  uint8_t payload[256];
  write_u32le(payload + 0, topic_len);
  memcpy(payload + 4, topic, topic_len);
  write_u32le(payload + 4 + topic_len, 0);
  uint32_t pl_len = 8u + topic_len;

  uint8_t fr[24 + sizeof(payload)];
  build_zcl1_req(fr, (uint16_t)ZI_EVENT_BUS_OP_SUBSCRIBE, rid, payload, pl_len);
  if (zi_write(h, (zi_ptr_t)(uintptr_t)fr, (zi_size32_t)(24u + pl_len)) != (int32_t)(24u + pl_len)) return 0;

  uint8_t resp[256];
  zi_zcl1_frame z;
  if (!read_frame_exact(h, resp, (uint32_t)sizeof(resp), &z)) return 0;
  if (z.op != ZI_EVENT_BUS_OP_SUBSCRIBE || z.rid != rid || z.payload_len != 4) return 0;
  if (read_u32le(resp + 12) != 1) return 0;
  if (read_u32le(z.payload) == 0) return 0;
  return 1;
}

static int read_event_data(zi_handle_t h, uint32_t expected_rid, uint8_t *out_data, uint32_t out_cap, uint32_t *out_len) {
  uint8_t buf[8192];
  zi_zcl1_frame z;
  if (!read_frame_exact(h, buf, (uint32_t)sizeof(buf), &z)) return 0;
  if (z.op != ZI_EVENT_BUS_EV_EVENT || z.rid != expected_rid) return 0;

  if (z.payload_len < 4u + 4u + 4u) return 0;
  const uint8_t *pl = z.payload;
  uint32_t topic_len = read_u32le(pl + 4);
  if (topic_len == 0 || 8u + topic_len + 4u > z.payload_len) return 0;
  uint32_t off = 8u + topic_len;
  uint32_t data_len = read_u32le(pl + off);
  off += 4;
  if (off + data_len != z.payload_len) return 0;
  if (data_len > out_cap) return 0;
  if (data_len) memcpy(out_data, pl + off, data_len);
  if (out_len) *out_len = data_len;
  return 1;
}

int main(void) {
  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);

  zi_caps_reset_for_test();
  zi_handles25_reset_for_test();

  if (!zi_caps_init()) {
    fprintf(stderr, "FAIL: init failed\n");
    return 1;
  }
  if (!zi_event_bus25_register()) {
    fprintf(stderr, "FAIL: register event/bus failed\n");
    return 1;
  }

  uint8_t open_req[40];
  build_open_req(open_req, ZI_CAP_KIND_EVENT, ZI_CAP_NAME_BUS);
  zi_handle_t h_host = zi_cap_open((zi_ptr_t)(uintptr_t)open_req);
  zi_handle_t h_guest = zi_cap_open((zi_ptr_t)(uintptr_t)open_req);
  if (h_host < 3 || h_guest < 3) {
    fprintf(stderr, "FAIL: open handles\n");
    return 1;
  }

  if (!subscribe(h_host, ZI_BUS_RPC_V1_TOPIC_REQ, 1)) {
    fprintf(stderr, "FAIL: host subscribe\n");
    return 1;
  }
  if (!subscribe(h_guest, ZI_BUS_RPC_V1_TOPIC_RESP, 2)) {
    fprintf(stderr, "FAIL: guest subscribe\n");
    return 1;
  }

  const uint64_t call_id = 7;
  const char *selector = "fetch.v1";
  uint8_t fetch_pl[256];
  uint32_t fetch_pl_len = build_fetch_req_v1(fetch_pl, (uint32_t)sizeof(fetch_pl), "GET", "https://example.invalid/", "");
  if (fetch_pl_len == 0) {
    fprintf(stderr, "FAIL: encode fetch payload\n");
    return 1;
  }

  uint8_t call_msg[256];
  uint32_t call_len = zi_bus_rpc_v1_write_call(call_msg, (uint32_t)sizeof(call_msg), call_id,
                                               (const uint8_t *)selector, (uint32_t)strlen(selector),
                                               fetch_pl, fetch_pl_len);
  if (call_len == 0) {
    fprintf(stderr, "FAIL: encode CALL\n");
    return 1;
  }

  if (!publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, call_msg, call_len, 10)) {
    fprintf(stderr, "FAIL: publish CALL\n");
    return 1;
  }

  // Host reads CALL(fetch.v1) and replies OK(fetch headers), then streams 2 chunks and ends.
  {
    uint8_t data[1024];
    uint32_t data_len = 0;
    if (!read_event_data(h_host, 10, data, (uint32_t)sizeof(data), &data_len)) {
      fprintf(stderr, "FAIL: host read\n");
      return 1;
    }
    zi_bus_rpc_v1_msg m;
    if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_CALL || m.call_id != call_id) {
      fprintf(stderr, "FAIL: host parse\n");
      return 1;
    }

    if (m.selector_len != strlen(selector) || memcmp(m.selector, selector, m.selector_len) != 0) {
      fprintf(stderr, "FAIL: host selector mismatch\n");
      return 1;
    }
    if (m.payload_len != fetch_pl_len || memcmp(m.payload, fetch_pl, fetch_pl_len) != 0) {
      fprintf(stderr, "FAIL: host fetch payload mismatch\n");
      return 1;
    }

    uint8_t ok_pl[64];
    write_u32le(ok_pl + 0, 1u);
    write_u32le(ok_pl + 4, 200u);
    write_u32le(ok_pl + 8, 0u);

    uint8_t ok_msg[128];
    uint32_t ok_len = zi_bus_rpc_v1_write_ok(ok_msg, (uint32_t)sizeof(ok_msg), call_id, ok_pl, 12u);
    if (ok_len == 0) {
      fprintf(stderr, "FAIL: encode OK\n");
      return 1;
    }
    if (!publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, ok_msg, ok_len, 11)) {
      fprintf(stderr, "FAIL: publish OK\n");
      return 1;
    }

    const char *c0 = "ab";
    const char *c1 = "cd";
    uint8_t msg[128];
    uint32_t n = 0;

    n = zi_bus_rpc_v1_write_stream_chunk(msg, (uint32_t)sizeof(msg), call_id,
                                         (uint32_t)ZI_BUS_RPC_V1_STREAM_RESP_BODY, 0,
                                         (const uint8_t *)c0, 2);
    if (n == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, msg, n, 12)) {
      fprintf(stderr, "FAIL: publish CHUNK0\n");
      return 1;
    }

    n = zi_bus_rpc_v1_write_stream_chunk(msg, (uint32_t)sizeof(msg), call_id,
                                         (uint32_t)ZI_BUS_RPC_V1_STREAM_RESP_BODY, 1,
                                         (const uint8_t *)c1, 2);
    if (n == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, msg, n, 13)) {
      fprintf(stderr, "FAIL: publish CHUNK1\n");
      return 1;
    }

    n = zi_bus_rpc_v1_write_stream_end(msg, (uint32_t)sizeof(msg), call_id,
                                       (uint32_t)ZI_BUS_RPC_V1_STREAM_RESP_BODY, 2);
    if (n == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, msg, n, 14)) {
      fprintf(stderr, "FAIL: publish END\n");
      return 1;
    }
  }

  // Guest reads OK and validates fetch header payload.
  {
    uint8_t data[1024];
    uint32_t data_len = 0;
    if (!read_event_data(h_guest, 11, data, (uint32_t)sizeof(data), &data_len)) {
      fprintf(stderr, "FAIL: guest read\n");
      return 1;
    }
    zi_bus_rpc_v1_msg m;
    if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_OK || m.call_id != call_id) {
      fprintf(stderr, "FAIL: guest parse\n");
      return 1;
    }

    uint32_t status = 0;
    if (!parse_fetch_ok_v1(m.payload, m.payload_len, &status) || status != 200u) {
      fprintf(stderr, "FAIL: fetch ok payload mismatch\n");
      return 1;
    }
  }

  // Guest reads streamed chunks and end.
  {
    uint8_t data[1024];
    uint32_t data_len = 0;
    zi_bus_rpc_v1_msg m;

    if (!read_event_data(h_guest, 12, data, (uint32_t)sizeof(data), &data_len)) {
      fprintf(stderr, "FAIL: guest read chunk0\n");
      return 1;
    }
    if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_STREAM_CHUNK || m.call_id != call_id) {
      fprintf(stderr, "FAIL: guest parse chunk0\n");
      return 1;
    }
    if (m.stream_kind != ZI_BUS_RPC_V1_STREAM_RESP_BODY || m.seq != 0 || m.chunk_len != 2 || memcmp(m.chunk, "ab", 2) != 0) {
      fprintf(stderr, "FAIL: chunk0 mismatch\n");
      return 1;
    }

    if (!read_event_data(h_guest, 13, data, (uint32_t)sizeof(data), &data_len)) {
      fprintf(stderr, "FAIL: guest read chunk1\n");
      return 1;
    }
    if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_STREAM_CHUNK || m.call_id != call_id) {
      fprintf(stderr, "FAIL: guest parse chunk1\n");
      return 1;
    }
    if (m.stream_kind != ZI_BUS_RPC_V1_STREAM_RESP_BODY || m.seq != 1 || m.chunk_len != 2 || memcmp(m.chunk, "cd", 2) != 0) {
      fprintf(stderr, "FAIL: chunk1 mismatch\n");
      return 1;
    }

    if (!read_event_data(h_guest, 14, data, (uint32_t)sizeof(data), &data_len)) {
      fprintf(stderr, "FAIL: guest read end\n");
      return 1;
    }
    if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_STREAM_END || m.call_id != call_id) {
      fprintf(stderr, "FAIL: guest parse end\n");
      return 1;
    }
    if (m.stream_kind != ZI_BUS_RPC_V1_STREAM_RESP_BODY || m.seq != 2) {
      fprintf(stderr, "FAIL: end mismatch\n");
      return 1;
    }
  }

  // CANCEL path: guest issues CALL2, host emits one chunk, guest cancels, host replies ERR.
  {
    const uint64_t call2 = 8;
    const char *sel = "stream.v1";

    uint8_t call_msg[128];
    uint32_t call_len = zi_bus_rpc_v1_write_call(call_msg, (uint32_t)sizeof(call_msg), call2,
                                                 (const uint8_t *)sel, (uint32_t)strlen(sel), NULL, 0);
    if (call_len == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, call_msg, call_len, 20)) {
      fprintf(stderr, "FAIL: publish CALL2\n");
      return 1;
    }

    // Host sees CALL2.
    {
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event_data(h_host, 20, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: host read CALL2\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_CALL || m.call_id != call2) {
        fprintf(stderr, "FAIL: host parse CALL2\n");
        return 1;
      }
    }

    // Host sends one chunk.
    {
      uint8_t msg[64];
      uint32_t n = zi_bus_rpc_v1_write_stream_chunk(msg, (uint32_t)sizeof(msg), call2,
                                                    (uint32_t)ZI_BUS_RPC_V1_STREAM_RESP_BODY, 0,
                                                    (const uint8_t *)"x", 1);
      if (n == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, msg, n, 21)) {
        fprintf(stderr, "FAIL: publish CALL2 chunk\n");
        return 1;
      }
    }

    // Guest reads chunk.
    {
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event_data(h_guest, 21, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: guest read CALL2 chunk\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_STREAM_CHUNK || m.call_id != call2) {
        fprintf(stderr, "FAIL: guest parse CALL2 chunk\n");
        return 1;
      }
    }

    // Guest cancels (to host on req topic).
    {
      uint8_t msg[32];
      uint32_t n = zi_bus_rpc_v1_write_cancel(msg, (uint32_t)sizeof(msg), call2);
      if (n == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, msg, n, 22)) {
        fprintf(stderr, "FAIL: publish CANCEL\n");
        return 1;
      }
    }

    // Host reads cancel and replies ERR.
    {
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event_data(h_host, 22, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: host read CANCEL\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_CANCEL || m.call_id != call2) {
        fprintf(stderr, "FAIL: host parse CANCEL\n");
        return 1;
      }

      uint8_t msg[128];
      uint32_t n = zi_bus_rpc_v1_write_err(msg, (uint32_t)sizeof(msg), call2,
                                           (const uint8_t *)"cancelled", 9,
                                           (const uint8_t *)"cancel", 6);
      if (n == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, msg, n, 23)) {
        fprintf(stderr, "FAIL: publish ERR\n");
        return 1;
      }
    }

    // Guest reads ERR.
    {
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event_data(h_guest, 23, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: guest read ERR\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_ERR || m.call_id != call2) {
        fprintf(stderr, "FAIL: guest parse ERR\n");
        return 1;
      }
      if (m.code_len != 9 || memcmp(m.code, "cancelled", 9) != 0) {
        fprintf(stderr, "FAIL: err code mismatch\n");
        return 1;
      }
    }
  }

  // Non-empty fetch headers round-trip.
  {
    const uint64_t call3 = 9;
    const char *selector3 = "fetch.v1";
    const char *req_headers = "Accept: text/plain\r\nX-Test: 1\r\n";
    const char *resp_headers = "Content-Type: text/plain\r\n";

    uint8_t fetch_pl[256];
    uint32_t fetch_len = build_fetch_req_v1(fetch_pl, (uint32_t)sizeof(fetch_pl), "GET", "https://example.invalid/hdr", req_headers);
    if (fetch_len == 0) {
      fprintf(stderr, "FAIL: encode fetch headers payload\n");
      return 1;
    }

    uint8_t call_msg[512];
    uint32_t call_len = zi_bus_rpc_v1_write_call(call_msg, (uint32_t)sizeof(call_msg), call3,
                                                 (const uint8_t *)selector3, (uint32_t)strlen(selector3),
                                                 fetch_pl, fetch_len);
    if (call_len == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, call_msg, call_len, 60)) {
      fprintf(stderr, "FAIL: publish CALL3\n");
      return 1;
    }

    // Host reads CALL3 and replies OK with response headers.
    {
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event_data(h_host, 60, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: host read CALL3\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_CALL || m.call_id != call3) {
        fprintf(stderr, "FAIL: host parse CALL3\n");
        return 1;
      }
      if (m.payload_len != fetch_len || memcmp(m.payload, fetch_pl, fetch_len) != 0) {
        fprintf(stderr, "FAIL: host CALL3 payload mismatch\n");
        return 1;
      }

      uint32_t resp_headers_len = (uint32_t)strlen(resp_headers);
      uint8_t ok_pl[128];
      write_u32le(ok_pl + 0, 1u);
      write_u32le(ok_pl + 4, 204u);
      write_u32le(ok_pl + 8, resp_headers_len);
      memcpy(ok_pl + 12, resp_headers, resp_headers_len);

      uint8_t ok_msg[256];
      uint32_t ok_len = zi_bus_rpc_v1_write_ok(ok_msg, (uint32_t)sizeof(ok_msg), call3, ok_pl, 12u + resp_headers_len);
      if (ok_len == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, ok_msg, ok_len, 61)) {
        fprintf(stderr, "FAIL: publish OK3\n");
        return 1;
      }
    }

    // Guest reads OK3 and validates headers bytes.
    {
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event_data(h_guest, 61, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: guest read OK3\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_OK || m.call_id != call3) {
        fprintf(stderr, "FAIL: guest parse OK3\n");
        return 1;
      }
      if (!parse_fetch_ok_v1_headers(m.payload, m.payload_len, 204u, resp_headers)) {
        fprintf(stderr, "FAIL: OK3 headers mismatch\n");
        return 1;
      }
    }
  }

  // Single-call: non-empty request headers + non-empty response headers + streamed response body.
  {
    const uint64_t call6 = 12;
    const char *selector6 = "fetch.v1";
    const char *req_headers = "Accept: text/plain\r\nX-Req: 1\r\n";
    const char *resp_headers = "Content-Type: text/plain\r\nX-Resp: 1\r\n";

    uint8_t fetch_pl[256];
    uint32_t fetch_len = build_fetch_req_v1(fetch_pl, (uint32_t)sizeof(fetch_pl), "GET", "https://example.invalid/stream", req_headers);
    if (fetch_len == 0) {
      fprintf(stderr, "FAIL: encode fetch stream payload\n");
      return 1;
    }

    uint8_t call_msg[512];
    uint32_t call_len = zi_bus_rpc_v1_write_call(call_msg, (uint32_t)sizeof(call_msg), call6,
                                                 (const uint8_t *)selector6, (uint32_t)strlen(selector6),
                                                 fetch_pl, fetch_len);
    if (call_len == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, call_msg, call_len, 100)) {
      fprintf(stderr, "FAIL: publish CALL6\n");
      return 1;
    }

    // Host reads CALL6, replies OK6 with headers, then streams body.
    {
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event_data(h_host, 100, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: host read CALL6\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_CALL || m.call_id != call6) {
        fprintf(stderr, "FAIL: host parse CALL6\n");
        return 1;
      }
      if (m.payload_len != fetch_len || memcmp(m.payload, fetch_pl, fetch_len) != 0) {
        fprintf(stderr, "FAIL: host CALL6 payload mismatch\n");
        return 1;
      }

      uint32_t resp_headers_len = (uint32_t)strlen(resp_headers);
      uint8_t ok_pl[256];
      write_u32le(ok_pl + 0, 1u);
      write_u32le(ok_pl + 4, 200u);
      write_u32le(ok_pl + 8, resp_headers_len);
      memcpy(ok_pl + 12, resp_headers, resp_headers_len);

      uint8_t ok_msg[512];
      uint32_t ok_len = zi_bus_rpc_v1_write_ok(ok_msg, (uint32_t)sizeof(ok_msg), call6, ok_pl, 12u + resp_headers_len);
      if (ok_len == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, ok_msg, ok_len, 101)) {
        fprintf(stderr, "FAIL: publish OK6\n");
        return 1;
      }

      uint8_t msg[128];
      uint32_t n = 0;
      n = zi_bus_rpc_v1_write_stream_chunk(msg, (uint32_t)sizeof(msg), call6,
                                           (uint32_t)ZI_BUS_RPC_V1_STREAM_RESP_BODY, 0,
                                           (const uint8_t *)"he", 2);
      if (n == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, msg, n, 102)) {
        fprintf(stderr, "FAIL: publish CALL6 chunk0\n");
        return 1;
      }
      n = zi_bus_rpc_v1_write_stream_chunk(msg, (uint32_t)sizeof(msg), call6,
                                           (uint32_t)ZI_BUS_RPC_V1_STREAM_RESP_BODY, 1,
                                           (const uint8_t *)"llo", 3);
      if (n == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, msg, n, 103)) {
        fprintf(stderr, "FAIL: publish CALL6 chunk1\n");
        return 1;
      }
      n = zi_bus_rpc_v1_write_stream_end(msg, (uint32_t)sizeof(msg), call6,
                                         (uint32_t)ZI_BUS_RPC_V1_STREAM_RESP_BODY, 2);
      if (n == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, msg, n, 104)) {
        fprintf(stderr, "FAIL: publish CALL6 end\n");
        return 1;
      }
    }

    // Guest reads OK6 + streamed body.
    {
      uint8_t data[1024];
      uint32_t data_len = 0;
      zi_bus_rpc_v1_msg m;

      if (!read_event_data(h_guest, 101, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: guest read OK6\n");
        return 1;
      }
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_OK || m.call_id != call6) {
        fprintf(stderr, "FAIL: guest parse OK6\n");
        return 1;
      }
      if (!parse_fetch_ok_v1_headers(m.payload, m.payload_len, 200u, resp_headers)) {
        fprintf(stderr, "FAIL: OK6 headers mismatch\n");
        return 1;
      }

      if (!read_event_data(h_guest, 102, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: guest read CALL6 chunk0\n");
        return 1;
      }
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_STREAM_CHUNK || m.call_id != call6) {
        fprintf(stderr, "FAIL: guest parse CALL6 chunk0\n");
        return 1;
      }
      if (m.stream_kind != ZI_BUS_RPC_V1_STREAM_RESP_BODY || m.seq != 0 || m.chunk_len != 2 || memcmp(m.chunk, "he", 2) != 0) {
        fprintf(stderr, "FAIL: CALL6 chunk0 mismatch\n");
        return 1;
      }

      if (!read_event_data(h_guest, 103, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: guest read CALL6 chunk1\n");
        return 1;
      }
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_STREAM_CHUNK || m.call_id != call6) {
        fprintf(stderr, "FAIL: guest parse CALL6 chunk1\n");
        return 1;
      }
      if (m.stream_kind != ZI_BUS_RPC_V1_STREAM_RESP_BODY || m.seq != 1 || m.chunk_len != 3 || memcmp(m.chunk, "llo", 3) != 0) {
        fprintf(stderr, "FAIL: CALL6 chunk1 mismatch\n");
        return 1;
      }

      if (!read_event_data(h_guest, 104, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: guest read CALL6 end\n");
        return 1;
      }
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_STREAM_END || m.call_id != call6) {
        fprintf(stderr, "FAIL: guest parse CALL6 end\n");
        return 1;
      }
      if (m.stream_kind != ZI_BUS_RPC_V1_STREAM_RESP_BODY || m.seq != 2) {
        fprintf(stderr, "FAIL: CALL6 end mismatch\n");
        return 1;
      }
    }
  }

  // POST with request-body streaming (REQ_BODY).
  {
    const uint64_t call4 = 10;
    const char *selector4 = "fetch.v1";
    const char *req_headers = "Content-Length: 3\r\n";

    uint8_t fetch_pl[256];
    uint32_t fetch_len = build_fetch_req_v1(fetch_pl, (uint32_t)sizeof(fetch_pl), "POST", "https://example.invalid/post", req_headers);
    if (fetch_len == 0) {
      fprintf(stderr, "FAIL: encode fetch post payload\n");
      return 1;
    }

    uint8_t call_msg[512];
    uint32_t call_len = zi_bus_rpc_v1_write_call(call_msg, (uint32_t)sizeof(call_msg), call4,
                                                 (const uint8_t *)selector4, (uint32_t)strlen(selector4),
                                                 fetch_pl, fetch_len);
    if (call_len == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, call_msg, call_len, 70)) {
      fprintf(stderr, "FAIL: publish CALL4\n");
      return 1;
    }

    // Guest streams request body: "xy" then "z" then END.
    {
      uint8_t msg[128];
      uint32_t n = 0;
      n = zi_bus_rpc_v1_write_stream_chunk(msg, (uint32_t)sizeof(msg), call4,
                                           (uint32_t)ZI_BUS_RPC_V1_STREAM_REQ_BODY, 0,
                                           (const uint8_t *)"xy", 2);
      if (n == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, msg, n, 71)) {
        fprintf(stderr, "FAIL: publish CALL4 chunk0\n");
        return 1;
      }
      n = zi_bus_rpc_v1_write_stream_chunk(msg, (uint32_t)sizeof(msg), call4,
                                           (uint32_t)ZI_BUS_RPC_V1_STREAM_REQ_BODY, 1,
                                           (const uint8_t *)"z", 1);
      if (n == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, msg, n, 72)) {
        fprintf(stderr, "FAIL: publish CALL4 chunk1\n");
        return 1;
      }
      n = zi_bus_rpc_v1_write_stream_end(msg, (uint32_t)sizeof(msg), call4,
                                         (uint32_t)ZI_BUS_RPC_V1_STREAM_REQ_BODY, 2);
      if (n == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, msg, n, 73)) {
        fprintf(stderr, "FAIL: publish CALL4 end\n");
        return 1;
      }
    }

    // Host reads CALL4 and streamed request body.
    {
      uint8_t data[1024];
      uint32_t data_len = 0;
      zi_bus_rpc_v1_msg m;

      if (!read_event_data(h_host, 70, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: host read CALL4\n");
        return 1;
      }
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_CALL || m.call_id != call4) {
        fprintf(stderr, "FAIL: host parse CALL4\n");
        return 1;
      }

      if (!read_event_data(h_host, 71, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: host read CALL4 chunk0\n");
        return 1;
      }
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_STREAM_CHUNK || m.call_id != call4) {
        fprintf(stderr, "FAIL: host parse CALL4 chunk0\n");
        return 1;
      }
      if (m.stream_kind != ZI_BUS_RPC_V1_STREAM_REQ_BODY || m.seq != 0 || m.chunk_len != 2 || memcmp(m.chunk, "xy", 2) != 0) {
        fprintf(stderr, "FAIL: CALL4 chunk0 mismatch\n");
        return 1;
      }

      if (!read_event_data(h_host, 72, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: host read CALL4 chunk1\n");
        return 1;
      }
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_STREAM_CHUNK || m.call_id != call4) {
        fprintf(stderr, "FAIL: host parse CALL4 chunk1\n");
        return 1;
      }
      if (m.stream_kind != ZI_BUS_RPC_V1_STREAM_REQ_BODY || m.seq != 1 || m.chunk_len != 1 || memcmp(m.chunk, "z", 1) != 0) {
        fprintf(stderr, "FAIL: CALL4 chunk1 mismatch\n");
        return 1;
      }

      if (!read_event_data(h_host, 73, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: host read CALL4 end\n");
        return 1;
      }
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_STREAM_END || m.call_id != call4) {
        fprintf(stderr, "FAIL: host parse CALL4 end\n");
        return 1;
      }
      if (m.stream_kind != ZI_BUS_RPC_V1_STREAM_REQ_BODY || m.seq != 2) {
        fprintf(stderr, "FAIL: CALL4 end mismatch\n");
        return 1;
      }

      // Host replies OK(status=201, no headers).
      uint8_t ok_pl[12];
      write_u32le(ok_pl + 0, 1u);
      write_u32le(ok_pl + 4, 201u);
      write_u32le(ok_pl + 8, 0u);
      uint8_t ok_msg[128];
      uint32_t ok_len = zi_bus_rpc_v1_write_ok(ok_msg, (uint32_t)sizeof(ok_msg), call4, ok_pl, 12u);
      if (ok_len == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, ok_msg, ok_len, 74)) {
        fprintf(stderr, "FAIL: publish OK4\n");
        return 1;
      }
    }

    // Guest reads OK4 and validates status.
    {
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event_data(h_guest, 74, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: guest read OK4\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_OK || m.call_id != call4) {
        fprintf(stderr, "FAIL: guest parse OK4\n");
        return 1;
      }
      if (!parse_fetch_ok_v1_headers(m.payload, m.payload_len, 201u, "")) {
        fprintf(stderr, "FAIL: OK4 mismatch\n");
        return 1;
      }
    }
  }

  // Malformed fetch.v1 payload => ERR(fetch.invalid).
  {
    const uint64_t call5 = 11;
    const char *selector5 = "fetch.v1";
    uint8_t bad_pl[4];
    write_u32le(bad_pl + 0, 2u);

    uint8_t call_msg[128];
    uint32_t call_len = zi_bus_rpc_v1_write_call(call_msg, (uint32_t)sizeof(call_msg), call5,
                                                 (const uint8_t *)selector5, (uint32_t)strlen(selector5),
                                                 bad_pl, 4u);
    if (call_len == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, call_msg, call_len, 80)) {
      fprintf(stderr, "FAIL: publish CALL5\n");
      return 1;
    }

    // Host reads CALL5 and replies ERR.
    {
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event_data(h_host, 80, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: host read CALL5\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_CALL || m.call_id != call5) {
        fprintf(stderr, "FAIL: host parse CALL5\n");
        return 1;
      }
      int invalid = 0;
      if (m.selector_len != strlen(selector5) || memcmp(m.selector, selector5, m.selector_len) != 0) invalid = 1;
      if (!invalid && !validate_fetch_req_v1(m.payload, m.payload_len)) invalid = 1;

      uint8_t err_msg[128];
      const char *code = invalid ? "fetch.invalid" : "fetch.io";
      uint32_t err_len = zi_bus_rpc_v1_write_err(err_msg, (uint32_t)sizeof(err_msg), call5,
                                                 (const uint8_t *)code, (uint32_t)strlen(code),
                                                 (const uint8_t *)"bad fetch payload", 17);
      if (err_len == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, err_msg, err_len, 81)) {
        fprintf(stderr, "FAIL: publish ERR5\n");
        return 1;
      }
    }

    // Guest reads ERR5.
    {
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event_data(h_guest, 81, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: guest read ERR5\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_ERR || m.call_id != call5) {
        fprintf(stderr, "FAIL: guest parse ERR5\n");
        return 1;
      }
      if (m.code_len != 13 || memcmp(m.code, "fetch.invalid", 13) != 0) {
        fprintf(stderr, "FAIL: ERR5 code mismatch\n");
        return 1;
      }
    }
  }

  // Malformed fetch.v1 payload: length claims more bytes than present => ERR(fetch.invalid).
  {
    const uint64_t call7 = 13;
    const char *selector7 = "fetch.v1";

    uint8_t bad_pl[8];
    write_u32le(bad_pl + 0, 1u);
    write_u32le(bad_pl + 4, 10u);

    uint8_t call_msg[256];
    uint32_t call_len = zi_bus_rpc_v1_write_call(call_msg, (uint32_t)sizeof(call_msg), call7,
                                                 (const uint8_t *)selector7, (uint32_t)strlen(selector7),
                                                 bad_pl, (uint32_t)sizeof(bad_pl));
    if (call_len == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, call_msg, call_len, 120)) {
      fprintf(stderr, "FAIL: publish CALL7\n");
      return 1;
    }

    // Host reads CALL7 and replies ERR.
    {
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event_data(h_host, 120, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: host read CALL7\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_CALL || m.call_id != call7) {
        fprintf(stderr, "FAIL: host parse CALL7\n");
        return 1;
      }
      int invalid = 0;
      if (m.selector_len != strlen(selector7) || memcmp(m.selector, selector7, m.selector_len) != 0) invalid = 1;
      if (!invalid && !validate_fetch_req_v1(m.payload, m.payload_len)) invalid = 1;

      uint8_t err_msg[128];
      const char *code = invalid ? "fetch.invalid" : "fetch.io";
      uint32_t err_len = zi_bus_rpc_v1_write_err(err_msg, (uint32_t)sizeof(err_msg), call7,
                                                 (const uint8_t *)code, (uint32_t)strlen(code),
                                                 (const uint8_t *)"bad fetch payload", 17);
      if (err_len == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, err_msg, err_len, 121)) {
        fprintf(stderr, "FAIL: publish ERR7\n");
        return 1;
      }
    }

    // Guest reads ERR7.
    {
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event_data(h_guest, 121, data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "FAIL: guest read ERR7\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_ERR || m.call_id != call7) {
        fprintf(stderr, "FAIL: guest parse ERR7\n");
        return 1;
      }
      if (m.code_len != 13 || memcmp(m.code, "fetch.invalid", 13) != 0) {
        fprintf(stderr, "FAIL: ERR7 code mismatch\n");
        return 1;
      }
    }
  }

  (void)zi_end(h_host);
  (void)zi_end(h_guest);

  printf("PASS: event/bus rpc v1 fetch.v1 headers + req-body streaming + invalid + STREAM + CANCEL\n");
  return 0;
}
