<!-- SPDX-FileCopyrightText: 2025 Frogfish -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# SIR 1.0 (Semantic IR, JSONL)

SIR is the **stable boundary format** between:

- **Front-ends** (parsers / lowerers) that translate a source language into a *language-agnostic* semantic representation.
- **Back-ends** (optimizers / code generators / lowerers) that translate that representation into a target (WASM, LLVM IR, native, another IR, …).

SIR is a **streaming, line-delimited JSON format (JSONL)**:

- **One JSON object per line**.
- Each line is a **record**.
- Each record is validated by **`record.schema.json`**.

Version: **`sir-v1.0`**.

---

## Why SIR exists

Lowering directly from a high-level language into assembly (or assembly-like IR) is painful:

- you lose semantics early,
- optimization turns into pattern-matching,
- tooling gets brittle (source mapping, diagnostics, indexing, refactoring).

SIR is a **mid-point**:

- Still semantic enough to be analyzable and optimizable.
- Flat/regular enough to stream, diff, grep, and pipe between tools.
- Strict enough to be a *contract* (schema-validated).

Think of SIR as **“semantic assembly”**:

- structured enough for compilers,
- simple enough for tooling,
- and (as it turns out) not terrible for humans.

---

## Files

- `record.schema.json` — JSON Schema for **one JSONL record**.
  A `.sir.jsonl` file is valid if **every line** parses as JSON and validates against this schema.

---

## The shape of every record (normative)

Every record MUST include:

- `ir: "sir-v1.0"`
- `k`: the record kind tag

SIR v1.0 is a **closed schema**: unknown fields are rejected (unless the schema says otherwise).

---

## Big idea: a stream of typed records

SIR is not “one giant JSON document”. It is:

- **a stream of independent records**,
- that can be consumed incrementally,
- and stitched into a graph using **stable numeric IDs**.

That makes it suitable for:

- compiler passes that don’t need the whole program in memory,
- indexers that build symbol/type maps while reading,
- linters and diagnostics that can report errors early,
- log-friendly debugging.

---

## Record kinds (v1.0)

SIR defines two layers that can co-exist in the same stream:

### 1) Tooling / envelope records

These do not change program semantics; they exist to make compilers usable.

- `meta` — stream metadata (producer, unit, build options, feature flags)
- `src` — source text anchoring (for diagnostics and round-tripping)
- `diag` — diagnostics (info/warn/error) with optional source / node references
- `ext` — explicit extensions (debug/tooling data, experiments) without loosening validation

### 2) Semantic graph records

These carry semantic identity and form the IR graph.

- `sym` — symbol identity (functions, vars, consts, types, params, fields, labels)
- `type` — type nodes (prim/ptr/array/fn/struct/union/enum/…)
- `node` — semantic nodes (the “everything else” graph: blocks, statements, expressions, declarations, patterns, …)

### 3) Low-level / mnemonic interop records (optional)

These exist for “assembly-like” views, debugging, or as a lowering target.

- `label` — legacy label record
- `instr` — instruction record (`m` mnemonic + `ops` operands)
- `dir` — directive record (data/layout/backend interop)

A toolchain may emit only a subset, but any record that appears MUST validate.

---

## Identity: stable numeric IDs (not source spans)

SIR’s semantic identity is **numeric and explicit**.

- Semantic identity records (`sym`, `type`, `node`) carry an integer `id`.
- Other records refer to them using a **typed reference object**:

```json
{"t":"ref","id":123,"k":"node"}
```

This is deliberate:

- **Source spans are not identity.** `src` / `loc` describe *where it came from*, not *what it is*.
- **Names are not identity.** Names can collide, change, or be absent.

### Practical consequence

If you want tools to reliably say “this is the same symbol/type/node” as the program evolves, you should:

- use `id` + `NodeRef` as the *truth*,
- and treat `src_ref` / `loc` as *debug provenance*.

---

## Streaming and ordering

SIR is designed to be read in a single forward pass.

Recommended conventions:

- Emit a `meta` record first.
- Emit `src` records early so later nodes can point at them.
- Emit `sym` / `type` / `node` records before their first use when possible.

The schema allows forward references, but **producers SHOULD avoid them** when streaming.

---

## Source mapping (`src` + `loc`) and diagnostics (`diag`)

### `src` records

A `src` record creates a stable *anchor* you can reference later:

- `id` — unique source anchor ID
- `file`, `line`, `col`, `end_line`, `end_col`, `text` — optional slice metadata

Later records can attach:

- `src_ref: <id>`
- `loc: { line, col, unit }`

### `diag` records

Diagnostics are first-class records:

- `level`: `info | warn | error`
- `msg`: human message
- optional `src_ref` / `loc`
- optional `about`: a typed reference to the symbol/type/node the diagnostic is about

This is what makes SIR good for:

- compilers,
- IDE integration,
- build tooling,
- and rich error reporting.

---

## Types (`type`)

`type` records represent a language-agnostic type graph.

The important part is: **types have IDs**, and other things reference them.

Examples:

- primitive type: `kind: "prim"`, `prim: "i32"`
- pointer type: `kind: "ptr"`, `of: <type id>`
- function type: `kind: "fn"`, `params: [...]`, `ret: <type id>`
- aggregate types: `struct` / `union` / `enum` / `array`

SIR intentionally keeps the type schema permissive so different languages can map into it.

---

## Symbols (`sym`)

`SymRecord` is the place where “names become identity”.

- `id`: stable identity for a symbol within the stream
- `name`: human name
- `kind`: `fn | var | const | type | param | field | label`
- optional `linkage`: `local | public | extern`
- optional `type_ref`: links symbol to a `type`
- optional `value`: for `const`

Symbols are how you build:

- symbol tables,
- export/import maps,
- debug info,
- and stable references for tooling.

---

## Nodes (`node`): the semantic graph

The `node` record is the general semantic building block.

- `id`: stable identity
- `tag`: semantic tag (examples: `fn`, `block`, `stmt.return`, `expr.call`, `expr.binop`, …)
- optional `type_ref`: type of the node (especially expressions)
- optional `inputs`: dependencies (as `NodeRef`)
- optional `fields`: tag-specific payload (small, stable keys; put big/debuggy things in `ext`)

This is the core trick:

> **SIR does not hardcode every AST variant into the top-level schema.**
> Instead, it defines a strict *container* (`node`) and lets `tag + fields + inputs` express the graph.

That keeps v1.0 stable while still letting the IR grow.

---

## Values and operands (`Value`)

Across SIR records you’ll see a small set of typed leaf values:

- `sym` / `lbl` / `reg` — named references in low-level contexts
- `num` / `str` — literals
- `mem` — address-like operand (base + disp + size)
- `ref` — semantic reference to a `sym`/`type`/`node` by numeric ID

Important: **semantic references use `ref`**.

The other operand forms are mostly for low-level interop and readability.

---

## Extensions (`ext`): grow without breaking validation

If you need to attach extra tool/debug data, you do it explicitly:

- `k: "ext"`
- `name`: extension identifier (namespace-friendly)
- `payload`: arbitrary JSON object
- optional `about`: reference to what it describes (`sym`/`type`/`node`)

This is how you add “bells and whistles” while keeping the core schema closed.

---

## Low-level mnemonics (`instr` / `dir` / `label`)

SIR can carry a low-level “mnemonic stream” view:

- `instr`: `m` mnemonic + `ops` operands
- `dir`: directives (data/layout/backend interop)
- `label`: local labels

This is useful when:

- you lower SIR into a target-specific instruction stream,
- you want a debug print that looks like assembly,
- you need a canonical interchange format for a late-stage backend.

You can treat this as **one possible payload** that lives inside the same streaming envelope.

---

## Typical pipeline

A realistic toolchain often looks like:

1. **Parse** source → AST/CST
2. **Lower** AST → SIR semantic graph (`sym`/`type`/`node`)
3. **Analyze / optimize** in semantic space
4. **Lower** semantic graph → mnemonic stream (`instr`/`dir`/`label`) or directly to a target IR
5. **Codegen** to the final artifact

Throughout the pipeline, emit:

- `src` anchors
- `diag` records
- `ext` records for debug/tooling

This is what makes the build pipeline observable and debuggable.

---

## Contract and versioning

- If it validates against `record.schema.json`, it is SIR 1.0.
- Meaning-changing changes require a new `sir-vX.Y`.
- Additive evolution should prefer:
  - new `node.tag` values,
  - new optional fields,
  - and `ext` records.

Consumers MUST reject unknown `ir` versions.

---

## Example: tiny semantic stream

Below is a tiny stream that introduces a type, a function symbol, and a node that represents a function body.

```jsonl
{"ir":"sir-v1.0","k":"meta","producer":"sir-demo","unit":"example","ext":{"target":"wasm32"}}

{"ir":"sir-v1.0","k":"type","id":1,"kind":"prim","prim":"i32"}

{"ir":"sir-v1.0","k":"sym","id":10,"name":"add","kind":"fn","linkage":"public","type_ref":1}

{"ir":"sir-v1.0","k":"node","id":20,"tag":"fn","type_ref":1,
 "inputs":[{"t":"ref","id":10,"k":"sym"}],
 "fields":{"name":"add","params":["a","b"],"body":"..."}}
```

Notes:

- The schema does not force a single canonical AST; the `node` tag + fields carry the shape.
- For richer structure (blocks/exprs), you typically introduce more `node` records and connect them by `inputs`/`ref`s.

---

## Example: tiny mnemonic stream

```jsonl
{"ir":"sir-v1.0","k":"label","name":"L0"}
{"ir":"sir-v1.0","k":"instr","m":"i32.add","ops":[{"t":"reg","v":"a"},{"t":"reg","v":"b"}]}
{"ir":"sir-v1.0","k":"instr","m":"ret","ops":[{"t":"reg","v":"$0"}]}
```

---

## Practical guidance

### For producers

- Choose a stable `id` allocation strategy (monotonic integers are fine).
- Emit `src` and `loc` aggressively; it pays off immediately in diagnostics.
- Use `sym` for anything you want tooling to “remember”.
- Use `ext` rather than sprinkling ad-hoc fields into core records.

### For consumers

- Treat the stream as authoritative.
- Build maps keyed by numeric `id`.
- Accept that SIR can be partially present (some producers emit only a subset).
- Prefer semantic `ref` links over name/spans when reasoning about identity.

---

## Non-goals

- SIR is not a UI format; it is an interchange contract.
- SIR does not try to encode every source-language surface feature directly.
- SIR does not promise cross-build stable IDs unless a producer chooses to.

If you want a human-friendly syntax, compile it *to* SIR JSONL.
