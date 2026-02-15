# Semantic IR v0 contract (sem2sir input)

This document defines the **Semantic IR v0** that `sem2sir` accepts **as input for SIR emission**.

Note: `sem2sir` also has a `--check` mode that acts as a broader structural validator for Stage4-shaped inputs (including some untyped forms used in fixtures). This document is the contract for the *typed* subset used by `--emit-sir`.

It is written to prevent a recurring failure mode:

- treating `sem2sir` as “a backend for language X”,
- pushing language-specific meaning into the lowerer,
- or recovering meaning later from token text/lexemes.

**Semantic IR v0 is not a surface language.**
It is the *post-meaning* representation a compiler-compiler pipeline should produce after parsing and semantic commitment.

---

## What this is

Semantic IR v0 is a **closed-world JSONL** IR with:

- a fixed set of node constructors (unknown constructors are rejected),
- explicit operator IDs (semantic, not punctuation),
- explicit type IDs / type policy (no inference),
- enough structure for a backend emitter to generate SIR (and later, other targets).

`sem2sir` is a **consumer** of this IR and enforces the contract.

## Why this exists

The compiler-compiler goal is that *surface languages are stressors*, not first-class special cases.

To achieve this, we need an immutable meaning boundary:

- meaning is committed **before** the lowerer,
- the lowerer only consumes committed meaning,
- adding a new language should primarily mean:
  - new grammar/spec/pack,
  - and/or new front-end lowering passes,
  - **not** teaching the lowerer new operator spellings or special cases.

That’s why this contract is closed, strict, and intentionally boring.

---

## Non-negotiable rules

### 1) Closed world

- Every AST node is an object beginning with key `"k"`.
- `k` must be one of the constructors listed in this document.
- Unknown constructors are a hard error.

### 2) No semantic recovery from token text

- Operator meaning MUST NOT be derived from token text like `"+"`, `"DIV"`, etc.
- Type meaning MUST NOT be inferred from token text beyond the explicit builtin-type dictionary / policy.
- Token text is allowed only as a *witness* (identifier names, literals, link names, etc.).

### 3) No defaults / no implicitness

- Required fields must appear.
- Optional policies must be explicitly committed in `meta.types` if used.
- `diagnostics` must be `[]`: Semantic IR is assumed semantically valid.

### 4) Deterministic, stable shapes

- Field presence is structural contract.
- “Meaningful” strings are stable semantic IDs (e.g. `core.add`), not user-chosen ad-hoc tags.

---

## File format

Semantic IR v0 is emitted as **JSON Lines** (JSONL): one top-level JSON object per line.

Each line is a single semantic unit:

```json
{"diagnostics":[],"meta":{"types":{...}},"ast":{...}}
```

Top-level keys:

- `diagnostics`: array (must be empty)
- `meta`: object (must contain `types`)
- `ast`: object (root semantic node)

Notes:

- Additional top-level keys may exist (for forward compatibility), but `sem2sir` may ignore or reject them depending on strictness.

---

## `meta` contract

### `meta.types`

`meta.types` is a required object mapping **policy keys** to **normalized builtin type IDs**.

- Values must be one of the builtin type IDs recognized by `sem2sir`.
- This is not inference: it is an explicit, committed language rule.

Recognized policy keys (v0):

- `"@default.int"` (alias: `"__default_int"`)
  - permitted values: `"i32"` or `"i64"`
  - meaning: default type for integer literals when a context does not force a width
- `"@default.ptr.pointee"` (alias: `"__default_ptr_pointee"`)
  - permitted values: a load/store-capable *non-pointer* builtin type (v0 MVP set)
  - meaning: default pointee type for raw pointers when omitted upstream

Important: policies are allowed to exist even if your language never uses them.
What is forbidden is silently *assuming* a policy when it isn’t committed.

---

## Node model

### Common fields

All node objects may include:

- `k` (string): constructor name (required)
- `nid` (number): node ID (optional; debugging/stability aid)
- `span` (object): source span (optional witness)

`span` is a witness only. It must not be used to “recompute” meaning.

### Token leaf witness (`k: "tok"`)

Some fields are token leaves, represented as:

```json
{"k":"tok","text":"...", "i": 0, "kind": 123, "start_byte": 0, "end_byte": 1}
```

Rules:

- `k` must be `"tok"`.
- `text` must be present and a string.
- Other fields (`nid`, `i`, `kind`, `start_byte`, `end_byte`) are optional witnesses.

Token text may be used for:

- identifier spellings (e.g. `Name.id`),
- literal spellings (e.g. `Int.lit`),
- external link names.

Token text must NOT be used for:

- operator meaning,
- type meaning (beyond explicit builtin IDs / committed policies),
- control-flow meaning.

---

## Constructors (v0)

This section is the closed vocabulary. If it isn’t listed here, it isn’t Semantic IR v0.

### Program/unit

#### `Unit`

Fields:

- `items`: array of nodes (required; may be empty)

Notes:

- `Unit` does not currently require a module/package name. If a name is needed later, it must be added as an explicit field and treated as a witness (not a semantic switch).

#### `Proc`

Fields:

- `name`: `tok` (required)
- `params`: array of `Param` or `ParamPat` nodes (required; may be empty)
- `ret`: node (required; typically `TypeRef`)
- `decls`: array of nodes (optional)
- `body`: `Block` or `null` (required; `null` only when `extern: true`)
- `extern`: boolean (optional)
- `link_name`: `tok` (optional)

Notes:

- v0 permits `extern`/`link_name` as witnesses for FFI naming.
- If `extern` is `true`, then `body` must be `null` (declaration-only).

#### `Param`

Fields:

- `name`: `tok` (required)
- `type`: node (required; typically `TypeRef`)
- `mode`: `tok` or string (optional; reserved for calling conventions / byref in the future)

#### `ParamPat`

Typed parameter binder using a restricted pattern form.

Fields:

- `pat`: `PatBind` (required)
- `type`: node (required; typically `TypeRef`)
- `mode`: `tok` or string (optional)

Notes:

- Semantic IR v0 only supports name-binding parameters; complex/destructuring patterns are out of scope.
- `sem2sir` treats `ParamPat(PatBind(name), type)` as equivalent to `Param(name, type)`.

#### `PatBind`

Fields:

- `name`: `tok` (required)

#### `PatWild`

No additional fields.

### Statements / blocks

#### `Block`

Fields:

- `items`: array of nodes (required; may be empty)

#### `Var`

Fields:

- `name`: `tok` (required)
- `type`: node (required; typically `TypeRef`)
- `init`: node (required)

#### `VarPat`

Typed local binder using a restricted pattern form.

Fields:

- `pat`: `PatBind` (required)
- `type`: node (required; typically `TypeRef`)
- `init`: node (required)

Notes:

- Semantic IR v0 only supports name-binding locals; complex/destructuring patterns are out of scope.
- `sem2sir` treats `VarPat(PatBind(name), type, init)` as equivalent to `Var(name, type, init)`.

#### `ExprStmt`

Fields:

- `expr`: node (required)

#### `Return`

Fields:

- `value`: node (optional)

#### `If`

Fields:

- `cond`: node (required)
- `then`: `Block` (required)
- `else`: `Block` (optional)

#### `While`

Fields:

- `cond`: node (required)
- `body`: `Block` (required)

#### `Break`

No additional fields.

#### `Continue`

No additional fields.

### Expressions

#### `Name`

Fields:

- `id`: `tok` (required)

Meaning rule: `Name.id.text` is an identifier witness. Resolution is assumed done upstream.

#### `TypeRef`

Fields:

- `name`: `tok` (required)

Meaning rule:

- If `name.text` is a recognized builtin type ID, it is treated as that builtin.
- Otherwise it must be identifier-like (nominal type name) and its meaning is provided upstream.

#### `Int`

Fields:

- `lit`: `tok` (required)

Meaning rule: literal typing is determined upstream and/or via `meta.types` policy.

#### `True`, `False`, `Nil`

No additional fields.

#### `Paren`

Fields:

- `expr`: node (required)

#### `Not`, `Neg`, `BitNot`, `AddrOf`, `Deref`

Fields:

- `expr`: node (required)

#### `Call`

Fields:

- `callee`: node (required)
- `args`: node or null (required; `null` means arity 0)

Notes:

- The `args` field is required to keep the IR non-implicit (no “missing means empty”).
- A consumer may impose additional restrictions (e.g. only direct calls where `callee` is `Name`) without changing the IR contract.

#### `Args`

Fields:

- `items`: array of nodes (required; may be empty)

#### `Bin`

Fields:

- `op`: string (required)
- `op_tok`: `tok` (optional witness)
- `lhs`: node (required)
- `rhs`: node (required)

Meaning rule:

- `op` is a **semantic operator ID**, not punctuation, not `@text`.
- `op_tok` (when present) is only a span witness.

Allowed operator ID namespace (v0):

- `core.*` (stable)

`sem2sir` maintains a closed allowlist of operator IDs it understands.
If an operator is needed, the correct workflow is to extend the core operator ABI (not mint a language-private ID).

---

## Commitment rules (operators and types)

### Operators

- Surface spellings (e.g. `+`, `DIV`, `MOD`) are committed upstream into stable semantic IDs.
- The IR carries only the semantic ID in `Bin.op`.
- Consumers MUST NOT branch on `op_tok.text`.

### Types

- Surface spellings are committed upstream into either:
  - recognized builtin IDs (e.g. `i32`), or
  - nominal names that are resolved by upstream symbol/type checking.
- Any defaulting behavior must be committed in `meta.types`.

---

## Compatibility / evolution

Semantic IR v0 is intentionally small.
To grow expressiveness **without introducing language-specific nodes**, add:

- more core operator IDs,
- more core type forms (with explicit definitions / layouts),
- more core statement/expression constructors that are language-agnostic (CFG, switch, aggregate literals, field access, indexing, etc.).

Rule of thumb:

- If a construct is “surface sugar” (e.g. `match`, `defer`, `try`, `for`), it should desugar upstream into core IR.
- If a construct is “backend-essential” (e.g. aggregate layout, address computation), it belongs in core IR.

---

## Summary (the sentence that should stop confusion)

`sem2sir` is not a language backend.

It is a strict consumer of a closed, language-agnostic Semantic IR.
New languages/stressors must be handled by upstream grammar + lowering passes that commit meaning into this IR—never by teaching `sem2sir` to recover meaning from text.
