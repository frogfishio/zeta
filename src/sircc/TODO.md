# TODO (sircc roadmap — active)

Merged `TODO.md` + `TODO1.md` into this single file. Keep this file updated as features land.

Goal: a plain-C `sircc` binary that compiles `sir-v1.0` JSONL to native binaries via LLVM, with **complete mnemonic coverage** (see `schema/sir/v1.0/mnemonics.html`) and clear feature gates.

This TODO is organized as milestones. Each milestone should end with:
1) at least one runnable example program, 2) `--emit-llvm` golden tests, 3) negative tests (invalid SIR) producing good diagnostics.

## Milestone 0 — Build + Dev UX

- [ ] CI build matrix (macOS, Linux) with cached LLVM install/docs
- [x] `sircc --version` and `sircc --help` (basic)
- [x] `dist` bundle target: `dist/bin/<os>`, `dist/doc/sircc.md`, `dist/test/examples`
- [x] Support reporting: `sircc --print-support [--format text|json]`
- [x] Alpha smoke suite: `sircc --check` over `dist/test/examples`
- [x] Deterministic builds: pin target triple + data layout reporting
  - [x] `sircc --print-target` prints `triple`, `data_layout`, `endianness`, `ptrBits`
  - [x] Codegen always sets module `target triple` + `datalayout` (opt override > `meta.ext.target.triple` > host)
  - [x] Make “pinned triple” a producer requirement for reproducible artifacts (docs + validator: `--require-pinned-triple`)
  - [x] Add `--dump-records` (quick parse trace)
  - [x] Add `--verify-only` (parse + validate + report, no codegen)

## Milestone 1 — Parser + Validator (spec-first correctness)

### 1.1 JSONL + record handling
- [x] Parse **all** record kinds: `meta`, `src`, `diag`, `sym`, `type`, `node`, `ext`, `label`, `instr`, `dir`
- [x] Preserve `src_ref` + `loc` and plumb through to diagnostics (when present)
- [x] “Closed schema” behavior: reject unknown fields for the subset we claim to support (strict for all parsed kinds)
- [x] Accept stable **string ids** (and mixed int+string) for `src/sym/type/node` and `{"t":"ref","id":...}` (interned into dense internal ids)
 - [x] Record order independence: allow forward refs (defs-after-uses) for ids; still recommended to emit defs first for best diagnostics
  - [x] Feature gates: allow `meta.ext.features` anywhere in the stream (defer checks until end-of-parse)

### 1.2 `meta` contract for codegen
- [x] Define `meta.ext` keys used by sircc (document in `schema/sir/v1.0/README.md` or `src/sircc/README.md`)
  - [x] `target.triple` (default: host)
  - [x] `target.cpu` / `target.features` (optional; passed through to LLVM)
  - [x] `features` array for mnemonic feature gates (`simd:v1`, `adt:v1`, `fun:v1`, `closure:v1`, `coro:v1`, `eh:v1`, `gc:v1`, `atomics:v1`, `sem:v1`)
- [x] Validation: reject feature-gated mnemonics when the gate is not enabled (prefix-based for `instr.m`)

### 1.3 Typed operands model (shared across instr/dir)
- [x] Implement operand decoding for `Value` union (validation-level):
  - [x] `sym`, `lbl`, `reg`, `num`, `str`, `mem`, `ref`
- [x] Define the internal IR types/value model: integers/floats/bool/ptr + aggregates + sums (documented in `src/sircc/docs/src.md`)
- [x] Decide + document integer semantics: wraparound by default; explicit trap/saturating variants only where specified (documented in `src/sircc/docs/src.md`)

## Next widening tickets (MIR-driven)

These are the next “semantic widening” items to unlock real-language lowering (MIR parity) and make `sem:v1` practical.

- [x] **SIR-HL → SIR-Core legalizer contract + tool surface**
  - [x] Ownership + contracts doc (portable lowering moves to `sircc`): `src/sircc/docs/hl_core_contract.md`
  - [x] Add `sircc --lower-hl --emit-sir-core` to produce a lowered Core JSONL stream
  - [x] Add goldens/tests: HL input → Core output (verify-only) + HL-vs-Core compile/run diff
  - [x] Hoist `sem.*` used in expression positions into `let` (enables nested `sem.*` under other nodes)

- [x] **Struct types + deterministic layout (base v1.0)**
  - [x] Parse `type.kind:"struct"` with `fields[]` and lower it to LLVM struct types
  - [x] Implement size/align with padding rules (baseline, non-packed)
  - [x] Use target ABI alignments (derived from target triple) for `ptr.sizeof/alignof/offset` and struct layout (no ambient host defaults)
  - [x] Add fully explicit layout contract (ptrBits/endian/*Align/structAlign) as a producer requirement for reproducible streams across LLVM versions (`--require-target-contract`, enabled by `--deterministic`)

- [x] **Structured constants / aggregates (agg:v1)** (LLVM)
  - [x] Define/implement structured constants for arrays/structs (zero/array/repeat/struct) so globals/initializers can be deterministic and large payloads aren’t duplicated (node form: `const.zero/array/repeat/struct`)
  - [x] Add `global`-like data declarations (or equivalent `sym`/node model) + `ptr.sym` support for data symbols (not just functions)
    - [x] `sym(kind=var|const)` can define LLVM globals (scalar initializer subset) and `ptr.sym` resolves them
    - [x] `sym.value` may reference const nodes for initializers (array/repeat examples)
    - [x] Runnable examples: `global_i32_ptrsym`, `global_array_const`, `global_array_repeat`
  - [ ] ZASM parity: globals + const payload emission (defer until the zasm data model is decided)

- [x] **Higher-order callables**
  - [x] `fun:v1`: `type.kind:"fun"`, `fun.sym`, `fun.cmp.*`, `call.fun` (node form + LLVM)
  - [x] `closure:v1`: `type.kind:"closure"`, `closure.*`, `call.closure` (node form + LLVM)

- [x] **Sum types / ADTs (adt:v1)**
  - [x] `type.kind:"sum"` and ops: `adt.make/tag/is/get` with normative layout (node form + LLVM)

- [x] **Semantic algebra (sem:v1)**
  - [x] Implement deterministic desugaring for `sem.if`, `sem.and_sc`, `sem.or_sc`, `sem.match_sum` into base records/nodes, then validate + lower the desugared form (node-lowered to CFG + φ)

## Productionizing (LLVM-first)

Goal: make the LLVM backend + node frontend stable enough that an integrator can start generating/consuming these packs without “paper cuts”.

- [x] **Callable opacity hardening** (fun/closure are opaque; no pointer arithmetic / raw loads)
  - [x] Reject `ptr.*` ops on `fun`/`closure` typed values (force `fun.*` / `closure.*`)
  - [x] Reject `call.indirect` on `fun`/`closure` typed values (force `call.fun` / `call.closure`)
  - [x] Add negative fixtures for each rejected misuse (ptr.add/ptr.to_i64/store)

- [x] **Type-rule validation upgrades (node frontend)**
  - [x] `fun.sym`: require the referenced symbol is a function and its signature matches `fun.sig`
  - [x] `closure.make`: require `code : fun(codeSig)` and `env : envTy` (reject mismatches)
  - [x] `closure.sym`: require symbol signature matches derived `codeSig` and `env` type matches (env type enforced)
  - [x] `call.fun` / `call.closure`: improve errors (show expected vs actual arity/types)
  - [x] `sem.*`: validate branch operand shape (`{kind:"val"|"thunk", ...}`) and thunk arity (0-arg for `sem.if/and_sc/or_sc`, 0-arg or 1-arg for `sem.match_sum` cases)
    - [x] `sem.match_sum`: when thunk arity is 1, require param type matches payload type
  - [x] `--verify-only` semantic validator covers fun/closure/adt/sem pack shapes (not just LLVM lowering)

- [x] **ADT layout + determinism hardening**
  - [x] Add explicit tests for padding/align edge cases (payload align > 4, mixed sizes)
  - [x] Add negative tests: out-of-range variant traps; `adt.get` wrong-variant trap; nullary get rejected

- [x] **Diagnostics hardening**
  - [x] Replace remaining `errf(...)` in new packs with `err_codef(...)` (stable codes)
    - [x] `fun:v1` / `closure:v1` / `adt:v1` / `sem:v1`: all pack-specific errors use `err_codef(...)`
    - [x] `agg:v1`-style `const.*` nodes and `sym(kind=var|const)` globals use `err_codef(...)` for validation/lowering failures
    - [x] CFG/effects lowering (`let`/`store.*`/`mem.*`/`eff.*`/`term.*`/CFG `fn`) uses `err_codef(...)` for stable diagnostic codes
    - [x] LLVM/node lowering uses `err_codef(...)` for user-facing errors (no generic `sircc.error` fallbacks for common validation failures)
  - [x] Ensure every error produced during lowering includes record context (`k/id/tag`) when available
    - [x] Expression lowering always sets node context (push/pop in `lower_expr`)
    - [x] CFG/effects lowering errors use node context (`SIRCC_ERR_NODE`) where possible

- [x] **Conformance suite expansion**
  - [x] Add a “pack corpus” under `dist/test/examples` covering fun/closure/adt/sem (positive)
  - [x] Add `sircc --check` coverage for these new examples

- [x] **Integrator-facing docs**
  - [x] Document the exact supported node tags/shapes for fun/closure/adt/sem (including current limitations like closure env equality)
  - [x] Add “producer rules” checklist: required features, type defs, and ordering expectations

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
- [x] `--emit-llvm`, `--emit-obj`, default executable
- [x] C interop: `fn.fields.linkage` (`public|local`) controls emitted symbol linkage for object outputs
- [ ] Cross-platform link driver strategy (clang/lld/cc) with `--linker` and `--ldflags`
- [ ] Emit static libs / shared libs (later): `--emit-lib`
- [ ] Cross-target LLVM backends: keep native-only init as default, add an opt-in “all targets” build/init mode for true cross compilation

## Milestone 3 — Mnemonics: Base (ungated, required)

Implement in the same order as below (earlier items unblock later ones). Each bullet means:
1) validator rules, 2) LLVM lowering, 3) 2–5 focused tests.

### 3.1 Integer arithmetic and bitwise (pure) — 18
- [x] `i8/i16/i32/i64`: `add sub mul and or xor not neg` (lowering implemented for `node.tag` `iN.*`)
- [x] Shifts/rotates with masked shift count: `shl shr.s shr.u rotl rotr` (implemented for `node.tag`; rotates lowered via `llvm.fshl/fshr`)
- [x] Min/max: `min.s min.u max.s max.u` (implemented for `node.tag` with deterministic tie-break: if `a==b` result is `a`)

### 3.2 Integer division and remainder (explicit behavior) — 4
- [x] `.trap` variants: emit explicit trap path (`llvm.trap`) on div-by-zero / overflow where specified (implemented for `node.tag` `iN.(div|rem).(s|u).trap`)
- [x] `.sat` variants: total semantics (implemented for `node.tag` `iN.(div|rem).(s|u).sat` with CFG to avoid UB)

### 3.3 Integer comparisons (pure) — 1
- [x] `cmp.eq ne slt sle sgt sge ult ule ugt uge` families → `i1` (lowering implemented for `node.tag` `iN.cmp.*`)
- [x] `eqz` family → `bool` (implemented for `node.tag` `iN.eqz`)

### 3.4 Bit-twiddling (pure) — 3
- [x] `clz ctz popc` (lowering implemented for `node.tag` `iN.clz/ctz/popc` via LLVM intrinsics)

### 3.5 Boolean ops (pure) — 2
- [x] `bool.not`, `bool.and/or/xor` (lowering implemented for `node.tag` `bool.*`)

### 3.6 Floating point (pure, deterministic) — 10
- [x] `f32/f64`: `add sub mul div neg abs sqrt min max` (implemented for `node.tag`)
- [x] Canonical NaN rules (no payload propagation): canonicalize NaN results to qNaN bits; IR-based test added (`float_nan_canon_ops.sir.jsonl`)
  - [x] Float comparisons and conversions (implemented for `node.tag`: `f32/f64.cmp.*`, `f32/f64.from_i{32,64}.{s,u}`, `i{32,64}.trunc_sat_f{32,64}.{s,u}`)

### 3.7 Value-level conditional (pure) — 1
- [x] `select` lowering with type checking (implemented for `node.tag`; validates bool cond + matching operand types; accepts optional `fields.ty` and checks against `type_ref` when present)

### 3.8 Conversions and casts (pure; closed patterns) — 3
- [x] `zext`, `sext`, `trunc` (lowering implemented for `node.tag` `i<dst>.(zext|sext|trunc).i<src>`)

### 3.9 Pointer ops (pure) — 4
- [x] `ptr.sym`, `ptr.add/sub`, `ptr.cmp.eq/ne` (implemented for `node.tag` `ptr.sym`, `ptr.add/sub`, `ptr.cmp.eq/ne`, `ptr.to_i64`, `ptr.from_i64`)

### 3.10 Address calculation and layout (pure; target-explicit) — 3
- [x] `ptr.offset`, `ptr.alignof`, `ptr.sizeof` (implemented for `node.tag`; current layout uses a deterministic host-layout subset: prim sizes + host `sizeof(void*)`, plus `type.kind:"array"`/`"ptr"` recursion; TODO: wire real `unit.target.ptrBits`/data-layout reporting)
  - [x] Pointer size/width now derived from selected target triple (not host `sizeof(void*)*8`)

### 3.11 Memory effects — 10
- [x] `load.*` / `store.*` baseline for ints/floats/ptr (supports `align` + `vol`; **if `align` omitted we default to 1 to avoid implicit UB**; float loads/stores canonicalize NaNs)
- [x] `mem.copy` / `mem.fill` (overlap=\"allow\" -> memmove; overlap=\"disallow\" -> runtime overlap check + deterministic trap; `align*` must be >0 when present; `overlap` is a closed set)
- [x] `alloca` (mnemonic-style: `fields.ty` + `flags:{count (i64/ref), align, zero}`) → `ptr`
- [x] `eff.fence` with mode validation (`relaxed` is a no-op; others lower to LLVM fence)
- [x] Remaining target-specified alignment/trapping semantics for loads/stores/mem ops (no implicit UB): validate align is power-of-two and trap on misaligned access when `align>1`

### 3.12 Calls (effects) — 2
- [x] `call` with signature/type checking (direct calls + `call.indirect` with explicit `fields.sig` fn type; validates arg count/types for non-varargs and pointer-arg bitcasts)
- [x] `term.ret` / `term.unreachable` (or the table’s exact terminators) with CFG validation (implemented: `term.ret`, `term.unreachable`, `term.trap`)

### 3.13 Control flow (terminators) — 7
- [x] `term.br`, `term.condbr`, `term.switch`, etc. (exact names per table) (implemented: `term.br`, `term.condbr` (alias `term.cbr`), `term.switch` in CFG-form `fn`, plus block args/params via `bparam` PHIs; gated `term.invoke`/`term.resume` remain Milestone 4)
- [x] Enforce “no implicit fallthrough” rule (every block must end in a terminator) (implemented in lowering + `--verify-only` CFG validator)

## Milestone 4 — Mnemonics: Feature-gated packages

Each package must be fully skippable unless its `unit.features` gate is enabled.

### 4.1 Atomics (effects; atomics:v1) — 4
- [x] Validate + lower ordering modes (`relaxed/acquire/release/acqrel/seqcst`) (closed set; load/store legality enforced)
- [ ] Implement atomic load/store/RMW/CAS as specified (partial)
  - [x] `atomic.load.i8/i16/i32/i64` (LLVM atomic load)
  - [x] `atomic.store.i8/i16/i32/i64` (LLVM atomic store)
  - [x] `atomic.rmw.{add,and,or,xor,xchg}.i8/i16/i32/i64` (LLVM atomicrmw)
  - [ ] `atomic.cmpxchg.*` (blocked: current node model is single-result; cmpxchg is multi-result in spec)

### 4.2 SIMD (simd:v1) — 13
- [x] **Phase 0: contracts + representation (must be explicit)**
  - [x] Add `type.kind:"vec"` (feature-gated) with `{lane:<type-ref>, lanes:<pos-int>}`
    - [x] Validate lane type is one of: `i8/i16/i32/i64/f32/f64/bool`
    - [x] Validate `lanes > 0`
  - [x] Decide + document the **bool lane ABI** (recommendation for determinism + memory semantics):
    - [x] `vec(bool,N)` is represented as lanes of bytes in memory (0/1), with vector ops treating nonzero as true
    - [x] In LLVM lowering, represent `vec(bool,N)` as `<N x i8>` and normalize results to 0/1
  - [x] Deterministic traps: `vec.extract` / `vec.replace` MUST trap on out-of-range lane indices
  - [x] Deterministic memory order: lane 0 at lowest address; lane bytes follow `meta.ext.target.endian`

- [x] **Phase 1: type lowering + size/align**
  - [x] Extend type parsing/validation to accept `kind:"vec"` only when `meta.ext.features` includes `simd:v1`
  - [x] Lower `vec(laneTy,lanes)` to LLVM fixed vector type (lane lowered per scalar rules; bool lane uses the chosen ABI)
  - [x] Implement size/align for `vec` in `ptr.sizeof/alignof/offset` (packed lanes; align = lane align)

- [ ] **Phase 2: vector construction and lane ops (4)**
  - [x] `vec.splat` (broadcast scalar into all lanes)
  - [x] `vec.extract` (runtime bounds-check `idx:i32`; trap if `idx<0 || idx>=lanes`)
  - [x] `vec.replace` (bounds-check + insert; trap on OOB)
  - [x] `vec.shuffle` (validate `flags.idx` length == lanes; trap if any `idx` not in `[0,2*lanes)`; then lower to LLVM shuffle)

- [ ] **Phase 3: lane-wise arithmetic/logic (5)**
  - [x] `vec.add/sub/mul` (int + float lanes)
    - [x] Float results must be NaN-canonicalized per lane (match scalar rules)
  - [x] `vec.and/or/xor` (int lanes; for bool lanes operate on 0/1 and keep result normalized)
  - [x] `vec.not` (int lanes; for bool lanes implement logical-not producing 0/1)
  - [x] `vec.cmp.{eq,ne,lt,le,gt,ge}` (int + float lanes) → `vec(bool,lanes)`
    - [x] Float comparisons follow scalar ordered rules (NaN compares false for ordered predicates)
  - [x] `vec.select` (mask: `vec(bool,lanes)`; lower to LLVM select using `mask != 0` as the i1 lane mask)

- [x] **Phase 4: vector memory access + bitcast (4)**
  - [x] `load.vec` (vector load; explicit `align/vol`; misaligned trap when `align>1`; default align=1)
  - [x] `store.vec` (vector store; same `align/vol` rules)
  - [x] `vec.bitcast` (equal-size reinterpret cast between vectors; validate byte-size equality)
  - [x] Add a small `vec` “ABI sanity” doc section + examples (load/store order, bool lane rules) (added to `docs/mir_producer.md`)

- [ ] **Phase 5: tests + dist integration**
  - [ ] Add positive fixtures:
    - [x] `vec_splat_extract` (added: `examples/simd_splat_extract.sir.jsonl`)
    - [x] `vec_i32_add_extract_replace` (added: `examples/simd_i32_add_extract_replace.sir.jsonl`)
    - [x] `vec_f32_mul_nan_canon` (added: `examples/simd_f32_mul_nan_canon_bits.sir.jsonl`)
    - [x] `vec_cmp_select_bool_mask` (added: `examples/simd_cmp_select_bool_mask.sir.jsonl`)
    - [x] `vec_shuffle_two_inputs` (added: `examples/simd_shuffle_two_inputs.sir.jsonl`)
    - [x] `vec_load_store_roundtrip` (covered by `examples/simd_splat_extract.sir.jsonl`)
  - [ ] Add negative fixtures:
    - [x] missing `ty` / wrong `ty` kind (added: `examples/bad_simd_splat_missing_type_ref.sir.jsonl`, `examples/bad_simd_splat_wrong_type_ref.sir.jsonl`)
    - [x] lane type not allowed / lanes == 0 (added: `examples/bad_simd_vec_lane_unsupported.sir.jsonl`, `examples/bad_simd_vec_lanes_zero.sir.jsonl`)
    - [x] `vec.shuffle` wrong idx length (added: `examples/bad_simd_shuffle_idx_len.sir.jsonl`)
  - [ ] Add runtime-trap fixtures (expected nonzero exit):
    - [x] `vec.extract` OOB index (added: `examples/simd_extract_oob_traps.sir.jsonl`)
    - [x] `vec.replace` OOB index (added: `examples/simd_replace_oob_traps.sir.jsonl`)
    - [x] `vec.shuffle` OOB idx element (added: `examples/simd_shuffle_oob_traps.sir.jsonl`)
  - [x] Wire fixtures into `sircc --check`
  - [x] Ensure all SIMD errors use `err_codef` with stable codes and include node context in JSON diagnostics (added targeted `sircc.vec.*` codes + `SIRCC_ERR_NODE` paths)

### 4.3 ADT sums (adt:v1) — 4 (+ notes)
- [x] Sum construction: `adt.make`
- [x] Tag inspection: `adt.tag`
- [x] Payload extraction: `adt.get`
- [x] Nullary/empty payload cases + layout rules
- [x] Follow the spec’s matching-lowering note: stable case ordering, `term.switch` over tag

### 4.4 Function values (fun:v1) — 4
- [x] First-class function pointers, `fun.sym`, indirect calls, signature checking

### 4.5 Closures (closure:v1) — 7
- [x] Environment layout/type strategy (explicit first parameter rule)
- [x] `closure.make`, `closure.call`, `closure.sym`, etc.
- [x] Capture-by-value vs capture-by-ref semantics (as specified)

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
- [ ] `gc.read_barrier`, `gc.write_barrier`, `gc.card_mark` (or the table’s exact ops)
- [ ] Validator: missing required barriers is invalid when the model says they’re required

### 4.10 Declarative semantics (sem:v1) — 4 (requires adt:v1 for sum matching)
- [x] `sem.if` (non-strict conditional: branches as values/thunks)
- [x] `sem.and_sc` / `sem.or_sc` (short-circuit with explicit evaluation order)
- [x] `sem.match_sum` (pattern match over sums)

### 4.11 Structured control intent (sem:v1) — roadmap
- [x] loops: `sem.while` + `sem.break` + `sem.continue` (continue targets the loop header; body thunk returns `i32 action`)
- [x] expression-level conditional: `sem.cond` (ternary; lowered as `sem.if`)
- [x] multi-way branch: `sem.switch` (integer switch intent; lowers to `term.switch`)
- [x] function-level cleanup (MVP): `sem.defer` (injected before returns; supported in body-form + CFG-form functions)
- [x] scoped cleanup (MVP): `sem.scope` (inline structural block + run defers on fallthrough/return)
- [ ] scoped cleanup (future): `sem.scope` extended to cover Core CFG exits (`term.br`/`term.condbr`/`term.switch`) + unwind paths (when `eh:*` exists)

## Milestone 5 — Completeness: docs + conformance + examples

- [x] Generate a machine-readable “mnemonic support table” and enforce Milestone 3 coverage (`src/sircc/tools/mnemonic_coverage.py`, `sircc_mnemonic_coverage_m3` test)
- [ ] Provide one example `.sir.jsonl` per mnemonic family (small, focused)
- [ ] Conformance runner: compile + execute (where possible) and compare outputs
  - [x] Minimal CTest conformance set: compile+run core examples and assert exit codes (mem/cfg smoke)
- [ ] Final hardening:
  - [ ] No UB surprises: all traps/saturation explicit
  - [ ] Determinism: NaN canonicalization, stable switch ordering, stable layout reporting
  - [x] Diagnostics: include `src_ref/loc` in every error when available (and always prefix at least the input file for non-record errors)
  - [x] Diagnostics: `--diagnostics json` includes `context` when `--diag-context N` is set

## Experimental — ZASM (zir v1.1) backend

This is a separate emission path (`sircc --emit-zasm`) targeting `zasm-v1.1` JSONL (zir), intended for `zem` execution and future “real lowering” beyond LLVM.

- [x] `--emit-zasm` emits `zasm-v1.1` JSONL for `zir_main`
- [x] Deterministic per-record `"id"` in emitted zasm JSONL
- [x] `--emit-zasm-map <path>` sidecar mapping zasm record ids to SIR node context
- [x] Emits `EXTERN` for `decl.fn` and `PUBLIC zir_main`
- [x] Emits `STR` for `cstr` nodes
- [x] Lowers simple memory statements in `zir_main`: `mem.fill`, `mem.copy`, `store.i8` (via `FILL`, `LDIR`, `ST8`)
- [x] Supports return-time `load.i8` and `i32.zext.i8` (via `LD8U`)
- [x] Name binding: `let` + `name` (including call-result spill to temp slots)
- [x] Load/store widths: `load.{i8,i16,i32,i64,ptr}` and `store.{i8,i16,i32,i64}` (slot-backed values supported)
- [x] Addressing: `alloca.*`, `ptr.sym`, `ptr.add` (const disp), `ptr.offset` (const idx*size) via `mem` disp
- [x] CFG form `zir_main`: `fields.entry` + `fields.blocks`, `term.{br,condbr,ret}`, block params (`bparam`) + `term.br args`
- [x] CFG blocks support core non-terminators: `let`, `store.*`, `mem.copy`, `mem.fill`, and simple arithmetic lets
- [x] Expand CFG conditions: `i32.cmp.{eq,ne,slt,sle,sgt,sge,ult,ule,ugt,uge}` and `i64.cmp.*` (slot operands supported)
- [x] Dynamic pointer arithmetic (non-const `ptr.add` / `ptr.offset`)
- [x] `mem.fill` / `mem.copy` accept pointer expressions (not just allocas)
- [x] More ZASM integer ops: `i32/i64.{mul,and,or,xor,shl,shr.s,shr.u,rotl,rotr}`
- [x] ZASM div/rem: `i32/i64.{div.{s,u},rem.{s,u}}`
- [x] ZASM CFG loop smoke test (bparam + condbr + br args)
- [x] ZASM unary ops: `i32/i64.{clz,ctz,popc}` (let-bound)
- [x] ZASM diagnostics include node context (`about.k/id/tag`) in JSON mode where possible
- [x] ZASM test: JSON diagnostics include `about` for backend lowering errors
- [x] ZASM negative tests: invalid `ptr.offset` and “too many call args” produce JSON diagnostics
- [x] ZASM micro-opt: avoid duplicate slot loads for `a op a`
- [x] ZASM block-local reg cache: reuse repeated slot loads into `HL`/`DE` (conservative; reset at labels and around calls)
- [x] ZASM reg-cache regression test: store then reload same slot must reflect new value

### Optional: compare LLVM vs `lower`

- [ ] Add an opt-in test suite comparing `sircc` (LLVM) output vs `lower` output on the same inputs (stdout + exit code + size)
