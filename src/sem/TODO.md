# SEM roadmap (execution parity + CI harness)

This file tracks what SEM can *execute today* and what’s next, grouped into explicit stages.

Scope notes:
- `sem` is a **SIR JSONL frontend + lowering layer**. It parses `.sir.jsonl` and builds a structured module for `sircore`.
- `sircore` remains **format-agnostic** (no JSONL); SEM owns parsing/IO and user-facing diagnostics.
- All checkboxes below are **end-to-end**: parse → lower → validate → run (unless marked verify-only).

Design goal:
- “Meet the contract and it runs”: if a producer emits SIR within the blessed compiler-kit subset, it should execute under `sem` deterministically with actionable failures.

## Status snapshot (today)

- [x] CFG form: `fn.entry` + `fn.blocks` (basic block emission + patching)
- [x] `term.br` + block params (`block.params` + `bparam` + `term.br.args`)
- [x] `term.cbr` / `term.condbr`
- [x] `term.switch` (MVP: i32 scrutinee, `const.i32` literals)
- [x] Memory MVP: `alloca.i8/i32/i64`, `load.i8/i32/i64`, `store.i8/i32/i64`
- [x] Calls MVP: `call.indirect` with callee `decl.fn` (extern) or `ptr.sym` (in-module by name)
- [x] Values MVP: `const.i8/i32/i64/bool`, `cstr`, `name`, `i32.add`, `binop.add`, `i32.cmp.eq`

---

## Staged parity roadmap (what it takes)

### Stage A — dist corpus parity (CI-ready)

**Goal:** `sem --check dist/test/examples` passes the same normative corpus shipped in `dist/` (or we explicitly split the corpus into “compile-only” vs “run in sem”).

Acceptance:
- [ ] `cmake --build build --target dist` produces `dist/` and then:
  - [ ] `./dist/bin/<os>/sircc --check` passes
  - [ ] `./dist/bin/<os>/sem --check ./dist/test/sem/examples` passes (verify-only)
  - [ ] `./dist/bin/<os>/sem --check --check-run ./dist/test/sem/run` passes (run)

Work items:
- [x] Fix `sem --check` CLI flag parsing (so `--diagnostics json --all` works as flags, not paths)
- [x] Add missing “glue” integer ops used by the dist examples
  - [x] `i32.zext.i8` (blocks `mem_copy_fill`)
  - [x] `i64.zext.i32` / `i32.trunc.i64` parity as needed by examples/frontends
- [x] Add commonly-used trap/sat variants present in shipped examples
  - [x] `i32.div.s.trap` (blocks `sem_if_thunk_trap_not_taken`)
- [x] Allow side-effecting `call.indirect` in `block.stmts` (execute as statement; discard result)
- [x] Split dist corpus into `sem` verify/run subsets
  - [x] `dist/test/sem/examples` (verify-only subset)
  - [x] `dist/test/sem/run` (runnable subset for `--check-run`)

### Stage B — SIR-Core execution parity (compiler-kit Core)

**Goal:** SEM can execute essentially the same “SIR-Core compiler kit” surface that integrators will emit early on, with deterministic traps and stable diagnostics.

Acceptance:
- [x] Document a blessed “SEM-runnable Core subset” (and keep it aligned with `sem --print-support`)
- [x] Add a small set of “Core runnable” examples that cover: CFG joins, memory, ptr math, extern calls, globals

Work items:
- [ ] Core types parity (execution)
  - [x] `type.kind:"prim"`: `i8/i16/i32/i64/bool/i1/f32/f64/void` (execution rules, not just parsing)
  - [x] `type.kind:"ptr"` treated consistently (typed ptr, but no host pointers)
  - [x] `type.kind:"array"` basics (size/stride) for `ptr.offset` and init data
  - [x] `type.kind:"struct"` (enough for by-pointer access; no “peek” contracts)
- [ ] Core memory parity (execution)
  - [x] `load.i16` / `store.i16`
  - [x] `load.f32` / `store.f32`
  - [x] `load.f64` / `store.f64`
  - [x] Alignment rules and deterministic misalignment traps match the `sircc` contract
- [ ] Core calls parity (execution)
  - [x] `call` (direct call) if required by producers (or document “use call.indirect only”)
- [ ] Diagnostics parity (developer UX)
  - [x] When unsupported: emit “what to do instead” hints (e.g. for extern calls, point to `decl.fn`)
  - [x] Ensure `--diagnostics json` includes stable fields for CI parsing (code/path/line/node/tag/fid/ip/op when available)

### Stage C — Pack parity (closes “split personality” for integrators)

**Goal:** Integrators can use packs for real frontends and still run tests under SEM before compiling with sircc.

Acceptance:
- [ ] For each pack: add a positive + negative fixture pair and run them in `sem --check`

Work items:
- [ ] `adt:v1` execution parity
  - [x] `adt.make`, `adt.tag`, `adt.is`, `adt.get`
  - [x] Match the normative layout/semantics contract used by `sircc` (MVP: `{tag:i32,payload}` in guest memory)
- [ ] `fun:v1` execution parity
  - [x] `fun.sym` (MVP: only in-module function symbols)
  - [x] `fun.cmp.eq`, `fun.cmp.ne`
  - [x] `call.fun` (MVP: callee must be `fun.sym`)
- [ ] `closure:v1` execution parity
  - [ ] `closure.make`, `closure.env`, `closure.code`
  - [ ] `closure.cmp.eq`, `closure.cmp.ne`
  - [ ] `call.closure`
- [ ] `sem:v1` parity (avoid IR drift)
  - [x] Decide: implement lowering rules in SEM (MVP, inline control-flow in sircore bytecode)
  - [x] Support runnable intent set (MVP): `sem.if`, `sem.and_sc`, `sem.or_sc`, `sem.switch`, `sem.while`, `sem.continue`, `sem.defer`, `sem.scope`
  - [x] Remaining intent set: `sem.cond`

### Stage D — CI-grade emulator “superpowers” (optional, makes SEM a platform)

**Goal:** SEM becomes a first-class automated testing platform (record/replay, determinism, robust harness outputs).

Acceptance:
- [x] Stable machine-readable summary output for batch runs (one JSON record per case + a final summary)
- [ ] A recommended “CI recipe” in docs (caps policy, fs sandboxing, tapes)

Work items:
- [ ] Record/replay as a standard workflow
  - [ ] Document tape schema + determinism contract
  - [ ] Add `--tape-out` / `--tape-in` examples to `dist/doc/` and `dist/test/`
- [ ] Harness UX
  - [x] `sem --check` supports `--format text|json` (like `sircc --check`)
  - [x] Add `--list` mode (discover `*.sir.jsonl` cases without running)
  - [x] `--list`/`--check` warn+skip non-`.sir.jsonl` file inputs
- [ ] Instrumentation hooks (using `sircore` events)
  - [x] coverage JSONL (instruction hits/counts)
  - [x] basic trace JSONL (`sem --run --trace-jsonl-out PATH`)
  - [x] trace filters (by fn / op)
  - [x] source mapping in trace/coverage (include `node` + `line` when available)
  - [ ] replayable crash minimization hooks (longer-term)

---

## P0 (ship-grade DX): make failures actionable

- [x] Structured diagnostics end-to-end (first-error, actionable)
  - [x] Record first diagnostic (code + message + line + node id when available)
  - [x] Emit via `sem --diagnostics text|json`
  - [x] Optional: collect and emit multiple diagnostics (`--all`) (best-effort)
  - [x] Include `fid`/`ip`/`op` in JSON diagnostics when available (validator + exec failures)
- [ ] Tighten validation (SEM-side) with clear messages
  - [ ] Wrong mnemonic payload shape (missing fields, wrong types)
  - [x] Wrong arity/type for common op arguments (i32.* casts/binops/unops)
  - [x] Wrong addr shape/type for load/store (`addr` must be a ref to `ptr`)
  - [ ] CFG invariants (terminator last, all blocks reachable optional mode, etc.)
- [x] Document “what’s a string” for SEM execution
  - [x] Specify `cstr` in SEM: byte sequence semantics (UTF-8-by-convention) + null-termination policy (none implicit)
  - [x] Decide whether SEM treats strings as `(ptr,len)` or `ptr` to NUL-terminated bytes for host calls (current: `ptr` to bytes; len is separate)

## P1 (close the “base v1.0” gap): core mnemonics commonly emitted by compilers

### Terminators / control flow
- [x] `term.ret`
- [x] `term.br` (+ args)
- [x] `term.cbr`
- [x] `term.switch` (MVP i32)
- [x] `term.trap`
- [x] `term.unreachable`
- [ ] `term.invoke` (EH-style; likely later unless needed by MIR)
- [ ] `term.throw` / `term.resume` (EH/coro; later)

### Calls / symbols
- [x] `call.indirect`
- [ ] `call` (direct call, if present in spec; map to internal call)
- [ ] `ptr.sym` validation rule: require in-module decl vs allow unresolved extern (decide + document)
- [ ] Exporting functions to C (interop)
  - [ ] Define how “public symbol name” is represented in SIR (`fn.linkage`, `fn.export_name`, or a directive)
  - [ ] Ensure sircc and sem agree (doc + verifier diagnostics)

### Integer / boolean ops (broad coverage before floats/vectors)
- [x] `i32.add` (plus `binop.add` alias)
- [x] `i32.sub / i32.mul / i32.and / i32.or / i32.xor / i32.not / i32.neg`
- [x] `i32.shl / i32.shr.s / i32.shr.u` (masked shift counts)
- [x] `i32.div.*.sat / i32.rem.*.sat` (total, non-trapping)
- [x] `i32.trunc.i64`
- [ ] i8/i16/i64 integer ops (later)
- [x] `bool.and / bool.or / bool.xor`
- [x] `bool.not`
- [x] `select` (SSA-ish conditional value select)
- [x] Minimal compare set beyond `i32.cmp.eq` (`ne`, signed `< > <= >=`, unsigned `< > <= >=`)

### Pointers + memory (beyond stack slots)
- [x] `load.ptr` / `store.ptr`
- [x] `ptr.add / ptr.sub`
- [x] `ptr.offset`
- [x] `ptr.to_i64 / ptr.from_i64`
- [x] `ptr.cmp.eq / ptr.cmp.ne`
- [x] `ptr.sizeof` / `ptr.alignof` (MVP: prims/arrays)
- [x] `mem.copy` (overlap: disallow/allow; disallow traps on overlap)
- [x] `mem.fill`

### Types (parser + lowering)
- [x] `type.kind:"prim"` accepts `prim:"void"` (signature-only; no values)
- [x] `type.kind:"ptr"` is treated as an untyped pointer

## P2 (data): globals + structured constants (enables “real programs”)

- [x] Globals via `sym` records (module-level data)
  - [x] Define execution-time memory model for globals in SEM (deterministic alloc at module start)
  - [x] `ptr.sym` to globals (address-of) for lowered globals
- [ ] `const` records / structured constants (agg:v1 style)
  - [x] `const.zero`
  - [x] `const.array`
  - [x] `const.repeat`
  - [x] `const.struct`
- [x] `load.f32 / load.f64` and `store.f32 / store.f64` (needed once globals/constants include floats)

## P3 (language power): functions, closures, ADTs, and SEM intent nodes

### First-class functions (fun:v1)
- [ ] `fun.sym`
- [ ] `fun.cmp.eq / fun.cmp.ne`
- [ ] `call.fun`

### Closures (closure:v1)
- [ ] `closure.sym`
- [ ] `closure.make`
- [ ] `closure.code`
- [ ] `closure.env`
- [ ] `closure.cmp.eq / closure.cmp.ne`
- [ ] `call.closure`

### ADTs / sums (adt:v1)
- [x] `adt.make`
- [x] `adt.tag`
- [x] `adt.is`
- [x] `adt.get`
- [x] Normative layout contract for sums (size/align/payload rules) for SEM execution

### `sem:*` intent mnemonics (semantic desugaring)
- [x] `sem.if` (MVP: value-only lowers to `select`, thunk branches use inline control-flow)
- [x] `sem.and_sc` (short-circuit)
- [x] `sem.or_sc` (short-circuit)
- [x] `sem.switch` (MVP: i32 scrutinee, const.i32 lits, thunk/val bodies)
- [x] `sem.while` (MVP: thunk(cond)->bool + thunk(body)->i32 control code 0=continue 1=break, lowered as loop in bytecode)
- [x] `sem.defer` / `sem.scope` (MVP: stack of fun.sym thunks, runs on return + fallthrough)
- [x] `sem.continue` (MVP: allowed in legacy thunk bodies; treated as `return 0` from the thunk)
- [x] `sem.break` (MVP: allowed in legacy thunk bodies; treated as `return 1` from the thunk)
- [x] `sem.match_sum` (desugar to `adt.tag` + `term.switch` + join-args)

## P4 (later / optional packs)

- [ ] Atomics: `atomic.load.*`, `atomic.store.*`, `atomic.cmpxchg.*`
- [ ] SIMD/vectors: `load.vec`, `store.vec`, and all `vec.*` mnemonics
- [ ] Coroutines: `coro.*`
- [ ] Exceptions: `eh.*` + `term.invoke/throw/resume`
- [ ] GC: `gc.*`
- [ ] Effects: `eff.fence` (if needed for atomics/mem model)

## Tooling + tests (keeps us honest)

- [x] Add a `sem --print-support [--json]` that reports supported mnemonics (SEM runner subset)
- [ ] Add a “normative” SEM suite runner (single command)
  - [ ] Run: parse+validate+execute known-good fixtures
  - [ ] Include negative fixtures to lock diagnostics
- [ ] Add coverage-driven fixtures for tricky semantics
  - [ ] Switch fallthrough doesn’t exist (ensure single target)
  - [ ] Block-param correctness (overlapping src/dst slots; multi-arg joins)
  - [ ] Memory aliasing basics (store then load same address)
