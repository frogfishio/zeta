<!-- SPDX-FileCopyrightText: 2025 Frogfish -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# SIR 1.0 (Semantic IR, JSONL)

This directory defines the **stable pipeline boundary** for *declarative compilers*:

- A **front-end** (parser + lowerer) reads a source language and emits **SIR** as **JSONL** (**one JSON object per line**).
- A **back-end** (lowerer/codegen) reads **SIR JSONL** and produces the next stage (e.g. ZIR, LLVM IR, WAT, native code, etc.).

Version: **sir-v1.0** (stable contract).

SIR is intentionally:

- **Language-agnostic**: it models common semantics (decls/stmts/exprs/types) without baking in any one surface syntax.
- **Streaming-friendly**: JSONL enables incremental tools (indexers, linters, IDEs) and easy piping.
- **Deterministic**: record shapes are normative; if it is not in the schema, it is invalid.

## Files

- `record.schema.json` — JSON Schema for **one JSONL record**.
  A `.sir.jsonl` file is valid if **every line** parses as JSON and validates against this schema.

## Record model (normative)

Every record MUST include:

- `ir: "sir-v1.0"`
- `k`: the record kind tag

SIR is a *typed, reference-based* IR. Nodes are introduced by records that carry an integer `id`, and other records may refer to them using typed references.

### Core kinds

The v1.0 contract defines these top-level record kinds:

- `meta` — stream metadata (producer, unit, options)
- `src` — source text anchoring (for diagnostics and round-tripping)
- `diag` — diagnostics (info/warn/error) with optional source references
- `sym` — symbol identity (optional for v1, recommended)
- `type` — type nodes
- `decl` — declarations (functions, variables, type decls)
- `stmt` — statements
- `expr` — expressions
- `block` — statement lists / structured bodies

A toolchain MAY emit only a subset (e.g. no `sym` in an MVP), but any emitted record MUST validate.

### IDs and references

- Records that define nodes (e.g. `type`, `decl`, `stmt`, `expr`, `block`, `sym`, `src`) use an integer `id` that MUST be unique within the stream.
- References are typed objects:
  - `{"t":"type","id":12}`
  - `{"t":"expr","id":40}`
  - `{"t":"decl","id":7}`
  - `{"t":"sym","id":3}`

Implementations SHOULD emit a referenced node **before** the first record that references it.

## Conventions

These are recommendations for producers/consumers; the schema is the source of truth.

- **Streaming**: emit `meta` first when possible.
- **Ordering**: emit `src` records early so later nodes can point at them.
- **Locations**:
  - `loc` is optional but strongly recommended for good errors.
  - `src_ref` should reference a prior `src` record `id` when exact slices matter.
- **Unknown fields**: v1.0 schemas are *closed*; unknown fields SHOULD be rejected.
- **Stability**: changing meaning without a version bump is forbidden.

## Minimal semantics in v1.0

SIR 1.0 is intentionally conservative:

- Expressions cover a small, composable core (names, literals, calls, binops, assigns, etc.).
- Statements cover structured control flow (block/if/while/return/expr-stmt).
- Types cover a minimal set (prim/pointer/function) sufficient to describe many languages.

Higher-level or language-specific features should be lowered *into* this core or introduced in a new SIR version.

## Extension policy

- Any SIR 1.0 stream must validate against `record.schema.json`.
- Additive evolution (new record kinds, new enum values, new optional fields) requires a **schema update** and typically a **version bump**.
- Consumers MUST reject unknown `ir` versions.

## Example record stream

Below is a tiny SIR program equivalent to:

```text
fn add(a: i32, b: i32) -> i32 { return a + b; }
```

```jsonl
{"ir":"sir-v1.0","k":"meta","producer":"sir-demo","unit":"example"}

{"ir":"sir-v1.0","k":"type","id":1,"kind":"prim","name":"i32"}

{"ir":"sir-v1.0","k":"decl","id":10,"kind":"param","name":"a","ty":{"t":"type","id":1}}
{"ir":"sir-v1.0","k":"decl","id":11,"kind":"param","name":"b","ty":{"t":"type","id":1}}

{"ir":"sir-v1.0","k":"expr","id":20,"kind":"name","name":"a"}
{"ir":"sir-v1.0","k":"expr","id":21,"kind":"name","name":"b"}
{"ir":"sir-v1.0","k":"expr","id":22,"kind":"binop","op":"add","lhs":{"t":"expr","id":20},"rhs":{"t":"expr","id":21}}

{"ir":"sir-v1.0","k":"stmt","id":30,"kind":"return","value":{"t":"expr","id":22}}
{"ir":"sir-v1.0","k":"block","id":40,"stmts":[{"t":"stmt","id":30}]}

{"ir":"sir-v1.0","k":"decl","id":50,"kind":"fn","name":"add","params":[{"t":"decl","id":10},{"t":"decl","id":11}],"ret":{"t":"type","id":1},"body":{"t":"block","id":40}}
```

## Practical guidance

- **Front-ends**: Lower surface CST/AST into SIR by emitting stable core nodes and recording `loc`/`src_ref` for traceability.
- **Back-ends**: Consume SIR as a graph of nodes (IDs + typed refs) and run normalization passes before lowering to a target IR.
- **Tooling**: JSONL enables grep-friendly debugging and streaming diagnostics.

