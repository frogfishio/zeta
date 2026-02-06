# zi_ctl message ABI (conceptual spec, v0)

This document defines the **pure message ABI** used by `sircore` to call the host.

All host interaction is performed via a single entrypoint:

```
zi_ctl(req_bytes, req_len, resp_bytes, resp_cap) -> n_or_err
```

Where:

- request and response are **binary** messages
- guest never passes raw host pointers
- variable-length data uses `(offset,len)` within the message buffer
- both directions are treated as untrusted and validated

The selector set is intended to align with existing zABI naming (`zi_*`), but encoded as selector ids.

## 1. Design goals

- **Zero trust**: guest cannot access anything without explicit host capability.
- **Forward compatible**: selectors can be added without breaking old guests.
- **Deterministic**: host nondeterminism is explicit and can be snapshotted/replayed by tooling.
- **Language neutral**: request/response structs are stable across C, Rust, Go, etc.

## 2. Message framing

Every request begins with a fixed-size header followed by selector-specific payload.

### 2.1. Common integer sizes / endianness

- All message integers are **little-endian**.
- All offsets/lengths are `u32` unless otherwise noted.
- Handles are `u64`.

Rationale: stable wire format independent of target endianness; conversions are explicit.

### 2.2. Request header (`zi_req_hdr_v0`)

Conceptual fields:

- `u32 abi_magic`  (e.g. `'ZICT'`)
- `u16 abi_major` / `u16 abi_minor`
- `u32 selector`   (operation id)
- `u32 flags`      (selector-generic; must be 0 for now)
- `u64 corr_id`    (caller-chosen correlation id; may be 0)
- `u32 payload_len`
- `u32 reserved`   (must be 0)

The full request size is `sizeof(hdr) + payload_len`.

### 2.3. Response header (`zi_resp_hdr_v0`)

Conceptual fields:

- `u32 abi_magic`
- `u16 abi_major` / `u16 abi_minor`
- `i32 rc`         (0 ok, negative errno-like)
- `u64 corr_id`    (echoed)
- `u32 payload_len`
- `u32 reserved`

The return value of `zi_ctl` is:

- `>= 0`: number of response bytes written (`sizeof(hdr)+payload_len`)
- `< 0`: transport error (host could not produce a response)

`sircore` treats **any** malformed framing as an immediate deterministic trap (or VM error, depending on mode).

## 3. Memory model: spans and offsets

Selectors refer to byte regions via `span` descriptors:

- `u32 off` (offset into message buffer)
- `u32 len` (length in bytes)

Validation rule:

- `off+len` must be within the message size/capacity and must not overflow.

Strings are UTF-8 byte sequences. Null terminators are not required unless the selector says so.

## 4. Capability model

All IO and privileged operations are performed against **opaque handles** (`u64`).

How handles are obtained:

1) list caps (host policy decides what exists)
2) open cap by key/name/descriptor
3) use resulting handle with typed selectors (read/write/etc.)

`sircore` never fabricates handles; handle `0` is reserved as “invalid”.

## 5. MVP selector set (conceptual)

The initial set is intentionally small and mirrors the existing zABI surface:

### 5.1 ABI version

- `ZI_ABI_VERSION`: returns `{u32 major,u32 minor,u32 patch}`

### 5.2 Capabilities

- `ZI_CAP_COUNT`: returns `u32 n`
- `ZI_CAP_GET_SIZE(i)`: returns `u32 need`
- `ZI_CAP_GET(i, out_span)`: writes a capability descriptor
- `ZI_CAP_OPEN(req_span)`: returns `u64 handle_or_err`

Capability descriptors are host-defined but must be treated as structured bytes (not pointers).

### 5.3 Handle flags (introspection)

- `ZI_HANDLE_HFLAGS(handle)`: returns `u64 flags_or_0`

### 5.4 I/O

- `ZI_READ(handle, dst_span)`: returns `i64 n_or_err`
- `ZI_WRITE(handle, src_span)`: returns `i64 n_or_err`
- `ZI_END(handle)`: returns `i32 rc`

### 5.5 Process argv/env (snapshotted)

- `ZI_ARGC()`: returns `i32 argc`
- `ZI_ARGV_LEN(i)`: returns `i32 len_or_err`
- `ZI_ARGV_COPY(i, out_span)`: returns `i32 written_or_err`
- `ZI_ENV_GET_LEN(key_span)`: returns `i32 len_or_err`
- `ZI_ENV_GET_COPY(key_span, out_span)`: returns `i32 written_or_err`

### 5.6 Heap / arenas

For debug tooling, it is useful for alloc/free to go through host selectors:

- `ZI_ALLOC(size, align?)`: returns `u64 guest_ptr_or_err`
- `ZI_FREE(ptr)`: returns `i32 rc`

Note: “guest_ptr” is a **logical address in the guest address space**, not a host pointer.

### 5.7 Telemetry (optional)

- `ZI_TELEMETRY(topic_span, msg_span)`: returns `i32 rc`

## 6. Determinism knobs (host policy)

Tools like `instrument` may wrap `zi_ctl` to implement:

- short reads (chunking)
- env snapshots / clear env
- deterministic RNG
- redzones/poison/quarantine for guest heap
- record/replay of host interactions

These must be visible in host behavior and should never be hidden inside `sircore`.

## 7. Versioning rules

- `sircore` requires `abi_major` match.
- `abi_minor` may increase; unknown selectors must return a structured “not supported” error (`rc = -ENOSYS`).
- selectors must remain backward compatible within a major version.

## 8. Open questions (to resolve when making v1)

- exact numeric selector ids (registry)
- exact error code set (errno-like vs custom)
- whether spans may reference request buffer only, response buffer only, or both
- handle namespace and standard handle ids for stdin/stdout/stderr

