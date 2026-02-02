<!-- SPDX-FileCopyrightText: 2025 Frogfish -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# zld (JSONL IR -> WAT)

`zld` reads ZASM JSONL IR (streamed from `zas`) and emits WebAssembly Text (WAT).

## MVP capabilities (current)

- Imports zABI 2.5 syscalls under module `"env"` (e.g. `zi_read`, `zi_write`, `zi_end`, `zi_alloc`, `zi_free`, `zi_telemetry`).
- Exports `main` (module entrypoint).
- Emits a `__heap_base` global for the allocator to seed dynamic memory.
- Supports Zilog-mode subset used by `examples/hello.asm`:
  - `LD HL, <sym|num>`
  - `LD DE, <sym|num>`
  - `LD A, <sym|num>`
  - `LD BC, <sym|num>`
  - `LD IX, <sym|num>`
  - `LD <reg>, <reg>` (HL/DE/A/BC/IX)
  - `LD A, (HL)` / `LD (HL), A`
  - `INC HL`
  - `INC DE`
  - `INC BC`
  - `DEC DE`
  - `DEC BC`
  - `ADD HL, <num>`
  - `ADD HL, DE`
  - `SUB HL, <num>`
  - `SUB HL, DE`
  - `CP HL, <DE|sym|num>`
  - `JR <label>`
  - `JR <COND>, <label>` with `GE`, `GT`, `LE`, `LT`, `EQ`, `NE` (signed vs 0)
  - `CALL <sym>`
  - `RET`
  - `DB` / `DW` / `RESB` / `PUBLIC` / `EXTERN` directives
  - `STR` / `EQU` directives

## Control flow lowering (v1)

`zld` keeps function slicing by call targets, but inside each function it builds basic blocks
from labels (plus an implicit `__entry` block). It then emits a PC-dispatch loop with
`br_table` over the block index. Conditional and unconditional `JR` update the PC and
branch to the dispatcher; fallthrough sets the PC to the next block. Empty label blocks
are packed out by redirecting their PC target to the next real block (or exit).

For debug visibility, each instruction emits a `;; line N: MNEMONIC` comment in WAT.

## Primitives

`zld` targets the zABI syscall surface (`zi_*` imports under module `"env"`).

Leading-underscore primitives (e.g. `_in/_out/_log/_alloc/_free`) are **not supported** and fail with a hard error.

## Directives

- `DB` emits a data segment and a global pointer to its offset.
- `DW` emits a global i32 constant.
- `RESB` reserves zero-initialized bytes: defines a global pointer at the current data
  cursor, advances by N bytes, and aligns the cursor to 4. No data segment is emitted.
- `STR` emits bytes like `DB` and also defines `<name>_len` as a constant.
- `EQU` defines a constant symbol from a numeric literal.
- `PUBLIC name` emits a module export for `name`.
- `EXTERN "mod","field",name` emits a function import as `$name`.
  - If `field` is a string, `name` is required.
  - Imported functions use zABI-specific signatures based on the imported name.

## Memory layout (v1)

- Memory is declared with a minimum of one page.
- Use `--mem-max` to emit a maximum page cap in the module.
- Data cursor starts at offset **8** and stays 4-byte aligned.
- `__heap_base` is emitted as the first free byte after static data.
- `__heap_base` is exported for host allocators (e.g., `zrun`).

## Usage

```bash
cat examples/hello.asm | bin/zas | bin/zld > out.wat
wat2wasm out.wat -o out.wasm
```

## CLI flags

- `zld --verify` validates symbols/labels without emitting WAT.
- `zld --version` prints the tool version.
- `zld --manifest` emits a JSON manifest (exports/imports/primitives).
- `zld --names` emits a custom name section for functions/globals.
- `zld --mem-max <size>` sets a maximum memory size (bytes/kb/mb/gb), rounded up to pages.
- `zld --conform` enforces JSONL conformance checks (for non-`zas` producers).
- `zld --conform=strict` enforces the IR schema constraints from `schema/ir/v1/record.schema.json`.
- `zld --tool` enables filelist + `-o` output mode (non-stream).
- `zld -o <path>` writes WAT/manifest output to a file (tool mode only).
- `zld --verbose` emits debug-friendly diagnostics to stderr.
- `zld --json` emits diagnostics as JSON lines (stderr). See [docs/diagnostics.md](../diagnostics.md).

## Tool mode

```bash
bin/zld --tool -o build/app.wat build/app.jsonl
```

## IR versioning

`zld` requires every JSONL record to include an `ir` version tag and rejects missing or
unknown versions.


