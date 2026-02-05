# sircc (SIR v1.0) — Integrator Release Notes (MIR stage)

These notes describe the current “integrator-ready” surface of `sircc` for MIR producers.

## Status (what’s ready)

- **Milestone 3 (base/core mnemonics) is complete** for the **node-frontend → LLVM backend** path (implemented + tested).
- `sircc --print-support --format json` reports `"milestone3":{"missing":0}` on the shipped build.
- `--diagnostics json` emits structured diagnostics with stable `"code"` strings and `about:{k,id,tag}` context for node-related failures (including CFG/effect lowering).

## What you ship to alpha users

The `dist/` bundle is intended to be copy/pasteable:

- `dist/bin/<os>/sircc`
- `dist/doc/sircc.md`
- `dist/test/examples/*.sir.jsonl`
- Run the normative smoke suite: `dist/bin/<os>/sircc --check [--format text|json]`

## Build / bundle (repo root)

- Configure: `cmake -S . -B build`
- Build: `cmake --build build -j`
- Bundle: `cmake --build build --target dist`

## Primary integration path: emit SIR node streams

### File format

- Input is JSONL: `*.sir.jsonl` (one JSON record per line).
- Records are tagged with `"ir":"sir-v1.0"`.

### IDs

- `src/sym/type/node` ids may be **integers or strings** (sircc interns them into dense internal ids).
- Recommendation for producers: keep ids stable and readable; strings are fine (e.g. `"node:main:entry"`).

### Ordering guidance

- Prefer **defs before uses**.
- Forward-ref / order-independence is not guaranteed yet; keep streams mostly top-down to avoid surprises.

### Feature gates (required for gated packs)

When you emit feature-gated mnemonics, you must enable them via `meta.ext.features`, e.g.:

- `["agg:v1","fun:v1","closure:v1","adt:v1","sem:v1"]`

Packs explicitly **not** in scope right now (parked):

- `atomics:v1`, `simd:v1`, `coro:v1`, `eh:v1`, `gc:v1`

## C interop

### Calling external C (C ABI)

Pattern: `decl.fn` + `call.indirect`.

Examples in the bundle:

- `dist/test/examples/hello_world_puts.sir.jsonl` (libc `puts`)
- `dist/test/examples/hello_zabi25_write.sir.jsonl` (zABI25 `zi_write`)

### Exporting a SIR function to C

Emit a `fn` node with:

- `fields.name` = the exported C symbol name
- `fields.linkage:"public"` (defaults to external when set; use `"local"` for internal-only)

Consumers can then link against the produced object/binary using that symbol name.

## Recommended tool modes for integrators

### Validation and debugging

- Parse + validate only: `sircc --verify-only <file.sir.jsonl>`
- Parse trace: `sircc --dump-records --verify-only <file.sir.jsonl>`
- Structured diagnostics: `sircc --diagnostics json --diag-context N`

### “What does sircc support?”

- `sircc --print-support [--format text|json] [--full]`

### Target/determinism knobs (recommended for reproducible output)

- Print target: `sircc --print-target [--target-triple <triple>]`
- Determinism mode: `sircc --deterministic`
- Enforce pinned triple: `sircc --require-pinned-triple`
- Enforce explicit target contract: `sircc --require-target-contract`

## Notes / caveats

- The current “integrator-ready” surface is the **node** representation compiled via LLVM. The `instr/label/dir` mnemonic frontend is not the primary integration path yet.
- If you need a stable, machine-readable view of what’s missing (by pack), rely on `--print-support --format json`.

