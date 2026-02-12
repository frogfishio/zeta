# Below is a minimal-but-complete stacker opcode family that fits how ZASM already “thinks” (Zilog-ish mnemonics, explicit operands, easy to JIT, easy to validate, easy to lower to WAT).

---

1) Stacker state model (what exists)

Two virtual stacks:
	•	DS = data stack (64-bit cells)
	•	RS = return/control stack (optional, but very useful for Forth-ish lowering)

Cells are 64-bit. i32 values are represented as either:
	•	zext (preferred for “WASM-ish” i32), or
	•	sext (preferred for “Z80-ish” signed math)
…and the opcode spells which, so lowering is deterministic.

This mirrors WASM’s “everything is typed but runtime stack is just values”.

---

2) Interop ops (bridge classic regs ↔ stack)

These are the money ops. They’re why stacker is an add-on instead of a new machine.

Push/pop classic regs
	•	SPUSH.HL / SPOP.HL
	•	SPUSH.DE / SPOP.DE
	•	SPUSH.BC / SPOP.BC
	•	SPUSH.IX / SPOP.IX
	•	SPUSH.A  / SPOP.A   (if you treat A as 64 or 32; pick one and lock it)

Push immediates (typed)
	•	SPUSH.I32 #imm  (zext to cell)
	•	SPUSH.S32 #imm  (sext to cell)
	•	SPUSH.I64 #imm64

Move/peek between stack and regs without popping (optional but handy)
	•	SPEEK.HL #k   (k=0 is TOS, does not pop)
	•	SPOKE #k, HL  (write stack slot k from HL)

WASM lowering: SPUSH.<reg> ↔ local.get $reg64 ; SPOP.<reg> ↔ local.set $reg64.
SPEEK/SPOKE just use temp locals + stack shuffles.

---

3) Pure stack shuffles (core DS discipline)

These should be small and orthogonal:
	•	SDROP        (pop 1)
	•	SDUP         (dup TOS)
	•	SSWAP        (swap TOS, NOS)
	•	SOVER        (push NOS)
	•	SROT         (rotate top 3: a b c → b c a)
	•	SNIP         (a b → b)
	•	STUCK        (a b → b a b)

Two-cell variants (optional, but great for 64-bit pairs / abi-like things):
	•	S2DROP, S2DUP, S2SWAP, S2OVER

WASM note: WASM doesn’t have dup/swap, but lowering can always synthesize with scratch locals.

---

4) Stack ALU ops (typed, no hidden flags)

Avoid “flag registers” for stacker; return booleans as explicit values (WASM style).

i32 ops (operate on low 32 bits; push i32 result zext)
	•	SADD.I32, SSUB.I32, SMUL.I32
	•	SDIV.S32, SDIV.U32, SREM.S32, SREM.U32
	•	SAND.I32, SOR.I32, SXOR.I32
	•	SSHL.I32, SSHR.S32, SSHR.U32
	•	SEQ.I32, SNE.I32, SLT.S32, SLT.U32, SGE.S32, SGE.U32, etc.
	•	SEQZ.I32

i64 ops

Same family:
	•	SADD.I64, SDIV.S64, SSHR.U64, etc.

This keeps stacker “WASM-aligned” by construction.
Also makes Mach-O lowering / ARM64 JIT straightforward: typed op → typed machine op.

---

5) Memory ops that match WASM (addr lives on stack)

This is huge for “100% aligned with WASM”:
	•	SLOAD.I32   (addr → val)
	•	SLOAD.I64
	•	SLOAD8.U32, SLOAD8.S32, SLOAD16.*, SLOAD32.*
	•	SSTORE.I32  (addr, val → )
	•	SSTORE.I64
	•	SSTORE8, SSTORE16, SSTORE32

If you care about alignment/volatility:
	•	SLOAD.I32 align #n
	•	SSTORE.I32 align #n
	•	plus optional +vol flag like your SIR style.

WASM lowering is literally 1:1 here.
Non-stack targets: your own rule applies — if the profile says “stacker required”, you either lower to a stack machine (WASM/JVM/CLR stack IL) or you error out.

---

6) Control-flow interop (keep CALL/JR/RET as-is)

You do not need “stacker CALL” if you treat stacker as a data substrate:
	•	Call conventions become lowering policy, not ISA.
	•	To call zABI/syscalls, you SPOP.HL, SPOP.DE, etc. (or SPEEK), then CALL zi_write.

That said, two optional opcodes are very useful for tooling and validation:
	•	SASSERT.DEPTH #n
Fails (trap) if DS depth ≠ n at that point. Great for fuzzing + verifying lowerings.
	•	SANNOT.SIG #id
Pure metadata for zem/debug traces (“this point expects (i32,i32)->i32”), can be stripped.

---

7) Return stack (RS) – optional but I’d add it

If you want Forth-like lowering without abusing DS for control:
	•	RPUSH / RPOP (64-bit cells)
	•	RDUP, RSWAP, RDROP
	•	RPEEK.HL #k / RPOKE #k, HL (optional)
	•	R>, >R style ops as aliases:
	•	S2R  (move TOS DS → RS)
	•	R2S  (move TOS RS → DS)

WASM lowering: RS is just a second value stack implemented via a linear-memory region + an rsp local (or in locals if shallow). Deterministic and explicit.

---

Naming: keep it Zilog-ish but unambiguous

I’d strongly avoid plain PUSH/POP because people will confuse it with the call stack. Prefix keeps your “coprocessor” feel:
	•	SPUSH.*, SPOP.*, S... for DS ops
	•	RPUSH, RPOP, R... for RS ops

This also makes illegal-in-profile validation trivial: “any mnemonic starting with S/R is stacker”.

---

Tiny example: “mixed work” the way you described

“Push HL, do stack work, pop HL back”:

; HL holds some value we want to transform with stacky code
SPUSH.HL

SPUSH.I32 #10
SADD.I32          ; (HL + 10) in i32 space

SPOP.HL           ; back to classic world
; now you can do normal LD/CP/JR etc

That is exactly “materialize into regs/locals, work, rejoin discipline” — and it lowers cleanly to WASM with locals.

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

Comparisons push an **i32 boolean** in a cell:

- `0` = false
- `1` = true

(Encoded as zext to 64-bit in the cell.)

---

## 2) Profiles / capability gating

Profiles gate legality at **lowering time**. There is no runtime autoswitch.

Recommended profiles:

- **`+stacker:v1`** (base):
  - `cell64` DS ops
  - `.I64` ALU ops
  - interop ops (push/pop regs)
  - memory ops at `.I64` / `.I32` granularity (see below)
- **`+stacker.i32ops:v1`** (optional):
  - enables `.I32` / `.S32` arithmetic/bitops/compares and the 8/16/32-bit load/store variants

Notes:

- Profiles do **not** change cell width. Cells remain 64-bit.
- If a lowering target cannot support a required profile (e.g. no FP/SIMD/stack substrate), lowering MUST error out.

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

**Width rule:** decide once whether `A` is treated as a 32-bit or 64-bit register in your concrete machine and keep it consistent.

### Push immediates (typed)

- `SPUSH.I32 #imm`  (zext32 → cell)
- `SPUSH.S32 #imm`  (sext32 → cell)
- `SPUSH.I64 #imm64`

### Peek/poke (does not change stack depth)

Optional but extremely handy for lowering and debugging:

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

Two-cell variants (optional; great for paired values / ABI-like shapes):

- `S2DROP`, `S2DUP`, `S2SWAP`, `S2OVER`

**WASM note:** WASM doesn’t have dup/swap, but lowering can always synthesize with scratch locals.

---

## 6) Stack ALU ops (typed, no flags)

Stacker should not introduce “flag registers”. Return booleans as explicit values.

### i32 family (requires `+stacker.i32ops:v1`)

Operate on `low32` and return a zext32 (or sext32 for `.S32` forms) cell.

Arithmetic / bitwise:

- `SADD.I32`, `SSUB.I32`, `SMUL.I32`
- `SDIV.S32`, `SDIV.U32`, `SREM.S32`, `SREM.U32`
- `SAND.I32`, `SOR.I32`, `SXOR.I32`
- `SSHL.I32`, `SSHR.S32`, `SSHR.U32`

Comparisons (push 0/1 as i32-in-cell):

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

Comparisons (push 0/1 as i32-in-cell):

- `SEQ.I64`, `SNE.I64`
- `SLT.S64`, `SLT.U64`, `SLE.S64`, `SLE.U64`
- `SGT.S64`, `SGT.U64`, `SGE.S64`, `SGE.U64`
- `SEQZ.I64`

---

## 7) Memory ops (WASM-shaped: addr lives on DS)

This is the core of “100% aligned with WASM”. Address is a cell; lowering chooses interpretation.

### Loads

- `SLOAD.I32`   ( addr → val )  
  Loads 32 bits from memory; returns zext32 in cell.
- `SLOAD.I64`   ( addr → val )

Sized loads (require `+stacker.i32ops:v1`):

- `SLOAD8.U32`, `SLOAD8.S32`
- `SLOAD16.U32`, `SLOAD16.S32`
- `SLOAD32.U32`, `SLOAD32.S32`  (explicit forms if you want to spell extension)

### Stores

- `SSTORE.I32`  ( addr val → )  
  Stores low32(val).
- `SSTORE.I64`  ( addr val → )

Sized stores (require `+stacker.i32ops:v1`):

- `SSTORE8`, `SSTORE16`, `SSTORE32`

### Alignment / volatility (optional)

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

Optional tooling/validation opcodes:

- `SASSERT.DEPTH #n`  
  Trap/fail if DS depth ≠ n.
- `SANNOT.SIG #id`  
  Debug metadata for traces (“expects (i32,i32)->i32 here”). Safe to strip.

---

## 9) Return stack (RS) — optional but recommended

If you want Forth-like lowering without abusing DS for control:

Core:

- `RPUSH` / `RPOP`  (64-bit cells)
- `RDUP`, `RSWAP`, `RDROP`

Interop between DS and RS:

- `S2R`   ( DS: a → ; RS: → a )
- `R2S`   ( RS: a → ; DS: → a )

Peek/poke (optional):

- `RPEEK.HL #k` / `RPOKE #k, HL`

**WASM lowering hint:** RS can be implemented as a second stack backed by linear memory (with an `rsp` local), or synthesized with locals if bounded by validation.

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

…without introducing states a WASM machine couldn’t represent.
# Stacker (ZASM extension) — v1 opcode family

Stacker is an **optional coprocessor/profile** that adds a WASM-aligned value-stack substrate to ZASM **without replacing** the classic register substrate.

Design goals:

- **Add-on substrate, not a new machine:** push classic regs/locals into the stack world, do work, pop back *seamlessly*.
- **WASM-aligned by construction:** every dynamic state introduced by stacker is representable on a WASM-style value stack.
- **Deterministic lowering:** no hidden type state, no “auto-switch”; legality is decided by the selected profile at lowering time.
- **Tool/JIT friendly:** simple validation (stack effects), easy to fuzz/trace in `zem`, easy to lower to WAT/JVM/CLR.

---

## 1) State model

Stacker introduces two virtual stacks:

- **DS** — data stack
- **RS** — return/control stack *(optional but recommended for Forth-ish lowering)*

### Cell width (hard invariant)

- **All stack cells are 64-bit (`cell64`).**
- Smaller widths (e.g. i32) are represented *inside* a cell.

This keeps the machine simple and makes classic-reg interop cheap.

### i32-in-a-cell canonicalization

Stacker does **not** carry a typed stack at runtime. Instead, the opcode suffix spells semantics.

- `*.I32` operations:
  - consume operands from cells
  - operate on `low32(x)` / `low32(y)`
  - produce a cell where the result is **zero-extended** to 64-bit

- `*.S32` operations (where applicable):
  - consume operands from cells
  - operate on `low32` in a **signed** domain
  - produce a cell where the result is **sign-extended** to 64-bit

Typed immediates:

- `SPUSH.I32 #imm` → `zext32(imm)` into a cell
- `SPUSH.S32 #imm` → `sext32(imm)` into a cell
- `SPUSH.I64 #imm64` → 64-bit immediate into a cell

### Boolean representation

Comparisons push an **i32 boolean** in a cell:

- `0` = false
- `1` = true

(Encoded as `zext32` into the 64-bit cell.)

---

## 2) Profiles / capability gating

Profiles gate legality at **lowering time**. There is no runtime autoswitch.

Recommended profiles:

- **`+stacker:v1`** (base)
  - `cell64` DS ops
  - `.I64/.S64/.U64` ALU ops
  - classic regs ↔ DS interop ops
  - memory ops at `.I64` and `.I32` (see below)

- **`+stacker.i32ops:v1`** (optional)
  - enables `.I32/.S32/.U32` arithmetic/bitops/compares
  - enables 8/16/32-bit load/store variants

Notes:

- Profiles do **not** change cell width.
- If a lowering target cannot support a required profile, lowering MUST **error out**.

---

## 3) Naming conventions

Avoid plain `PUSH/POP` to prevent confusion with call stacks.

- **DS ops**: `S*` prefix (e.g. `SPUSH.*`, `SADD.I64`)
- **RS ops**: `R*` prefix (e.g. `RPUSH`, `RPOP`, `R2S`)

Typed suffixes:

- `.I64` — 64-bit domain (value-level)
- `.S64` / `.U64` — signed/unsigned domains (div/shifts/compares)
- `.I32` — 32-bit domain, result `zext32` → cell
- `.S32` / `.U32` — signed/unsigned 32-bit domains where relevant

> Recommendation: keep ALU ops **always suffixed** (no plain `SADD`) to avoid hidden type state.

---

## 4) Interop ops (classic regs ↔ DS)

These are the “money ops” — they make stacker an add-on instead of a separate machine.

### Push/pop classic regs

- `SPUSH.HL` / `SPOP.HL`
- `SPUSH.DE` / `SPOP.DE`
- `SPUSH.BC` / `SPOP.BC`
- `SPUSH.IX` / `SPOP.IX`
- `SPUSH.A`  / `SPOP.A`

**Width rule:** choose once whether `A` is treated as 32-bit or 64-bit in the concrete machine and keep it consistent.

### Push immediates (typed)

- `SPUSH.I32 #imm`  *(zext32 → cell)*
- `SPUSH.S32 #imm`  *(sext32 → cell)*
- `SPUSH.I64 #imm64`

### Peek/poke (optional; does not change stack depth)

- `SPEEK.HL #k`   *(k=0 is TOS; does not pop)*
- `SPOKE #k, HL`  *(write DS slot k from HL)*

WASM lowering hint:

- `SPUSH.<reg>` ↔ `local.get $reg64`
- `SPOP.<reg>`  ↔ `local.set $reg64`

---

## 5) Pure stack shuffles (DS)

Keep the shuffle set small and orthogonal:

- `SDROP`        *( a → )*
- `SDUP`         *( a → a a )*
- `SSWAP`        *( a b → b a )*
- `SOVER`        *( a b → a b a )*
- `SROT`         *( a b c → b c a )*
- `SNIP`         *( a b → b )*
- `STUCK`        *( a b → b a b )*

Two-cell variants (optional; useful for paired values / ABI-like shapes):

- `S2DROP`, `S2DUP`, `S2SWAP`, `S2OVER`

WASM note: WASM has no `dup/swap`, but lowering can always synthesize with scratch locals.

---

## 6) Stack ALU ops (typed, no flags)

Stacker should not introduce flag registers. Comparisons push booleans as values.

### i32 family (requires `+stacker.i32ops:v1`)

Arithmetic / bitwise:

- `SADD.I32`, `SSUB.I32`, `SMUL.I32`
- `SDIV.S32`, `SDIV.U32`, `SREM.S32`, `SREM.U32`
- `SAND.I32`, `SOR.I32`, `SXOR.I32`
- `SSHL.I32`, `SSHR.S32`, `SSHR.U32`

Comparisons (push 0/1 as i32-in-cell):

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

Comparisons (push 0/1 as i32-in-cell):

- `SEQ.I64`, `SNE.I64`
- `SLT.S64`, `SLT.U64`, `SLE.S64`, `SLE.U64`
- `SGT.S64`, `SGT.U64`, `SGE.S64`, `SGE.U64`
- `SEQZ.I64`

---

## 7) Memory ops (WASM-shaped: addr lives on DS)

Address is a cell. Lowering decides the address interpretation (e.g. u32 offset in wasm32).

### Loads

- `SLOAD.I32`   *( addr → val )*  
  Loads 32 bits; returns `zext32` in a cell.
- `SLOAD.I64`   *( addr → val )*

Sized loads (requires `+stacker.i32ops:v1`):

- `SLOAD8.U32`, `SLOAD8.S32`
- `SLOAD16.U32`, `SLOAD16.S32`
- `SLOAD32.U32`, `SLOAD32.S32` *(explicit extension forms if you want them)*

### Stores

- `SSTORE.I32`  *( addr val → )*  
  Stores `low32(val)`.
- `SSTORE.I64`  *( addr val → )*

Sized stores (requires `+stacker.i32ops:v1`):

- `SSTORE8`, `SSTORE16`, `SSTORE32`

### Alignment / volatility (optional)

If you care about alignment/volatility, mirror your attribute-tail style:

- `SLOAD.I32 align #n`
- `SSTORE.I32 align #n`
- optional `+vol`

WASM lowering for the core loads/stores can be 1:1.

---

## 8) Control-flow interop (keep CALL/JR/RET as-is)

Stacker is a **data substrate**. You do not need a parallel CALL/branch ISA.

Calling zABI/syscalls is lowering policy:

- `SPOP.HL`, `SPOP.DE`, `SPOP.BC`, ...
- `CALL zi_write`

Optional tooling/validation opcodes:

- `SASSERT.DEPTH #n` — trap/fail if DS depth ≠ n
- `SANNOT.SIG #id` — trace/debug metadata (“expects (i32,i32)->i32 here”), safe to strip

---

## 9) Return stack (RS) — optional but recommended

If you want Forth-like lowering without abusing DS for control:

Core:

- `RPUSH` / `RPOP` *(64-bit cells)*
- `RDUP`, `RSWAP`, `RDROP`

Interop between DS and RS:

- `S2R`  *( DS: a → ; RS: → a )*
- `R2S`  *( RS: a → ; DS: → a )*

Peek/poke (optional):

- `RPEEK.HL #k` / `RPOKE #k, HL`

WASM lowering hint: RS can be implemented as a second stack backed by linear memory (with an `rsp` local), or synthesized with locals if bounded by validation.

---

## 10) Tiny example (mixed work)

Push HL → do stack work → pop HL back:

```asm
SPUSH.HL

SPUSH.I32 #10
SADD.I32          ; (HL + 10) in i32 space → zext32 in cell

SPOP.HL           ; back to classic world
; continue with classic LD/CP/JR/etc.
```

This matches the goal:

- materialize values into classic regs/locals,
- do work in the stack substrate,
- rejoin classic logic,

…without introducing states a WASM machine couldn’t represent.
# Stacker (ZASM extension) — v1 opcode family

Stacker is an **optional coprocessor/profile** that adds a WASM-aligned value-stack substrate to ZASM **without replacing** the classic register substrate.

Design goals:

- **Add-on substrate, not a new machine:** push classic regs/locals into the stack world, do work, pop back *seamlessly*.
- **WASM-aligned by construction:** every dynamic state introduced by stacker is representable on a WASM-style value stack.
- **Deterministic lowering:** no hidden type state, no “auto-switch”; legality is decided by the selected profile at lowering time.
- **Tool/JIT friendly:** simple validation (stack effects), easy to fuzz/trace in `zem`, easy to lower to WAT/JVM/CLR.

---

## 1) State model

Stacker introduces two virtual stacks:

- **DS** — data stack
- **RS** — return/control stack *(optional but recommended for Forth-ish lowering)*

### Cell width (hard invariant)

- **All stack cells are 64-bit (`cell64`).**
- Smaller widths (e.g. i32) are represented *inside* a cell.

This keeps the machine simple and makes classic-reg interop cheap.

### i32-in-a-cell canonicalization

Stacker does **not** carry a typed stack at runtime. Instead, the opcode suffix spells semantics.

- `*.I32` operations:
  - consume operands from cells
  - operate on `low32(x)` / `low32(y)`
  - produce a cell where the result is **zero-extended** to 64-bit

- `*.S32` operations (where applicable):
  - consume operands from cells
  - operate on `low32` in a **signed** domain
  - produce a cell where the result is **sign-extended** to 64-bit

Typed immediates:

- `SPUSH.I32 #imm` → `zext32(imm)` into a cell
- `SPUSH.S32 #imm` → `sext32(imm)` into a cell
- `SPUSH.I64 #imm64` → 64-bit immediate into a cell

### Boolean representation

Comparisons push an **i32 boolean** in a cell:

- `0` = false
- `1` = true

(Encoded as `zext32` into the 64-bit cell.)

---

## 2) Profiles / capability gating

Profiles gate legality at **lowering time**. There is no runtime autoswitch.

Recommended profiles:

- **`+stacker:v1`** (base)
  - `cell64` DS ops
  - `.I64/.S64/.U64` ALU ops
  - classic regs ↔ DS interop ops
  - memory ops at `.I64` and `.I32` (see below)

- **`+stacker.i32ops:v1`** (optional)
  - enables `.I32/.S32/.U32` arithmetic/bitops/compares
  - enables 8/16/32-bit load/store variants

Notes:

- Profiles do **not** change cell width.
- If a lowering target cannot support a required profile, lowering MUST **error out**.

---

## 3) Naming conventions

Avoid plain `PUSH/POP` to prevent confusion with call stacks.

- **DS ops**: `S*` prefix (e.g. `SPUSH.*`, `SADD.I64`)
- **RS ops**: `R*` prefix (e.g. `RPUSH`, `RPOP`, `R2S`)

Typed suffixes:

- `.I64` — 64-bit domain (value-level)
- `.S64` / `.U64` — signed/unsigned domains (div/shifts/compares)
- `.I32` — 32-bit domain, result `zext32` → cell
- `.S32` / `.U32` — signed/unsigned 32-bit domains where relevant

> Recommendation: keep ALU ops **always suffixed** (no plain `SADD`) to avoid hidden type state.

---

## 4) Interop ops (classic regs ↔ DS)

These are the “money ops” — they make stacker an add-on instead of a separate machine.

### Push/pop classic regs

- `SPUSH.HL` / `SPOP.HL`
- `SPUSH.DE` / `SPOP.DE`
- `SPUSH.BC` / `SPOP.BC`
- `SPUSH.IX` / `SPOP.IX`
- `SPUSH.A`  / `SPOP.A`

**Width rule:** choose once whether `A` is treated as 32-bit or 64-bit in the concrete machine and keep it consistent.

### Push immediates (typed)

- `SPUSH.I32 #imm`  *(zext32 → cell)*
- `SPUSH.S32 #imm`  *(sext32 → cell)*
- `SPUSH.I64 #imm64`

### Peek/poke (optional; does not change stack depth)

- `SPEEK.HL #k`   *(k=0 is TOS; does not pop)*
- `SPOKE #k, HL`  *(write DS slot k from HL)*

WASM lowering hint:

- `SPUSH.<reg>` ↔ `local.get $reg64`
- `SPOP.<reg>`  ↔ `local.set $reg64`

---

## 5) Pure stack shuffles (DS)

Keep the shuffle set small and orthogonal:

- `SDROP`        *( a → )*
- `SDUP`         *( a → a a )*
- `SSWAP`        *( a b → b a )*
- `SOVER`        *( a b → a b a )*
- `SROT`         *( a b c → b c a )*
- `SNIP`         *( a b → b )*
- `STUCK`        *( a b → b a b )*

Two-cell variants (optional; useful for paired values / ABI-like shapes):

- `S2DROP`, `S2DUP`, `S2SWAP`, `S2OVER`

WASM note: WASM has no `dup/swap`, but lowering can always synthesize with scratch locals.

---

## 6) Stack ALU ops (typed, no flags)

Stacker should not introduce flag registers. Comparisons push booleans as values.

### i32 family (requires `+stacker.i32ops:v1`)

Arithmetic / bitwise:

- `SADD.I32`, `SSUB.I32`, `SMUL.I32`
- `SDIV.S32`, `SDIV.U32`, `SREM.S32`, `SREM.U32`
- `SAND.I32`, `SOR.I32`, `SXOR.I32`
- `SSHL.I32`, `SSHR.S32`, `SSHR.U32`

Comparisons (push 0/1 as i32-in-cell):

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

Comparisons (push 0/1 as i32-in-cell):

- `SEQ.I64`, `SNE.I64`
- `SLT.S64`, `SLT.U64`, `SLE.S64`, `SLE.U64`
- `SGT.S64`, `SGT.U64`, `SGE.S64`, `SGE.U64`
- `SEQZ.I64`

---

## 7) Memory ops (WASM-shaped: addr lives on DS)

Address is a cell. Lowering decides the address interpretation (e.g. u32 offset in wasm32).

### Loads

- `SLOAD.I32`   *( addr → val )*  
  Loads 32 bits; returns `zext32` in a cell.
- `SLOAD.I64`   *( addr → val )*

Sized loads (requires `+stacker.i32ops:v1`):

- `SLOAD8.U32`, `SLOAD8.S32`
- `SLOAD16.U32`, `SLOAD16.S32`
- `SLOAD32.U32`, `SLOAD32.S32` *(explicit extension forms if you want them)*

### Stores

- `SSTORE.I32`  *( addr val → )*  
  Stores `low32(val)`.
- `SSTORE.I64`  *( addr val → )*

Sized stores (requires `+stacker.i32ops:v1`):

- `SSTORE8`, `SSTORE16`, `SSTORE32`

### Alignment / volatility (optional)

If you care about alignment/volatility, mirror your attribute-tail style:

- `SLOAD.I32 align #n`
- `SSTORE.I32 align #n`
- optional `+vol`

WASM lowering for the core loads/stores can be 1:1.

---

## 8) Control-flow interop (keep CALL/JR/RET as-is)

Stacker is a **data substrate**. You do not need a parallel CALL/branch ISA.

Calling zABI/syscalls is lowering policy:

- `SPOP.HL`, `SPOP.DE`, `SPOP.BC`, ...
- `CALL zi_write`

Optional tooling/validation opcodes:

- `SASSERT.DEPTH #n` — trap/fail if DS depth ≠ n
- `SANNOT.SIG #id` — trace/debug metadata (“expects (i32,i32)->i32 here”), safe to strip

---

## 9) Return stack (RS) — optional but recommended

If you want Forth-like lowering without abusing DS for control:

Core:

- `RPUSH` / `RPOP` *(64-bit cells)*
- `RDUP`, `RSWAP`, `RDROP`

Interop between DS and RS:

- `S2R`  *( DS: a → ; RS: → a )*
- `R2S`  *( RS: a → ; DS: → a )*

Peek/poke (optional):

- `RPEEK.HL #k` / `RPOKE #k, HL`

WASM lowering hint: RS can be implemented as a second stack backed by linear memory (with an `rsp` local), or synthesized with locals if bounded by validation.

---

## 10) Tiny example (mixed work)

Push HL → do stack work → pop HL back:

```asm
SPUSH.HL

SPUSH.I32 #10
SADD.I32          ; (HL + 10) in i32 space → zext32 in cell

SPOP.HL           ; back to classic world
; continue with classic LD/CP/JR/etc.
```

This matches the goal:

- materialize values into classic regs/locals,
- do work in the stack substrate,
- rejoin classic logic,

…without introducing states a WASM machine couldn’t represent.