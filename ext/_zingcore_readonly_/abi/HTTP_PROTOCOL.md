# net/http v1 Protocol Specification (Draft)

**Version:** 1  
**Capability Name:** `net/http`  
**Status:** Draft (proposed for zABI 2.5+)  

## Overview

The `net/http` capability provides sandboxed HTTP/1.1 client and server functionality to guest programs.

Design goals:
- Guests can implement Express-style servers without implementing TCP accept loops, HTTP parsing, chunking, or multipart boundary handling.
- Structured request metadata (method/path/headers) is delivered as parsed fields.
- Bodies are provided either inline (bounded) or as stream handles.
- Multipart form parsing is provided by the capability (Option A): guest iterates parts and receives each part body as a stream handle.

Non-goals (v1):
- HTTPS/TLS.
- HTTP/2.
- Automatic JSON object model in the ABI.

## Capability Identity

- kind: `"net"`
- name: `"http"`
- version: `1`

The cap is opened via `zi_cap_open()` and yields a bidirectional handle.

## I/O Model

Once opened, the handle carries ZCL1 frames over the standard stream syscalls:
- Guests `zi_write(handle, ...)` ZCL1 **requests/commands**.
- Guests `zi_read(handle, ...)` ZCL1 **responses** and **events**.
- Guests `zi_end(handle)` closes the capability handle.

A single `net/http` handle is multiplexed:
- A guest may create one or more listeners (server).
- A guest may issue outbound requests (client).
- Incoming request events are delivered asynchronously on the same handle.

### Backpressure

Implementations MAY apply backpressure:
- `zi_write` MAY return `ZI_E_AGAIN` if too many unread frames are queued.
- Guests SHOULD follow the pattern: write → drain reads → write.

## Sandboxing

### Listening (Server)

Implementations MUST gate listening by policy. Default policy SHOULD be loopback-only.

Recommended environment variable:
- `ZI_NET_LISTEN_ALLOW`

Suggested semantics (normative for this capability if used):
- If unset/empty: only `127.0.0.1` and `::1` are permitted bind addresses.
- If set to `any`: any bind address is permitted.
- Otherwise: comma-separated entries of `host:port` or `host:*` or `loopback`.

### Connecting (Client)

Outbound connections MUST be gated by policy (recommended: reuse `ZI_NET_ALLOW` from `net/tcp`).

## Limits and Defaults

This protocol intentionally requires implementations to enforce resource limits to prevent memory/CPU exhaustion and request-smuggling style ambiguity.

The ABI does not mandate *how* limits are configured. For the reference runtime, the following environment variables are RECOMMENDED knobs.

For v1, these limits are intended to be **runtime-wide** ("server-wide"): they apply uniformly to all `net/http` listeners and requests within the process.

If an environment variable is unset or invalid, implementations SHOULD fall back to the stated default.

### Core HTTP limits

- `ZI_HTTP_MAX_REQ_LINE_BYTES` (default: `8192`)
  - Maximum bytes in the request line (method + SP + target + SP + version + CRLF).
- `ZI_HTTP_MAX_HEADER_BYTES` (default: `65536`)
  - Maximum total bytes across all header lines (including CRLFs) for a single request.
- `ZI_HTTP_MAX_HEADER_COUNT` (default: `128`)
  - Maximum number of header fields.
- `ZI_HTTP_MAX_INLINE_BODY_BYTES` (default: `1048576`)
  - Maximum body size that may be delivered as `INLINE_BYTES`.
  - Bodies larger than this MUST be delivered as `STREAM` (or rejected if streaming is unsupported).
- `ZI_HTTP_MAX_INFLIGHT_REQUESTS` (default: `256`)
  - Maximum number of requests currently tracked by the cap (across all listeners).

### Multipart limits (Option A)

- `ZI_HTTP_MAX_MULTIPART_PARTS` (default: `128`)
  - Maximum number of parts per request.
- `ZI_HTTP_MAX_MULTIPART_HEADER_BYTES` (default: `16384`)
  - Maximum total bytes of headers for a single part.
- `ZI_HTTP_MAX_MULTIPART_HEADER_COUNT` (default: `64`)
  - Maximum number of header fields per part.
- `ZI_HTTP_MAX_MULTIPART_NAME_BYTES` (default: `256`)
  - Maximum bytes in the parsed form field name.
- `ZI_HTTP_MAX_MULTIPART_FILENAME_BYTES` (default: `1024`)
  - Maximum bytes in the parsed filename.

### Client limits

- `ZI_HTTP_MAX_FETCH_URL_BYTES` (default: `8192`)
  - Maximum size of the absolute URL passed to `FETCH`.

### Default transfer semantics (v1)

Implementations SHOULD default to conservative semantics:
- If `Transfer-Encoding: chunked` is not supported, reject such requests (treat as `ZI_E_INVALID`).
- If `Transfer-Encoding: chunked` is supported, the runtime MUST decode the chunked framing and expose the decoded bytes as a request body `STREAM` (i.e., `body_kind=2`).
- Close the connection after completing a response, unless keep-alive is explicitly supported.

Notes (reference runtime behavior):
- Server-side incoming requests with `Transfer-Encoding: chunked` are supported and are exposed as `body_kind=STREAM`.
- Multipart (Option A) advertisement is conservative; runtimes MAY choose not to advertise `MULTIPART` for chunked bodies.
- Client-side `FETCH` supports `Transfer-Encoding: chunked` responses by decoding and exposing the decoded bytes as `body_kind=STREAM`.

## ZCL1 Operations

All frames use ZCL1 v1 (see `ZCL1_PROTOCOL.md`).

### Operation codes

Client ops:
- `1` LISTEN
- `2` CLOSE_LISTENER
- `3` FETCH

Server response ops:
- `10` RESPOND_START
- `11` RESPOND_INLINE
- `12` RESPOND_STREAM

Multipart ops (Option A):
- `20` MULTIPART_BEGIN
- `21` MULTIPART_NEXT
- `22` MULTIPART_END

Event ops:
- `100` EV_REQUEST

Notes:
- `rid` correlates a request/response pair.
- For `EV_REQUEST`, the runtime chooses a `rid` that becomes the request identifier. The guest MUST echo that same `rid` when responding and when iterating multipart.

## Common Types

All integers are little-endian.

### Address Encoding

IP addresses are returned as 16 bytes in IPv4-mapped-IPv6 form (same as `net/tcp`).

### Header Encoding

Headers are encoded as a list of `(name, value)` byte strings.

Rules:
- Header names MUST be ASCII and SHOULD be normalized to lowercase by the runtime.
- Duplicate headers MAY appear multiple times (e.g., `set-cookie`).
- A runtime MAY also provide a combined form; if it does, it MUST document which headers are combined.

### Body Kinds

`body_kind` (u32):
- `0` NONE
- `1` INLINE_BYTES
- `2` STREAM
- `3` MULTIPART

## LISTEN (op=1)

Create an HTTP listener.

### Request Payload

```
Offset | Size | Field
-------|------|-------------------------
0      | 4    | port (0=ephemeral)
4      | 4    | flags (reserved; 0)
8      | 4    | bind_host_len
12     | N    | bind_host (UTF-8, no NUL)
```

- If `bind_host_len==0`, the runtime SHOULD bind loopback.
- `port=0` requests an ephemeral port.

### Response Payload (ok)

```
Offset | Size | Field
-------|------|-------------------------
0      | 4    | listener_id
4      | 4    | bound_port
8      | 16   | bound_addr (IPv4-mapped-IPv6)
```

### Errors

- `ZI_E_DENIED` if sandbox forbids the bind.
- `ZI_E_INVALID` on malformed payload.
- `ZI_E_IO` on OS bind/listen errors.

## CLOSE_LISTENER (op=2)

Close a previously created listener.

### Request Payload

```
Offset | Size | Field
-------|------|-------------------------
0      | 4    | listener_id
```

### Response Payload (ok)

Empty.

## EV_REQUEST (op=100)

Emitted by the runtime when an HTTP request is received.

### Frame

- `op=100`
- `rid` is the **request id** (chosen by runtime)

### Payload

```
Offset | Size | Field
-------|------|-------------------------
0      | 4    | listener_id
4      | 4    | method_len
8      | N    | method (UTF-8, e.g. "GET")
8+N    | 4    | path_len
12+N   | P    | path (UTF-8, starts with "/"; may include "?query")
12+N+P | 4    | scheme_len
16+N+P | S    | scheme ("http" for v1)
16+N+P+S | 4  | authority_len
20+... | A    | authority (Host header value if present; else empty)
20+... | 16   | remote_addr
36+... | 4    | remote_port
40+... | 4    | header_count
44+... | ...  | headers
...    | 4    | body_kind
...    | ...  | body descriptor (see below)
```

Headers encoding (`header_count` entries):

```
Offset | Size | Field
-------|------|-------------------------
0      | 4    | name_len
4      | N    | name
4+N    | 4    | value_len
8+N    | M    | value
```

Body descriptor:
- If `body_kind==0`: no additional fields.
- If `body_kind==1` (INLINE_BYTES):
  - `u32 body_len`, then `body_len` bytes.
- If `body_kind==2` (STREAM):
  - `i32 body_handle` (readable). Read yields body bytes; EOF indicates end-of-body.
- If `body_kind==3` (MULTIPART):
  - `i32 body_handle` MUST be `0`.
  - The guest MUST use MULTIPART_* ops to consume the request body.
  - Rationale: allowing both raw reads and multipart iteration would permit double-consumption / ambiguous ownership of the underlying stream.

### Request Limits (recommended)

Implementations SHOULD enforce and document bounds such as:
- max request line bytes
- max total header bytes
- max header count
- max inline body bytes
- max concurrent in-flight requests

## RESPOND_START (op=10)

Start a response for an `EV_REQUEST`.

### Request

- The guest MUST use the same `rid` as the corresponding `EV_REQUEST`.

Payload:

```
Offset | Size | Field
-------|------|-------------------------
0      | 4    | status_code (e.g. 200)
4      | 4    | flags (reserved; 0)
8      | 4    | header_count
12     | ...  | headers
```

Headers use the same encoding as requests.

### Response (ok)

Empty.

Notes:
- After `RESPOND_START`, the guest may send either `RESPOND_INLINE` or `RESPOND_STREAM`.
- Implementations MAY allow `RESPOND_INLINE` without a prior `RESPOND_START` by treating it as implicit start.

## RESPOND_INLINE (op=11)

Send a complete response with an inline body (bounded).

### Request

- Uses the request `rid`.

Payload:

```
Offset | Size | Field
-------|------|-------------------------
0      | 4    | status_code
4      | 4    | flags (reserved; 0)
8      | 4    | header_count
12     | ...  | headers
...    | 4    | body_len
...    | B    | body_bytes
```

### Response (ok)

Empty.

## RESPOND_STREAM (op=12)

Begin a streaming response body.

### Request

- Uses the request `rid`.

Payload:

```
Offset | Size | Field
-------|------|-------------------------
0      | 4    | status_code
4      | 4    | flags (reserved; 0)
8      | 4    | header_count
12     | ...  | headers
```

### Response (ok)

```
Offset | Size | Field
-------|------|-------------------------
0      | 4    | body_handle (writable)
```

Semantics:
- The guest writes raw body bytes to `body_handle` using `zi_write`.
- The guest MUST call `zi_end(body_handle)` to finish the response.
- The runtime SHOULD close the underlying TCP connection if v1 does not support keep-alive.

## MULTIPART_BEGIN (op=20)

Prepare multipart iteration for a request.

### Request

- Uses the request `rid`.
- Empty payload.

### Response (ok)

Payload:

```
Offset | Size | Field
-------|------|-------------------------
0      | 4    | flags (reserved; 0)
```

Errors:
- `ZI_E_INVALID` if the request is not multipart.
- `ZI_E_BOUNDS` / `ZI_E_IO` for internal parsing failures.

## MULTIPART_NEXT (op=21)

Return the next multipart part.

### Request

- Uses the request `rid`.
- Empty payload.

### Response (ok)

Payload:

```
Offset | Size | Field
-------|------|-------------------------
0      | 4    | done (0/1)
4      | ...  | if done==0: part descriptor
```

Part descriptor:

```
Offset | Size | Field
-------|------|-------------------------
0      | 4    | name_len
4      | N    | name (form field name)
4+N    | 4    | filename_len
8+N    | F    | filename (may be empty)
8+N+F  | 4    | content_type_len
12+... | T    | content_type (may be empty)
12+...+T | 4  | header_count
16+... | ...  | headers (same encoding)
...    | 4    | body_handle (readable)
```

Semantics:
- `body_handle` yields bytes for this part only.
- EOF on `body_handle` indicates end-of-part.
- The guest SHOULD call `zi_end(body_handle)` when finished.
- The runtime MUST ensure correct boundary handling even if the guest reads parts incrementally.

## MULTIPART_END (op=22)

Finalize multipart iteration.

### Request

- Uses the request `rid`.
- Empty payload.

### Response (ok)

Empty.

## FETCH (op=3)

Perform an outbound HTTP request (client).

### Request Payload

```
Offset | Size | Field
-------|------|-------------------------
0      | 4    | method_len
4      | N    | method (e.g. "GET")
4+N    | 4    | url_len
8+N    | U    | url (absolute, e.g. "http://example.com/path")
8+N+U  | 4    | header_count
12+... | ...  | headers
...    | 4    | body_kind (0=none, 1=inline, 2=stream)
...    | ...  | body descriptor
```

Body descriptor:
- `body_kind==0`: none
- `body_kind==1`: `u32 body_len` + bytes
- `body_kind==2`: `i32 body_handle` (readable; runtime will stream from it)

### Response Payload (ok)

```
Offset | Size | Field
-------|------|-------------------------
0      | 4    | status_code
4      | 4    | header_count
8      | ...  | headers
...    | 4    | body_kind (0=none, 1=inline, 2=stream)
...    | ...  | body descriptor
```

Response body descriptor:
- `body_kind==1`: `u32 body_len` + bytes (bounded)
- `body_kind==2`: `i32 body_handle` (readable)

## Keep-Alive / Transfer-Encoding

v1 may be implemented in a conservative mode:
- For server responses, the runtime MAY close the connection after completing a response.
- Implementations MAY reject requests with `Transfer-Encoding: chunked` (recommended for v1) using `ZI_E_INVALID`.

Future versions may add:
- chunked decoding/encoding
- keep-alive policies
- HTTP/2

## Security Considerations

Implementations MUST:
- Enforce header and body size limits to prevent resource exhaustion.
- Enforce listener bind allowlists.
- Enforce outbound connect allowlists.
- Validate and bound multipart parsing (max parts, max per-part headers, max header bytes).

