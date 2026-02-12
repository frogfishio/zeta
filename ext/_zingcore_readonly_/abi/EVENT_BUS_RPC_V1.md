# RPC-over-event/bus v1 — Lambda-style Host↔Guest Messaging

This document defines a *convention* (“application protocol”) for building bidirectional request/response and streaming transfers on top of the golden capability `event/bus@v1`.

It is intended for “lambda-like” guests:
- the **host** runs the real reactor (HTTP server, device IO, UI)
- the **guest** is a small handler that consumes events and emits responses

The goal is to avoid implementing TCP/TLS/HTTP/etc inside the guest while still supporting large uploads/downloads via streaming.

The key words MUST, MUST NOT, SHOULD, and MAY are to be interpreted as described in RFC 2119.

## Transport

Transport is `event/bus@v1` as specified in `EVENT_BUS_PROTOCOL.md`.

All RPC messages are carried as the `data` bytes of `EVENT` frames.

## Topics

This convention uses two fixed topics:

- Requests: `rpc/v1/req`
- Responses: `rpc/v1/resp`

Rationale:
- The host subscribes once to `rpc/v1/req`.
- Guests subscribe once to `rpc/v1/resp`.
- Correlation is done by an explicit `call_id` field in every message.

Note: With fixed topics, every guest will observe all responses; guests MUST filter by `call_id` (and optionally by `client_id` inside payloads for multi-tenant embeddings).

## Message envelope

All integers are little-endian.

Every message begins with:

- `u32 msg_type`
- `u64 call_id`

`call_id` MUST be non-zero.

## Message types

`msg_type` values:

- `1` `CALL`
- `2` `OK`
- `3` `ERR`
- `10` `STREAM_CHUNK`
- `11` `STREAM_END`
- `20` `CANCEL`

### CALL (msg_type=1)

A call initiates a bidirectional session. It carries a selector and an initial payload.

Payload:

- `u32 selector_len`
- `bytes[selector_len] selector`
- `u32 payload_len`
- `bytes[payload_len] payload`

The meaning of `selector` and `payload` is application-defined.

Examples:
- selector `http.handle.v1` : host delivers an HTTP request, guest returns response
- selector `fetch.v1` : guest requests the host to fetch a URL, host streams response

### OK (msg_type=2)

A successful completion message.

Payload:

- `u32 payload_len`
- `bytes[payload_len] payload`

### ERR (msg_type=3)

A failure completion message.

Payload:

- `u32 code_len`
- `bytes[code_len] code`  (UTF-8 recommended)
- `u32 msg_len`
- `bytes[msg_len] msg`    (UTF-8 recommended)

### STREAM_CHUNK (msg_type=10)

Stream body bytes in either direction.

Payload:

- `u32 stream_kind`  (`0` = request body, `1` = response body)
- `u32 seq`          (monotonic per `(call_id, stream_kind)`, starting at 0)
- `u32 bytes_len`
- `bytes[bytes_len] bytes`

Ordering:
- A receiver MUST accept `STREAM_CHUNK` messages in increasing `seq` order.

### STREAM_END (msg_type=11)

Signals end-of-stream for a stream kind.

Payload:

- `u32 stream_kind` (`0` = request body, `1` = response body)
- `u32 seq`         (the final sequence number + 1; i.e. the next seq after the last chunk)

### CANCEL (msg_type=20)

Requests cancellation of an in-flight `call_id`.

Payload:

- empty

Cancellation rules:
- Cancellation is best-effort.
- If a party receives `CANCEL`, it SHOULD stop emitting further `STREAM_CHUNK` for that `call_id`.
- The cancelling party SHOULD accept either `OK` or `ERR` as a terminal outcome.

## Streaming fetch pattern (recommended)

To support large uploads/downloads without `async/default` futures:

- Guest publishes `CALL(fetch.v1, metadata)` on `rpc/v1/req`.
- Guest publishes request body via `STREAM_CHUNK(stream_kind=0)` and terminates with `STREAM_END(stream_kind=0)`.
- Host emits response headers via `OK` (or a dedicated header payload in `OK`) and streams response body via `STREAM_CHUNK(stream_kind=1)` followed by `STREAM_END(stream_kind=1)`.

This allows true streaming while keeping the guest small.

## Selector: fetch.v1 (recommended schema)

This section standardizes a minimal, *portable* payload schema for `selector = "fetch.v1"`.

Goal:
- Enable “tiny guests” to request the host to fetch a URL (or file-like resource) without implementing TCP/TLS/HTTP.
- Support both request upload bodies and response download bodies via `STREAM_CHUNK` / `STREAM_END`.

### CALL(fetch.v1, payload)

`CALL.payload` bytes for `fetch.v1` are:

- `u32 version` (MUST be `1`)
- `u32 method_len`
- `bytes[method_len] method` (ASCII recommended, e.g. `"GET"`, `"POST"`)
- `u32 url_len`
- `bytes[url_len] url` (UTF-8; URL / URI string)
- `u32 headers_len`
- `bytes[headers_len] headers`

Notes:
- `headers` is an opaque byte blob. v1 RECOMMENDS HTTP/1.1-style header lines (UTF-8): `"Key: Value\r\n"...`.
- If the guest needs to upload a request body, it SHOULD stream it using:
	- `STREAM_CHUNK(stream_kind=0 /* req body */)`
	- `STREAM_END(stream_kind=0)`

### OK(fetch.v1, payload)

The host MUST send exactly one terminal `OK` or `ERR`.

For `OK`, the `OK.payload` bytes for `fetch.v1` are:

- `u32 version` (MUST be `1`)
- `u32 status` (HTTP-like numeric status; for non-HTTP resources, use an appropriate conventional mapping)
- `u32 headers_len`
- `bytes[headers_len] headers` (opaque; v1 RECOMMENDS HTTP/1.1-style header lines)

After sending `OK`, the host MAY stream the response body using:
- `STREAM_CHUNK(stream_kind=1 /* resp body */)`
- `STREAM_END(stream_kind=1)`

### ERR(fetch.v1, payload)

`ERR.code` and `ERR.msg` are application-defined.

v1 RECOMMENDS codes like:
- `"fetch.invalid"` (bad payload)
- `"fetch.denied"` (policy/security)
- `"fetch.timeout"`
- `"fetch.not_found"`
- `"fetch.io"`
- `"fetch.cancelled"`

### Example transcript (annotated)

This section shows one concrete wire example for `fetch.v1`.

All RPC messages below are carried as the `data` bytes of an `EVENT` frame.
- Guest publishes requests on topic `rpc/v1/req`.
- Host publishes responses on topic `rpc/v1/resp`.

#### Example A: GET with streamed response body

Guest → Host: `CALL` (topic `rpc/v1/req`), `call_id = 123` (`0x7b`)

Field breakdown (little-endian hex for fixed-width integers; ASCII shown for strings):

- `msg_type` = `CALL(1)`: `01 00 00 00`
- `call_id` = `123`: `7b 00 00 00 00 00 00 00`
- `selector_len` = `8`: `08 00 00 00`
- `selector` = `"fetch.v1"`: `66 65 74 63 68 2e 76 31`
- `payload_len` = `43 (0x2b)`: `2b 00 00 00`
- `payload` (`fetch.v1` request metadata):
	- `version` = `1`: `01 00 00 00`
	- `method_len` = `3`: `03 00 00 00`
	- `method` = `"GET"`: `47 45 54`
	- `url_len` = `24 (0x18)`: `18 00 00 00`
	- `url` = `"https://example.invalid/"` (24 UTF-8 bytes)
	- `headers_len` = `0`: `00 00 00 00`

Host → Guest: `OK` (topic `rpc/v1/resp`), headers-only; body will be streamed

- `msg_type` = `OK(2)`: `02 00 00 00`
- `call_id` = `123`: `7b 00 00 00 00 00 00 00`
- `payload_len` = `12`: `0c 00 00 00`
- `payload` (`fetch.v1` response headers):
	- `version` = `1`: `01 00 00 00`
	- `status` = `200`: `c8 00 00 00`
	- `headers_len` = `0`: `00 00 00 00`

Host → Guest: stream response body bytes

`STREAM_CHUNK` #0 (topic `rpc/v1/resp`), response body `"ab"`:
- `msg_type` = `STREAM_CHUNK(10)`: `0a 00 00 00`
- `call_id` = `123`: `7b 00 00 00 00 00 00 00`
- `stream_kind` = `1`: `01 00 00 00`
- `seq` = `0`: `00 00 00 00`
- `bytes_len` = `2`: `02 00 00 00`
- `bytes` = `"ab"`: `61 62`

`STREAM_CHUNK` #1 (topic `rpc/v1/resp`), response body `"cd"`:
- `msg_type` = `STREAM_CHUNK(10)`: `0a 00 00 00`
- `call_id` = `123`: `7b 00 00 00 00 00 00 00`
- `stream_kind` = `1`: `01 00 00 00`
- `seq` = `1`: `01 00 00 00`
- `bytes_len` = `2`: `02 00 00 00`
- `bytes` = `"cd"`: `63 64`

`STREAM_END` (topic `rpc/v1/resp`), response body done:
- `msg_type` = `STREAM_END(11)`: `0b 00 00 00`
- `call_id` = `123`: `7b 00 00 00 00 00 00 00`
- `stream_kind` = `1`: `01 00 00 00`
- `seq` = `2` (next sequence number): `02 00 00 00`

#### Example B: cancellation

Guest → Host: `CANCEL` (topic `rpc/v1/req`), `call_id = 124` (`0x7c`)

- `msg_type` = `CANCEL(20)`: `14 00 00 00`
- `call_id` = `124`: `7c 00 00 00 00 00 00 00`
- no payload

Host → Guest: terminal `ERR` (topic `rpc/v1/resp`)

- `msg_type` = `ERR(3)`: `03 00 00 00`
- `call_id` = `124`: `7c 00 00 00 00 00 00 00`
- `code_len` = `15`: `0f 00 00 00`
- `code` = `"fetch.cancelled"` (15 UTF-8 bytes)
- `msg_len` = `6`: `06 00 00 00`
- `msg` = `"cancel"` (6 UTF-8 bytes)

## Backpressure

`event/bus@v1` delivery is best-effort if a subscriber’s output buffering is full.

Implementations using this convention SHOULD:
- keep chunks modest (e.g. 16–64 KiB)
- tolerate retry at the application layer (e.g. resend chunk) if desired
- consider adding an application-level `ACK(seq)` if stronger delivery is needed

v1 does not standardize ACKs.
