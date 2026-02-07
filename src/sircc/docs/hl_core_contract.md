# SIR-HL → SIR-Core: ownership, contracts, and plan

This document defines how we “take load off MIR” without turning `sircc` into a bespoke compiler for every source language.

The core idea is a **two-level SIR contract**:

- **SIR-Core**: a small, stable, executable subset. This is the hard boundary for codegen correctness and long-term stability.
- **SIR-HL (packs / intent)**: more expressive, but **deterministically lowered** into SIR-Core by a shared `sircc` legalizer pipeline.

If you meet the SIR-Core contract, `sircc` is “just a compiler”. If you emit SIR-HL, `sircc` becomes your shared lowering engine.

## Ownership model

### Frontend / generator (language side) owns

These are **language semantics** and must not be pushed into SIR:

- parsing, scoping, name resolution
- type checking / inference
- language-specific evaluation rules (unless encoded explicitly as SIR-HL intent)
- surface sugar decisions (e.g. Oberon designators, report-spec corner cases)

### `sircc` (SIR toolchain) owns

These are **portable compilation problems** that every language hits:

- lowering of *portable intent* (`sem:v1`) into CFG + core ops
- closure conversion *when encoded as portable closure pack* (`closure:v1`)
- ADT layout and tag/payload operations *when encoded as portable sum pack* (`adt:v1`)
- aggregate layout + initializers (`type.kind:"struct"`, `const.*`, globals) under a pinned target contract
- verification, diagnostics, and reproducible codegen contracts

### What “SIR takes care of” means

SIR can only “take care of it” if:

1. the construct is **language-agnostic**, and
2. the lowering is **fully specified and deterministic**, and
3. the lowering results in **SIR-Core only**.

If any of those is false, the construct stays frontend-owned.

## The contracts

### SIR-Core contract (stable)

SIR-Core is the smallest set that:

- is **fully verified** by `sircc --verify-only`
- is **fully codegenned** by `sircc` (LLVM backend)
- is expected to be **stable across languages** and long-lived

Practically: “Stage 3 base v1.0” + a pinned target contract + the interop rules (extern import/export).

The exact support set is surfaced by:

- `sircc --print-support --format json` (authoritative)

### SIR-HL contract (expressive, but not directly codegen)

SIR-HL is anything beyond SIR-Core that:

- is gated by `meta.ext.features` (packs)
- **must** be accepted by the verifier only when the feature gate is enabled
- **must** be lowered by `sircc` into SIR-Core before codegen

SIR-HL comes in two shapes:

1) **Packs**: `agg:v1`, `fun:v1`, `closure:v1`, `adt:v1`, …
2) **Intent**: `sem:v1` (`sem.if`, `sem.and_sc`, `sem.or_sc`, `sem.match_sum`, …)

## Interchange + versioning rules

### Inputs

- Primary interchange is `sir-v1.0` JSONL.
- Producers may use int or string ids; string ids are recommended for injected/derived nodes.

### Outputs (lowered)

We add a `sircc` lowering stage that produces a **lowered SIR-Core stream**:

- `sircc --lower-hl --emit-sir-core out.sir.jsonl in.sir.jsonl`
- output is still `sir-v1.0`, but guaranteed to satisfy the SIR-Core contract

This output becomes:

- canonical for debugging (“what did your HL mean?”)
- the unit of testing for optimizer/legalizer changes

Note: the current MVP implementation only lowers the “pure/val” cases of `sem:v1` (see P1), and will reject thunk-based `sem:*` nodes with a diagnostic.
Note: the current implementation supports thunk-based `sem:v1` lowering (CFG desugaring) and will also hoist nested `sem.*` used in expression positions into `let` bindings during lowering, so `sem.*` can appear under other expression nodes.

### Backward compatibility

- SIR-Core is backward compatible within a major version (v1.x).
- Packs/intent are feature-gated; adding a new pack does not affect old producers.
- Any change to a lowering rule is treated like an ABI change unless it is provably semantics-preserving under the Core contract.

## Diagnostics contract

When `sircc` rejects a SIR-HL construct, diagnostics must:

- name the **pack/feature gate** (e.g. “requires sem:v1”)
- explain the **required Core shape** (e.g. “extern calls must use decl.fn + call.indirect”)
- provide context lines when available (`--diag-context`)

## Why this helps MIR (what disappears)

If MIR emits SIR-HL instead of inventing its own “almost-Oberon” semantics:

- MIR no longer needs bespoke CFG expansion for if/and/or/match (`sem:v1` does it)
- MIR no longer needs ad-hoc closure layouts (`closure:v1` does it)
- MIR no longer needs bespoke sum/variant layouts (`adt:v1` does it)
- MIR can stop duplicating aggregate initializer logic (`agg:v1` does it)

MIR still owns language semantics, but it stops owning the “portable lowering grind”.

## Implementation plan (grouped, prioritized)

### P0 — Make the contract real (tooling surface)

- [ ] Add a legalizer entrypoint:
  - [ ] `sircc --lower-hl` (runs packs/intent lowering only; no LLVM)
  - [ ] `--emit-sir-core <path>` (writes lowered JSONL)
  - [ ] `--lower-only` synonym for “no codegen”
- [ ] Ensure `--print-support` can report:
  - [ ] Core mnemonics
  - [ ] Supported packs/intent lowering
  - [ ] Unsupported-but-known mnemonics (so integrators can plan)
- [ ] Add `sircc --check` mode to optionally run:
  - [ ] `--check verify` (verify-only)
  - [ ] `--check lower` (verify + lower to core)
  - [ ] `--check build` (verify + lower + compile)

### P1 — `sem:v1` intent lowering (huge DX win)

- [x] MVP: lower the pure/val cases into Core expressions
  - [x] `sem.if` with `val/val` branches → `select`
  - [x] `sem.and_sc` / `sem.or_sc` with `rhs kind=val` → `bool.and` / `bool.or`
- [x] Full: implement `sem.if` lowering → CFG form (blocks/terms) + validate (required for thunk branches)
- [x] Full: implement `sem.and_sc` / `sem.or_sc` lowering → CFG with short-circuit (required for thunk branches)
- [x] Implement `sem.match_sum` lowering → `adt.tag` + `term.switch` + join args
- [x] Hoist nested `sem.*` used as operands (use-position) into `let` so lowering is uniform
- [x] Emit stable derived ids (`"sircc:lower:..."`) so producers can diff outputs
- [ ] Add golden tests:
  - [ ] input = sem intent
  - [ ] output = core CFG

### P2 — Aggregates + initializers as “stdlib enabler” (already mostly done)

- [ ] Lock the layout contract docs and verifier knobs:
  - [ ] require pinned target triple + layout keys (determinism)
- [ ] Make `agg:v1` lowering produce Core-only forms for global init

### P3 — Callables + ADTs as the “modern language unlock”

- [ ] `fun:v1` lowering to Core call forms (direct/indirect) with explicit signatures
- [ ] `closure:v1` lowering:
  - [ ] closure layout contract (code ptr + env ptr)
  - [ ] capture materialization rules
- [ ] `adt:v1` lowering:
  - [ ] sum layout contract (tag + payload)
  - [ ] `adt.make/tag/is/get` lowering into Core memory ops where possible

### P4 — Optimization hooks (optional, but planned)

- [ ] Emit optional “intent provenance” sidecar (JSONL) mapping lowered core regions → originating constructs
- [ ] Keep it best-effort and non-semantic (tooling only) until a versioned hint schema exists
