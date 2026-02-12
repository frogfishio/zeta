# sys/info@v1 — ZCL1 Protocol

This document is the normative wire protocol for the capability `sys/info@v1`.

It provides:
1. **System info** (static-ish): OS/arch/cpu count/page size, and best-effort identifiers.
2. **System stats** (dynamic): load averages and memory availability/pressure (best-effort).
3. **Time and entropy**: wall-clock `now()` and a random seed for guest RNG initialization.

The key words MUST, MUST NOT, SHOULD, and MAY are to be interpreted as described in RFC 2119.

## Identity

- Kind: `sys`
- Name: `info`
- Version: `1`

Open parameters: none.

## Transport

The handle is a ZCL1 request/response stream.

- Guest sends ZCL1 request frames via `zi_write(handle, ...)`.
- Host returns ZCL1 response frames readable via `zi_read(handle, ...)`.

ZCL1 frames:
- `version` MUST be `1`.
- Responses use ZCL1 `status` field:
  - `status=1` for OK responses.
  - `status=0` for error responses (with ZCL1 error payload).

## Operations

All integers are little-endian.

### INFO (op=1)

Request payload: empty.

OK response payload:

- `u32 version` (MUST be `1`)
- `u32 flags` (bitset; see below)
- `u32 cpu_count` (online logical CPUs; MUST be >= 1 when available)
- `u32 page_size` (bytes; SHOULD be >= 1)
- `u32 os_len` + `bytes[os_len] os` (UTF-8)
- `u32 arch_len` + `bytes[arch_len] arch` (UTF-8)
- `u32 model_len` + `bytes[model_len] model` (UTF-8; best-effort hardware model)
- `u32 hostname_len` + `bytes[hostname_len] hostname` (UTF-8; MAY be empty for privacy)

`flags` bits:
- `0x1` => `os` present
- `0x2` => `arch` present
- `0x4` => `model` present
- `0x8` => `hostname` present

Notes:
- Implementations MAY redact or omit strings by setting their lengths to `0` and clearing the corresponding flag.

### STATS (op=2)

Request payload: empty.

OK response payload:

- `u32 version` (MUST be `1`)
- `u32 flags` (bitset; see below)
- `u64 realtime_ns` (UNIX epoch time in nanoseconds; best-effort)
- If `flags & 0x1` (load averages present):
  - `u32 load1_milli`
  - `u32 load5_milli`
  - `u32 load15_milli`
  (each is load average * 1000)
- If `flags & 0x2` (memory present):
  - `u64 mem_total_bytes`
  - `u64 mem_avail_bytes`
  - `u32 mem_pressure_milli` (0..1000; 0=low pressure; 1000=high pressure)

`flags` bits:
- `0x1` => load averages present
- `0x2` => memory stats present

### TIME_NOW (op=3)

Request payload: empty.

OK response payload:

- `u32 version` (MUST be `1`)
- `u64 realtime_ns` (UNIX epoch time in nanoseconds)
- `u64 monotonic_ns` (monotonic time in nanoseconds since an unspecified point)

### RANDOM_SEED (op=4)

Request payload: empty.

OK response payload:

- `u32 version` (MUST be `1`)
- `u32 seed_len`
- `bytes[seed_len] seed`

Requirements:
- `seed_len` SHOULD be `32`.
- `seed` SHOULD be generated from the host’s best available entropy source.

## Errors

For malformed requests, unknown ops, or internal failures, implementations MUST return a ZCL1 error frame via `zi_zcl1_write_error(...)`.

