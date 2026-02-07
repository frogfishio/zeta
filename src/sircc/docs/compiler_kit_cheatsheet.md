# SIR “compiler kit” cheat sheet (v1.0 as implemented by `sircc`)

This is a **producer-facing dictionary**: “what vocabulary exists” + “what JSONL shape to emit”.

It is intentionally **implementation-realistic**: if `sircc` doesn’t accept / lower it today, it is not presented here as supported.

Related docs:
- `src/sircc/docs/src.md` (sircc CLI + semantics overview)
- `src/sircc/docs/ast_to_sir_cookbook.md` (end-to-end AST→SIR patterns)
- `src/sircc/docs/data_v1.md` (string/bytes baseline conventions)

---

## IDs and references (don’t fight the stream)

### Record ids (`id`)

For `type` and `node` records, `id` may be:
- an integer `>= 0`, or
- a non-empty string

They do **not** need to be sequential.

Practical recommendation (robust to injection/instrumentation):
- Use **string ids** with namespaces, e.g. `"t:i32"`, `"n:main:entry"`, `"n:lit:hello"`.

### Ref values (`{"t":"ref","id":...}`)

Many fields use a *ref-value*:

```json
{"t":"ref","id":"n:some_id"}
```

This references a prior (or same-unit resolvable) semantic id in the `sym` / `type` / `node` space.

You may also include a kind hint:

```json
{"t":"ref","k":"type","id":"t:i32"}
```

---

## Primitive types (LLVM backend)

`sircc`’s LLVM lowering supports these `type.kind:"prim"` spellings:

- `bool` (alias of `i1`)
- `i1`
- `i8`, `i16`, `i32`, `i64`
- `f32`, `f64`
- `void` (only meaningful as a function return type)

There are **no unsigned primitive types** (`u8/u16/u32/u64`) in `sircc` today.

### Example: define primitives

```json
{"ir":"sir-v1.0","k":"type","id":"t:bool","kind":"prim","prim":"bool"}
{"ir":"sir-v1.0","k":"type","id":"t:i32","kind":"prim","prim":"i32"}
{"ir":"sir-v1.0","k":"type","id":"t:i64","kind":"prim","prim":"i64"}
{"ir":"sir-v1.0","k":"type","id":"t:f64","kind":"prim","prim":"f64"}
```

### “u32” / “char32” today (recommended producer strategy)

If your source language has a distinct `u32` type, you have two workable options:

1) **Nominal alias** (type identity distinct in your pipeline; ABI is still i32):
```json
{"ir":"sir-v1.0","k":"type","id":"t:u32","kind":"prim","prim":"i32","name":"u32"}
```

2) Keep the type as `i32`, but use **unsigned mnemonics** where needed (e.g. unsigned compares).

Either way, the contract is:
- representation is `i32`
- semantics are controlled by which mnemonics you emit (signed vs unsigned variants)

---

## Derived types (type records)

### Pointers (`ptr`)

```json
{"ir":"sir-v1.0","k":"type","id":"t:p_i8","kind":"ptr","of":"t:i8"}
```

Notes:
- pointer width comes from the selected target triple
- do not invent `prim:"ptr"`; pointers are `kind:"ptr"`

### Arrays (`array`)

Fixed-length arrays:

```json
{"ir":"sir-v1.0","k":"type","id":"t:arr_16_i8","kind":"array","of":"t:i8","len":16}
```

### Structs (`struct`)

```json
{"ir":"sir-v1.0","k":"type","id":"t:pair","kind":"struct","name":"pair","fields":[
  {"name":"a","type_ref":"t:i32"},
  {"name":"b","type_ref":"t:i32"}
]}
```

Layout:
- is derived from the selected target ABI/layout (LLVM)
- if you need deterministic layout across machines, pin the target triple/cpu/features via `meta.ext.target.*` and treat that as part of your build contract

### Vectors (`vec`)

```json
{"ir":"sir-v1.0","k":"type","id":"t:v4i32","kind":"vec","lane":"t:i32","lanes":4}
```

LLVM ABI note:
- `vec(bool, N)` is represented as `<N x i8>` (0/1) rather than `<N x i1>` for determinism.

### Function signatures (`fn`)

A function *type*:

```json
{"ir":"sir-v1.0","k":"type","id":"t:puts_sig","kind":"fn","params":["t:p_i8"],"ret":"t:i32"}
```

If you need C varargs:

```json
{"ir":"sir-v1.0","k":"type","id":"t:printf_sig","kind":"fn","params":["t:p_i8"],"ret":"t:i32","varargs":true}
```

### Callable values (`fun`, `closure`) and sums (`sum`)

These exist as type kinds in `sircc` and are used by the packs:
- `fun:v1`
- `closure:v1`
- `adt:v1` (sum types)

If you’re producing SIR directly from AST, prefer using the pack mnemonics rather than “peeking” at representations.

---

## Constants and literals (node records)

### Integer constants (`const.i*`)

```json
{"ir":"sir-v1.0","k":"node","id":"n:zero","tag":"const.i32","type_ref":"t:i32","fields":{"value":0}}
{"ir":"sir-v1.0","k":"node","id":"n:one64","tag":"const.i64","type_ref":"t:i64","fields":{"value":1}}
```

### Boolean constants (`const.bool`)

`fields.value` is `0` or `1`:

```json
{"ir":"sir-v1.0","k":"node","id":"n:t","tag":"const.bool","type_ref":"t:bool","fields":{"value":1}}
{"ir":"sir-v1.0","k":"node","id":"n:f","tag":"const.bool","type_ref":"t:bool","fields":{"value":0}}
```

### Float constants (`const.f32` / `const.f64`)

Use IEEE-754 bits (lossless):

```json
{"ir":"sir-v1.0","k":"node","id":"n:pi","tag":"const.f64","type_ref":"t:f64","fields":{"bits":"0x400921fb54442d18"}}
```

### C string literals (`cstr`)

`cstr` produces a pointer to a NUL-terminated byte string constant.

```json
{"ir":"sir-v1.0","k":"node","id":"n:lit","tag":"cstr","fields":{"value":"hello\\n"}}
```

Notes:
- `fields.value` is a JSON string; the byte sequence is whatever UTF-8 the JSON parser yields.
- `sircc` appends a trailing `\\0` (C-string semantics).
- Under `data:v1` + `--verify-strict`, `cstr` nodes must set `type_ref` to the canonical `cstr` type (see below).

---

## `data:v1`: bytes + UTF-8 strings (encoding story)

If you need a portable, enforced “string/bytes story”, enable `data:v1`:

```json
{"ir":"sir-v1.0","k":"meta","ext":{"features":["data:v1"]}}
```

Then you MUST define these canonical types:

### Canonical types (required shapes)

```json
{"ir":"sir-v1.0","k":"type","id":"p:i8","kind":"prim","prim":"i8"}
{"ir":"sir-v1.0","k":"type","id":"p:i64","kind":"prim","prim":"i64"}
{"ir":"sir-v1.0","k":"type","id":"p:cstr","kind":"ptr","name":"cstr","of":"p:i8"}
{"ir":"sir-v1.0","k":"type","id":"p:ptr_i8","kind":"ptr","of":"p:i8"}

{"ir":"sir-v1.0","k":"type","id":"p:bytes","kind":"struct","name":"bytes","fields":[
  {"name":"data","type_ref":"p:ptr_i8"},
  {"name":"len","type_ref":"p:i64"}
]}

{"ir":"sir-v1.0","k":"type","id":"p:string.utf8","kind":"struct","name":"string.utf8","fields":[
  {"name":"data","type_ref":"p:ptr_i8"},
  {"name":"len","type_ref":"p:i64"}
]}
```

Meaning:
- `bytes` is a byte slice (`len` is bytes)
- `string.utf8` is a UTF-8 byte slice (`len` is bytes; may contain `0`)
- `cstr` is a pointer to NUL-terminated bytes (the NUL convention is not validated)

### Under `data:v1`, “encoding” is carried by the canonical type name

There is no extra “encoding tag” in the stream: the name `string.utf8` is the encoding contract.

### Interop guidance

- Prefer ABIs that take `(ptr(i8), len)` (e.g. `zi_write(fd, buf, len)`).
- Only use `cstr` for ABIs that require NUL-termination (e.g. `puts`).
- Converting `string.utf8` → `cstr` is explicit (allocate `len+1`, copy, store trailing `0`).

---

## Minimal “Hello world” pattern (extern call)

This is the smallest useful pattern showing types + `decl.fn` + `cstr` + `call.indirect`:

```json
{"ir":"sir-v1.0","k":"meta","unit":"hello_world_puts"}

{"ir":"sir-v1.0","k":"type","id":"t:i8","kind":"prim","prim":"i8"}
{"ir":"sir-v1.0","k":"type","id":"t:p_i8","kind":"ptr","of":"t:i8"}
{"ir":"sir-v1.0","k":"type","id":"t:i32","kind":"prim","prim":"i32"}
{"ir":"sir-v1.0","k":"type","id":"t:puts_sig","kind":"fn","params":["t:p_i8"],"ret":"t:i32"}
{"ir":"sir-v1.0","k":"type","id":"t:main_sig","kind":"fn","params":[],"ret":"t:i32"}

{"ir":"sir-v1.0","k":"node","id":"n:puts","tag":"decl.fn","type_ref":"t:puts_sig","fields":{"name":"puts"}}
{"ir":"sir-v1.0","k":"node","id":"n:s","tag":"cstr","fields":{"value":"hello world\\n"}}
{"ir":"sir-v1.0","k":"node","id":"n:call","tag":"call.indirect","type_ref":"t:i32","fields":{
  "sig":{"t":"ref","id":"t:puts_sig"},
  "args":[{"t":"ref","id":"n:puts"},{"t":"ref","id":"n:s"}]
}}

{"ir":"sir-v1.0","k":"node","id":"n:rc","tag":"const.i32","type_ref":"t:i32","fields":{"value":0}}
{"ir":"sir-v1.0","k":"node","id":"n:ret","tag":"term.ret","fields":{"value":{"t":"ref","id":"n:rc"}}}
{"ir":"sir-v1.0","k":"node","id":"n:entry","tag":"block","fields":{"stmts":[{"t":"ref","id":"n:call"},{"t":"ref","id":"n:ret"}]}}
{"ir":"sir-v1.0","k":"node","id":"n:main","tag":"fn","type_ref":"t:main_sig","fields":{"name":"main","params":[],"body":{"t":"ref","id":"n:entry"}}}
```

Verifier rule (important for integrators):
- if you want to call an external symbol, declare it with `decl.fn` (don’t rely on `ptr.sym` to name an undeclared symbol).
