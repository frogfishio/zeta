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

  uint8_t resp_buf[256];
  zi_zcl1_frame z;
  if (!read_frame_exact(h, resp_buf, (uint32_t)sizeof(resp_buf), &z)) return 0;
  if (z.op != ZI_EVENT_BUS_OP_PUBLISH || z.rid != rid || z.payload_len != 4) return 0;
  if (read_u32le(resp_buf + 12) != 1) return 0;
  return 1;
}

static int subscribe(zi_handle_t h, const char *topic, uint32_t rid, uint32_t *out_sub_id) {
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
  uint32_t sub_id = read_u32le(z.payload);
  if (sub_id == 0) return 0;
  if (out_sub_id) *out_sub_id = sub_id;
  return 1;
}

static int read_event(zi_handle_t h, uint32_t expected_rid, char *topic_buf, uint32_t topic_cap,
                      uint8_t *data_buf, uint32_t data_cap, uint32_t *out_data_len) {
  uint8_t buf[8192];
  zi_zcl1_frame z;
  if (!read_frame_exact(h, buf, (uint32_t)sizeof(buf), &z)) return 0;
  if (z.op != ZI_EVENT_BUS_EV_EVENT || z.rid != expected_rid) return 0;

  // event payload: sub_id, topic_len, topic, data_len, data
  if (z.payload_len < 4u + 4u + 4u) return 0;
  const uint8_t *pl = z.payload;
  uint32_t topic_len = read_u32le(pl + 4);
  if (topic_len == 0 || 8u + topic_len + 4u > z.payload_len) return 0;
  const uint8_t *topic = pl + 8;
  uint32_t off = 8u + topic_len;
  uint32_t data_len = read_u32le(pl + off);
  off += 4;
  if (off + data_len != z.payload_len) return 0;

  if (topic_buf && topic_cap) {
    uint32_t n = (topic_len < (topic_cap - 1)) ? topic_len : (topic_cap - 1);
    memcpy(topic_buf, topic, n);
    topic_buf[n] = '\0';
  }
  if (data_len > data_cap) return 0;
  if (data_len) memcpy(data_buf, pl + off, data_len);
  if (out_data_len) *out_data_len = data_len;
  return 1;
}

int main(void) {
  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);

  zi_caps_reset_for_test();
  zi_handles25_reset_for_test();

  if (!zi_caps_init()) {
    fprintf(stderr, "zi_caps_init failed\n");
    return 1;
  }
  if (!zi_event_bus25_register()) {
    fprintf(stderr, "zi_event_bus25_register failed\n");
    return 1;
  }

  // Open two bus handles: host(server) + guest(client).
  uint8_t open_req[40];
  build_open_req(open_req, ZI_CAP_KIND_EVENT, ZI_CAP_NAME_BUS);
  zi_handle_t h_host = zi_cap_open((zi_ptr_t)(uintptr_t)open_req);
  zi_handle_t h_guest = zi_cap_open((zi_ptr_t)(uintptr_t)open_req);
  if (h_host < 3 || h_guest < 3) {
    fprintf(stderr, "open bus handles failed\n");
    return 1;
  }

  uint32_t sub_host = 0;
  uint32_t sub_guest = 0;

  if (!subscribe(h_host, ZI_BUS_RPC_V1_TOPIC_REQ, 1, &sub_host)) {
    fprintf(stderr, "host subscribe failed\n");
    return 1;
  }
  if (!subscribe(h_guest, ZI_BUS_RPC_V1_TOPIC_RESP, 2, &sub_guest)) {
    fprintf(stderr, "guest subscribe failed\n");
    return 1;
  }

  // Guest publishes CALL.
  const uint64_t call_id = 123;
  const char *selector = "fetch.v1";
  uint8_t fetch_pl[256];
  uint32_t fetch_pl_len = build_fetch_req_v1(fetch_pl, (uint32_t)sizeof(fetch_pl), "GET", "https://example.invalid/", "");
  if (fetch_pl_len == 0) {
    fprintf(stderr, "build_fetch_req_v1 failed\n");
    return 1;
  }

  uint8_t call_msg[256];
  uint32_t call_len = zi_bus_rpc_v1_write_call(call_msg, (uint32_t)sizeof(call_msg), call_id,
                                               (const uint8_t *)selector, (uint32_t)strlen(selector),
                                               fetch_pl, fetch_pl_len);
  if (call_len == 0) {
    fprintf(stderr, "write_call failed\n");
    return 1;
  }

  if (!publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, call_msg, call_len, 10)) {
    fprintf(stderr, "publish CALL failed\n");
    return 1;
  }

  // Host receives CALL event and replies OK with fetch headers, then streams two response-body chunks and ends.
  {
    char topic[64];
    uint8_t data[1024];
    uint32_t data_len = 0;
    if (!read_event(h_host, 10, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
      fprintf(stderr, "host read_event failed\n");
      return 1;
    }
    if (strcmp(topic, ZI_BUS_RPC_V1_TOPIC_REQ) != 0) {
      fprintf(stderr, "host got unexpected topic\n");
      return 1;
    }

    zi_bus_rpc_v1_msg m;
    if (!zi_bus_rpc_v1_parse(data, data_len, &m)) {
      fprintf(stderr, "host parse failed\n");
      return 1;
    }
    if (m.msg_type != ZI_BUS_RPC_V1_CALL || m.call_id != call_id) {
      fprintf(stderr, "host got wrong message\n");
      return 1;
    }
    if (m.selector_len != strlen(selector) || memcmp(m.selector, selector, m.selector_len) != 0) {
      fprintf(stderr, "host selector mismatch\n");
      return 1;
    }
    if (m.payload_len != fetch_pl_len || memcmp(m.payload, fetch_pl, fetch_pl_len) != 0) {
      fprintf(stderr, "host fetch payload mismatch\n");
      return 1;
    }

    uint8_t ok_pl[64];
    // fetch OK payload: version=1, status=200, headers_len=0
    write_u32le(ok_pl + 0, 1u);
    write_u32le(ok_pl + 4, 200u);
    write_u32le(ok_pl + 8, 0u);

    uint8_t ok_msg[128];
    uint32_t ok_len = zi_bus_rpc_v1_write_ok(ok_msg, (uint32_t)sizeof(ok_msg), call_id, ok_pl, 12u);
    if (ok_len == 0) {
      fprintf(stderr, "write_ok failed\n");
      return 1;
    }
    if (!publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, ok_msg, ok_len, 11)) {
      fprintf(stderr, "publish OK failed\n");
      return 1;
    }

    const char *c0 = "ab";
    const char *c1 = "cd";
    uint8_t chunk_msg[128];
    uint32_t chunk_len = 0;

    chunk_len = zi_bus_rpc_v1_write_stream_chunk(chunk_msg, (uint32_t)sizeof(chunk_msg), call_id,
                                                 (uint32_t)ZI_BUS_RPC_V1_STREAM_RESP_BODY, 0,
                                                 (const uint8_t *)c0, 2);
    if (chunk_len == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, chunk_msg, chunk_len, 12)) {
      fprintf(stderr, "publish CHUNK0 failed\n");
      return 1;
    }

    chunk_len = zi_bus_rpc_v1_write_stream_chunk(chunk_msg, (uint32_t)sizeof(chunk_msg), call_id,
                                                 (uint32_t)ZI_BUS_RPC_V1_STREAM_RESP_BODY, 1,
                                                 (const uint8_t *)c1, 2);
    if (chunk_len == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, chunk_msg, chunk_len, 13)) {
      fprintf(stderr, "publish CHUNK1 failed\n");
      return 1;
    }

    uint8_t end_msg[64];
    uint32_t end_len = zi_bus_rpc_v1_write_stream_end(end_msg, (uint32_t)sizeof(end_msg), call_id,
                                                      (uint32_t)ZI_BUS_RPC_V1_STREAM_RESP_BODY, 2);
    if (end_len == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, end_msg, end_len, 14)) {
      fprintf(stderr, "publish END failed\n");
      return 1;
    }
  }

  // Guest receives OK and validates fetch headers payload.
  {
    char topic[64];
    uint8_t data[1024];
    uint32_t data_len = 0;
    if (!read_event(h_guest, 11, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
      fprintf(stderr, "guest read_event failed\n");
      return 1;
    }
    if (strcmp(topic, ZI_BUS_RPC_V1_TOPIC_RESP) != 0) {
      fprintf(stderr, "guest got unexpected topic\n");
      return 1;
    }
    zi_bus_rpc_v1_msg m;
    if (!zi_bus_rpc_v1_parse(data, data_len, &m)) {
      fprintf(stderr, "guest parse failed\n");
      return 1;
    }
    if (m.msg_type != ZI_BUS_RPC_V1_OK || m.call_id != call_id) {
      fprintf(stderr, "guest got wrong message\n");
      return 1;
    }

    uint32_t status = 0;
    if (!parse_fetch_ok_v1(m.payload, m.payload_len, &status) || status != 200u) {
      fprintf(stderr, "guest fetch ok payload mismatch\n");
      return 1;
    }
  }

  // Guest receives streamed response body chunks and end.
  {
    char topic[64];
    uint8_t data[1024];
    uint32_t data_len = 0;

    if (!read_event(h_guest, 12, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
      fprintf(stderr, "guest read_event chunk0 failed\n");
      return 1;
    }
    if (strcmp(topic, ZI_BUS_RPC_V1_TOPIC_RESP) != 0) {
      fprintf(stderr, "guest chunk0 unexpected topic\n");
      return 1;
    }
    zi_bus_rpc_v1_msg m;
    if (!zi_bus_rpc_v1_parse(data, data_len, &m)) {
      fprintf(stderr, "guest chunk0 parse failed\n");
      return 1;
    }
    if (m.msg_type != ZI_BUS_RPC_V1_STREAM_CHUNK || m.call_id != call_id || m.stream_kind != ZI_BUS_RPC_V1_STREAM_RESP_BODY || m.seq != 0) {
      fprintf(stderr, "guest chunk0 mismatch\n");
      return 1;
    }
    if (m.chunk_len != 2 || memcmp(m.chunk, "ab", 2) != 0) {
      fprintf(stderr, "guest chunk0 bytes mismatch\n");
      return 1;
    }

    if (!read_event(h_guest, 13, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
      fprintf(stderr, "guest read_event chunk1 failed\n");
      return 1;
    }
    if (!zi_bus_rpc_v1_parse(data, data_len, &m)) {
      fprintf(stderr, "guest chunk1 parse failed\n");
      return 1;
    }
    if (m.msg_type != ZI_BUS_RPC_V1_STREAM_CHUNK || m.call_id != call_id || m.stream_kind != ZI_BUS_RPC_V1_STREAM_RESP_BODY || m.seq != 1) {
      fprintf(stderr, "guest chunk1 mismatch\n");
      return 1;
    }
    if (m.chunk_len != 2 || memcmp(m.chunk, "cd", 2) != 0) {
      fprintf(stderr, "guest chunk1 bytes mismatch\n");
      return 1;
    }

    if (!read_event(h_guest, 14, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
      fprintf(stderr, "guest read_event end failed\n");
      return 1;
    }
    if (!zi_bus_rpc_v1_parse(data, data_len, &m)) {
      fprintf(stderr, "guest end parse failed\n");
      return 1;
    }
    if (m.msg_type != ZI_BUS_RPC_V1_STREAM_END || m.call_id != call_id || m.stream_kind != ZI_BUS_RPC_V1_STREAM_RESP_BODY || m.seq != 2) {
      fprintf(stderr, "guest end mismatch\n");
      return 1;
    }
  }

  // Non-empty headers: guest sends fetch.v1 with request headers; host returns OK with response headers.
  {
    const uint64_t call3 = 125;
    const char *selector3 = "fetch.v1";
    const char *req_headers = "Accept: text/plain\r\nX-Test: 1\r\n";
    const char *resp_headers = "Content-Type: text/plain\r\n";

    uint8_t fetch3_pl[512];
    uint32_t fetch3_len = build_fetch_req_v1(fetch3_pl, (uint32_t)sizeof(fetch3_pl), "GET", "https://example.invalid/hdr", req_headers);
    if (fetch3_len == 0) {
      fprintf(stderr, "build_fetch_req_v1 headers failed\n");
      return 1;
    }

    uint8_t call3_msg[512];
    uint32_t call3_msg_len = zi_bus_rpc_v1_write_call(call3_msg, (uint32_t)sizeof(call3_msg), call3,
                                                      (const uint8_t *)selector3, (uint32_t)strlen(selector3),
                                                      fetch3_pl, fetch3_len);
    if (call3_msg_len == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, call3_msg, call3_msg_len, 30)) {
      fprintf(stderr, "publish CALL3 failed\n");
      return 1;
    }

    // Host receives CALL3 and replies OK(headers).
    {
      char topic[64];
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event(h_host, 30, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "host read_event call3 failed\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_CALL || m.call_id != call3) {
        fprintf(stderr, "host parse call3 failed\n");
        return 1;
      }
      if (m.selector_len != strlen(selector3) || memcmp(m.selector, selector3, m.selector_len) != 0) {
        fprintf(stderr, "host selector3 mismatch\n");
        return 1;
      }
      if (m.payload_len != fetch3_len || memcmp(m.payload, fetch3_pl, fetch3_len) != 0) {
        fprintf(stderr, "host fetch3 payload mismatch\n");
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
      if (ok_len == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, ok_msg, ok_len, 31)) {
        fprintf(stderr, "publish OK call3 failed\n");
        return 1;
      }
    }

    // Guest receives OK and validates headers bytes.
    {
      char topic[64];
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event(h_guest, 31, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "guest read_event call3 ok failed\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_OK || m.call_id != call3) {
        fprintf(stderr, "guest parse call3 ok failed\n");
        return 1;
      }
      if (!parse_fetch_ok_v1_headers(m.payload, m.payload_len, 204u, resp_headers)) {
        fprintf(stderr, "guest call3 ok headers mismatch\n");
        return 1;
      }
    }
  }

  // Single-call coverage: non-empty request headers + non-empty response headers + streamed response body.
  {
    const uint64_t call6 = 128;
    const char *selector6 = "fetch.v1";
    const char *req_headers = "Accept: text/plain\r\nX-Req: 1\r\n";
    const char *resp_headers = "Content-Type: text/plain\r\nX-Resp: 1\r\n";

    uint8_t fetch6_pl[512];
    uint32_t fetch6_len = build_fetch_req_v1(fetch6_pl, (uint32_t)sizeof(fetch6_pl), "GET", "https://example.invalid/stream", req_headers);
    if (fetch6_len == 0) {
      fprintf(stderr, "build_fetch_req_v1 stream failed\n");
      return 1;
    }

    uint8_t call6_msg[512];
    uint32_t call6_msg_len = zi_bus_rpc_v1_write_call(call6_msg, (uint32_t)sizeof(call6_msg), call6,
                                                      (const uint8_t *)selector6, (uint32_t)strlen(selector6),
                                                      fetch6_pl, fetch6_len);
    if (call6_msg_len == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, call6_msg, call6_msg_len, 90)) {
      fprintf(stderr, "publish CALL6 failed\n");
      return 1;
    }

    // Host reads CALL6, replies OK with headers, then streams response body.
    {
      char topic[64];
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event(h_host, 90, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "host read_event call6 failed\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_CALL || m.call_id != call6) {
        fprintf(stderr, "host parse call6 failed\n");
        return 1;
      }
      if (m.selector_len != strlen(selector6) || memcmp(m.selector, selector6, m.selector_len) != 0) {
        fprintf(stderr, "host selector6 mismatch\n");
        return 1;
      }
      if (m.payload_len != fetch6_len || memcmp(m.payload, fetch6_pl, fetch6_len) != 0) {
        fprintf(stderr, "host fetch6 payload mismatch\n");
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
      if (ok_len == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, ok_msg, ok_len, 91)) {
        fprintf(stderr, "publish OK6 failed\n");
        return 1;
      }

      uint8_t chunk_msg[128];
      uint32_t chunk_len = 0;
      chunk_len = zi_bus_rpc_v1_write_stream_chunk(chunk_msg, (uint32_t)sizeof(chunk_msg), call6,
                                                   (uint32_t)ZI_BUS_RPC_V1_STREAM_RESP_BODY, 0,
                                                   (const uint8_t *)"he", 2);
      if (chunk_len == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, chunk_msg, chunk_len, 92)) {
        fprintf(stderr, "publish CALL6 chunk0 failed\n");
        return 1;
      }
      chunk_len = zi_bus_rpc_v1_write_stream_chunk(chunk_msg, (uint32_t)sizeof(chunk_msg), call6,
                                                   (uint32_t)ZI_BUS_RPC_V1_STREAM_RESP_BODY, 1,
                                                   (const uint8_t *)"llo", 3);
      if (chunk_len == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, chunk_msg, chunk_len, 93)) {
        fprintf(stderr, "publish CALL6 chunk1 failed\n");
        return 1;
      }

      uint8_t end_msg[64];
      uint32_t end_len = zi_bus_rpc_v1_write_stream_end(end_msg, (uint32_t)sizeof(end_msg), call6,
                                                        (uint32_t)ZI_BUS_RPC_V1_STREAM_RESP_BODY, 2);
      if (end_len == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, end_msg, end_len, 94)) {
        fprintf(stderr, "publish CALL6 end failed\n");
        return 1;
      }
    }

    // Guest reads OK6 + chunks + end.
    {
      char topic[64];
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event(h_guest, 91, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "guest read_event ok6 failed\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_OK || m.call_id != call6) {
        fprintf(stderr, "guest parse ok6 failed\n");
        return 1;
      }
      if (!parse_fetch_ok_v1_headers(m.payload, m.payload_len, 200u, resp_headers)) {
        fprintf(stderr, "guest ok6 headers mismatch\n");
        return 1;
      }

      if (!read_event(h_guest, 92, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "guest read_event call6 chunk0 failed\n");
        return 1;
      }
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_STREAM_CHUNK || m.call_id != call6) {
        fprintf(stderr, "guest parse call6 chunk0 failed\n");
        return 1;
      }
      if (m.stream_kind != ZI_BUS_RPC_V1_STREAM_RESP_BODY || m.seq != 0 || m.chunk_len != 2 || memcmp(m.chunk, "he", 2) != 0) {
        fprintf(stderr, "guest call6 chunk0 mismatch\n");
        return 1;
      }

      if (!read_event(h_guest, 93, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "guest read_event call6 chunk1 failed\n");
        return 1;
      }
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_STREAM_CHUNK || m.call_id != call6) {
        fprintf(stderr, "guest parse call6 chunk1 failed\n");
        return 1;
      }
      if (m.stream_kind != ZI_BUS_RPC_V1_STREAM_RESP_BODY || m.seq != 1 || m.chunk_len != 3 || memcmp(m.chunk, "llo", 3) != 0) {
        fprintf(stderr, "guest call6 chunk1 mismatch\n");
        return 1;
      }

      if (!read_event(h_guest, 94, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "guest read_event call6 end failed\n");
        return 1;
      }
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_STREAM_END || m.call_id != call6) {
        fprintf(stderr, "guest parse call6 end failed\n");
        return 1;
      }
      if (m.stream_kind != ZI_BUS_RPC_V1_STREAM_RESP_BODY || m.seq != 2) {
        fprintf(stderr, "guest call6 end mismatch\n");
        return 1;
      }
    }
  }

  // POST + request-body streaming: guest streams req-body, host verifies chunk order, host replies OK.
  {
    const uint64_t call4 = 126;
    const char *selector4 = "fetch.v1";
    const char *req_headers = "Content-Length: 3\r\n";

    uint8_t fetch4_pl[512];
    uint32_t fetch4_len = build_fetch_req_v1(fetch4_pl, (uint32_t)sizeof(fetch4_pl), "POST", "https://example.invalid/post", req_headers);
    if (fetch4_len == 0) {
      fprintf(stderr, "build_fetch_req_v1 post failed\n");
      return 1;
    }

    uint8_t call4_msg[512];
    uint32_t call4_msg_len = zi_bus_rpc_v1_write_call(call4_msg, (uint32_t)sizeof(call4_msg), call4,
                                                      (const uint8_t *)selector4, (uint32_t)strlen(selector4),
                                                      fetch4_pl, fetch4_len);
    if (call4_msg_len == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, call4_msg, call4_msg_len, 40)) {
      fprintf(stderr, "publish CALL4 failed\n");
      return 1;
    }

    // Guest streams request body: "xy" + "z" then END.
    {
      uint8_t msg[128];
      uint32_t n = 0;
      n = zi_bus_rpc_v1_write_stream_chunk(msg, (uint32_t)sizeof(msg), call4,
                                           (uint32_t)ZI_BUS_RPC_V1_STREAM_REQ_BODY, 0,
                                           (const uint8_t *)"xy", 2);
      if (n == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, msg, n, 41)) {
        fprintf(stderr, "publish CALL4 req chunk0 failed\n");
        return 1;
      }
      n = zi_bus_rpc_v1_write_stream_chunk(msg, (uint32_t)sizeof(msg), call4,
                                           (uint32_t)ZI_BUS_RPC_V1_STREAM_REQ_BODY, 1,
                                           (const uint8_t *)"z", 1);
      if (n == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, msg, n, 42)) {
        fprintf(stderr, "publish CALL4 req chunk1 failed\n");
        return 1;
      }
      n = zi_bus_rpc_v1_write_stream_end(msg, (uint32_t)sizeof(msg), call4,
                                         (uint32_t)ZI_BUS_RPC_V1_STREAM_REQ_BODY, 2);
      if (n == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, msg, n, 43)) {
        fprintf(stderr, "publish CALL4 req end failed\n");
        return 1;
      }
    }

    // Host reads CALL4 and the streamed request body.
    {
      char topic[64];
      uint8_t data[1024];
      uint32_t data_len = 0;

      if (!read_event(h_host, 40, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "host read_event call4 failed\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_CALL || m.call_id != call4) {
        fprintf(stderr, "host parse call4 failed\n");
        return 1;
      }

      if (!read_event(h_host, 41, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "host read_event call4 chunk0 failed\n");
        return 1;
      }
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_STREAM_CHUNK || m.call_id != call4) {
        fprintf(stderr, "host parse call4 chunk0 failed\n");
        return 1;
      }
      if (m.stream_kind != ZI_BUS_RPC_V1_STREAM_REQ_BODY || m.seq != 0 || m.chunk_len != 2 || memcmp(m.chunk, "xy", 2) != 0) {
        fprintf(stderr, "host call4 chunk0 mismatch\n");
        return 1;
      }

      if (!read_event(h_host, 42, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "host read_event call4 chunk1 failed\n");
        return 1;
      }
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_STREAM_CHUNK || m.call_id != call4) {
        fprintf(stderr, "host parse call4 chunk1 failed\n");
        return 1;
      }
      if (m.stream_kind != ZI_BUS_RPC_V1_STREAM_REQ_BODY || m.seq != 1 || m.chunk_len != 1 || memcmp(m.chunk, "z", 1) != 0) {
        fprintf(stderr, "host call4 chunk1 mismatch\n");
        return 1;
      }

      if (!read_event(h_host, 43, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "host read_event call4 end failed\n");
        return 1;
      }
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_STREAM_END || m.call_id != call4) {
        fprintf(stderr, "host parse call4 end failed\n");
        return 1;
      }
      if (m.stream_kind != ZI_BUS_RPC_V1_STREAM_REQ_BODY || m.seq != 2) {
        fprintf(stderr, "host call4 end mismatch\n");
        return 1;
      }

      // Host replies OK(version/status/headers_len=0).
      uint8_t ok_pl[12];
      write_u32le(ok_pl + 0, 1u);
      write_u32le(ok_pl + 4, 201u);
      write_u32le(ok_pl + 8, 0u);
      uint8_t ok_msg[128];
      uint32_t ok_len = zi_bus_rpc_v1_write_ok(ok_msg, (uint32_t)sizeof(ok_msg), call4, ok_pl, 12u);
      if (ok_len == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, ok_msg, ok_len, 44)) {
        fprintf(stderr, "publish CALL4 OK failed\n");
        return 1;
      }
    }

    // Guest reads OK and validates status.
    {
      char topic[64];
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event(h_guest, 44, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "guest read_event call4 ok failed\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_OK || m.call_id != call4) {
        fprintf(stderr, "guest parse call4 ok failed\n");
        return 1;
      }
      if (!parse_fetch_ok_v1(m.payload, m.payload_len, NULL) || read_u32le(m.payload + 4) != 201u) {
        fprintf(stderr, "guest call4 ok status mismatch\n");
        return 1;
      }
    }
  }

  // Malformed fetch.v1 payload: host replies ERR(fetch.invalid).
  {
    const uint64_t call5 = 127;
    const char *selector5 = "fetch.v1";
    uint8_t bad_pl[4];
    // version=2 (invalid)
    write_u32le(bad_pl + 0, 2u);

    uint8_t call5_msg[128];
    uint32_t call5_len = zi_bus_rpc_v1_write_call(call5_msg, (uint32_t)sizeof(call5_msg), call5,
                                                  (const uint8_t *)selector5, (uint32_t)strlen(selector5),
                                                  bad_pl, 4u);
    if (call5_len == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, call5_msg, call5_len, 50)) {
      fprintf(stderr, "publish CALL5 failed\n");
      return 1;
    }

    // Host reads CALL5 and replies ERR(fetch.invalid).
    {
      char topic[64];
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event(h_host, 50, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "host read_event call5 failed\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_CALL || m.call_id != call5) {
        fprintf(stderr, "host parse call5 failed\n");
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
      if (err_len == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, err_msg, err_len, 51)) {
        fprintf(stderr, "publish CALL5 ERR failed\n");
        return 1;
      }
    }

    // Guest reads ERR and checks code.
    {
      char topic[64];
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event(h_guest, 51, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "guest read_event call5 err failed\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_ERR || m.call_id != call5) {
        fprintf(stderr, "guest parse call5 err failed\n");
        return 1;
      }
      if (m.code_len != 13 || memcmp(m.code, "fetch.invalid", 13) != 0) {
        fprintf(stderr, "guest call5 err code mismatch\n");
        return 1;
      }
    }
  }

  // Malformed fetch.v1 payload: length claims more bytes than present => ERR(fetch.invalid).
  {
    const uint64_t call7 = 129;
    const char *selector7 = "fetch.v1";

    uint8_t bad_pl[8];
    // version=1, method_len=10 but no method bytes follow.
    write_u32le(bad_pl + 0, 1u);
    write_u32le(bad_pl + 4, 10u);

    uint8_t call_msg[128];
    uint32_t call_len = zi_bus_rpc_v1_write_call(call_msg, (uint32_t)sizeof(call_msg), call7,
                                                 (const uint8_t *)selector7, (uint32_t)strlen(selector7),
                                                 bad_pl, (uint32_t)sizeof(bad_pl));
    if (call_len == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, call_msg, call_len, 110)) {
      fprintf(stderr, "publish CALL7 failed\n");
      return 1;
    }

    // Host reads CALL7 and replies ERR(fetch.invalid).
    {
      char topic[64];
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event(h_host, 110, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "host read_event call7 failed\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_CALL || m.call_id != call7) {
        fprintf(stderr, "host parse call7 failed\n");
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
      if (err_len == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, err_msg, err_len, 111)) {
        fprintf(stderr, "publish CALL7 ERR failed\n");
        return 1;
      }
    }

    // Guest reads ERR7 and checks code.
    {
      char topic[64];
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event(h_guest, 111, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "guest read_event call7 err failed\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_ERR || m.call_id != call7) {
        fprintf(stderr, "guest parse call7 err failed\n");
        return 1;
      }
      if (m.code_len != 13 || memcmp(m.code, "fetch.invalid", 13) != 0) {
        fprintf(stderr, "guest call7 err code mismatch\n");
        return 1;
      }
    }
  }

  // CANCEL flow: guest issues CALL, host sends one chunk, guest cancels, host replies ERR.
  {
    const uint64_t call2 = 124;

    uint8_t call_msg2[256];
    uint32_t call_len2 = zi_bus_rpc_v1_write_call(call_msg2, (uint32_t)sizeof(call_msg2), call2,
                                                  (const uint8_t *)"stream.v1", 9,
                                                  (const uint8_t *)"", 0);
    if (call_len2 == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, call_msg2, call_len2, 20)) {
      fprintf(stderr, "publish CALL2 failed\n");
      return 1;
    }

    // Host receives CALL2.
    {
      char topic[64];
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event(h_host, 20, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "host read_event call2 failed\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_CALL || m.call_id != call2) {
        fprintf(stderr, "host parse call2 failed\n");
        return 1;
      }
    }

    // Host sends one chunk.
    {
      uint8_t chunk_msg[64];
      uint32_t chunk_len = zi_bus_rpc_v1_write_stream_chunk(chunk_msg, (uint32_t)sizeof(chunk_msg), call2,
                                                            (uint32_t)ZI_BUS_RPC_V1_STREAM_RESP_BODY, 0,
                                                            (const uint8_t *)"x", 1);
      if (chunk_len == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, chunk_msg, chunk_len, 21)) {
        fprintf(stderr, "publish call2 chunk failed\n");
        return 1;
      }
    }

    // Guest reads chunk.
    {
      char topic[64];
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event(h_guest, 21, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "guest read_event call2 chunk failed\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_STREAM_CHUNK || m.call_id != call2) {
        fprintf(stderr, "guest parse call2 chunk failed\n");
        return 1;
      }
    }

    // Guest cancels.
    {
      uint8_t cancel_msg[32];
      uint32_t cancel_len = zi_bus_rpc_v1_write_cancel(cancel_msg, (uint32_t)sizeof(cancel_msg), call2);
      if (cancel_len == 0 || !publish(h_guest, ZI_BUS_RPC_V1_TOPIC_REQ, cancel_msg, cancel_len, 22)) {
        fprintf(stderr, "publish CANCEL failed\n");
        return 1;
      }
    }

    // Host receives cancel and replies ERR.
    {
      char topic[64];
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event(h_host, 22, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "host read_event cancel failed\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_CANCEL || m.call_id != call2) {
        fprintf(stderr, "host parse cancel failed\n");
        return 1;
      }

      uint8_t err_msg[128];
      uint32_t err_len = zi_bus_rpc_v1_write_err(err_msg, (uint32_t)sizeof(err_msg), call2,
                                                 (const uint8_t *)"cancelled", 9,
                                                 (const uint8_t *)"cancel", 6);
      if (err_len == 0 || !publish(h_host, ZI_BUS_RPC_V1_TOPIC_RESP, err_msg, err_len, 23)) {
        fprintf(stderr, "publish ERR failed\n");
        return 1;
      }
    }

    // Guest receives ERR.
    {
      char topic[64];
      uint8_t data[1024];
      uint32_t data_len = 0;
      if (!read_event(h_guest, 23, topic, (uint32_t)sizeof(topic), data, (uint32_t)sizeof(data), &data_len)) {
        fprintf(stderr, "guest read_event err failed\n");
        return 1;
      }
      zi_bus_rpc_v1_msg m;
      if (!zi_bus_rpc_v1_parse(data, data_len, &m) || m.msg_type != ZI_BUS_RPC_V1_ERR || m.call_id != call2) {
        fprintf(stderr, "guest parse err failed\n");
        return 1;
      }
      if (m.code_len != 9 || memcmp(m.code, "cancelled", 9) != 0) {
        fprintf(stderr, "guest err code mismatch\n");
        return 1;
      }
    }
  }

  (void)sub_host;
  (void)sub_guest;
  (void)zi_end(h_host);
  (void)zi_end(h_guest);

  return 0;
}
