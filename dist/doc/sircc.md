# sircc (SIR JSONL compiler)

`sircc` compiles `sir-v1.0` `*.sir.jsonl` programs to native code using LLVM.

This repo’s “shipable bundle” is produced by the build target `dist`, which creates:

- `dist/bin/<os>/sircc` (the compiler binary)
- `dist/bin/<os>/sirc` (optional: `.sir` → `.sir.jsonl`, when built with `-DSIR_ENABLE_SIRC=ON`)
- `dist/test/` (a small normative example set you can run immediately)

## Quickstart

Build the bundle:

```sh
cmake -S . -B build
cmake --build build --target dist
```

Optional (macOS arm64 only): enable LLVM-vs-lower comparison tests (for improving `lower`):

```sh
cmake -S . -B build -DSIRCC_ENABLE_LOWER_COMPARE_TESTS=ON
cmake --build build
ctest --test-dir build -R '^sircc_compare_lower_vs_llvm_' --output-on-failure
```

Then run:

```sh
./dist/bin/<os>/sircc --help
./dist/bin/<os>/sircc --print-target
./dist/bin/<os>/sircc ./dist/test/examples/mem_copy_fill.sir.jsonl -o /tmp/mem_copy_fill && /tmp/mem_copy_fill; echo $?
```

If you also built `sirc`, you can do the full pipeline from `.sir`:

```sh
./dist/bin/<os>/sirc ./dist/test/examples/hello.sir -o /tmp/hello.sir.jsonl
./dist/bin/<os>/sircc /tmp/hello.sir.jsonl -o /tmp/hello && /tmp/hello
```

## CLI

```text
sircc <input.sir.jsonl> -o <output> [--emit-llvm|--emit-obj|--emit-zasm] [--clang <path>] [--target-triple <triple>]
sircc --verify-only <input.sir.jsonl>
sircc --dump-records --verify-only <input.sir.jsonl>
sircc --print-target [--target-triple <triple>]
sircc --print-support [--format text|json] [--full]
sircc --check [--dist-root <path>|--examples-dir <path>] [--format text|json]
sircc [--runtime libc|zabi25] [--zabi25-root <path>] ...
sircc [--diagnostics text|json] [--color auto|always|never] [--diag-context N] [--verbose] [--strip] ...
sircc --version
```

Notes:
- default output is a native executable (links via `clang`)
- `--emit-llvm` writes LLVM IR (`.ll`)
- `--emit-obj` writes an object file (`.o`)
- `--emit-zasm` writes a `zasm-v1.1` JSONL stream (zir) (`.jsonl`)
- if `meta.ext.target.triple` is present, it is used unless `--target-triple` overrides it
- `meta.ext.target.cpu` and `meta.ext.target.features` (optional) are passed through to LLVM target machine creation
  - `cpu` defaults to `"generic"`
  - `features` defaults to empty string (LLVM-style feature string, e.g. `"+neon,-crc"`; target-dependent)
- `--strip` runs `strip` on the output executable (useful for smaller distribution artifacts)
- `--require-pinned-triple` fails if neither `--target-triple` nor `meta.ext.target.triple` is provided
- `--diagnostics json` emits errors as `diag` JSONL records (useful for tooling)
- `--diag-context N` prints the offending JSONL record plus `N` surrounding lines (also included as `context` in JSON diagnostics)
- `--print-support` prints which SIR mnemonics are implemented vs missing (from the normative `mnemonics.html` table)
- `--check` runs a small “try immediately” suite over `dist/test/examples` (or a custom `--examples-dir`)
- `--runtime zabi25` links against the zABI 2.5 host runtime (default root is autodetected; override via `--zabi25-root` or `SIRCC_ZABI25_ROOT`)
  - zABI mode expects you to export an entrypoint named `zir_main` (the host shim provides `main()` and calls `zir_main()` after installing the zABI host).

## Type and semantics (LLVM/node path)

This section describes the core value model and execution semantics `sircc` implements for the Stage-3 node frontend when compiling via LLVM.

### Value and type model

- Integers: `i8`, `i16`, `i32`, `i64`
- Booleans: `i1` (`bool.*` and comparisons produce `i1`)
- Floats: `f32`, `f64`
- Pointers: `ptr` (target pointer width is derived from the selected target triple; `ptr.to_i64` / `ptr.from_i64` are available)
- Aggregates:
  - `type.kind:"struct"` (ABI layout derived from target triple; determinism can be enforced via `meta.ext.target.*` contract)
  - `type.kind:"array"`
  - `type.kind:"sum"` (via `adt:v1`)
- Callables:
  - `type.kind:"fn"` (direct/indirect calls)
  - `type.kind:"fun"` / `type.kind:"closure"` (via `fun:v1` / `closure:v1`; both are treated as opaque values)

Not currently in scope: SIMD/vector types and atomic/eh/gc/coro packs.

### Integer semantics

- Default integer operations are **two’s-complement wraparound** (modulo 2^N) unless the mnemonic explicitly specifies traps or saturation.
- Division/remainder:
  - `.trap` variants deterministically trap on divide-by-zero and signed overflow (e.g. `INT_MIN / -1` for signed).
  - `.sat` variants are total (no UB): they clamp/produce specified saturated results.
- Shifts/rotates use a **masked shift count** (so shifting by >= bitwidth is well-defined and deterministic).

### Floating point semantics

- `f32`/`f64` arithmetic uses LLVM FP ops, plus **canonical NaN** behavior: results are canonicalized to a quiet NaN (payloads are not propagated).

### Memory + effects

- `load.*` / `store.*` support `align` and `vol`.
  - If `align` is omitted, `sircc` defaults to `1` (avoids implicit UB from over-assumed alignment).
  - If `align > 1`, `sircc` emits a deterministic runtime trap on misaligned access.
- `mem.copy` / `mem.fill` lower to LLVM bulk ops.
  - `mem.copy overlap:"allow"` uses memmove semantics.
  - `mem.copy overlap:"disallow"` emits a deterministic overlap check and traps on overlap.
- `eff.fence` validates `mode`; `relaxed` is a no-op, other modes lower to an LLVM fence.

### `ptr.sym` producer rule

`ptr.sym` is “address of a known symbol” and must name a symbol declared in-module (so `sircc` can validate/typecheck deterministically).

- For function symbols: emit a `fn` or `decl.fn` record with matching signature.
- For global data: emit a `sym` record with `kind:"var"` or `kind:"const"`.
- To call an external C function: prefer `decl.fn` + `call.indirect` (do not rely on unresolved `ptr.sym` names).

## Packs (node frontend)

These feature gates are enabled via `meta.ext.features` (array of strings). If a gate is missing, `sircc` rejects gated node tags.

### `fun:v1` (higher-order functions)

- Types:
  - `type.kind:"fun"` with `sig:<fn-type-id>`
- Nodes:
  - `fun.sym` produces a function value for a symbol name
    - producer rule: the symbol must have a prior `fn` or `decl.fn` node in the stream, and the referenced signature must match `fun.sig`
    - `fun.sym` rejects collisions with data globals (`sym(kind=var|const)`)
  - `call.fun` calls a `fun` value: `fields.args:[callee, arg0, ...]`
  - `fun.cmp.eq/ne` compares function values for equality
- Opaqueness rule:
  - `fun` values are opaque (no `ptr.*` arithmetic/casts on `fun`-typed nodes; use `fun.*` / `call.fun`).

### `closure:v1` (closures)

- Types:
  - `type.kind:"closure"` with `callSig:<fn-type-id>` and `env:<type-id>`
- Nodes:
  - `closure.make` builds a closure from `{code, env}`:
    - `code` must be a `fun` value of derived signature `codeSig = (envTy, callSig.params...) -> callSig.ret`
    - `env` type must match `envTy`
  - `closure.sym` builds a closure from an extern `code` symbol + `env` value
  - `call.closure` calls a closure value: `fields.args:[callee, arg0, ...]`
  - `closure.code` / `closure.env` project components
  - `closure.cmp.eq/ne` compares closures (currently supports integer/pointer env equality only)
- Opaqueness rule:
  - `closure` values are opaque (no `ptr.*` arithmetic/casts on `closure`-typed nodes; use `closure.*` / `call.closure`).

### `adt:v1` (sum types)

- Types:
  - `type.kind:"sum"` with `variants:[{name, ty?}, ...]` where `ty` is optional for nullary variants
- Nodes:
  - `adt.make` constructs a sum value (`flags.variant` selects the variant)
  - `adt.tag` extracts the `i32` tag
  - `adt.is` compares tag against `flags.variant` (out-of-range variant deterministically traps)
  - `adt.get` extracts the payload for `flags.variant` (wrong-variant deterministically traps; nullary get is rejected)

### `sem:v1` (semantic algebra; deterministic desugaring)

- Nodes:
  - `sem.if` (value-level conditional): `args:[cond, thenBranch, elseBranch]`
  - `sem.and_sc` / `sem.or_sc` (short-circuit)
  - `sem.match_sum` (sum matching): `fields.sum`, `args:[scrut]`, `fields.cases[]`, `fields.default`
- Branch operands are objects:
  - `{ "kind":"val", "v": <node-ref> }`
  - `{ "kind":"thunk", "f": <fun/closure node-ref> }`
    - thunks are 0-arg for `sem.if/and_sc/or_sc`
    - for `sem.match_sum`, case bodies may be 0-arg thunks or 1-arg thunks (payload passed); the thunk parameter type must match the payload type

## ZASM backend (zir) notes

`--emit-zasm` is an experimental backend that emits a `zasm-v1.1` JSONL stream intended to be checked by `ircheck` and executed by `zem`.

Current calling convention model (ZASM64 “Lembeh” ABI, as used by the backend):
- integer/pointer args are passed in registers in order: `HL`, `DE`, `BC`, `IX`
- integer return value is in `HL`
- `CALL` is treated as clobbering all general registers (`HL`, `DE`, `BC`, `IX`, `A`) unless explicitly saved/restored by the caller

If you call an extern from ZASM output, design it as if it were a freestanding ABI:
- don’t rely on preserved registers unless you save them yourself
- pass only primitive ints/pointers (no structs, no varargs) until the ABI model is extended
