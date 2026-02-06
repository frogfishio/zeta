# SIRCORE — SIR execution core (library)

`sircore` is the **zero‑trust** execution engine for SIR. It interprets an in‑memory SIR module and executes it deterministically.

`sircore` is a **library only**:

- no JSONL parsing (frontends own file formats)
- no filesystem/env/stdin/stdout access
- no ambient host authority
- no JIT (that belongs to `svm`)

All host interaction is performed via a single message‑ABI call: **`zi_ctl`**.

## Goals

- **Deterministic semantics**: same module + same inputs + same host responses ⇒ same outputs.
- **Zero trust**: guest code cannot access host resources unless the host grants capabilities.
- **Tooling ready**: a stable event stream allows tracing, coverage, fuzzing, and certs without forking the VM.
- **Format independence**: frontends may use JSONL today and CBOR tomorrow without changing `sircore`.

## Non‑goals (in `sircore`)

- high‑performance JIT or AOT
- GC / managed heap
- “smart” platform policies (those live in `sem`/`instrument`)
- direct linking to libc as an ABI

## Data model boundary (no JSONL inside)

`sircore` executes a **structured module** provided by the caller (e.g. `sem`):

- types, symbols, nodes, and optional source mapping
- resolved references (dense indices), validated invariants
- frozen, immutable execution graph

Frontends handle:

- parsing (`*.sir.jsonl`, CBOR, etc.)
- multi‑file logistics / stream merging
- user‑facing error formatting and context

## Host boundary: pure `zi_ctl`

`sircore` can only call out via `zi_ctl(req, resp)`:

- request/response are **binary** messages
- no raw host pointers are ever passed
- variable‑length data uses `(offset,len)` into the message buffer
- the guest must treat host responses as untrusted and validate them

This ABI is capability‑based: all IO is performed against opaque handles returned by capability open calls.

## Determinism + reproducibility

To keep execution reproducible across hosts:

- all nondeterminism (time, randomness, env, fs, scheduling) is modeled as `zi_ctl` selectors
- `sircore` never reads host env directly; the host may choose to snapshot it (`sem --inherit-env`)
- `sircore` defines canonical trap behavior for invalid operations (bounds/misalignment/etc.)

## Observability hooks (conceptual)

`sircore` should emit structured events to the embedding tool:

- step events (node executed, ids, tags)
- memory read/write events (logical, not host pointers)
- host call events (selector + sizes + rc)
- trap events (code + about)

`instrument` consumes these to implement tracing/coverage/fuzzing/etc.
