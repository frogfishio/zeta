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

`sem` implements the host’s `zi_ctl` entrypoint and routes selectors to:

- argv/env snapshots
- stdin/stdout/stderr handles
- optional file capabilities (when enabled)
- heap allocator (with debug options like poison/redzones)
- telemetry sink

The `zi_ctl` selector set should match the existing zABI naming (`zi_read`, `zi_write`, etc.), but encoded as selector ids.
