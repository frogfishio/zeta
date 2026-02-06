# SEM (`sem --run`) roadmap (tracked against `schema/sir/v1.0/mnemonics.html`)

This file tracks what SEM can *execute today* and what’s next, grouped by ROI.

Scope notes:
- SEM is a **SIR JSONL frontend + lowering layer**. It parses `.sir.jsonl` and builds a structured module for `sircore`.
- `sircore` should remain **format-agnostic** (no JSONL); SEM owns parsing/IO.
- The checkboxes below are about SEM end-to-end support: parse → lower → validate → run.

## Status snapshot (today)

- [x] CFG form: `fn.entry` + `fn.blocks` (basic block emission + patching)
- [x] `term.br` + block params (`block.params` + `bparam` + `term.br.args`)
- [x] `term.cbr` / `term.condbr`
- [x] `term.switch` (MVP: i32 scrutinee, `const.i32` literals)
- [x] Memory MVP: `alloca.i8/i32/i64`, `load.i8/i32/i64`, `store.i8/i32/i64`
- [x] Calls MVP: `call.indirect` with callee `decl.fn` (extern) or `ptr.sym` (in-module by name)
- [x] Values MVP: `const.i8/i32/i64`, `cstr`, `name`, `i32.add`, `binop.add`, `i32.cmp.eq`

## P0 (ship-grade DX): make failures actionable

- [x] Structured diagnostics end-to-end (first-error, actionable)
  - [x] Record first diagnostic (code + message + line + node id when available)
  - [x] Emit via `sem --diagnostics text|json`
  - [ ] Optional: collect and emit multiple diagnostics (`--all`)
- [ ] Tighten validation (SEM-side) with clear messages
  - [ ] Wrong mnemonic payload shape (missing fields, wrong types)
  - [ ] Wrong arity/type for op arguments
  - [ ] CFG invariants (terminator last, all blocks reachable optional mode, etc.)
- [ ] Document “what’s a string” for SEM execution
  - [ ] Specify `cstr` in SEM: byte sequence semantics (UTF-8-by-convention) + null-termination policy (if any)
  - [ ] Decide whether SEM treats strings as `(ptr,len)` or `ptr` to NUL-terminated bytes for host calls

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
- [ ] `i8.or / i16.or / i32.or / i64.or`
- [ ] `bool.and / bool.or / bool.xor`
- [ ] `bool.not`
- [ ] `select` (SSA-ish conditional value select)
- [ ] Minimal compare set beyond `i32.cmp.eq` (at least `ne`, signed `< > <= >=` as needed by MIR)

### Pointers + memory (beyond stack slots)
- [ ] `load.ptr` / `store.ptr`
- [ ] `ptr.add / ptr.sub`
- [ ] `ptr.offset`
- [ ] `ptr.to_i64 / ptr.from_i64` (currently only `ptr.to_i64` passthrough in SEM)
- [ ] `ptr.cmp.eq / ptr.cmp.ne`
- [ ] `ptr.sizeof` / `ptr.alignof` (at least for prims; later for structs)
- [ ] `mem.copy`
- [ ] `mem.fill`

## P2 (data): globals + structured constants (enables “real programs”)

- [ ] `global` records (module-level data)
  - [ ] Define execution-time memory model for globals in SEM (static region in guest mem)
  - [ ] Relocations: `ptr.sym` to globals, address-taken semantics
- [ ] `const` records / structured constants (agg:v1 style)
  - [ ] `const.zero`
  - [ ] `const.array`
  - [ ] `const.repeat`
  - [ ] `const.struct`
- [ ] `load.f32 / load.f64` and `store.f32 / store.f64` (needed once globals/constants include floats)

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
- [ ] `adt.make`
- [ ] `adt.tag`
- [ ] `adt.is`
- [ ] `adt.get`
- [ ] Normative layout contract for sums (size/align/payload rules) for SEM execution

### `sem:*` intent mnemonics (semantic desugaring)
- [ ] `sem.if` (desugar to base blocks/terms + validate)
- [ ] `sem.and_sc` (short-circuit)
- [ ] `sem.or_sc` (short-circuit)
- [ ] `sem.match_sum` (desugar to `adt.tag` + `term.switch` + join-args)

## P4 (later / optional packs)

- [ ] Atomics: `atomic.load.*`, `atomic.store.*`, `atomic.cmpxchg.*`
- [ ] SIMD/vectors: `load.vec`, `store.vec`, and all `vec.*` mnemonics
- [ ] Coroutines: `coro.*`
- [ ] Exceptions: `eh.*` + `term.invoke/throw/resume`
- [ ] GC: `gc.*`
- [ ] Effects: `eff.fence` (if needed for atomics/mem model)

## Tooling + tests (keeps us honest)

- [ ] Add a `sem --print-support [--format text|json]` that reports supported mnemonics (SEM runner subset)
- [ ] Add a “normative” SEM suite runner (single command)
  - [ ] Run: parse+validate+execute known-good fixtures
  - [ ] Include negative fixtures to lock diagnostics
- [ ] Add coverage-driven fixtures for tricky semantics
  - [ ] Switch fallthrough doesn’t exist (ensure single target)
  - [ ] Block-param correctness (overlapping src/dst slots; multi-arg joins)
  - [ ] Memory aliasing basics (store then load same address)
