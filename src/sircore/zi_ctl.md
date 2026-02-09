# `zi_ctl` message ABI for `sircore` (ZCL1 / zABI 2.5 aligned)

This document defines the **pure message ABI** used by `sircore` (and its frontends like `sem` / `instrument`) to call the host.

We intentionally align with the existing zABI 2.5 control-plane:

- framing is **ZCL1** (`magic="ZCL1"`, header size 24 bytes)
- the syscall name is **`zi_ctl`**
- core error codes are `ZI_E_*` (negative `int32_t`)

The authoritative wire contract lives in:

- `ext/zingcore_readonly/2.5/abi/ZCL1_PROTOCOL.md`
- `ext/zingcore_readonly/2.5/abi/ZI_CTL_PROTOCOL.md`
- `ext/zingcore_readonly/2.5/zingcore/include/zi_sysabi25.h`

## 1. Function signature(s)

### 1.1 zABI syscall form (guest pointers)

zABI 2.5 defines:

```c
int32_t zi_ctl(zi_ptr_t req_ptr, zi_size32_t req_len,
               zi_ptr_t resp_ptr, zi_size32_t resp_cap);
```

Return value:

- `>= 0`: number of bytes written into `resp_ptr`
- `< 0`: `ZI_E_*` error code (transport-level failure; no response frame produced)

### 1.2 In-process embedding form (host pointers)

When `sircore` is embedded as a native library, the embedding tool typically provides a callback with the same semantics but using host pointers:

```c
typedef int32_t (*sir_zi_ctl_fn)(
  void* user,
  const uint8_t* req, uint32_t req_len,
  uint8_t* resp, uint32_t resp_cap
);
```

The **wire framing is identical**; only the pointer interpretation differs.

## 2. Wire framing: ZCL1 (normative)

All integers are **little-endian**.

Header layout (24 bytes) + payload:

```
Offset | Size | Field       | Meaning
-------|------|-------------|------------------------------------
0      | 4    | magic       | ASCII "ZCL1"
4      | 2    | version     | MUST be 1
6      | 2    | op          | operation code (u16)
8      | 4    | rid         | request id (u32 correlation id)
12     | 4    | status      | request: 0; response: 1 ok / 0 err
16     | 4    | reserved    | MUST be 0
20     | 4    | payload_len | payload byte length
24     | N    | payload     | op-specific bytes
```

Validation rules (`sircore` and tools MUST enforce):

- `magic == "ZCL1"`
- `version == 1`
- `reserved == 0`
- `24 + payload_len <= req_len` (requests) / `<= resp_bytes_written` (responses)

## 3. Error responses (status=0 payload)

When a response frame has `status=0`, the payload is a packed UTF‑8 triple (no NUL terminators):

```
u32 trace_len;  u8 trace[trace_len];
u32 msg_len;    u8 msg[msg_len];
u32 detail_len; u8 detail[detail_len];
```

Tools should treat these as:

- `trace`: stable origin identifier (grep-friendly)
- `msg`: one-line human message
- `detail`: optional extended detail (may be empty)

## 4. Operation codes (op registry)

ZCL1 reserves:

- ops `1..999` for core zABI protocols
- ops `>= 1000` for tool / user-defined protocols

zABI 2.5 currently defines these `zi_ctl` ops (see `zi_sysabi25.h`):

- `ZI_CTL_OP_CAPS_LIST = 1`
- `ZI_CTL_OP_CAPS_DESCRIBE = 2` (reserved; not yet normatively specified here)
- `ZI_CTL_OP_CAPS_OPEN = 3` (reserved; not yet normatively specified here)

### 4.1 `CAPS_LIST` (op=1)

Request:

- `status=0`
- `payload_len=0`

Success response (`status=1`) payload (packed LE):

```
u32 version;    // MUST be 1
u32 cap_count;
cap_entry[cap_count];
```

Each `cap_entry`:

```
u32 kind_len; u8 kind[kind_len];   // UTF-8 (e.g. "file", "async")
u32 name_len; u8 name[name_len];   // UTF-8 (e.g. "fs", "default")
u32 flags;                           // ZI_CAP_* bitset
```

`flags` values (from `zi_sysabi25.h`):

- `ZI_CAP_CAN_OPEN`  (bit 0): can be opened
- `ZI_CAP_PURE`      (bit 1): deterministic/pure service
- `ZI_CAP_MAY_BLOCK` (bit 2): may block

Error response (`status=0`): ZCL1 error payload (see §3).

## 5. How `sircore` uses `zi_ctl`

`sircore` treats `zi_ctl` as the *only* host boundary it needs:

- it emits requests as ZCL1 frames
- it validates responses as untrusted bytes
- it never assumes any capability exists unless the host provides it

Integration choices (frontends decide, `sircore` stays policy-free):

1) **Native zABI mode**: use `CAPS_LIST` / `CAPS_OPEN` to obtain handles and then interact using whatever protocols the host provides.
2) **Tool-defined mode**: reserve `op >= 1000` for a “sem host protocol” (argv/env/fs/stdio) when running purely under `sem` without binding to a specific zABI runtime.

`sircore` is compatible with either, as long as the embedding layer provides deterministic behavior and stable diagnostics.

## 6. Tool-defined SEM host protocol (op >= 1000)

When running under `sem`, the host may expose additional deterministic “snapshotted” data via `zi_ctl` ops `>= 1000`.

Notes:

- These ops are **deny-by-default**. If not enabled, the host returns `status=0` with a ZCL1 error payload (see §3) and a `trace` like `sem.zi_ctl.denied`.
- Strings are UTF-8 **bytes** (no implicit NUL terminators).
- All integers are little-endian.

### 6.1 `SEM_ARGV_COUNT` (op=1000)

Request: empty payload.

Success response payload:

```
u32 count;
```

### 6.2 `SEM_ARGV_GET` (op=1001)

Request payload:

```
u32 index;
```

Success response payload:

```
u32 len;
u8  bytes[len];
```

### 6.3 `SEM_ENV_COUNT` (op=1002)

Request: empty payload.

Success response payload:

```
u32 count;
```

### 6.4 `SEM_ENV_GET` (op=1003)

Request payload:

```
u32 index;
```

Success response payload:

```
u32 key_len;
u8  key[key_len];
u32 val_len;
u8  val[val_len];
```

