# zi_ctl Protocol Specification

**Version**: 1  
**Status**: Normative (zABI 2.5)

This document specifies the `zi_ctl` control-plane protocol for zABI 2.5.

## Overview

`zi_ctl` is the authoritative discovery and control-plane mechanism in zABI 2.5. It uses the ZCL1 framing protocol (see `ZCL1_PROTOCOL.md`).

## Function Signature

```c
int32_t zi_ctl(zi_ptr_t req_ptr, zi_size32_t req_len,
               zi_ptr_t res_ptr, zi_size32_t res_cap);
```

### Parameters

- `req_ptr`: Guest pointer to request frame (ZCL1, 24+ bytes).
- `req_len`: Length of request frame.
- `res_ptr`: Guest pointer to response buffer.
- `res_cap`: Capacity of response buffer in bytes.

### Return Value

- `>= 0`: Number of bytes written to response buffer.
- `< 0`: Error code (see `zi_sysabi25.h`).

### Errors

- `ZI_E_BOUNDS`: Invalid pointers or buffer too small.
- `ZI_E_INVALID`: Malformed request frame.
- `ZI_E_NOSYS`: Operation not supported.

## Operation Codes

All `zi_ctl` operations use ZCL1 frames with the following `op` codes:

| Op | Name | Description |
|----|------|-------------|
| 1  | CAPS_LIST | Enumerate registered capabilities |

Future versions may add additional ops (e.g., `CAPS_INFO`, `CTL_PING`).

## CAPS_LIST (op=1)

Lists all capabilities registered in the runtime.

### Request

- `op`: `1` (CAPS_LIST)
- `payload_len`: `0` (no payload)

### Response (status=1)

Payload structure (packed little-endian):

```
Offset | Size | Field
-------|------|------------------
0      | 4    | version (1)
4      | 4    | cap_count
8      | ...  | cap entries
```

Each cap entry (variable size):
```
Offset | Size | Field
-------|------|------------------
0      | 4    | kind_len
4      | N    | kind (UTF-8, no NUL)
4+N    | 4    | name_len
8+N    | M    | name (UTF-8, no NUL)
8+N+M  | 4    | flags
```

#### `flags` (u32 bitset)

- `0x01`: `ZI_CAP_CAN_OPEN` — Capability can be opened via `zi_cap_open`.
- `0x02`: `ZI_CAP_MAY_BLOCK` — Operations may block (I/O, network).

### Response (status=0)

Error payload (see `ZCL1_PROTOCOL.md` § Error Response Payload).

### Example

Request (rid=1):
```
ZCL1 frame:
  magic: "ZCL1"
  version: 1
  op: 1
  rid: 1
  status: 0
  reserved: 0
  payload_len: 0
```

Response (2 caps registered):
```
ZCL1 frame:
  magic: "ZCL1"
  version: 1
  op: 1
  rid: 1
  status: 1
  reserved: 0
  payload_len: <computed>

Payload:
  version: 1
  cap_count: 2
  
  cap[0]:
    kind_len: 4
    kind: "file"
    name_len: 2
    name: "fs"
    flags: 0x03 (CAN_OPEN | MAY_BLOCK)
  
  cap[1]:
    kind_len: 5
    kind: "async"
    name_len: 7
    name: "default"
    flags: 0x03 (CAN_OPEN | MAY_BLOCK)
```

## Error Handling

If the response buffer (`res_cap`) is too small to hold the response:
- The runtime MUST return `ZI_E_BOUNDS`.
- The caller SHOULD retry with a larger buffer.

If the request frame is malformed:
- The runtime MUST return `ZI_E_INVALID`.

If the requested operation is not supported:
- The runtime MUST return `ZI_E_NOSYS`.

## Determinism Guarantees

- Capability enumeration order MUST match registration order.
- Multiple calls to CAPS_LIST within a process lifetime MUST return identical results (unless capabilities are dynamically registered/unregistered, which is not supported in zABI 2.5 baseline).

## Security Considerations

- CAPS_LIST is read-only and does not require sandboxing.
- Future control-plane ops (e.g., dynamic cap registration) SHOULD be gated by capability flags.

## References

- ZCL1 framing: `ZCL1_PROTOCOL.md`
- Core ABI: `ABI_V2_5.md`
- Implementation: `src/zingcore/2.5/zingcore/src/zi_syscalls_core25.c`
- Tests: `src/zingcore/2.5/zingcore/tests/test_sysabi25_ctl_caps_list.c`
