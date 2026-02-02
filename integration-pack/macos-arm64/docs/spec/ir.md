<!-- SPDX-FileCopyrightText: 2025 Frogfish -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# ZASM IR v1.1 (Normative)

## Purpose

The JSONL IR is the stable boundary between `zas` and `zld`.
Each line is a JSON object representing one record.

## Format

- One JSON object per line (JSONL).
- Each record MUST include: `"ir":"zasm-v1.1"`.
- `zld` MUST reject unknown or missing IR versions.

## Record kinds

- `label` — defines a label
- `instr` — instruction
- `dir` — directive

IR v1.1 also defines additive records that improve tooling and diagnostics:

- `meta` — stream metadata
- `src` — source mapping table
- `diag` — structured diagnostics

## Operands

Operands are typed objects:

- `{ "t":"sym", "v":"NAME" }`
- `{ "t":"num", "v":123 }`
- `{ "t":"str", "v":"text" }`
- `{ "t":"mem", "base":"HL" }`

IR v1.1 adds additive operand forms for ABI clarity and better lowering:

- `{ "t":"reg", "v":"A" }` (register-typed operand)
- `{ "t":"mem", "base":"HL", "disp": 4 }` (memory displacement)

## Location info

Optional `loc` object:

```json
"loc": { "line": 12, "col": 3 }
```

Tools SHOULD use `loc` for error reporting when present.

## Validation

The schema lives at:

- `schema/ir/v1.1/record.schema.json`

All records MUST validate against the schema.

## Versioning

- IR version is **v1.1**.
- Backward-compatible additions MAY be introduced in later minor versions.

## Compatibility note

Some tools may accept `zasm-v1.0` streams for backwards compatibility, but the supported integration target is v1.1.
