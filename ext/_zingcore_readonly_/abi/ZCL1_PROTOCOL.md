# ZCL1 Wire Protocol Specification

**Version**: 1  
**Status**: Normative (zABI 2.5)

This document specifies the ZCL1 (Zing Control Layer 1) framing protocol used by `zi_ctl` and several zABI 2.5 capabilities.

## Design Goals

- Simple binary framing for request/response protocols
- Deterministic parsing with no optional fields
- Fixed header size for fast validation
- Support for both synchronous request/response and asynchronous event streams

## Frame Structure

All integers are **little-endian**.

### Frame Layout (24 bytes header + payload)

```
Offset | Size | Field         | Description
-------|------|---------------|------------------------------------------
0      | 4    | magic         | ASCII "ZCL1" (0x5A, 0x43, 0x4C, 0x31)
4      | 2    | version       | Protocol version (1 for ZCL1)
6      | 2    | op            | Operation code (request/response type)
8      | 4    | rid           | Request ID (caller-chosen correlation)
12     | 4    | status        | Status code (1=ok, 0=error for responses)
16     | 4    | reserved      | Reserved (must be 0)
20     | 4    | payload_len   | Payload length in bytes
24     | N    | payload       | Payload bytes (length = payload_len)
```

Total frame size: `24 + payload_len` bytes.

### Field Semantics

#### `magic` (4 bytes)
- MUST be the ASCII bytes `"ZCL1"` (`0x5A434C31` little-endian as u32).
- Parsers MUST reject frames with incorrect magic.

#### `version` (u16)
- MUST be `1` for ZCL1.
- Parsers MUST reject frames with unknown versions.

#### `op` (u16)
- Identifies the operation or event type.
- Operation codes are defined per-capability or per-protocol (e.g., `zi_ctl` ops, `async/default` ops).
- Op codes 1-999 are reserved for core zABI protocols.
- Op codes >=1000 are available for user-defined protocols.

#### `rid` (u32)
- Request identifier chosen by the caller.
- Used to correlate responses and events with requests.
- The caller SHOULD use unique `rid` values within a session to avoid ambiguity.
- The responder MUST echo the `rid` in all response/event frames for that request.

#### `status` (u32)
- For **requests**: MUST be `0`.
- For **responses**: `1` = success (ok), `0` = error.
- Parsers MAY treat other status values as errors.

#### `reserved` (u32)
- MUST be `0`.
- Reserved for future protocol extensions.

#### `payload_len` (u32)
- Length of the payload in bytes.
- MUST NOT exceed the buffer capacity.
- A payload_len of `0` is valid (empty payload).

#### `payload` (variable)
- Operation-specific payload bytes.
- Interpretation depends on `op` and `status`.

## Request/Response Patterns

### Synchronous Request-Response

1. Caller writes a request frame (status=0) to a handle.
2. Responder reads the request.
3. Responder writes a response frame (status=1 for ok, status=0 for error) with the same `rid`.
4. Caller reads the response.

### Asynchronous Event Streams

Some operations emit multiple events after the initial response:

1. Caller sends an INVOKE request.
2. Responder sends a response (status=1, op=INVOKE).
3. Responder may append additional event frames (op=ACK, FUTURE_OK, FUTURE_FAIL, etc.) with the same `rid`.
4. Caller drains all frames with matching `rid`.

### Error Response Payload

When `status=0`, the payload MUST be a packed error structure:

```
Offset | Size | Field
-------|------|-------------------------
0      | 4    | trace_len
4      | N    | trace (UTF-8, no NUL)
4+N    | 4    | msg_len
8+N    | M    | msg (UTF-8, no NUL)
8+N+M  | 4    | detail_len
12+N+M | K    | detail (UTF-8, no NUL)
```

- `trace`: origin identifier (grep-friendly short string).
- `msg`: human-readable error message.
- `detail`: optional extended detail (may be empty).

## Parsing Rules

A compliant parser MUST:
1. Validate magic bytes exactly match `"ZCL1"`.
2. Validate version == 1.
3. Validate reserved field == 0.
4. Validate payload_len does not exceed buffer bounds.
5. Reject frames that do not satisfy all validation rules.

A parser MAY:
- Impose a maximum frame size for resource protection.
- Reject frames with unknown `op` codes (operation-specific).

## Example: `zi_ctl` CAPS_LIST

Request (24 bytes, no payload):
```
Offset | Value      | Meaning
-------|------------|------------------
0-3    | "ZCL1"     | magic
4-5    | 0x0001     | version = 1
6-7    | 0x0001     | op = CAPS_LIST (1)
8-11   | 0x0000002A | rid = 42
12-15  | 0x00000000 | status = 0 (request)
16-19  | 0x00000000 | reserved
20-23  | 0x00000000 | payload_len = 0
```

Response (status=1, payload contains cap list):
```
Offset | Value      | Meaning
-------|------------|------------------
0-3    | "ZCL1"     | magic
4-5    | 0x0001     | version = 1
6-7    | 0x0001     | op = CAPS_LIST (1)
8-11   | 0x0000002A | rid = 42 (echoed)
12-15  | 0x00000001 | status = 1 (ok)
16-19  | 0x00000000 | reserved
20-23  | <len>      | payload_len
24+    | <data>     | packed cap list
```

## Capability-Specific Protocols

Each capability that uses ZCL1 MUST define:
- Its operation codes (`op` values).
- Payload structures for each operation.
- Event types (if asynchronous).
- Error codes and their meanings.

See:
- `ZI_CTL_PROTOCOL.md` for `zi_ctl` ops.
- `ASYNC_DEFAULT_PROTOCOL.md` for `async/default` ops.
- `HOPPER_PROTOCOL.md` for `proc/hopper` ops.

## Security Considerations

- Implementations MUST validate `payload_len` to prevent buffer overflows.
- Implementations SHOULD enforce maximum frame sizes to prevent resource exhaustion.
- Callers SHOULD use unique `rid` values to avoid correlation confusion.
- Responders MUST NOT trust `op` or `rid` for authorization decisions; those are routing metadata only.

## Versioning

This specification describes **ZCL1 version 1**.

Future versions:
- ZCL1 version 2+ MAY introduce new fields or semantics.
- The `version` field enables negotiation and backward compatibility.
- Parsers MUST reject unknown versions unless explicitly designed to support them.

## References

- zABI 2.5 core spec: `ABI_V2_5.md`
- `zi_ctl` implementation: `src/zingcore/2.5/zingcore/src/zi_syscalls_core25.c`
- ZCL1 helpers: `src/zingcore/2.5/zingcore/src/zi_zcl1.c`
