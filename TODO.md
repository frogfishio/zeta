# TODO (sircc roadmap)

Goal: a plain-C `sircc` binary that compiles `sir-v1.0` JSONL to native binaries via LLVM, with **complete mnemonic coverage** (see `schema/sir/v1.0/mnemonics.html`) and clear feature gates.

This TODO is organized as milestones. Each milestone should end with:
1) at least one runnable example program, 2) `--emit-llvm` golden tests, 3) negative tests (invalid SIR) producing good diagnostics.

## Milestone 0 — Build + Dev UX

- [ ] CI build matrix (macOS, Linux) with cached LLVM install/docs
- [x] `sircc --version` and `sircc --help` (basic)
- [ ] Deterministic builds: pin target triple + data layout reporting (`--print-target`)
- [x] Add `--dump-records` (quick parse trace)
- [x] Add `--verify-only` (parse + validate + report, no codegen)

## Milestone 1 — Parser + Validator (spec-first correctness)

### 1.1 JSONL + record handling
- [x] Parse **all** record kinds: `meta`, `src`, `diag`, `sym`, `type`, `node`, `ext`, `label`, `instr`, `dir`
- [x] Preserve `src_ref` + `loc` and plumb through to diagnostics (when present)
- [x] “Closed schema” behavior: reject unknown fields for the subset we claim to support (strict for all parsed kinds)
- [ ] Record order independence: allow forward refs with a fixup pass (still warn if producer violates “emit defs first” guideline)

### 1.2 `meta` contract for codegen
- [ ] Define `meta.ext` keys used by sircc (document in `schema/sir/v1.0/README.md` or `src/sircc/README.md`)
  - [x] `target.triple` (default: host)
  - [ ] `target.cpu` / `target.features` (optional)
  - [x] `features` array for mnemonic feature gates (`simd:v1`, `adt:v1`, `fun:v1`, `closure:v1`, `coro:v1`, `eh:v1`, `gc:v1`, `atomics:v1`, `sem:v1`)
- [x] Validation: reject feature-gated mnemonics when the gate is not enabled (prefix-based for `instr.m`)

### 1.3 Typed operands model (shared across instr/dir)
- [x] Implement operand decoding for `Value` union (validation-level):
  - [x] `sym`, `lbl`, `reg`, `num`, `str`, `mem`, `ref`
- [ ] Define the internal IR types: `iN`, `fN`, pointers, vectors, aggregates, sums
- [ ] Decide + document integer semantics: wraparound by default; explicit trap/saturating variants only where specified

## Milestone 2 — Core backend architecture (so “all mnemonics” is tractable)

### 2.1 Two frontends, one backend
- [ ] **Instr frontend** (primary for mnemonic completeness):
  - [ ] Parse `label`/`instr`/`dir` into a CFG + data segments
  - [ ] Resolve labels and build basic blocks
  - [ ] Track “virtual registers” (`reg`) with SSA construction (or explicit φ insertion)
- [ ] **Node frontend** (keep, but treat as sugar):
  - [ ] Lower `node` graph into the instr frontend’s IR (so we don’t maintain two LLVM lowerers)

### 2.2 LLVM lowering framework
- [ ] Module builder: target triple, data layout, CPU/features, opt level knobs
- [ ] Per-function lowering context with:
  - [ ] local symbol table (`sym`/`reg`) → LLVM values
  - [ ] type table (`type`) → LLVM types
  - [ ] debug locations (from `src`/`loc`) when available
- [ ] Pass pipeline: verify, minimal canonicalization, optional `-O{0,1,2,3}`

### 2.3 Linking and output
- [ ] `--emit-llvm`, `--emit-obj`, default executable
- [ ] Cross-platform link driver strategy (clang/lld/cc) with `--linker` and `--ldflags`
- [ ] Emit static libs / shared libs (later): `--emit-lib`

## Milestone 3 — Mnemonics: Base (ungated, required)

Implement in the same order as below (earlier items unblock later ones). Each bullet means:
1) validator rules, 2) LLVM lowering, 3) 2–5 focused tests.

### 3.1 Integer arithmetic and bitwise (pure) — 18
- [x] `i8/i16/i32/i64`: `add sub mul and or xor not neg` (lowering implemented for `node.tag` `iN.*`)
- [ ] Shifts/rotates with masked shift count: `shl shr.s shr.u rotl rotr` (implemented: `shl shr.s shr.u`; TODO: `rotl rotr`)
- [ ] Min/max/select-style ops in this section (if present in the table)

### 3.2 Integer division and remainder (explicit behavior) — 4
- [ ] `.trap` variants: emit explicit trap path (e.g. `llvm.trap`) on div-by-zero / overflow where specified
- [ ] `.sat` variants: emit saturating semantics exactly as spec says

### 3.3 Integer comparisons (pure) — 1
- [x] `cmp.eq ne slt sle sgt sge ult ule ugt uge` families → `i1` (lowering implemented for `node.tag` `iN.cmp.*`)

### 3.4 Bit-twiddling (pure) — 3
- [x] `clz ctz popc` (lowering implemented for `node.tag` `iN.clz/ctz/popc` via LLVM intrinsics)

### 3.5 Boolean ops (pure) — 2
- [x] `bool.not`, `bool.and/or/xor` (lowering implemented for `node.tag` `bool.*`)

### 3.6 Floating point (pure, deterministic) — 10
- [ ] `f32/f64`: `add sub mul div neg abs sqrt min max` (implemented: `add sub mul div neg abs sqrt`; TODO: `min max` + NaN canonicalization details)
- [ ] Canonical NaN rules (no payload propagation): document implementation strategy + tests

### 3.7 Value-level conditional (pure) — 1
- [x] `select` lowering with type checking (lowering implemented for `node.tag` `select`; TODO: strict type checks)

### 3.8 Conversions and casts (pure; closed patterns) — 3
- [x] `zext`, `sext`, `trunc` (lowering implemented for `node.tag` `i<dst>.(zext|sext|trunc).i<src>`)

### 3.9 Pointer ops (pure) — 4
- [ ] `ptr.sym`, `ptr.add/sub`, `ptr.cmp.eq/ne`

### 3.10 Address calculation and layout (pure; target-explicit) — 3
- [ ] `ptr.offset`, `ptr.alignof`, `ptr.sizeof`

### 3.11 Memory effects — 10
- [ ] `load.*` / `store.*` for ints and floats (alignment + optional width rules)
- [ ] `memcpy/memmove/memset` (or whatever the table defines) via LLVM intrinsics

### 3.12 Calls (effects) — 2
- [ ] `call` with signature/type checking (implemented: direct `call` to referenced `fn` node; TODO: signature checks + indirect/varargs)
- [ ] `term.ret` / `term.unreachable` (or the table’s exact terminators) with CFG validation (implemented: `term.ret`, `term.unreachable`, `term.trap`)

### 3.13 Control flow (terminators) — 7
- [ ] `term.br`, `term.condbr`, `term.switch`, etc. (exact names per table) (implemented: `term.br`, `term.cbr`, `term.switch` in CFG-form `fn`, plus block args/params via `bparam` PHIs; TODO: `term.invoke`/`term.resume` and ptr scrut)
- [ ] Enforce “no implicit fallthrough” rule (every block must end in a terminator) (implemented in lowering + `--verify-only` CFG validator)

## Milestone 4 — Mnemonics: Feature-gated packages

Each package must be fully skippable unless its `unit.features` gate is enabled.

### 4.1 Atomics (effects; atomics:v1) — 4
- [ ] Validate + lower ordering modes (`relaxed/acquire/release/acqrel/seqcst`)
- [ ] Implement atomic load/store/RMW/CAS as specified

### 4.2 SIMD (simd:v1) — 13
- [ ] Vector construction and lanes — 4
- [ ] Lane-wise arithmetic/logic — 5
- [ ] Vector memory access — 4

### 4.3 ADT sums (adt:v1) — 4 (+ notes)
- [ ] Sum construction: `adt.make`
- [ ] Tag inspection: `adt.tag`
- [ ] Payload extraction: `adt.get`
- [ ] Nullary/empty payload cases + layout rules
- [ ] Follow the spec’s matching-lowering note: stable case ordering, `term.switch` over tag

### 4.4 Function values (fun:v1) — 4
- [ ] First-class function pointers, `fun.sym`, indirect calls, signature checking

### 4.5 Closures (closure:v1) — 7
- [ ] Environment layout/type strategy (explicit first parameter rule)
- [ ] `closure.make`, `closure.call`, `closure.sym`, etc.
- [ ] Capture-by-value vs capture-by-ref semantics (as specified)

### 4.6 Coroutines (coro:v1) — 5
- [ ] Define runtime ABI for coroutine frames (explicit, documented)
- [ ] `coro.start/resume/yield/complete` equivalents
- [ ] Ensure result protocol is represented with `adt:v1` sum types as required

### 4.7 Exceptions (eh:v1) — 7
- [ ] `term.invoke` with normal+unwind successors
- [ ] `term.throw` / landing pads (platform-specific strategy; document macOS vs Linux)
- [ ] Runtime/ABI decision: Itanium EH vs SEH (start with Itanium for clang toolchains)

### 4.8 GC (gc:v1) — 10
- [ ] Managed reference representation + root set modeling
- [ ] Safepoints that return updated refs (moving collector compatibility)
- [ ] `gc.alloc`, `gc.root`, `gc.safepoint`, etc. (exact names per table)
- [ ] Document that “complete support” requires a runtime; ship a minimal one in `src/sirrt` (or similar)

### 4.9 GC barriers (gc:v1) — 3
- [ ] `gc.read_barrier`, `gc.write_barrier`, `gc.card_mark` (or table’s exact ops)
- [ ] Validator: missing required barriers is invalid when the model says they’re required

### 4.10 Declarative semantics (sem:v1) — 4 (requires adt:v1 for sum matching)
- [ ] `sem.if` (non-strict conditional: branches as values/thunks)
- [ ] `sem.and_sc` / `sem.or_sc` (short-circuit with explicit evaluation order)
- [ ] `sem.match_sum` (pattern match over sums)

## Milestone 5 — Completeness: docs + conformance + examples

- [ ] Generate a machine-readable “mnemonic support table” from `mnemonics.html` and compare against implemented handlers
- [ ] Provide one example `.sir.jsonl` per mnemonic family (small, focused)
- [ ] Conformance runner: compile + execute (where possible) and compare outputs
- [ ] Final hardening:
  - [ ] No UB surprises: all traps/saturation explicit
  - [ ] Determinism: NaN canonicalization, stable switch ordering, stable layout reporting
  - [ ] Diagnostics: include `src_ref/loc` in every error when available
