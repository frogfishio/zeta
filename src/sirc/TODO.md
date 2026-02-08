# SIRC TODO — “bonafide compiler” DX hardening

This is the worklist for turning `sirc` from a useful sugar translator into a **product-grade compiler frontend** that emits SIR JSONL with strong DX guarantees.

Legend:
- `[ ]` not started
- `[x]` done

## P0 — Ship-worthy CLI + diagnostics

- [x] Add `--diagnostics text|json` (JSONL on stderr) with stable diagnostic codes and a consistent record shape.
- [x] Add `--all` to continue after the first error (collect + print multiple diagnostics).
  - [x] In `--tool` mode: continue compiling other files and don’t append partial output for failing inputs.
  - [x] Intra-file recovery + multi-diagnostic collection (Bison error recovery).
- [x] Add caret diagnostics in text mode (1–3 context lines with `^` under the span).
- [x] Define and document exit codes: `0` success, `1` compile/verify error, `2` tool error (I/O, OOM, internal).

## P0 — Source mapping contract (output)

- [x] Emit `k:"src"` records and attach `src_ref`/`loc` to emitted semantic records (type/node; and later sym/ext/diag when sirc emits them).
- [x] Make source mapping deterministic (stable `src_ref` allocation; no dependence on hash iteration order).
- [x] Add `--emit-src {none|loc|src_ref|both}` (or similar) to control output size and determinism in fixtures.

## P0 — CLI parity / “tool mode”

- [x] Add `--tool -o <out.jsonl> <input.sir>...` (multi-input filelist mode, like `zas`).
- [x] Add `--lint` (parse/validate only; no output JSONL).
- [x] Add `--print-support [--format text|json]` listing exactly what syntax/features/packs `sirc` can emit.
- [x] Keep `--help` and `--version` up to date.
- [x] Keep `--ids string|numeric` (`string` default).

## P0 — Conformance + regression suite

- [ ] Golden fixtures: `.sir → .sir.jsonl` exact-match tests for core constructs (and for each supported pack).
  - [x] `sem:v1` fixtures (all `examples/sem_*.sir`).
  - [ ] Base/core v1.0 fixtures (hello/add/mem/cfg/etc.) once we decide the “minimal blessed” set.
    - [x] `hello`, `add`, `cfg_if` (golden tests).
  - [ ] Pack fixtures (`fun:v1`, `closure:v1`, `adt:v1`) beyond verification-only.
- [ ] Negative fixtures: assert diagnostic code + span for common failures (unknown type, wrong arg count, bad attrs).
  - [x] Harness: CTest runner asserts `--diagnostics json` fields (code + loc + msg substring).
  - [x] Initial fixtures for sem-pack authoring guardrails (bad branch kind, bad keys).
  - [ ] Add “frontend hygiene” fixtures once we add sirc-side checks (unknown type, wrong arg count, bad attrs).
- [ ] Pipeline fixtures: `sirc` output must pass `sircc --verify-only` (already covered by existing tests; expand).
- [x] Add minimal CLI regression tests: `--lint`, `--diagnostics json`, and `--tool` multi-input.
- [ ] Optional runtime smoke: selected fixtures run via `sem` and assert exit code/stdout (when deterministic).

## P1 — Language usability + docs

- [ ] Write `docs/sirc.md` as the authoritative reference: syntax, types, attrs, `as Type`, CFG form, ids.
- [ ] Add a “cookbook” section: extern calls, hello world, closure, ADT, CFG blocks/switch, traps, memory ops.
- [ ] Document string-id stability + collision/uniqueness rules (what is guaranteed across recompiles).

## P1 — Sem-pack authoring (avoid “split personality”)

- [x] Add sugar for `sem.*` records that require branch objects (thunk vs val) while keeping 1-line≈1-record:
  - [x] `sem.if`, `sem.and_sc`, `sem.or_sc`, `sem.cond`, `sem.while`, `sem.break`, `sem.continue`, `sem.switch`, `sem.match_sum`, `sem.defer`, `sem.scope`
- [x] Decide and document the minimal surface syntax for branch objects:
  - `val <expr>` and `thunk <expr>` (where thunk expr evaluates to a fun/closure value)
  - `sem.scope(..., body: do <stmts...> end)` uses a structural `block` node (not a CFG block)
- [x] Add examples + verify tests for each `sem.*` construct.

## P1 — Quality-of-implementation

- [ ] Reduce/resolve bison conflicts (or at minimum add regression fixtures for ambiguous corners).
- [ ] Improve error recovery enough for `--all` mode (synchronize at newlines).
- [ ] Tighten attribute handling: unknown keys warnings vs errors (under `--strict`).

## P2 — Nice-to-have DX

- [ ] `sirc fmt` (or `--format`) for canonical `.sir` formatting.
- [ ] `--emit {jsonl|minjsonl}` or similar for stable compact output.
- [ ] “Explain” mode: show the exact JSON record a line will emit (`--explain-line <n>`).

## Status snapshot (today)

- [x] String ids emitted by default in `sirc`; numeric ids available via `--ids numeric`.
- [x] `sem` can parse numeric or string ids (interned).
- [x] `sirc` can author: `fun:v1`, `closure:v1`, `adt:v1` (examples exist and pass `sircc --verify-only`).
