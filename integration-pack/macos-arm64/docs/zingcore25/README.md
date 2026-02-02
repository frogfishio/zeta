# zingcore2.5 (new runtime)

This folder is the start of the **zABI 2.5 reference runtime** we ship and evolve as a first-class product.

We are intentionally freezing the existing vendor snapshot under `src/zem/zingcore2.2_final/` as a reference/compatibility anchor.

## Goals


## Non-goals (for now)

## Design principles (“by the book”)
- **No constructor side effects** in product code paths unless explicitly intended and tested.
  - Cap/selector registration should be explicit and deterministic.

### Native “all caps” hostlib

For native-hosted programs (including objects produced by `lower`) that want a
permissive, C-like environment, use `zi_hostlib25_init_all()`.

- Wires zingcore25 for native pointers via `zi_mem_v1_native_init()`.
- Exposes stdio as handles `0`/`1`/`2` (stdin/stdout/stderr).
- Registers the currently implemented golden capabilities.

See `zingcore/include/zi_hostlib25.h`.
- **Enumerability**: capability/selector introspection should be first-class (no test-only hacks).
- **Errors are structured**: stable error codes + stable messages where possible.
- **Strictness**: invalid inputs are rejected consistently; no silent truncation.

## Core ABI (golden)

The zABI 2.5 **core wire ABI** is intentionally small and stable. The full contract is declared in
`zingcore/include/zi_sysabi25.h`.

Core calls (always present):

- `zi_abi_version`
- `zi_ctl`
- `zi_read`, `zi_write`, `zi_end`
- `zi_alloc`, `zi_free`
- `zi_telemetry`

There is no `zi_abi_features` in the core ABI: discovery/negotiation is done via `zi_ctl`.

### What the core calls mean

- `zi_ctl` is the **authoritative** control-plane mechanism. Discovery, extensibility, and
  structured replies happen here (ZCL1 framing).
- `zi_read` / `zi_write` / `zi_end` operate on **host-defined handles** (`zi_handle_t`). There is no
  baked-in notion of stdin/stdout/stderr; those are provided by the embedding program.
- `zi_telemetry` is a **best-effort sink**. If the host does not install a telemetry hook it is a
  noop.

### “Facade” reality (wiring model)

The `zi_*` syscall entrypoints are a thin dispatch layer:

- If the embedding installs host hooks via `zi_runtime25_set_host()`, syscalls forward to those
  hooks.
- Otherwise `zi_read/write/end` can use the internal handle table (see `zi_handles25.*`) if the host
  has allocated handles with per-handle ops.
- If neither is wired, the syscall returns `ZI_E_NOSYS`.

This design keeps the ABI stable while allowing very different embeddings (native process, WASM
host, test harnesses, etc.).

## Caps discovery (optional extension)

If a runtime exposes any capabilities, it must provide the caps extension (`zi_cap_*` and
`zi_handle_hflags`). Capability discovery is done via `zi_ctl` (CAPS_LIST) which returns a
deterministic list and per-cap flags/metadata.

## File capability (golden)

zingcore 2.5 includes a production-ready **file system capability**:

- kind: `"file"`
- name: `"fs"`
- version: `1`

It is opened via `zi_cap_open()` and returns a stream handle usable with `zi_read`/`zi_write`/`zi_end`.

### zi_cap_open request format

`zi_cap_open(req_ptr)` reads a packed little-endian request (40 bytes):

- `u64 kind_ptr`
- `u32 kind_len`
- `u64 name_ptr`
- `u32 name_len`
- `u32 mode` (reserved; must be 0)
- `u64 params_ptr`
- `u32 params_len`

### file/fs open params format

When kind/name select file/fs, `params_ptr` points at a packed little-endian params blob (20 bytes):

- `u64 path_ptr` (UTF-8 bytes, not NUL-terminated)
- `u32 path_len`
- `u32 oflags` (`ZI_FILE_O_*` in `zingcore/include/zi_file_fs25.h`)
- `u32 create_mode` (used when `ZI_FILE_O_CREATE` is set; e.g. 0644)

### Sandboxing via ZI_FS_ROOT

If the environment variable `ZI_FS_ROOT` is set:

- Guest paths must be absolute (start with `/`).
- Any `..` path segment is rejected.
- The runtime resolves the guest path *under* that directory using `openat()` and rejects symlinks in any path segment (escape-resistant).

## Argv capability (golden)

zingcore 2.5 exposes process arguments as a capability (not a core syscall):

- kind: `"proc"`
- name: `"argv"`
- version: `1`

Open semantics:

- `zi_cap_open` with kind/name matching `proc/argv` returns a **read-only** stream handle.
- Open params must be empty (`params_len=0`).

Stream format (packed little-endian):

- `u32 version` (currently 1)
- `u32 argc`
- Repeat `argc` times: `u32 len`, then `bytes[len]`

## Env capability (golden)

zingcore 2.5 exposes environment variables as a capability (not a core syscall):

- kind: `"proc"`
- name: `"env"`
- version: `1`

Open semantics:

- `zi_cap_open` with kind/name matching `proc/env` returns a **read-only** stream handle.
- Open params must be empty (`params_len=0`).

Stream format (packed little-endian):

- `u32 version` (currently 1)
- `u32 envc`
- Repeat `envc` times: `u32 len`, then `bytes[len]` (typically `KEY=VALUE`)

## Hopper capability (golden)

zingcore 2.5 exposes a minimal Hopper instance as a capability:

- kind: `"proc"`
- name: `"hopper"`
- version: `1`

Open semantics:

- `zi_cap_open` with kind/name matching `proc/hopper` returns a **read-write** handle.
- Open params are either empty (`params_len=0`, defaults) or a packed little-endian blob (12 bytes):
  - `u32 version` (must be 1)
  - `u32 arena_bytes`
  - `u32 ref_count`

I/O semantics:

- Requests are written as ZCL1 frames to the handle.
- Responses are read back as ZCL1 frames from the same handle.

Supported ops (ZCL1 `op`):

- `1` INFO (empty payload) → payload: `u32 hopper_abi_version, u32 default_layout_id, u32 arena_bytes, u32 ref_count`
- `3` RECORD (payload: `u32 layout_id`) → payload: `u32 hopper_err, i32 ref`
- `4` FIELD_SET_BYTES (payload: `i32 ref, u32 field_index, u32 len, bytes[len]`) → payload: `u32 hopper_err`
- `5` FIELD_GET_BYTES (payload: `i32 ref, u32 field_index`) → payload: `u32 hopper_err, u32 len, bytes[len]`
- `6` FIELD_SET_I32 (payload: `i32 ref, u32 field_index, i32 v`) → payload: `u32 hopper_err`
- `7` FIELD_GET_I32 (payload: `i32 ref, u32 field_index`) → payload: `u32 hopper_err, i32 v`

Note: this initial golden cap uses a small built-in catalog (layout_id=1) suitable for smoke/conformance tests.

## Async capability (golden)

zingcore 2.5 exposes a basic async invocation surface as a capability:

- kind: `"async"`
- name: `"default"`
- version: `1`

Open semantics:

- `zi_cap_open` with kind/name matching `async/default` returns a **read-write** handle.
- Open params must be empty (`params_len=0`).

I/O semantics:

- Requests are written as ZCL1 frames to the handle.
- The cap writes back responses and may append additional **event** frames to the same stream.

Supported request ops (ZCL1 `op`):

- `1` LIST (empty payload) → payload is a packed list of registered async selectors.
- `2` INVOKE → routes to a registered selector in the `zi_async` registry and emits ACK + future completion events.
- `3` CANCEL → cancels an in-flight future if the selector provides a cancel callback; emits `FUTURE_CANCEL` on success.

Built-in selectors (for smoke/conformance):

- `ping.v1` → ACK + `FUTURE_OK("pong")`
- `fail.v1` → ACK + `FUTURE_FAIL(code="demo.fail")`
- `hold.v1` → ACK only; must be cancelled via CANCEL

## TCP capability (golden)

zingcore 2.5 exposes basic TCP client connections as a capability:

- kind: `"net"`
- name: `"tcp"`
- version: `1`

Open semantics:

- `zi_cap_open` with kind/name matching `net/tcp` returns a **read-write** stream handle.
- Open params are a packed little-endian blob (20 bytes):
  - `u64 host_ptr` (UTF-8 bytes, not NUL-terminated)
  - `u32 host_len`
  - `u32 port` (1..65535)
  - `u32 flags` (reserved; must be 0)

Sandboxing via `ZI_NET_ALLOW`:

- If `ZI_NET_ALLOW` is unset/empty: only loopback hosts are allowed (`localhost`, `127.0.0.1`, `::1`).
- If `ZI_NET_ALLOW=any`: any host:port is allowed.
- Otherwise: a comma-separated allowlist of `host:port`, `host:*`, or `loopback`.

## Example: stdio + extra caps

See `zingcore/examples/stdio_caps_demo.c` for a concrete embedding that:

- Initializes `zingcore25`.
- Wires native memory mapping so `zi_ctl` can read/write request/response buffers.
- Registers three extra caps.
- Allocates three stream handles backed by POSIX fds (stdin/stdout/stderr) and then uses
  `zi_read`/`zi_write` on them.

## Explicit registration (no linker magic)

zingcore 2.5 intentionally avoids constructor-based or linker-section auto-registration.
The embedding program (host runtime glue) performs registration explicitly at startup:

1. Call `zingcore25_init()` (or `zi_caps_init()` + `zi_async_init()` if staying low-level).
2. Register capabilities via `zi_cap_register()`.
3. Register selectors via `zi_async_register()`.

This keeps startup deterministic and makes it obvious what the host exposes.

## Layout

- `zingcore/include/` — public headers (what embedders compile against)
- `zingcore/src/` — implementation modules (runtime, memory, streams, env, argv, telemetry)
- `zingcore/caps/` — capability implementations and selector modules
- `abi/` — ABI and capability specifications (human-readable)

## Migration plan (high-level)

1. Keep `zingcore2.2_final/` frozen.
2. Build `zingcore2.5/` runtime behind a separate build target.
3. Add/extend conformance tests that validate ABI behavior at the boundary.
4. Switch `zem`/`zrun` to link `zingcore2.5` when conformance is met.
