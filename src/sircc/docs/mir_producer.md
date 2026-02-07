# MIR → SIR (sir-v1.0) producer checklist (for `sircc`)

This doc is a practical “emit this shape” guide for MIR producers targeting the **node frontend** of `sircc` (Stage/Milestone 3, LLVM backend).

For the longer-term plan to move portable lowering complexity out of MIR and into the SIR toolchain (via SIR-HL packs/intent lowered into a stable SIR-Core contract), see:

- `src/sircc/docs/hl_core_contract.md`

## Quick checklist

- Emit `{"ir":"sir-v1.0","k":"meta", ...}` first (recommended; not required).
- Use stable ids (ints or strings). Prefer readable string ids for injected/derived nodes.
- Prefer “defs before uses” for better diagnostics, but forward refs are supported.
- If you use gated packs, enable them via `meta.ext.features`.
- If you need reproducible cross-machine artifacts, pin the target via `meta.ext.target.*` and/or `sircc --deterministic` + `--require-*` flags.

## `meta.ext` keys used by sircc

All keys live under the `k:"meta"` record’s `ext` object.

```json
{"ir":"sir-v1.0","k":"meta","unit":"your_unit","ext":{
  "target":{"triple":"arm64-apple-darwin24.5.0","cpu":"generic","features":""},
  "features":["agg:v1","fun:v1","closure:v1","adt:v1","sem:v1"]
}}
```

- `meta.ext.target.triple` (string): default target triple (overridden by `--target-triple`).
- `meta.ext.target.cpu` (string, optional): LLVM CPU string (defaults to `"generic"`).
- `meta.ext.target.features` (string, optional): LLVM feature string (defaults to empty).
- `meta.ext.features` (array of strings): feature gates.

## Record/ID conventions

- `type.id`, `node.id`, `sym.id`, `src.id` may be **numbers or strings**.
- References are objects: `{"t":"ref","id":<same kind of id>}`.
- For stability (and easy injection), prefer string ids like:
  - `{"id":"fn:main"}`, `{"id":"node:ret0"}`, `{"id":"ty:i32"}`.

## Minimal “main returns 0” skeleton

```json
{"ir":"sir-v1.0","k":"meta","unit":"skel"}

{"ir":"sir-v1.0","k":"type","id":"t_i32","kind":"prim","prim":"i32"}
{"ir":"sir-v1.0","k":"type","id":"t_main","kind":"fn","params":[],"ret":"t_i32"}

{"ir":"sir-v1.0","k":"node","id":"c0","tag":"const.i32","type_ref":"t_i32","fields":{"value":0}}
{"ir":"sir-v1.0","k":"node","id":"ret0","tag":"term.ret","fields":{"value":{"t":"ref","id":"c0"}}}
{"ir":"sir-v1.0","k":"node","id":"b0","tag":"block","fields":{"stmts":[{"t":"ref","id":"ret0"}]}}
{"ir":"sir-v1.0","k":"node","id":"fn:main","tag":"fn","type_ref":"t_main","fields":{"name":"main","linkage":"public","params":[],"body":{"t":"ref","id":"b0"}}}
```

## Calling external C functions (C ABI)

Pattern: `decl.fn` + `call.indirect`.

Do **not** call extern symbols via `ptr.sym` unless they are declared in-module: `sircc` treats `ptr.sym` as “address of a known symbol” and rejects unknown names during `--verify-only`.

Notes:
- Ordering does **not** matter: `sircc` supports forward references, so `decl.fn` can appear before or after uses (but declaring imports up front tends to produce clearer diagnostics).
- The same rule applies to global data: use `sym(kind=var|const)` for data symbols referenced by `ptr.sym`.

```json
{"ir":"sir-v1.0","k":"type","id":"t_i8","kind":"prim","prim":"i8"}
{"ir":"sir-v1.0","k":"type","id":"t_pchar","kind":"ptr","of":"t_i8"}
{"ir":"sir-v1.0","k":"type","id":"t_i32","kind":"prim","prim":"i32"}
{"ir":"sir-v1.0","k":"type","id":"t_puts","kind":"fn","params":["t_pchar"],"ret":"t_i32"}

{"ir":"sir-v1.0","k":"node","id":"decl:puts","tag":"decl.fn","type_ref":"t_puts","fields":{"name":"puts"}}
{"ir":"sir-v1.0","k":"node","id":"s0","tag":"cstr","fields":{"value":"hello from sircc"}}
{"ir":"sir-v1.0","k":"node","id":"call0","tag":"call.indirect","type_ref":"t_i32","fields":{"sig":{"t":"ref","id":"t_puts"},"args":[{"t":"ref","id":"decl:puts"},{"t":"ref","id":"s0"}]}}
```

## Exporting a function for C to call

Emit a `fn` with:

- `fields.name`: exported symbol name
- `fields.linkage:"public"`

Example is also covered by `ctest` as `sircc_cinterop_export_add2`.

## Packs used by MIR

Enable via `meta.ext.features`.

- `agg:v1`: structured constants + globals (`const.*`, `sym(kind=var|const)`, `ptr.sym`)
  - `ptr.sym` requires an in-module declaration: `fn`/`decl.fn` for functions, `sym(kind=var|const)` for globals
- `simd:v1`: vector type + SIMD ops (`type.kind:"vec"`, `vec.*`, `load.vec`, `store.vec`)
- `fun:v1`: `type.kind:"fun"`, `fun.sym`, `call.fun`, `fun.cmp.*`
- `closure:v1`: `type.kind:"closure"`, `closure.make/sym/code/env`, `call.closure`, `closure.cmp.*`
- `adt:v1`: `type.kind:"sum"`, `adt.make/tag/is/get`
- `sem:v1`: `sem.if`, `sem.and_sc`, `sem.or_sc`, `sem.match_sum` (deterministically desugared)

## SIMD pack notes (simd:v1)

- Define vector types as `{"k":"type","kind":"vec","lane":<type-ref>,"lanes":<pos-int>}`.
- Lane types must be one of: `i8/i16/i32/i64/f32/f64/bool`.
- Deterministic lane/memory order:
  - Lane 0 is stored at the lowest address; lane `i` follows lane `i-1`.
  - Byte order inside each lane follows `meta.ext.target.endian`.
- **Bool vector ABI (sircc)**: `vec(bool,N)` lowers to `<N x i8>` and is normalized to `0/1` (nonzero is treated as true).
- Deterministic traps:
  - `vec.extract` / `vec.replace` trap when `idx<0 || idx>=lanes`.
  - `vec.shuffle` traps if any `flags.idx[i]` is outside `[0, 2*lanes)`.
- `vec.cmp.*` results are `vec(bool,lanes)`; either set `node.type_ref` explicitly, or ensure a matching `vec(bool,lanes)` type definition exists so the consumer can infer it.

## Recommended: validate and introspect

- Validate only: `sircc --verify-only your.sir.jsonl`
- Machine-readable failures: `sircc --diagnostics json --diag-context 2 --verify-only your.sir.jsonl`
- Support surface: `sircc --print-support --format json`
- Target ABI report: `sircc --print-target`

## Safety limits (ingestion)

`sircc` enforces high default safety limits when reading JSONL:

- `SIRCC_MAX_LINE_BYTES`: max bytes per JSONL record line (default: 16 MiB)
- `SIRCC_MAX_RECORDS`: max non-blank records per input file (default: 5,000,000)

These are intended to prevent accidental OOM/degenerate inputs; raise them if you have extremely large modules.
