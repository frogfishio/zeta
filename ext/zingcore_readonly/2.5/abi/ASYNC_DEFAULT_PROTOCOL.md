# async/default Capability Protocol Specification

**Capability**: `async/default` version 1  
**Status**: Golden (zABI 2.5)

This document specifies the wire protocol for the `async/default` capability.

## Overview

The `async/default` capability provides asynchronous invocation of registered selectors with future-based completion tracking. It uses ZCL1 framing over a bidirectional handle.

## Opening the Capability

```c
zi_handle_t zi_cap_open(zi_ptr_t req_ptr);
```

Open request structure (40 bytes, little-endian):
```
Offset | Size | Field
-------|------|------------------
0      | 8    | kind_ptr (→ "async")
8      | 4    | kind_len (5)
12     | 8    | name_ptr (→ "default")
20     | 4    | name_len (7)
24     | 4    | mode (must be 0)
28     | 8    | params_ptr
36     | 4    | params_len
```

### Open Parameters

- `params_len` MUST be `0` (no parameters).
- `params_ptr` is ignored.

### Return Value

- `>= 3`: Valid handle (read-write, endable).
- `< 0`: Error code (e.g., `ZI_E_INVALID`, `ZI_E_NOENT`).

## I/O Model

Once opened:
- **Write** ZCL1 request frames to the handle via `zi_write`.
- **Read** ZCL1 response/event frames from the handle via `zi_read`.
- **Close** the handle via `zi_end` when done.

### Backpressure

- `zi_write` returns `ZI_E_AGAIN` if a response is pending (one request at a time).
- `zi_read` returns `ZI_E_AGAIN` if no frames are available.

## Threading & Scheduling Model (Non-Normative)

The `async/default` capability defines **observable** asynchronous behavior (futures + event stream), but it does **not** define a specific threading model.

In particular, `async/default` does **not** guarantee:
- fibers or green threads
- OS threads or parallel execution
- preemption, fairness, or time slicing

An implementation MAY execute selector work:
- inline (during `zi_write` handling)
- on a host thread pool
- on a cooperative scheduler (fibers)
- via an external event loop

Guest programs MUST treat selector completion as asynchronous and be prepared to observe events on subsequent reads.

### Ordering Guarantees (Normative)

For a single opened `async/default` handle:
- Responses/events are emitted as **ZCL1 frames** read from the same handle.
- For an `INVOKE` that succeeds (`error_code == OK`), the runtime MUST emit either:
  - `ACK` followed by exactly one of `FUTURE_OK` / `FUTURE_FAIL` / `FUTURE_CANCEL`, or
  - `FAIL` (synchronous failure) with no future completion event.
- For an `INVOKE` request, events related to that invocation MUST use the same `rid` as the `INVOKE`.

### Backpressure & Concurrency (Normative)

This v1 protocol is deliberately single-flight per handle:
- The runtime MAY return `ZI_E_AGAIN` from `zi_write` while there are unread frames queued for that handle.
- Guests SHOULD follow the pattern: `zi_write(request)` → `zi_read(all frames)` → repeat.

## Operation Codes

| Op | Name | Type | Description |
|----|------|------|-------------|
| 1  | LIST | Request | Enumerate registered selectors |
| 2  | INVOKE | Request | Invoke a selector |
| 3  | CANCEL | Request | Cancel an in-flight future |
| 100 | ACK | Event | Selector acknowledged future |
| 101 | FAIL | Event | Selector failed synchronously |
| 102 | FUTURE_OK | Event | Future completed successfully |
| 103 | FUTURE_FAIL | Event | Future failed |
| 104 | FUTURE_CANCEL | Event | Future was cancelled |

## Error Codes

Used in response payloads (u32):

| Code | Name | Meaning |
|------|------|---------|
| 0    | OK | Success |
| 1    | NOENT | Selector/future not found |
| 2    | INVALID | Invalid request parameters |
| 3    | OOM | Out of memory |
| 4    | INTERNAL | Internal error |
| 5    | UNSUPPORTED | Operation not supported by selector |

## LIST (op=1)

Enumerate all registered async selectors.

### Request

- `payload_len`: `0`

### Response (status=1)

Payload structure (packed little-endian):

```
Offset | Size | Field
-------|------|------------------
0      | 4    | version (1)
4      | 4    | selector_count
8      | ...  | selector entries
```

Each selector entry:
```
Offset | Size | Field
-------|------|------------------
0      | 4    | kind_len
4      | N    | kind (UTF-8)
4+N    | 4    | name_len
8+N    | M    | name (UTF-8)
8+N+M  | 4    | selector_len
12+N+M | K    | selector (UTF-8)
```

### Example

Request:
```
ZCL1 frame (op=1, rid=10, status=0, payload_len=0)
```

Response:
```
ZCL1 frame (op=1, rid=10, status=1, payload=...)

Payload:
  version: 1
  selector_count: 3
  
  [0]:
    kind: "async"
    name: "default"
    selector: "ping.v1"
  
  [1]:
    kind: "async"
    name: "default"
    selector: "fail.v1"
  
  [2]:
    kind: "async"
    name: "default"
    selector: "hold.v1"
```

## INVOKE (op=2)

Invoke a registered selector.

### Request

Payload structure:
```
Offset | Size | Field
-------|------|------------------
0      | 4    | kind_len
4      | N    | kind (UTF-8)
4+N    | 4    | name_len
8+N    | M    | name (UTF-8)
8+N+M  | 4    | selector_len
12+N+M | K    | selector (UTF-8)
12+N+M+K | 8  | future_id (u64)
20+N+M+K | 4  | params_len
24+N+M+K | P  | params (selector-specific)
```

- `future_id` MUST be unique and non-zero within this handle's lifetime.
- `params`: Selector-specific payload (may be empty).

### Response (status=1)

Payload:
```
Offset | Size | Field
-------|------|------------------
0      | 4    | error_code (0=OK)
```

If `error_code == 0`, the selector was invoked successfully. Events will follow.

### Event Stream

After a successful INVOKE response, the handle emits:

1. **ACK** (op=100) — Selector acknowledged the request:
   ```
   Payload (8 bytes):
     future_id: u64
   ```

2. **FUTURE_OK** (op=102) — Future completed successfully:
   ```
   Payload:
     future_id: u64
     value_len: u32
     value: bytes[value_len]
   ```

3. **FUTURE_FAIL** (op=103) — Future failed:
   ```
   Payload:
     future_id: u64
     code_len: u32
     code: UTF-8[code_len]
     msg_len: u32
     msg: UTF-8[msg_len]
   ```

4. **FUTURE_CANCEL** (op=104) — Future was cancelled:
   ```
   Payload (8 bytes):
     future_id: u64
   ```

All events use the same `rid` as the INVOKE request.

### Built-in Selectors

#### `ping.v1`

- Params: empty
- Behavior: Emits ACK + FUTURE_OK("pong")

#### `fail.v1`

- Params: empty
- Behavior: Emits ACK + FUTURE_FAIL(code="demo.fail")

#### `hold.v1`

- Params: empty
- Behavior: Emits ACK only; caller must CANCEL to complete.

## CANCEL (op=3)

Cancel an in-flight future.

### Request

Payload:
```
Offset | Size | Field
-------|------|------------------
0      | 8    | future_id
```

### Response (status=1)

Payload:
```
Offset | Size | Field
-------|------|------------------
0      | 4    | error_code
```

Error codes:
- `0` (OK): Future cancelled; FUTURE_CANCEL event follows.
- `1` (NOENT): Future not found (already completed or invalid).
- `5` (UNSUPPORTED): Selector does not support cancellation.

### Event

If successful, emits:
```
ZCL1 frame (op=104, rid=<cancel_rid>, status=1)

Payload (8 bytes):
  future_id: u64
```

## Error Handling

### Selector Not Found (INVOKE)

Response:
```
ZCL1 frame (op=2, rid=..., status=1)

Payload:
  error_code: 1 (NOENT)
```

Additionally, a FAIL event is emitted:
```
ZCL1 frame (op=101, rid=..., status=1)

Payload:
  future_id: 0
  code: "t_async_noent"
  msg: "selector not found"
```

### Duplicate Future ID

If `future_id` is already in use:
```
Response payload:
  error_code: 2 (INVALID)

FAIL event:
  code: "t_async_dup_future"
  msg: "duplicate/invalid future id"
```

## Determinism Guarantees

- Selector enumeration order MUST match registration order.
- Multiple LIST calls MUST return identical results within a process lifetime.
- Event order for a given `rid` is deterministic: INVOKE response, then ACK/FAIL, then completion event.

## Security Considerations

- Future IDs are scoped per-handle; different handles may reuse the same future_id safely.
- Selectors MUST validate their params; the cap does not enforce selector-specific validation.
- Malicious callers can exhaust the future table (max 64 per handle); this is by design for resource control.

## References

- ZCL1 framing: `ZCL1_PROTOCOL.md`
- Selector registration: `ABI_V2_5.md` § Async selector registry semantics
- Implementation: `src/zingcore/2.5/zingcore/src/zi_async_default25.c`
- Tests: `src/zingcore/2.5/zingcore/tests/test_sysabi25_async_default_cap.c`
