<!-- SPDX-FileCopyrightText: 2025 Frogfish -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# zasm IR v1 (JSONL)

This directory defines the **stable pipeline boundary** between `zas` and `zld`.

Version: **v1.0** (current stable contract).

- `zas` reads **ZASM** from stdin and writes **JSONL IR** (one JSON object per line).
- `zld` reads **JSONL IR** from stdin and writes a single **WAT module**.

## Files

- `record.schema.json` — JSON Schema for **one JSONL record**.
  A `.zobj.jsonl` file is valid if **every line** parses as JSON and validates against this schema.

## Conventions

- Mnemonics (`m`) are recommended uppercase but not required.
- Identifiers (`name`, `sym.v`) match: `[A-Za-z_.$][A-Za-z0-9_.$]*`
- `loc` is optional but recommended for good error messages.
- Memory operands use `{ "t": "mem", "base": "HL" }` (v1 only supports `HL`).
- Each record must include `ir: "zasm-v1.0"`; `zld` rejects unknown or missing versions.
- Linkage directives:
  - `PUBLIC` with one symbol arg (export).
  - `EXTERN` with module, field, and optional local name.
- Sugar directives:
  - `STR` emits bytes like `DB` and defines `<name>_len`.
  - `EQU` defines a constant symbol from a numeric literal.

## Extension policy

- v1 records must validate against `record.schema.json`.
- New operand kinds may be added only with schema updates and version notes.
- Unknown fields are not allowed by schema and should be rejected.

## Example record stream

```jsonl
{"ir":"zasm-v1.0","k":"label","name":"print_hello"}
{"ir":"zasm-v1.0","k":"instr","m":"CALL","ops":[{"t":"sym","v":"print_hello"}]}
{"ir":"zasm-v1.0","k":"dir","d":"DB","name":"msg","args":[{"t":"str","v":"Hello"},{"t":"num","v":10}]}
{"ir":"zasm-v1.0","k":"instr","m":"LD","ops":[{"t":"sym","v":"A"},{"t":"mem","base":"HL"}]}
```
