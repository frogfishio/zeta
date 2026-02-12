# SEM — SIR emulator toolchain (CLI)

`sem` is the “runs SIR” tool: the `zem`-equivalent for SIR.

It is a **thin CLI frontend** over `sircore`:

- reads programs from one or more inputs (JSONL today; CBOR later)
- builds a structured `sircore` module
- provides host capabilities via a **pure `zi_ctl` message ABI**
- renders diagnostics/events as text or JSONL

## Zero‑trust model

`sem` runs SIR in a sandboxed, capability‑based environment:

- default is “deny all” (no ambient filesystem/env/net)
- capabilities are explicitly enabled with CLI flags
- `sem` may snapshot host env/argv and provide them to the guest as capability-backed queries

`sircore` itself has no host access; it only issues `zi_ctl` requests.

## Responsibilities

### Frontend responsibilities (in `sem`)

- input format + file logistics (`*.sir.jsonl` stream mode, multi‑file tool mode)
- `src`/`src_ref` context lines and pretty diagnostics
- capability policy selection (what the guest is allowed to do)
- event sinks (trace, coverage, profiles, etc.)

### Execution responsibilities (in `sircore`)

- semantics, validation, deterministic execution
- emitting structured events for tooling

## ABI: `zi_ctl` (message based)

`sem` implements the host’s `zi_ctl` entrypoint and routes requests to:

- argv/env snapshots
- stdin/stdout/stderr handles
- optional file capabilities (when enabled)
- heap allocator (with debug options like poison/redzones)
- telemetry sink

`zi_ctl` uses ZCL1 framing; the operation is carried in the ZCL1 `op` field. See `src/sircore/zi_ctl.md`.

## Hosted zABI runtime (MVP)

`sem` is growing a **hosted zABI runtime** so you can trial a program under the emulator before lowering to a native binary.

### Guest syscalls: `zi_ctl` + `sys/loop` + `file/aio`

When running a SIR program under `sem --run`, the guest can call zABI primitives via `decl.fn` externs like:

- `zi_ctl` (ZCL1 message ABI)
- `zi_cap_open` (open a capability like `sys:loop` or `file:aio`)
- `zi_read`, `zi_write`, `zi_end`

These are **capability-gated**. Common gotchas:

- File I/O is provided via `file/aio` (golden cap). It is sandboxed by `--fs-root PATH`, which sets `ZI_FS_ROOT` for the hosted runtime.
- `env`/`argv` require explicit enablement via `--enable env` / `--enable argv` (or `--inherit-env`, `--params`, etc.).

Runnable samples ship in the dist bundle:

- `dist/test/sem/run/hello_zabi25_caps_list.sir.jsonl` (guest issues `zi_ctl CAPS_LIST` and prints the raw response)
- `sem --cat` (host-side helper that reads via `file/aio` + `sys/loop`)

Example commands (macOS bundle):

```sh
SEM=./dist/bin/macos/sem

$SEM --run ./dist/test/sem/run/hello_zabi25_caps_list.sir.jsonl

# Provide a sandbox directory that contains hello.txt.
$SEM --cat /hello.txt --fs-root ./dist/test/sem/run
```

The hosted runtime includes:

- guest memory mapping (`zi_ptr_t` is a guest pointer; never a host pointer)
- a handle table (`zi_read` / `zi_write` / `zi_end`)
- a minimal caps model with `sys/loop` + `file/aio` sandboxing (`--fs-root`)

Quick smoke test (read a file under a sandbox root):

```
sem --cat /a.txt --fs-root /path/to/sandbox
```

`sircore` VM smoke (prints using `zi_write` via the hosted runtime):

```
sem --sir-hello
```

`sircore` module smoke (structured module builder + interpreter):

```
sem --sir-module-hello
```

Run a small supported SIR subset from a `.sir.jsonl` file:

```
sem --run src/sircc/examples/hello_zabi25_write.sir.jsonl
```

Validate + lower (but do not execute) a `.sir.jsonl` file (useful for verifier-only fixtures like `ptr_layout.sir.jsonl`):

```
sem --verify src/sircc/examples/ptr_layout.sir.jsonl
```

Batch-verify a small suite (files and/or directories; directories are scanned for `*.sir.jsonl`):

```
sem --check src/sircc/examples/hello_zabi25_write.sir.jsonl src/sircc/examples/ptr_layout.sir.jsonl
```

The current `--run` MVP supports (growing over time):

For an up-to-date list, use:

- `sem --print-support`
- `sem --print-support --json`

We also keep a frozen “what runs today” doc for integrators:

- `src/sem/core_subset.md`
