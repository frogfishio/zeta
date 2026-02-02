# SIRC - Sir Compiler

SIRC is the reference compiler for **SIR**.

It compiles a **small, readable, line-oriented language** ("SIRC", shown below) into **SIR v1.0 JSONL** — **one record per line**.

## Why this exists

Most pipelines eventually want two things at the same time:

- **Human-authored, reviewable code** (so you can write test cases, repros, and small programs without drowning in JSON or raw assembly)
- **Machine-friendly IR** (so tools can validate, diff, index, cache, and stream it through build steps)

SIR is the contract for tools. SIRC is the ergonomic way to write it.

### The killer feature

**Assembly-like control with mid-level structure + tooling.**

You get:

- **Full control over lowering**: you can see exactly what an abstraction compiles into, tweak it, patternise it, and teach the compiler new/cheaper lowerings.
- **Typed operations and literals** (e.g. `3:i32`) so width/sign/ABI mistakes become diagnostics.
- **Structured control flow** when you want it (`return`, blocks, `term.*`) without hand-managing labels.
- **Streaming IR** you can `cat | grep | jq | diff` — stable and line-addressable.
- **First-class source mapping + diagnostics** (`src` / `diag`) without side channels.

### Who writes SIRC

- Compiler/back-end authors (reference programs, golden tests, lowering experiments)
- Tooling and build systems (indexing, caching, linting, validation, reduction)
- Low-level/perf work where “close to metal” matters but raw ASM/WAT is too brittle

### At a glance

```sir
unit u0 target wasm32 +agg:v1

fn add(a:i32, b:i32) -> i32 public
  return i32.add(a, b)
end
```

→ lowers into a **typed semantic stream** (SIR JSONL) that other tools can consume.

## What SIRC produces

SIRC emits a **stream** of SIR records. The stream is designed to work well in compiler toolchains and build systems:

- `meta` records: producer/unit/target/features metadata
- `src` records: source locations and (optionally) source text snippets
- `diag` records: warnings/errors with stable codes + source references
- semantic graph records (SIR layer):
  - `type` records: type constructors (prim/ptr/array/fn/struct/…)
  - `sym` records: symbols (fn/var/const/field/param/…)
  - `node` records: semantic nodes (functions, blocks, expressions, statements)
  - `ext` records: explicit extension payloads for tooling/debug/backends

Even when the input *looks* like “mnemonics”, the output is still a semantic stream: consumers can either interpret it at the SIR layer, or treat it as a lowering-friendly representation on the way to a backend (e.g. wasm).

## Why a streaming JSONL IR

JSONL is the boring choice on purpose:

- **Composable**: `cat | sirc | other-tool | jq` works.
- **Tooling-friendly**: you can attach source spans and diagnostics without inventing side channels.
- **Strict + extensible**: the schema stays tight; extra data goes into `ext` instead of “loosening” core records.
- **Stable diffs**: one record per line is easy to compare, filter, and cache.

## Surface language (the thing you write)

The text form is intentionally small:

- `unit … target … +feature` sets compilation context.
- `type … = …`, `const … = …`, `global … = …` declare program objects.
- `fn name(args…) -> ret …` defines functions.
- `let x:T = …` binds SSA-style locals.
- Calls look like `i32.add(a, b)`; typed numeric literals use `3:i32`.
- Control flow can be written in structured form (`return …`) or explicitly with `block` + `term.*`.

You should be able to read the program top-to-bottom without needing to understand the JSONL encoding.

## Build + run

SIRC uses flex/bison. On macOS, prefer the Homebrew versions (Apple’s `/usr/bin/bison` is typically very old):

- `brew install bison flex`

Standalone build (from repo root):

- `cmake -S src/sirc -B build/sirc`
- `cmake --build build/sirc`

Try it:

- `./build/sirc/sirc src/sirc/examples/hello.sir -o /tmp/hello.sir.jsonl`
- `./build/sircc/sircc /tmp/hello.sir.jsonl -o /tmp/hello && /tmp/hello`

Note: the current parser/translator is intentionally minimal (enough for extern calls + “hello world”); the big “Specimen program” below is aspirational and not fully supported yet.

Tip: use `target host` in the `unit` header to let the backend pick the default LLVM host triple.

## Supported subset (current)

This is what we treat as “basic product” right now:

- unit header: `unit <name> target host|<triple> [+feature[:v]]*`
- functions: `extern fn ...`, `fn ... end`, `public`, `let`, `return`
- values: identifiers, `true/false`, string literals
- values: integer literals (`3` and `3:i32`)
- values: float literals (`1.5`, `1.5:f32`, `1e3:f64`) (emitted as deterministic `const.f32/f64` bit-pattern records)
- values: decimal float literals (`1.5` and `1.5:f64` / `1.5:f32`) (emitted as deterministic `const.f32/f64` bit-pattern records)
- calls:
  - mnemonic calls via dotted names (e.g. `i32.add(a,b)`, `alloca.i32()`, `mem.copy(dst,src,len)`)
  - `load.<ty>(addr)` / `store.<ty>(addr, value)` (emits `fields.addr/value`; `align`/`vol` are optional via attributes)
  - `select(<ty>, cond, then, else)` (lowers to SIR `select`)
  - attribute tail (flat, no braces):
    - `+flag` ⇒ `fields.flags.flag=true`
    - `flags key value` ⇒ `fields.flags.key=value` (e.g. `eff.fence() flags mode seqcst`)
    - `key value` ⇒ `fields.key=value` (e.g. `term.trap code "bounds" msg "oob"`)
    - `flags [a, b, c]` ⇒ sugar for `+a +b +c`
    - special keys:
      - `call.indirect(fp, ...) sig <fnname>` emits `call.indirect` with `fields.sig` taken from `<fnname>`’s signature
      - `alloca(type) count <expr>` emits `alloca` with `fields.flags.count` as a node ref

## How lowering works (roughly)

For a given input file, SIRC typically emits:

1. A `meta` record for the unit (name/target/features/producer).
2. `src` records for each span SIRC wants to reference.
3. `type` records for referenced types (e.g. `i32`, pointers, arrays, function signatures).
4. `sym` records for declared symbols (functions/globals/consts).
5. `node` records that form the semantic graph:
   - function nodes
   - block nodes
   - expression nodes (calls, loads/stores, compares, selects)
   - statement/terminator nodes (`return`, `term.br`, `term.cbr`, …)
6. Optional `ext` records for backend/tooling payloads.

### Tiny illustration

This source:

```sir
fn add(a:i32, b:i32) -> i32 public
  return i32.add(a, b)
end
```

becomes (schematically) a stream like:

```jsonl
{"ir":"sir-v1.0","k":"sym","id":10,"name":"add","kind":"fn","linkage":"public"}
{"ir":"sir-v1.0","k":"type","id":20,"kind":"fn","params":[1,1],"ret":1}
{"ir":"sir-v1.0","k":"node","id":30,"tag":"fn","inputs":[{"t":"ref","id":10,"k":"sym"},{"t":"ref","id":20,"k":"type"}]}
{"ir":"sir-v1.0","k":"node","id":31,"tag":"expr.call","fields":{"callee":"i32.add"}}
{"ir":"sir-v1.0","k":"node","id":32,"tag":"stmt.return","inputs":[{"t":"ref","id":31,"k":"node"}]}
```

(The exact IDs/tags/fields are producer-defined details; the important part is that the output is a **typed, referenceable, streaming IR**.)

If you only want a mnemonic-like view for debugging or a particular backend, that belongs either in `node.fields` (when it’s semantic) or in an `ext` record (when it’s tool/backend specific).


## Specimen program

```sir
unit u0 target wasm32 +agg:v1

;; ----- pure arithmetic helpers -----

fn add(a:i32, b:i32) -> i32 public
  return i32.add(a, b)
end

fn sub(a:i32, b:i32) -> i32 public
  return i32.sub(a, b)
end

fn mul(a:i32, b:i32) -> i32 public
  return i32.mul(a, b)
end

fn max_u32(a:i32, b:i32) -> i32 public
  ;; unsigned max: pick a if a >=u b else b
  ;; (uses a compare + value-level select)
  let ge: bool = i32.cmp.uge(a, b)
  return select(i32, ge, a, b)
end

fn clamp_u32(x:i32, lo:i32, hi:i32) -> i32 public
  ;; clamp in unsigned space
  let x1: i32 = max_u32(x, lo)
  let le: bool = i32.cmp.ule(x1, hi)
  return select(i32, le, x1, hi)
end

;; ----- small memory example -----

fn sum2(ptr_base:ptr) -> i32 public
  ;; load two i32s from memory and add
  let x: i32 = load.i32(ptr_base)
  let p1: ptr = ptr.add(ptr_base, 4:i64)
  let y: i32 = load.i32(p1)
  return i32.add(x, y)
end

fn store_add(dst:ptr, a:i32, b:i32) -> i32 public
  ;; compute a+b, store it, also return it
  let r: i32 = i32.add(a, b)
  store.i32(dst, r)
  return r
end

;; ----- structured constants + globals (agg:v1) -----

;; [4 x i32]
type t_arr4_i32 = array(i32, 4)

;; const c_vec = [10, 20, 30, 40]
const c_vec : ^t_arr4_i32 = { kind:"array",
  elems:[ 10:i32, 20:i32, 30:i32, 40:i32 ]
}

global g_vec : ^t_arr4_i32 public = ^c_vec

fn sum_global_vec() -> i32 public
  ;; sum g_vec[0..4)
  let base: ptr = ptr.sym(g_vec)

  let p0: ptr = base
  let p1: ptr = ptr.add(base, 4:i64)
  let p2: ptr = ptr.add(base, 8:i64)
  let p3: ptr = ptr.add(base, 12:i64)

  let a: i32 = load.i32(p0)
  let b: i32 = load.i32(p1)
  let c: i32 = load.i32(p2)
  let d: i32 = load.i32(p3)

  let ab: i32 = i32.add(a, b)
  let cd: i32 = i32.add(c, d)
  return i32.add(ab, cd)
end

;; ----- control flow example -----

fn abs_i32(x:i32) -> i32 public
  ;; if x < 0 return -x else x
  let isneg: bool = i32.cmp.slt(x, 0:i32)
  let nx: i32 = i32.neg(x)
  return select(i32, isneg, nx, x)
end

fn gcd_u32(a:i32, b:i32) -> i32 public
  ;; Euclid loop using blocks + term.cbr / term.br
  block entry
    term.br to loop, args:[a, b]
  end

  block loop(x:i32, y:i32)
    let y0: bool = i32.eqz(y)
    term.cbr cond:y0,
      then:{ to done, args:[x] },
      else:{ to step, args:[x, y] }
  end

  block step(x:i32, y:i32)
    ;; r = x % y (total form to avoid trap)
    let r: i32 = i32.rem.u.sat(x, y)
    term.br to loop, args:[y, r]
  end

  block done(r:i32)
    term.ret value:r
  end
end

;; ----- “main” style entry -----

fn main(x:i32, y:i32) -> i32 public
  ;; use a few helpers
  let s: i32 = add(x, y)
  let p: i32 = mul(s, 3:i32)
  let g: i32 = gcd_u32(p, 100:i32)
  return clamp_u32(g, 0:i32, 1000:i32)
end
```
