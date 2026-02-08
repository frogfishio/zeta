# `sirc` reference (v1.0)

`sirc` is a small, line-oriented “sugar” language (`.sir`) that emits `sir-v1.0` JSONL.

Design goals:
- 1 source line ≈ 1 JSONL record (reviewable + greppable)
- deterministic output (suitable for golden fixtures)
- a small surface that can express the SIR constructs needed for `sircc` + `sem`

This document is the producer-facing contract for authoring `.sir`.

---

## CLI

Common modes:
- Compile one file: `sirc <input.sir> [-o <out.sir.jsonl>]`
- Tool mode (multi-file): `sirc --tool -o <out.jsonl> <a.sir> <b.sir> ...`
- Lint only: `sirc --lint <input.sir>`

Diagnostics:
- `--diagnostics text|json` (default: `text`)
- `--all` collect multiple errors
- Exit codes: `0` ok, `1` compile error, `2` tool error (I/O, OOM, internal)

IDs and source mapping:
- `--ids string|numeric` (default: `string`)
- `--emit-src none|loc|src_ref|both` (default: `loc`)

Frontend hygiene:
- `--strict` rejects “ignored” attribute tails on known constructs and enforces arity for a conservative subset (see below).

Support table:
- `sirc --print-support [--format text|json]`

---

## File shape

Every file starts with a unit header:

```sir
unit <unit_name> target host|<triple> [+feature[:v]]*
```

Examples:

```sir
unit hello target host
unit u0 target wasm32 +sem:v1 +adt:v1
```

`target host` means “use the backend default host triple”.

---

## Types

Primitive types:
- `i8 i16 i32 i64`
- `f32 f64`
- `bool`
- `void`
- `ptr` (sugar for `^i8`)

Derived types:
- Pointers: `^T`
- Arrays: `array(T, N)`
- Function signatures: `fn(T, ...) -> R`
- Pack types:
  - `fun(Sig)` (pack `fun:v1`)
  - `closure(CallSig, EnvTy)` (pack `closure:v1`)
  - `sum{None, Some:T, ...}` (pack `adt:v1`)

Type aliases:

```sir
type Byte = i8
type PByte = ^Byte
```

---

## Values and literals

- Identifiers: `x`, `my_fn`, `i32.add` (dotted names used for mnemonics)
- Booleans: `true`, `false`
- Strings: `"hello"` (emits a SIR `cstr` node)
- Integers: `123` defaults to `i32`
- Typed integers: `123:i64`, `0:i8`, etc
- Floats: `1.5` defaults to `f64`
- Typed floats: `1.5:f32`, `2.0:f64`

---

## Functions

Function definition:

```sir
fn name(a:i32, b:i32) -> i32 public
  return i32.add(a, b)
end
```

Extern declaration (for C/host imports):

```sir
extern fn zi_write(fd:i32, buf:ptr, len:i64) -> i64
```

Notes:
- `public` marks an exported symbol.
- Locals are SSA-like; mutation is explicit via `alloca` + `load.*`/`store.*`.

---

## Statements

Let-binding:

```sir
let x:i32 = i32.add(1:i32, 2:i32)
```

Return:

```sir
return x
return 0:i32
```

Expression statement (emit a node record, discard the value):

```sir
eff.fence() +mode=seqcst
```

---

## Calls (“mnemonics”) and attribute tails

### Calls

Mnemonic-style call:

```sir
i32.add(a, b)
mem.copy(dst, src, len)
```

Typed mnemonic call (forces the resulting node `type_ref`):

```sir
call.fun(f, x) as i32
```

Direct function call:

```sir
fn add2(a:i32, b:i32) -> i32 public
  return i32.add(a, b)
end

fn main() -> i32
  return add2(1:i32, 2:i32)
end
```

Indirect call:

```sir
extern fn add_sig(a:i32, b:i32) -> i32

fn main() -> i32
  let fp:ptr = ptr.sym(add2)
  let r:i32 = call.indirect(fp, 1:i32, 2:i32) sig add_sig
  return r
end
```

### Attribute tails (flat, no braces)

Attribute tails are a sequence of items appended to a call or terminator line:

- `+flag` → boolean flag in `fields.flags` (value `true`)
- `+key=<scalar>` → keyed scalar in `fields.flags` (recommended for anything “flags-like”)
- `key=<scalar>` → scalar field in `fields[key]`
- `flags key <scalar>` → legacy spelling for forcing a keyed scalar into `fields.flags`
- `flags [a, b, c]` → sugar for `+a +b +c`

Examples:

```sir
mem.copy(dst, src, len) +alignDst=1 +alignSrc=1 +overlap=disallow
eff.fence() +mode=seqcst
term.trap code="bounds" msg="oob"
```

Special keys (supported):
- `call.indirect(... ) sig <fnname>` (mandatory for `call.indirect`)
- `alloca(T) count <expr>` (optional count tail for `alloca`)

### `--strict` rules (high level)

`--strict` is intended to prevent “silent ignore” footguns.

In strict mode:
- rejects unsupported/ignored tail keys like `sig`/`count` on generic mnemonics
- rejects unknown attrs/flags on known constructs with tight schemas (`alloca`, `load.*`, `store.*`, `call.indirect`)
- enforces arg count for:
  - `ptr.sym`, `mem.copy`, `mem.fill`
  - a conservative subset of `i8/i16/i32/i64` ops (`add/sub/mul/...`, comparisons, unary ops)
  - direct calls and `call.indirect` against declared signatures

If a mnemonic isn’t in the strict arity table, strict mode does not enforce its arity (by design).

---

## Memory helpers

Stack allocation:

```sir
let p:ptr = alloca(i32) count 4:i64 +align=16 +zero
```

Typed load/store:

```sir
let x:i32 = load.i32(p) align=4 vol=true
store.i32(p, x) align=4
```

---

## CFG form (explicit blocks + terminators)

Use CFG when you want explicit control flow.

Function in CFG form:

```sir
fn main() -> i32 public
  block entry
    term.br to then
  end

  block then
    term.ret value: 0:i32
  end
end
```

Terminators:
- `term.br to <block> [args: [...]]`
- `term.cbr cond: <expr>, then: <block> [args], else: <block> [args]`
- `term.switch scrut: <expr>, cases: [...], default: <block>`
- `term.ret [value: <expr>]`
- `term.unreachable ...attrs`
- `term.trap ...attrs`

---

## `sem:v1` sugar (intentful constructs)

These constructs are “high-level intent” nodes that `sircc` can lower deterministically to SIR-Core CFG.

Branch operands:
- `val <expr>`: eager value
- `thunk <expr>`: a callable (`fun`/`closure`) that returns the value when invoked

### `sem.if` / `sem.cond`

```sir
let x:i32 = sem.if(true, val 1:i32, val 2:i32) as i32
let y:i32 = sem.cond(c, thunk f_then, thunk f_else) as i32
```

### Short-circuit

```sir
let b:bool = sem.and_sc(lhs, thunk rhs) 
let c:bool = sem.or_sc(lhs, thunk rhs)
```

### Switch / match

```sir
let x:i32 = sem.switch(scrut, cases: [
  case 0:i32 -> val 10:i32
], default: val 0:i32) as i32
```

```sir
let x:i32 = sem.match_sum(MySumTy, scrut, cases: [
  { variant: 0, body: val 1:i32 }
], default: val 0:i32) as i32
```

### Loop control + scope/defer

```sir
sem.while(thunk cond_fn, thunk body_fn)
sem.break
sem.continue
sem.defer(thunk cleanup_fn)
sem.scope(defers: [thunk d1, thunk d2], body: do
  ;; body statements here
end)
```

Note: `sem.scope(..., body: do ... end)` emits a *structural* `block` node (not a CFG block).

---

## Examples

See:
- `src/sirc/examples/*.sir` (authoring examples)
- `src/sirc/tests/golden/*.sir.jsonl` (exact expected emission)
- `src/sirc/tests/fixtures/*.sir` (negative diagnostics, incl. `--strict`)
