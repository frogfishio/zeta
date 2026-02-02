<!-- SPDX-FileCopyrightText: 2025 Frogfish -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# ZASM ISA v1.0.0 (Normative)

## Purpose and scope

ZASM is a human-facing assembly language that lowers to WAT. This document defines
its register model, instruction set, directives, and primitive calling conventions.
All conforming assemblers/linkers MUST follow these rules.

## Register model

Registers are i32 locals in the generated WAT.

- `HL`: primary accumulator and pointer (slice ptr)
- `DE`: secondary accumulator and length (slice len)
- `A`: byte accumulator
- `BC`: general purpose
- `IX`: general purpose

Rules:
- Registers are function-local.
- `HL`/`DE` form the standard slice `(ptr,len)` ABI.
- There is no flags register; comparisons use `CP` and `$cmp`.

## Instruction set

### Load/store

- `LD r, imm|sym`
- `LD r1, r2`
- `LD A, (HL)`
- `LD (HL), A`

`r`/`r1`/`r2` are from `{HL,DE,A,BC,IX}`. `(HL)` is the only supported memory form.

### Arithmetic

- `INC HL|DE|BC`
- `DEC DE|BC`
- `ADD HL, #imm`
- `ADD HL, DE`
- `SUB HL, #imm`
- `SUB HL, DE`

### Compare and branch

- `CP HL, rhs`
  - `rhs` is `DE`, immediate, or a symbol constant.
  - Sets `$cmp = HL - rhs` (signed).
- `JR label`
- `JR COND, label` with `COND` in `{EQ,NE,LT,LE,GT,GE}` (signed compare vs 0).

### Call/return

- `CALL sym`
- `RET`

## Directives

- `DB`  — data bytes
- `DW`  — i32 constant
- `RESB` — reserve bytes (zeroed)
- `STR` — data bytes + auto `<name>_len`
- `EQU` — constant symbol from a numeric literal
- `PUBLIC` — export a symbol
- `EXTERN` — import a function symbol

## Primitive calls (reserved `_` namespace)

All symbols starting with `_` are reserved for host primitives.

### `_in`
- Inputs: `HL = dst_ptr`, `DE = cap`
- Returns: `HL := nread`

### `_out`
- Inputs: `HL = ptr`, `DE = len`

### `_log`
- Inputs: `HL = topic_ptr`, `DE = topic_len`, `BC = msg_ptr`, `IX = msg_len`

### `_alloc`
- Input: `HL = size`
- Return: `HL := ptr` (or `-1` on OOM)

### `_free`
- Input: `HL = ptr`

## Error behavior

- Unknown mnemonic/directive: hard error.
- Unknown symbol: hard error.
- Duplicate symbol: hard error.

## Determinism requirements

- Emitted IR and WAT MUST be byte-for-byte deterministic for identical inputs.
- Data layout is deterministic: fixed base offset and 4-byte alignment.
- No timestamps, random IDs, or host-dependent formatting.

## Versioning

- ISA version is **v1.0.0**.
- Backward-compatible additions MAY be introduced in later minor versions.

