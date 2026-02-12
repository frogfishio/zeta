<!-- SPDX-FileCopyrightText: 2025 Frogfish -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# ZASM Opcode Encoding (Normative)

This document defines the **normative** opcode encoding for a clean-slate **64-bit
processor**. It is intentionally simple, fast to decode, and forward-compatible.
It aligns with the current mnemonic set in `schema/ir/v1/mnemonics.html`.

## Normative Language

The key words **MUST**, **MUST NOT**, **SHOULD**, and **MAY** are to be interpreted
as described in RFC 2119.

## 0. Target Backends

ZASM mnemonics are target-agnostic. The backend selects the encoding:

- `--target wasm`: current WAT/WASM emission.
- `--target zasm`: the native ZASM opcode encoding defined in this document.
- `--target rv64i`: RISC-V RV64I encoding (plus required extensions for certain mnemonics).

The mnemonic table in `schema/ir/v1/mnemonics.html` now includes RV64I equivalents.

## 1. Goals

- Deterministic decoding with fixed base width.
- Fast decode in C with minimal branching.
- Extensible opcode space and reserved ranges.
- Little-endian on the wire for immediates and multi-word instructions.

## 2. Instruction Word

Base instruction width is **32 bits**. All instructions are at least one word.
The data path is 64-bit; 32-bit mnemonics operate on the low 32 bits, **wrap**,
and then **zero-extend** results to 64 bits unless otherwise specified. Some
instructions carry extension words for larger immediates.

```
31         24 23      20 19      16 15      12 11           0
+------------+----------+----------+----------+--------------+
|   OPCODE   |   RD     |   RS1    |   RS2    |   IMM12      |
+------------+----------+----------+----------+--------------+
```

- **OPCODE**: 8 bits (major opcode).
- **RD/RS1/RS2**: 4 bits each (register index, 0-15).
- **IMM12**: 12-bit signed immediate (sign-extended).

**Decoding rules (normative):**

- The decoder MUST treat the first word as the base instruction word.
- Any opcode not listed in Section 6.1 is **reserved** and MUST be rejected.
- For opcodes that do not use a field (RD/RS1/RS2/IMM12), that field MUST be zero.
  Encodings with non-zero unused fields MUST be rejected.
- When an opcode requires extension words, they MUST immediately follow the base word.
  Extra or missing extension words MUST be rejected.

### Extension Words

Some opcodes interpret the base word as a header and consume 1-2 extension words.

```
Word0: [OPCODE | RD | RS1 | RS2 | IMM12]
Word1: IMM32 (LE)
Word2: IMM32 (LE)  (for 64-bit immediates)
```

## 3. Registers

Register file is **64-bit** and currently maps to the mnemonic set:

```
0 = HL
1 = DE
2 = A
3 = BC
4 = IX
5..15 = reserved
```

- No register is hardwired to zero; use immediate `0` for zero values.
- Link and stack conventions are ABI-level decisions, not ISA requirements.
- Using reserved register indices (5..15) is **illegal** and MUST be rejected.

## 4. Immediate Encoding

- **IMM12** is sign-extended.
- **IMM32** and **IMM64** are little-endian extension words.

## 5. Addressing Modes

Base modes (encoded by opcode):

- `REG`: arithmetic on registers.
- `REG_IMM12`: arithmetic with signed 12-bit immediate.
- `MEM_BASE_IMM12`: load/store with base + imm12 (symbols lower via relocations).
- `PC_REL`: PC-relative branch with imm12 or imm32.

## 6. Opcode Families (Mapped to Existing Mnemonics)

### 6.1 Opcode Table (Hex)

| Mnemonic | Opcode (hex) |
|---|---|
| CALL | 0x00 |
| RET | 0x01 |
| JR | 0x02 |
| CP | 0x03 |
| DROP | 0x04 |
| INC | 0x05 |
| DEC | 0x06 |
| ADD | 0x10 |
| SUB | 0x11 |
| MUL | 0x12 |
| DIVS | 0x13 |
| DIVU | 0x14 |
| REMS | 0x15 |
| REMU | 0x16 |
| AND | 0x17 |
| OR | 0x18 |
| XOR | 0x19 |
| ADD64 | 0x20 |
| SUB64 | 0x21 |
| MUL64 | 0x22 |
| DIVS64 | 0x23 |
| DIVU64 | 0x24 |
| REMS64 | 0x25 |
| REMU64 | 0x26 |
| AND64 | 0x27 |
| OR64 | 0x28 |
| XOR64 | 0x29 |
| SLA | 0x30 |
| SRA | 0x31 |
| SRL | 0x32 |
| ROL | 0x33 |
| ROR | 0x34 |
| CLZ | 0x35 |
| CTZ | 0x36 |
| POPC | 0x37 |
| SLA64 | 0x40 |
| SRA64 | 0x41 |
| SRL64 | 0x42 |
| ROL64 | 0x43 |
| ROR64 | 0x44 |
| CLZ64 | 0x45 |
| CTZ64 | 0x46 |
| POPC64 | 0x47 |
| EQ | 0x50 |
| NE | 0x51 |
| LTS | 0x52 |
| LES | 0x53 |
| GTS | 0x54 |
| GES | 0x55 |
| LTU | 0x56 |
| LEU | 0x57 |
| GTU | 0x58 |
| GEU | 0x59 |
| EQ64 | 0x60 |
| NE64 | 0x61 |
| LTS64 | 0x62 |
| LES64 | 0x63 |
| GTS64 | 0x64 |
| GES64 | 0x65 |
| LTU64 | 0x66 |
| LEU64 | 0x67 |
| GTU64 | 0x68 |
| GEU64 | 0x69 |
| LD | 0x70 |
| LD8U | 0x71 |
| LD8S | 0x72 |
| LD16U | 0x73 |
| LD16S | 0x74 |
| LD32 | 0x75 |
| LD64 | 0x76 |
| LD8U64 | 0x77 |
| LD8S64 | 0x78 |
| LD16U64 | 0x79 |
| LD16S64 | 0x7A |
| LD32U64 | 0x7B |
| LD32S64 | 0x7C |
| ST8 | 0x80 |
| ST8_64 | 0x81 |
| ST16 | 0x82 |
| ST16_64 | 0x83 |
| ST32 | 0x84 |
| ST32_64 | 0x85 |
| ST64 | 0x86 |
| LDIR | 0x90 |
| FILL | 0x91 |
| DB/DW/STR/RESB/EQU/PUBLIC/EXTERN | — (directive) |

Opcode values are **normative** for `--target zasm`. The assembler chooses
width based on suffix: `FOO` = 32-bit, `FOO64` = 64-bit (W=1).

**Macro-ops:** `LDIR` and `FILL` are **macro-ops** that MAY be encoded
as single opcodes for `--target zasm`, but implementations MAY also lower
them to explicit sequences. They are not required to map to a single
hardware instruction.

```
Core arithmetic/logical:

- `ADD`, `SUB`, `AND`, `OR`, `XOR`, `MUL`, `DIVS`, `DIVU`, `REMS`, `REMU`
- `ADD64`, `SUB64`, `AND64`, `OR64`, `XOR64`, `MUL64`, `DIVS64`, `DIVU64`,
  `REMS64`, `REMU64`

Shift/rotate:

- `SLA`, `SRA`, `SRL`, `ROL`, `ROR`
- `SLA64`, `SRA64`, `SRL64`, `ROL64`, `ROR64`

Bit/arith unary:

- `CLZ`, `CTZ`, `POPC`
- `CLZ64`, `CTZ64`, `POPC64`
- `INC`, `DEC` (32-bit only; zero-extend to 64-bit)

Compare/set:

- `EQ`, `NE`, `LTS`, `LES`, `GTS`, `GES`, `LTU`, `LEU`, `GTU`, `GEU`
- `EQ64`, `NE64`, `LTS64`, `LES64`, `GTS64`, `GES64`, `LTU64`, `LEU64`, `GTU64`, `GEU64`
- `CP` writes the compare register (signed 32-bit)

Loads (base + imm12):

- `LD8U`, `LD8S`, `LD16U`, `LD16S`, `LD32`, `LD64`
- `LD8U64`, `LD8S64`, `LD16U64`, `LD16S64`, `LD32U64`, `LD32S64`

Stores (base + imm12):

- `ST8`, `ST16`, `ST32`, `ST64`
- `ST8_64`, `ST16_64`, `ST32_64`

Control flow:

- `JR` (PC-relative jump; optional condition)
- `CALL`, `RET`

Stack/data helpers:

- `LD`, `DROP`, `FILL`, `LDIR`

Directives (not encoded as opcodes):

- `DB`, `DW`, `STR`, `RESB`, `EQU`, `PUBLIC`, `EXTERN`
```

## 6.2 Instruction Formats (Field Usage)

Each opcode uses one of the following formats. Fields not listed for a format
MUST be zero.

| Format | Used fields | Notes |
|---|---|---|
| RRR | RD, RS1, RS2 | Register-register operations |
| RRI12 | RD, RS1, IMM12 | Register-immediate operations |
| R | RD | Unary register operations |
| MEM | RD, RS1, IMM12 | Load from `[RS1 + imm12]` into RD |
| STORE | RS2, RS1, IMM12 | Store RS2 into `[RS1 + imm12]` |
| J | RS1, IMM12 | Jump/branch with condition code in RS1 |
| IMM | RD, IMM12 (+IMM32/IMM64) | Load immediate/label into RD |

### 6.2.1 Format Assignment (Normative)

- RRR: `ADD`, `SUB`, `MUL`, `DIVS`, `DIVU`, `REMS`, `REMU`,
  `AND`, `OR`, `XOR`, `ADD64`, `SUB64`, `MUL64`, `DIVS64`, `DIVU64`,
  `REMS64`, `REMU64`, `AND64`, `OR64`, `XOR64`
- RRI12: `SLA`, `SRA`, `SRL`, `ROL`, `ROR`, `SLA64`, `SRA64`, `SRL64`,
  `ROL64`, `ROR64`
- R: `CLZ`, `CTZ`, `POPC`, `CLZ64`, `CTZ64`, `POPC64`, `INC`, `DEC`,
  `EQ`, `NE`, `LTS`, `LES`, `GTS`, `GES`, `LTU`, `LEU`, `GTU`, `GEU`,
  `EQ64`, `NE64`, `LTS64`, `LES64`, `GTS64`, `GES64`, `LTU64`, `LEU64`,
  `GTU64`, `GEU64`
- MEM: `LD8U`, `LD8S`, `LD16U`, `LD16S`, `LD32`, `LD64`, `LD8U64`,
  `LD8S64`, `LD16U64`, `LD16S64`, `LD32U64`, `LD32S64`
- STORE: `ST8`, `ST16`, `ST32`, `ST64`, `ST8_64`, `ST16_64`, `ST32_64`
- J: `JR`
- IMM: `LD`, `CALL`, `CP`
- RRR (macro-op): `LDIR` uses `DE` (dst), `HL` (src), `BC` (len)
- RRR (macro-op): `FILL` uses `HL` (dst), `A` (byte), `BC` (len)

### 6.2.2 Jump Condition Codes

`JR` uses RS1 to encode the condition:

| RS1 | Condition |
|---|---|
| 0 | always |
| 1 | EQ |
| 2 | NE |
| 3 | LTS |
| 4 | LES |
| 5 | GTS |
| 6 | GES |
| 7 | LTU |
| 8 | LEU |
| 9 | GTU |
| 10 | GEU |

IMM12 is a signed PC-relative displacement in **words**. If the displacement
does not fit in 12 bits, the encoder MUST use an IMM32 extension word.

### 6.2.3 `LD` Immediate Semantics

`LD` loads an immediate, label, or absolute address into RD. It uses IMM12
when possible; otherwise it uses IMM32/IMM64 extension words. Memory loads
MUST use the explicit load opcodes (e.g., `LD64`) and not `LD`.

## 7. Width Semantics

- For 32-bit mnemonics, inputs use low 32 bits; results zero-extend to 64 bits.
- For 64-bit mnemonics, inputs and results use full 64 bits.
- Sign-sensitive ops (`DIVS`, `REMS`, `LTS`, etc.) interpret operands as signed.
- Loads with `S` suffix sign-extend; `U` suffix zero-extends.
- `LD32` is defined as zero-extend (equivalent to `LD32U64`).

## 8. RV64I Interpretation Notes

When targeting RV64I, these mnemonics lower to RV64I (and extensions) with
WASM-compatible semantics:

- `DIV*`/`REM*`: RV64 returns defined values on div0; ZASM requires traps.
  The backend MUST insert a guard sequence or call a trap helper.
- 32-bit ops (`ADD`, `SUB`, etc.) map to `*W` forms plus `zext.w` (or an
  equivalent mask) to enforce zero-extension.
- `CLZ/CTZ/POPC/ROL/ROR` require Zbb (bit-manip) or a software sequence.
- `EQ/NE/LT*/GT*` map to `slt/sltu` plus `seqz/snez` pseudo-ops.
- `JR` conditionals map to `beq/bne/blt/bge/bltu/bgeu` using the compare temp.

## 9. Extension Prefixes

`EXT0..EXT3` reserve opcodes `0xF0..0xF3` for future capability growth. The next
word is interpreted by an extension-specific decoder.

## 10. Reserved Ranges

- `0xE0..0xEF` reserved for vendor-specific ops.
- `0xF4..0xFF` reserved for extensions and diagnostics.

Encodings within reserved ranges are **illegal** unless defined by a future
version of this specification. Implementations MUST reject them.

## 11. Conformance Tests (Outline)

- Encode/decode round-trips for each opcode.
- Illegal encodings (reserved opcodes, invalid register indices).
- Immediate edge cases (min/max for 12/32/64).
- Load/store bounds behavior in the reference runtime.

## 12. Open Questions

- Do we need more than the current `{HL,DE,A,BC,IX}` register set for opcodes?
- Are extension words sufficient for all existing ZASM immediates?
- Should `CALL` use an explicit link register or a fixed link convention?

## 13. Consistency Checklist

- `FOO` (32-bit) ops wrap then zero-extend to 64-bit.
- `FOO64` ops use full 64-bit inputs and results.
- `LD32` is defined as zero-extend (same as `LD32U64`).
- `LDIR` and `FILL` are pseudo-ops (expand to sequences).
- RV64I backend inserts `zext.w` after `*w` ops to preserve zero-extension.
- RV64I div/rem lowers with explicit div0 traps to match WASM semantics.

## 14. Validation Steps

- Add encode/decode golden tests for every mnemonic in the opcode table.
- For each backend (`wasm`, `zasm`, `rv64i`), run a small corpus:
  - arithmetic/logical ops (32/64) with edge cases,
  - loads/stores with sign/zero extension,
  - div/rem trap semantics on div0,
  - compare/set + branch behavior.
- Add cross-backend equivalence tests: same input program yields identical outputs.
- Add a compliance test that verifies pseudo-ops expand deterministically.
