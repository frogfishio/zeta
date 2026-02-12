# Stacker (ZASM extension) — minimal-but-complete opcode family

This document defines a **minimal, orthogonal “stacker” coprocessor ISA** for ZASM.

Design goals:

- **Add-on substrate, not a new machine:** you can move values between classic regs/locals and the stack substrate *seamlessly*.
- **WASM-aligned by construction:** every dynamic state introduced by stacker is representable on a WASM-style value stack.
- **Deterministic lowering:** no hidden type state, no “auto-switch”; the chosen profile at lowering time decides what’s legal.
- **Tool-friendly:** easy to validate in `zem`, easy to fuzz/trace, easy to JIT.

---

## 1) State model (what exists)

Stacker introduces two virtual stacks:

- **DS** — data stack (values)
- **RS** — return/control stack (optional but very useful for Forth-ish lowering)

### Cell width (hard invariant)

- **All stack cells are 64-bit (`cell64`).**
- Smaller widths (e.g. i32) are represented *inside* a cell using explicit canonicalization rules.

This keeps the machine simple and makes classic-reg interop natural.

### Canonicalization for i32-in-a-cell

Stacker does **not** carry a typed stack at runtime. Instead, opcodes that operate on i32 spell the rules:

- `*.I32` operations:
  - Consume operands from cells.
  - Operate on `low32(x)` / `low32(y)`.
  - Produce a cell where the result is **zero-extended** to 64-bit.
- `*.S32` operations (when provided):
  - Consume operands from cells.
  - Operate on `low32` as *signed* where applicable.
  - Produce a cell where the result is **sign-extended** to 64-bit.

Typed immediates follow the same rule:

- `SPUSH.I32 #imm` → zext32 into a cell
- `SPUSH.S32 #imm` → sext32 into a cell

> Rationale: this gives you deterministic lowering without introducing hidden type state.

### Boolean representation

Comparisons produce booleans as **0/1 in the i32 domain** and then canonicalize into a cell using the rules in §1 (i32-in-a-cell canonicalization). Therefore the stored cell value is always exactly `0` or `1`.

**Zero-test semantics (normative):**

- `SEQZ.I32` tests `low32(x) == 0` in the i32 domain and returns `0/1` canonicalized per §1.
- `SEQZ.I64` tests the **full 64-bit cell value** `x == 0` and returns `0/1` as a cell.

### Fault model (normative)

Unless explicitly stated otherwise, **stacker faults TRAP** (terminate the current execution deterministically).

### Trap reasons (stable taxonomy)

When a stacker TRAP occurs, the runtime SHOULD classify it with a stable reason code (string or enum) suitable for tracing and differential testing.

Minimum required reason set:

- `STACK_UNDERFLOW_DS`
- `STACK_OVERFLOW_DS`
- `STACK_UNDERFLOW_RS`
- `STACK_OVERFLOW_RS`
- `DIV_BY_ZERO`
- `SDIV_OVERFLOW`
- `OOB_MEM`
- `ILLEGAL_OPCODE` (opcode executed without required active profile)

> NOTE: Assemblers/validators MAY additionally report static errors (e.g. out-of-range immediates). Those are not runtime traps.

### Trap conditions

Faults include:

- DS underflow (not enough items for an opcode)
- DS overflow (see depth bounds below)
- RS underflow/overflow when `+stacker.rs:v1` is active
- Division or remainder by zero
- Signed division overflow (MIN_INT / -1) for SDIV.S32 and SDIV.S64
- Out-of-bounds memory access (see §7)
- Use of an opcode that is not enabled by the active profile(s)

### Stack-effect determinism (normative)

Every stacker opcode has a **well-defined stack effect** on DS and (when enabled) RS.

- If an opcode requires *k* items from a stack, it MUST TRAP with `STACK_UNDERFLOW_DS` / `STACK_UNDERFLOW_RS` if fewer than *k* items are present **at the moment the opcode begins**.
- Underflow MUST be detected **before any mutation** (no partial pops/pushes, no partial stores).
- For opcodes that push results, overflow MUST be checked first as specified in “Precise overflow condition” (§1).
- NOP-style directives (`SANNOT.SIG`, `SASSERT.SHAPE`) have **zero** stack effect at runtime.

> Rationale: this removes backend divergence and makes traces/certs/reducers stable.

`zem` MUST report traps deterministically and SHOULD include the trap reason, mnemonic, and pc/record index.

### Stack depth bounds (normative)

For v1, DS and RS are **bounded** stacks with deterministic overflow traps.

- DS maximum depth MUST be at least **1024** cells.
- If `+stacker.rs:v1` is active, RS maximum depth MUST be at least **1024** cells.
- Implementations MAY choose larger bounds and MAY expose configurability, but they MUST NOT choose smaller bounds unless running in a clearly-labeled test mode.


Note: native/JIT backends commonly choose much larger bounds; the 1024-cell minimum is a conformance floor to keep traces/certs/reducers deterministic.

Overflow MUST TRAP with `STACK_OVERFLOW_DS` / `STACK_OVERFLOW_RS`.

Precise overflow condition (normative): an opcode that would increase a stack’s depth MUST TRAP with STACK_OVERFLOW_* if the stack depth is already equal to its maximum bound at the moment the opcode begins (i.e. overflow is detected before any mutation).

> Rationale: deterministic bounded stacks make `zem`/fuzzing/certs reliable and avoid backend divergence.

---

## 2) Profiles / capability gating



Profiles gate legality at **lowering time**. There is no runtime autoswitch.

Naming note (normative): throughout this document, we use the following runtime/product names:

- **ZX64** — the classic ZASM64-style runtime (register machine).
- **Z64+S** — ZX64 plus the **stacker coprocessor** profiles in this document (no CPU addendum opcodes).
- **ZX64S** — the **stack-processor** runtime specified only by Appendix A (`+stacker.cpu:v1`).


> IMPORTANT (normative): This document defines **stacker as a coprocessor ISA**.
>
> A *stack-processor* variant (ZX64S) exists as a separate product line and is specified only by the **ZX64S addendum** in Appendix A (`+stacker.cpu:v1`).
>
> **Z64+S (coprocessor runtime)** MUST NOT expose or accept the `+stacker.cpu:v1` opcode family. If any `+stacker.cpu:v1` opcode appears in a Z64+S program, validation/lowering MUST fail with `ILLEGAL_OPCODE`.

Recommended profiles:

- **`+stacker:v1`** (base):
  - `cell64` DS ops
  - `.I64` ALU ops
  - interop ops (push/pop regs)
  - memory ops at `.I64` / `.I32` granularity (see below)
- **`+stacker.rs:v1`** (optional):
  - enables the **RS** (return/control stack) ops in §9
- **`+stacker.i32ops:v1`** (optional):
  - enables `.I32` / `.S32` arithmetic/bitops/compares and the 8/16/32-bit load/store variants

Notes:

- Profiles do **not** change cell width. Cells remain 64-bit.
- If a lowering target cannot support a required profile (e.g. no FP/SIMD/stack substrate), lowering MUST error out.

### Required opcode sets (normative)

An implementation that claims support for a given profile MUST implement the following mnemonics with the semantics in this document.

- `+stacker:v1` MUST implement:
  - Interop: `SPUSH.HL/DE/BC/IX/A`, `SPOP.HL/DE/BC/IX/A`, `SPUSH.I64`
  - Shuffles: `SDROP`, `SDUP`, `SSWAP`, `SOVER`, `SROT`, `SNIP`, `STUCK`
  - ALU i64: `SADD.I64`, `SSUB.I64`, `SMUL.I64`, `SDIV.S64`, `SDIV.U64`, `SREM.S64`, `SREM.U64`,
            `SAND.I64`, `SOR.I64`, `SXOR.I64`, `SSHL.I64`, `SSHR.S64`, `SSHR.U64`,
            `SEQ.I64`, `SNE.I64`, `SLT.S64`, `SLT.U64`, `SLE.S64`, `SLE.U64`, `SGT.S64`, `SGT.U64`, `SGE.S64`, `SGE.U64`, `SEQZ.I64`
  - Memory: `SLOAD.I32`, `SLOAD.I64`, `SSTORE.I32`, `SSTORE.I64`
  - Validation directives (as NOP at runtime): `SASSERT.DEPTH`, `SANNOT.SIG`, `SASSERT.SHAPE`

- `+stacker.i32ops:v1` MUST additionally implement:
  - Immediates: `SPUSH.I32`, `SPUSH.S32`
  - ALU i32: `SADD.I32`, `SSUB.I32`, `SMUL.I32`, `SDIV.S32`, `SDIV.U32`, `SREM.S32`, `SREM.U32`,
            `SAND.I32`, `SOR.I32`, `SXOR.I32`, `SSHL.I32`, `SSHR.S32`, `SSHR.U32`,
            `SEQ.I32`, `SNE.I32`, `SLT.S32`, `SLT.U32`, `SLE.S32`, `SLE.U32`, `SGT.S32`, `SGT.U32`, `SGE.S32`, `SGE.U32`, `SEQZ.I32`
  - Memory sizes: `SLOAD8.U32`, `SLOAD8.S32`, `SLOAD16.U32`, `SLOAD16.S32`, `SSTORE8`, `SSTORE16`, `SSTORE32`

- `+stacker.rs:v1` MUST additionally implement:
  - RS core: `RPUSH`, `RPOP`, `RDUP`, `RSWAP`, `RDROP`
  - DS↔RS bridge: `S2R`, `R2S`


OPTIONAL mnemonics listed elsewhere (e.g. `S2DUP`, `SPEEK.*`, `RPEEK.*`, `SPUSH.TRUE`, `SPUSH.FALSE`) MAY be omitted without affecting profile conformance.

Clarification (normative): `SPUSH.TRUE` and `SPUSH.FALSE` are pure assembler/runtime sugar (equivalent to `SPUSH.I32 #1` and `SPUSH.I32 #0` respectively) and do not introduce any additional runtime state.

---

## 3) Naming conventions

Avoid plain `PUSH/POP` to prevent confusion with call stacks.

- **DS ops**: `S*` prefix (e.g. `SPUSH.*`, `SADD.I64`)
- **RS ops**: `R*` prefix (e.g. `RPUSH`, `RPOP`, `R2S`)

Typed suffixes:

- `.I64` — 64-bit unsigned domain (when relevant)
- `.S64` — 64-bit signed domain (div/shifts/compares)
- `.U64` — 64-bit unsigned domain (div/shifts/compares)
- `.I32` — 32-bit domain, result zext32 → cell
- `.S32` / `.U32` — signed/unsigned 32-bit domains where relevant

> Avoid un-suffixed ALU ops like `SADD` unless you are willing to carry a typed stack.

---

## 4) Interop ops (bridge classic regs ↔ DS)

These are the “money” ops. They’re why stacker is an add-on.

### Push/pop classic regs

- `SPUSH.HL` / `SPOP.HL`
- `SPUSH.DE` / `SPOP.DE`
- `SPUSH.BC` / `SPOP.BC`
- `SPUSH.IX` / `SPOP.IX`
- `SPUSH.A`  / `SPOP.A`

**Register width (normative):** in zasm-op, the classic registers `HL/DE/BC/IX/A` are treated as **64-bit** values for stacker interop.

- `SPUSH.<reg>` pushes the full 64-bit register value as a `cell64`.
- `SPOP.<reg>` pops a `cell64` and writes the full 64-bit value into the register.

If a future concrete machine defines any of these registers as 32-bit, that machine MUST specify the canonicalization rules in its own register-model document. Stacker itself remains `cell64`.

### Push immediates (typed)

- `SPUSH.I32 #imm`  (zext32 → cell)
- `SPUSH.S32 #imm`  (sext32 → cell)
- `SPUSH.I64 #imm64`

Immediate range / parsing (normative):

- `#imm` for `.I32` MUST fit in `0 .. 2^32-1` and is interpreted as an **unsigned 32-bit** literal.
- `#imm` for `.S32` MUST fit in `-2^31 .. 2^31-1` and is interpreted as a **signed 32-bit** literal.
- `#imm64` for `.I64` MUST fit in `0 .. 2^64-1` and is interpreted as an **unsigned 64-bit** literal.

Example (two’s-complement wrap): `SPUSH.I64 #-1` pushes `0xFFFF_FFFF_FFFF_FFFF`.


If an assembler accepts a negative literal for `.I64` (e.g. `SPUSH.I64 #-1`), it MUST interpret it in two’s-complement modulo 2^64 (so `-1` becomes `0xFFFF_FFFF_FFFF_FFFF`).

OPTIONAL convenience mnemonics:

- `SPUSH.TRUE` — pushes `1` as a `cell64` (equivalent to `SPUSH.I32 #1`)
- `SPUSH.FALSE` — pushes `0` as a `cell64` (equivalent to `SPUSH.I32 #0`)

These are pure sugar and do not introduce any additional runtime state.

### Peek/poke (does not change stack depth)

**OPTIONAL:** extremely handy for lowering and debugging:

- `SPEEK.HL #k`   (k=0 is TOS; does not pop)
- `SPOKE #k, HL`  (write DS slot k from HL)

**WASM lowering hint:** `SPUSH.<reg>` ↔ `local.get $reg64`; `SPOP.<reg>` ↔ `local.set $reg64`.

---

## 5) Pure stack shuffles (core DS discipline)

Keep this set small and orthogonal:

- `SDROP`        ( a → )
- `SDUP`         ( a → a a )
- `SSWAP`        ( a b → b a )
- `SOVER`        ( a b → a b a )
- `SROT`         ( a b c → b c a )
- `SNIP`         ( a b → b )
- `STUCK`        ( a b → b a b )

**OPTIONAL:** two-cell variants (great for paired values / ABI-like shapes):

- `S2DROP`, `S2DUP`, `S2SWAP`, `S2OVER`

**WASM note:** WASM doesn’t have dup/swap, but lowering can always synthesize with scratch locals.

---

## 6) Stack ALU ops (typed, no flags)

Stacker should not introduce “flag registers”. Return booleans as explicit values.

### Arithmetic semantics (normative)

- All integer arithmetic is **two’s-complement, wrapping modulo 2^N** for the chosen width (N=32 or N=64).
- Division/remainder follow the usual signed/unsigned domains.
- **Divide-by-zero TRAPS** for all div/rem ops.
- **Signed division overflow TRAPS** for signed div ops when `(lhs == MIN_INT && rhs == -1)`:
  - i32: `SDIV.S32` traps on `0x80000000 / -1`
  - i64: `SDIV.S64` traps on `0x8000000000000000 / -1`
- Shifts mask the shift amount like WASM.
  - The shift count is taken from `low32(count_cell)` (i.e. the low 32 bits of the count operand cell).
  - i32 shifts use `(low32(count_cell) & 31)`.
  - i64 shifts use `(low32(count_cell) & 63)`.

- Signed division and remainder semantics (normative):
  - SDIV.S32 / SDIV.S64 compute the quotient by truncating toward zero.
  - SREM.S32 / SREM.S64 compute the remainder such that: lhs == (rhs * quot) + rem, and rem has the same sign as lhs (or is 0).
  - Unsigned variants use Euclidean division over the corresponding unsigned domain.

### i32 family (requires `+stacker.i32ops:v1`)

Operate on `low32(x)` / `low32(y)` operands (as applicable) and canonicalize the result into a cell (zext32 for `.I32`/`.U32`, sext32 for `.S32`).

#### Result canonicalization for i32 ops (normative)

All 32-bit-domain opcodes (`*.I32`, `*.S32`, `*.U32`, and i32 comparisons) MUST apply the **i32-in-a-cell canonicalization** rules from §1.

In particular:

- Non-compare ops MUST canonicalize their 32-bit result into a `cell64` deterministically (per §1).
- Compare ops MUST compare in the 32-bit domain using `low32(x)` / `low32(y)` (signed or unsigned as indicated by the mnemonic), produce `0` or `1` in the i32 domain, and then canonicalize into a cell (so the stored value is always exactly `0` or `1`).

Backends MAY compute in a wider type internally, but the observable cell result MUST match these rules.

Arithmetic / bitwise:

- `SADD.I32`, `SSUB.I32`, `SMUL.I32`
- `SDIV.S32`, `SDIV.U32`, `SREM.S32`, `SREM.U32`
- `SAND.I32`, `SOR.I32`, `SXOR.I32`
- `SSHL.I32`, `SSHR.S32`, `SSHR.U32`

Comparisons (produce 0/1; canonicalize per §1):

- `SEQ.I32`, `SNE.I32`
- `SLT.S32`, `SLT.U32`, `SLE.S32`, `SLE.U32`
- `SGT.S32`, `SGT.U32`, `SGE.S32`, `SGE.U32`
- `SEQZ.I32`

### i64 family (base `+stacker:v1`)

Arithmetic / bitwise:

- `SADD.I64`, `SSUB.I64`, `SMUL.I64`
- `SDIV.S64`, `SDIV.U64`, `SREM.S64`, `SREM.U64`
- `SAND.I64`, `SOR.I64`, `SXOR.I64`
- `SSHL.I64`, `SSHR.S64`, `SSHR.U64`

Comparisons (produce 0/1 booleans as 0/1 cells):

- `SEQ.I64`, `SNE.I64`
- `SLT.S64`, `SLT.U64`, `SLE.S64`, `SLE.U64`
- `SGT.S64`, `SGT.U64`, `SGE.S64`, `SGE.U64`
- `SEQZ.I64`

---

## 7) Memory ops (WASM-shaped: addr lives on DS)

This is the core of “100% aligned with WASM”. Address is a cell; lowering chooses interpretation.

**Address interpretation (normative):**
- For wasm32 lowering, `addr` is interpreted as the **low32** of the cell (a u32 linear-memory offset). Implementations MUST bounds-check `addr` and any accessed byte range against the current linear memory size.
- For targets with a 64-bit linear memory model, `addr` is interpreted as the full cell value as an unsigned 64-bit offset, and MUST be bounds-checked against the current linear memory size.
- In all cases, the address is interpreted as an unsigned offset for bounds checks; a cell value that is negative in two’s-complement is treated modulo 2^64 (i.e. as a large unsigned offset) and will typically TRAP as OOB.
- For native/JIT lowering, `addr` is still a *virtual* address in the ZASM sandbox model (not an ambient host pointer). Backends MUST preserve the same address interpretation and bounds-checking rules described above for their chosen memory model.

### Memory endianness / alignment (normative)

- All multi-byte loads/stores are **little-endian**.
- Unaligned loads/stores are **permitted** and MUST behave as if performed bytewise.
- `align #n` (when present) is a **hint** to backends; it MUST NOT change observable behavior.
- Any out-of-bounds access TRAPS.

### Loads

- `SLOAD.I32`   ( addr → val )
  Loads 32 bits from memory, interprets the loaded value in the **U32** domain, and returns a cell containing `zext32(loaded_u32)`.
  (Normative alias: `SLOAD.I32` == `SLOAD32.U32` if that assembler sugar is enabled.)
- `SLOAD.I64`   ( addr → val )

Sized loads (require `+stacker.i32ops:v1`):

- `SLOAD8.U32`, `SLOAD8.S32`
- `SLOAD16.U32`, `SLOAD16.S32`

**Sized-load result canonicalization (normative):**

- `SLOAD8.U32` loads 8 bits, zero-extends to 32 bits, then canonicalizes to a cell via zext32.
- `SLOAD8.S32` loads 8 bits, sign-extends to 32 bits, then canonicalizes to a cell via sext32.
- `SLOAD16.U32` loads 16 bits, zero-extends to 32 bits, then canonicalizes to a cell via zext32.
- `SLOAD16.S32` loads 16 bits, sign-extends to 32 bits, then canonicalizes to a cell via sext32.

These opcodes do **not** observe or preserve any higher bits from the prior cell state.

Assembler sugar (OPTIONAL):
- `SLOAD32.U32` MAY alias `SLOAD.I32`.
- `SLOAD32.S32` MAY alias: `SLOAD.I32` followed by i32 sign-canonicalization (final cell = `sext32(i32(low32(x)))`).

Backends MAY implement `SLOAD32.S32` as a native sign-extending load where available, but the observable cell result MUST match the alias semantics above.

### Stores

- `SSTORE.I32`  ( addr val → )
  Stores `low32(val)` (the i32-domain payload of the cell).
  Normative detail: the stored bytes are exactly the little-endian encoding of `(low32(val) & 0xFFFF_FFFF)`; higher bits of the cell are ignored.
- `SSTORE.I64`  ( addr val → )

Sized stores (require `+stacker.i32ops:v1`):

- `SSTORE8`, `SSTORE16`, `SSTORE32`

**Profile gating note (normative):**

- `SSTORE32` is only legal when `+stacker.i32ops:v1` is active (it is considered a 32-bit-width store family member).
- Without `+stacker.i32ops:v1`, backends MUST reject `SSTORE32` at lowering/validation time as `ILLEGAL_OPCODE`.

### Alignment / volatility (OPTIONAL)

If you care about alignment/volatility, mirror your SIR-style attribute tails:

- `SLOAD.I32 align #n`
- `SSTORE.I32 align #n`
- Optional `+vol` flag

**WASM lowering:** the core loads/stores can be 1:1.

---

## 8) Control-flow interop (keep CALL/JR/RET as-is)

Stacker is a **data substrate**. You do not need a parallel call/branch ISA.

Calling zABI/syscalls is just a lowering policy:

- `SPOP.HL`, `SPOP.DE`, `SPOP.BC`, ...
- `CALL zi_write`

**OPTIONAL:** tooling/validation opcodes:

- `SASSERT.DEPTH #n`
  - Runtime check (normative).
  - Does not consume or produce stack values.
  - TRAPS if DS depth != n at the moment the opcode begins.
  - Executing this opcode without `+stacker:v1` enabled is a validation/lowering error (`ILLEGAL_OPCODE`).

- `SANNOT.SIG #id`
- **Validation-only directive** (no runtime semantics).
- Declares a stack-shape contract at this program point for tooling/backends.
- `#id` is a **u32 immediate** naming a contract in surrounding unit metadata (e.g. an `ext` record).
- Unknown `#id` is a **validation/lowering error**.
- Runtimes/interpreters/JITs MUST treat this as a **NOP**.
- `zem` runtime execution MUST treat this as a NOP; any enforcement occurs only in validation/lowering/tooling modes.

- `SASSERT.SHAPE #id`
- **Validation-only directive** (no runtime semantics).
- Requires that the validated stack-shape contract at this point matches `#id`.
- Mismatch is a **validation/lowering error**.
- Runtimes/interpreters/JITs MUST treat this as a **NOP**.
- `zem` runtime execution MUST treat this as a NOP; any enforcement occurs only in validation/lowering/tooling modes.

- Normative meaning for tooling/backends:
  - `#id` names a contract in unit metadata describing:
    - DS input shape (types/widths as lowered),
    - DS output shape,
    - (optionally) RS shape if `+stacker.rs:v1` is enabled.

- Notes:
  - These directives are safe to strip after verification.
  - They SHOULD be preserved through IR/tooling stages that generate JVM/CLR stack maps.

Example (tooling contract):

- `SANNOT.SIG #7` declares a stack contract at this point.
- `SASSERT.SHAPE #7` asserts it.

A typical contract description might look like:

- `DS: [i64, i32] → [i64]`

Meaning: at this program point, the validator/backends expect the data stack to contain an `i64` under an `i32` on entry, and to contain a single `i64` on exit.

Runtime note: both mnemonics are NOPs at runtime; enforcement happens only in validation/lowering/tooling.

---

## 9) Return stack (RS) — OPTIONAL but recommended

If you want Forth-like lowering without abusing DS for control, enable **`+stacker.rs:v1`**:

Core:

- `RPUSH` / `RPOP`  (64-bit cells)
- `RDUP`, `RSWAP`, `RDROP`

Interop between DS and RS:

- `S2R`   ( DS: a → ; RS: → a )
- `R2S`   ( RS: a → ; DS: → a )

**OPTIONAL:** peek/poke:

- `RPEEK.HL #k` / `RPOKE #k, HL`

**WASM lowering hint:** RS can be implemented as a second stack backed by linear memory (with an `rsp` local), or synthesized with locals if a validator can prove a fixed maximum depth. Both strategies MUST preserve the same observable behavior (including bounds-check failures when memory-backed). For v1, `zem` MUST implement RS as a bounded stack with deterministic overflow traps (the bound MAY be configurable), unless explicitly configured to use a memory-backed RS.

---

## 10) Tiny example (mixed work)

Push HL → do stack work → pop HL back:

```asm
; HL holds some value we want to transform with stacky code
SPUSH.HL

SPUSH.I32 #10
SADD.I32          ; (HL + 10) in i32 space → zext32 in cell

SPOP.HL           ; back to classic world
; now you can do normal LD/CP/JR etc
```

This matches the goal:

- materialize values into classic regs/locals,
- do work in the stack substrate,
- rejoin classic logic,
- without introducing states that a WASM machine couldn’t represent.

---

## 11) Backend notes (why this is not “WASM by other means”)


### What stacker enables beyond WASM (practical differences)

Stacker is *WASM-alignable*, but it deliberately gives your toolchain a few extra levers that WASM does not standardize:

- **Optional second stack (RS) as a real concept:** with `+stacker.rs:v1`, DS and RS are both explicit and can be validated/fuzzed/traced as such (Forth-style lowering without abusing DS).
- **First-class stack shuffles:** `SDUP/SSWAP/SOVER/...` are explicit ops, which improves code density and makes pattern-based optimizers (e.g. `zem` n-grams/peepholes) much more effective than synthesizing shuffles via locals.
- **A defined bridge between paradigms:** `SPUSH.<reg>` / `SPOP.<reg>` is a semantic boundary between classic ZASM “register world” and stack substrate work. WASM has locals, but it does not define a stable “classic register file” contract.
- **Tool-addressable shape contracts:** `SANNOT.SIG` / `SASSERT.SHAPE` provide a first-class place to attach stack-shape proofs and invariants for reducers/fuzzers and for verifier-heavy backends (JVM/CLR).

These differences are why stacker can be used for “mixed work”: stack-native code where it helps, and classic-reg code where it helps, with explicit, deterministic crossings.

### Forth / concatenative languages

What Forth wants is not “a WASM stack”; it wants **DS + RS semantics** and cheap stack shuffles.

- Enable **`+stacker.rs:v1`** for true Forth-style control/temporaries.
- Prefer expressing *stack effects* in tooling via `SANNOT.SIG` so reducers/fuzzers can keep programs well-shaped.
- Interop stays clean: you can `SPUSH.<reg>` / `SPOP.<reg>` around foreign calls and keep classic ZASM control flow.

### JVM / CLR (typed, verified stacks)

JVM bytecode and CLR IL are stack machines, but their stacks are **verifier-visible and typed**.

Stacker still avoids hidden runtime type state; instead:

- Typed opcode variants (`*.I32`, `*.S32`, `*.I64`, etc.) plus the cell canonicalization rules provide **deterministic meaning**.
- Backends that require verification MUST additionally emit **stack map / stack shape proofs** derived from:
  - the chosen stacker profile(s),
  - typed opcode variants,
  - `SANNOT.SIG` contracts at control-flow joins.

In other words: JVM/CLR lowering treats `SANNOT.SIG` as a **compile-time proof hook** (stack-map source), not as runtime behavior.

### Native JIT / AOT

Native backends are free to implement DS/RS as real stacks, spill to locals/temps, or registerize hot stack values, as long as they preserve the same **observable behavior** — including the address interpretation and bounds-checking rules in §7.

---

# Appendix A) ZX64S stack-processor addendum (NOT a coprocessor)

This appendix defines the **optional stack-processor** execution model used by the **ZX64S** runtime.

- **Scope:** ZX64S only.
- **Non-scope:** ZX64 (classic) and **Z64+S** (ZX64 plus stacker coprocessor).

**Normative separation:** the opcodes in this appendix MUST NOT be available when stacker is used as a coprocessor. They only exist when the selected target/runtime is **ZX64S**.

For avoidance of doubt: **Z64+S** is the coprocessor runtime name (classic ZASM + stacker coprocessor) and MUST NOT accept any `+stacker.cpu:v1` opcodes.


## A.1) Profile

- `+stacker.cpu:v1` — enables the ZX64S execution opcodes.

Requirements:

- `+stacker.cpu:v1` MUST imply `+stacker:v1`.
- `+stacker.cpu:v1` SHOULD be paired with `+stacker.rs:v1` (recommended).
- If `+stacker.cpu:v1` is selected for a target that is not ZX64S, lowering MUST fail (static error).

## A.1.1) Control-flow exclusivity (Option A — normative)

When `+stacker.cpu:v1` is active, **ZX64S is stacker-only for control flow**.

- The only opcodes permitted to modify `PC` are the ZX64S control-flow family defined in §A.3 (`SBR`, `SCBR`, `SCALL`, `SRET`).
- Any classic ZASM control-flow opcode appearing in a ZX64S program MUST be rejected at validation/lowering time with `ILLEGAL_OPCODE`.

At minimum, the following classic control-flow mnemonics MUST be rejected when `+stacker.cpu:v1` is active (this list is non-exhaustive):

- `JR` / `JR*` (all conditional/unconditional forms)
- `CALL` (including host ABI calls expressed via classic `CALL`)
- `RET`

Rationale (non-normative): this keeps `PC` semantics unambiguous (instruction-record index) and avoids having to specify an interaction model between classic control flow and stacker CPU control flow.

## A.2) State model (ZX64S)

ZX64S introduces one additional piece of machine state:

- **PC** — a program counter that selects the next instruction record.

### A.2.1) Labels and instruction records (normative)

- A **label** names the *next* instruction record after the label marker.
- Label resolution yields a **0-based instruction-record index**.
- A label marker is not itself an executable instruction.

### A.2.2) PC sequencing (normative)

- ZX64S execution is a fetch/execute loop over instruction records.
- After executing any instruction that does not explicitly set `PC` (e.g. `SBR`, `SCBR` taken, `SCALL`, `SRET`), the machine MUST advance to the next instruction record by setting `PC := PC + 1`.
- **Validation-only directives** (`SANNOT.SIG`, `SASSERT.SHAPE`) are treated as NOPs at runtime and MUST still participate in PC sequencing like any other instruction record (i.e. executing them advances `PC := PC + 1`).

### A.2.3) Entry and termination (normative)

- **Entry:** execution begins at `PC := 0` (the first instruction record in the program) unless the container/loader specifies a different entry label.
- **Termination:** if `PC` becomes equal to the number of instruction records (i.e. falls off the end), the machine MUST TRAP with reason `ILLEGAL_OPCODE` (treat as an invalid control transfer).

Rationale (non-normative): explicit entry/termination avoids backend divergence and keeps traces/certs stable.

## A.3) Minimal control-flow opcode family

These opcodes are the **minimum set** required for stacker to function as a standalone CPU.

### A.3.1) Unconditional branch

- `SBR #label`

Semantics (normative): set `PC := label(#label)`.

### A.3.2) Conditional branch (consume condition)

- `SCBR #label`  ( DS: cond → )

Semantics (normative):

- Pop `cond` from DS.
- If `cond != 0` using the **full 64-bit cell value**, set `PC := label(#label)`.
- Otherwise (not taken), set `PC := PC + 1`.

Additional rule (normative): `SCBR` MUST have no other observable side effects beyond consuming the condition cell and updating `PC`.

Notes:

- If a backend wants strict i32-style branching, it MUST lower explicitly via `SEQZ.I32` / `SEQZ.I64` and compare to 0/1; ZX64S does not carry hidden “i32 condition” state.

### A.3.3) Call / return (requires RS)

- `SCALL #label`
- `SRET`

Semantics (normative):

- `SCALL #label` MUST push the return address onto RS and then set `PC := label(#label)`.
  - The **return address** is the **0-based instruction-record index** of the instruction *immediately after* the `SCALL` record.
  - The return address MUST be stored on RS as a `cell64` whose numeric value equals that instruction-record index.
- `SRET` MUST pop one `cell64` from RS, interpret its numeric value as an instruction-record index, and set `PC := popped_return_pc`.

Trap behavior (normative):

- If `+stacker.rs:v1` is not active, `SCALL` and `SRET` MUST TRAP with `ILLEGAL_OPCODE`.
- RS underflow/overflow rules and bounds are as defined in §1.

## A.4) Host/ABI calls in ZX64S

ZX64S MUST still use the same host surface (e.g. zABI) but the *calling convention* MAY be stack-native.

A ZX64S implementation MAY provide a stack-native hostcall opcode. This is **OPTIONAL** and not required for `+stacker.cpu:v1` conformance.

Example name shown (non-normative; exact naming is implementation-defined):

- `SHCALL #selector`

Where `#selector` identifies a known ABI signature at lowering time.

Normative constraints:

- `SHCALL` MUST have a statically known stack effect (input/output shapes) determined by the selected ABI selector.
- If the selector is unknown or not supported by the target, lowering MUST fail (static error).
- At runtime, `SHCALL` MUST bounds-check all guest-memory accesses per §7 and MUST obey the same trap taxonomy in §1.
- `SHCALL` is part of the ZX64S CPU addendum only and MUST NOT be exposed by Z64+S coprocessor runtimes.

Rationale (non-normative): this mapping is friendlier to JVM/CLR-style stacks while preserving the same deterministic sandbox/ABI rules.
