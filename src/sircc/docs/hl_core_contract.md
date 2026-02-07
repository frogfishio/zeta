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

## AST → SIR “compiler kit” must-haves (avoid split-personality lowering)

This checklist is the “hard contract” work needed to switch from MIR to **AST → SIR (HL/Core) → sircc** without frontends reinventing semantics/ABI/layout piecemeal.

### P0 — Blockers (must be verifier-enforced)

- [ ] Define and freeze the blessed subsets:
  - [ ] One blessed **SIR-HL** surface subset (what AST emitters may generate)
  - [ ] One blessed **SIR-Core** executable subset (what backends/codegen accept)
  - [x] Provide a single gateway: `sircc --lower-hl --emit-sir-core` (HL → Core) and treat Core as the only stable codegen boundary
  - [ ] Expand `--verify-strict` into “no best-effort”: forbid ambiguous omissions (require types/sigs where inference is fragile)

- [ ] Target + layout contract (frontends must not guess):
  - [x] `--require-pinned-triple` and `--require-target-contract` for determinism
  - [ ] Document exactly what is **layout-defined** vs **opaque** (e.g. `fun`, `closure`, `sum`)
  - [ ] ABI rules must be explicit (no ambient host defaults): byref params, aggregate passing/return, calling convention assumptions

- [ ] Interop contract (imports + exports), documented and diagnostic-first:
  - [x] Imports: `decl.fn` + `call.indirect` pattern; `ptr.sym` producer rule enforced + actionable diagnostic
  - [x] Ordering clarified: forward refs allowed (decls before uses recommended for diagnostics)
  - [ ] Exports: document required fields/rules (`fn.name`, `linkage:"public"`, signature stability, C ABI expectations)
  - [ ] Decide/encode varargs + byref + aggregate ABI strategy (profile or pack, but one canonical answer)

- [ ] Baseline data story (encoding + interop) as a pack (no handwaving):
  - [x] `data:v1` enforced by verifier (`bytes`, `string.utf8`, `cstr`)
  - [ ] Decide how encoding is declared (module-wide `meta.ext.*` vs per-type) and freeze the rule
  - [ ] Define required explicit conversions (e.g. `string.utf8` ⇄ `cstr`) as library/host calls (no implicit magic)

- [ ] Globals + constants that real languages need (no per-frontend folklore):
  - [x] Structured constants / aggregate initializers (arrays/structs) and global data symbols (`sym(kind=var|const)`)
  - [ ] Ensure sums/ADTs have a deterministic global-init story (if supported by the pack)
  - [ ] Add strict verifier checks so “string literals” and other common payloads cannot be represented multiple incompatible ways

- [ ] Semantic “intent” constructs so AST emitters stay dumb:
  - [x] `sem:v1` deterministic desugaring (`sem.if`, `sem.and_sc`, `sem.or_sc`, `sem.match_sum`)
  - [ ] Fill intent gaps needed by most languages (examples: `sem.loop`, `sem.break`, `sem.continue`, `sem.defer`) or explicitly declare them frontend-owned

- [ ] Strict integration modes:
  - [x] `--verify-strict` exists
  - [ ] Add a `--lower-strict` (or tie to `--verify-strict`) so HL→Core lowering also rejects ambiguous shapes early
  - [ ] Make “strict” the recommended mode for integrators (documented defaults)

### P1 — Efficient emission (reduce boilerplate; keep emitters uniform)

- [x] Official preludes as “compiler kit batteries”:
  - [x] `--prelude <file>` and `--prelude-builtin data_v1|zabi25_min`
  - [x] Bundle preludes into `dist/lib/sircc/prelude`
  - [ ] Expand builtin preludes set (`core_types`, `c_abi`, etc.) and keep them versioned

- [ ] Canonical lowering cookbook for AST emitters:
  - [ ] “If AST has X, emit these SIR shapes” (vars, address-taken locals, short-circuit, switch, calls, VAR params, records/arrays)
  - [ ] Include “don’t do this” anti-patterns that cause split-personality lowering

- [ ] Module/link story (one consistent resolution model):
  - [ ] Decide whether a SIR “module” is always a single JSONL stream, or needs a formal import mechanism
  - [ ] Document name-resolution + collision rules (symbols/types) and how they interact with preludes

- [ ] Diagnostics as a first-class integration surface:
  - [ ] Stable diagnostic taxonomy (`code`) for producer errors; ensure diagnostics include actionable producer rules
  - [ ] Add “did you mean” suggestions for the most common mistakes (extern, type_ref mismatches, missing feature gates)

### P2 — Inevitable widening (prevent future frontends from inventing ad-hoc semantics)

- [ ] Unsigned integer types with defined semantics:
  - [ ] Add `u8/u16/u32/u64` (or a principled alternative) and comparison/shift/div rules
  - [ ] Specify C interop mapping explicitly (ABI profile)

- [ ] Feature packs with frozen shapes (even if not implemented yet):
  - [ ] `atomics:v1` (ordering + RMW/CAS)
  - [ ] `eh:v1` (invoke/throw/resume/lpads)
  - [ ] `coro:v1` (start/resume/drop)
  - [ ] `gc:v1` (roots, barriers, safepoints)
  - [ ] Rule: if not implemented, verifier should still be able to “recognize and gate” (so producers don’t invent their own)

- [ ] Optimization boundary policy (avoid “emitters optimize differently”):
  - [ ] Decide whether AST emitters should emit already-canonical Core, or richer HL and rely on sircc to canonicalize
  - [ ] Add canonicalization passes where it removes degrees of freedom (prevents fingerprints and semantic drift)

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
