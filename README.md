# Zeta

Zeta is a **compiler kit**: a set of tools and contracts for turning language frontends into verified, reproducible executables—without baking any one source language into the middle-end.

At the center of Zeta is **SIR**: a versioned, verifier-enforced interchange format with a clearly defined codegen boundary (**SIR-Core**) and an opt-in higher-level surface (**SIR-HL**) that is deterministically lowered into Core by the toolchain.

---

## What you get

- **A hard boundary you can target**  
  If you emit **SIR-Core**, `sircc` can verify it and compile it.

- **Shared lowering for portable intent**  
  If you emit **SIR-HL** (currently: `sem:v1` intent), `sircc` can lower it deterministically into **SIR-Core**, so frontends don’t reinvent CFG construction, short-circuiting, switch lowering, or scoped defers.

- **A capability-controlled execution harness**  
  `sem` runs a supported SIR subset under explicit host capabilities, supports recording/replay (“tapes”), and provides a safe platform for testing, verification, and instrumentation.

- **An authoritative support surface**  
  `sircc --print-support` reports exactly what’s implemented vs missing (also bundled as `dist/doc/support.html` and `dist/doc/support.json` in the `dist/` package).

---

## Components

### 1) SIR (IR contract)

**Goal:** A stable, verifier-enforced interchange format.

- **SIR-Core**: small, stable, executable subset (the codegen boundary)
- **SIR-HL**: feature-gated higher-level surface, deterministically lowered to Core
  - Today, the blessed HL subset is `sem:v1` intent nodes (examples):
    - `sem.if`, `sem.cond`, `sem.and_sc`, `sem.or_sc`
    - `sem.switch`, `sem.match_sum`
    - `sem.while`, `sem.break`, `sem.continue`
    - `sem.defer`, `sem.scope`

### 2) `sircc` (compiler + verifier + lowerer)

**Goal:** Make SIR real.

- Verify-only: `sircc --verify-only`
- Lower HL→Core: `sircc --lower-hl --emit-sir-core out.core.sir.jsonl in.sir.jsonl`
- Compile: `sircc ... -o out`

### 3) `sirc` (optional: human-friendly surface)

**Goal:** Translate readable `.sir` into 1:1 `sir-v1.0` JSONL.

- `.sir` → `.sir.jsonl`
- Included in `dist/` when built with `-DSIR_ENABLE_SIRC=ON`

### 4) `sem` (host harness / emulator frontend)

**Goal:** Run and test SIR programs under explicit host capabilities.

- Capability discovery / listing (`--caps`)
- Capability injection (`--cap ...` / `--cap-file-fs` / etc)
- Sandboxed FS (`--fs-root`)
- Record/replay tapes (`--tape-out`, `--tape-in`)

---

## Quickstart (build + run)

Build the copy/pasteable bundle in `./dist/`:

```sh
cmake -S . -B build
cmake --build build --target dist
```

Then try the normative examples:

```sh
./dist/bin/<os>/sircc --check
./dist/bin/<os>/sircc ./dist/test/examples/mem_copy_fill.sir.jsonl -o /tmp/mem_copy_fill && /tmp/mem_copy_fill; echo $?
```

Support surface (authoritative):

```sh
./dist/bin/<os>/sircc --print-support --format html --full > /tmp/sircc_support.html
```

Or open the bundled docs:
- `dist/doc/sircc.md`
- `dist/doc/compiler_kit_cheatsheet.md`
- `dist/doc/support.html`

---

## Mental model

Zeta separates concerns into two “worlds” with a clean handoff:

1. **Frontend world:** *AST → SIR*
2. **Toolchain world (Zeta):** *SIR-HL/Core → verify/lower → compile/run*

### Architecture diagram (high level)

```
   ┌───────────────────────────┐
   │        FRONTENDS           │
   │ (language-specific logic)  │
   │  scope, typing, lowering   │
   └──────────────┬────────────┘
                  │  sir-v1.0 JSONL
                  v
   ┌───────────────────────────┐
   │           SIR              │
   │  SIR-HL (intent packs)     │
   │          ↓ sircc --lower   │
   │  SIR-Core (codegen bound)  │
   └──────────────┬────────────┘
                  │
                  v
   ┌───────────────────────────┐
   │           sircc            │
   │  verify / lower / compile  │
   └──────────────┬────────────┘
                  │
        ┌─────────┴─────────┐
        v                   v
┌────────────────┐  ┌──────────────────┐
│ native binaries │  │ sem host harness │
│ (LLVM backend)  │  │ run/record/replay│
└────────────────┘  └──────────────────┘
```

---

## Terminology

- **SIR**: Zeta’s IR contract (historically “semantic”; the stability contract is the point)
- **SIR-Core**: the smallest set that `sircc` verifies and codegens directly
- **SIR-HL**: feature-gated higher-level forms lowered deterministically to Core
- **Packs**: feature-gated extensions (e.g. `data:v1`, `sem:v1`)
- **Intent**: HL nodes that express portable semantics (currently: `sem:v1`)

---

## Integration modes

### If you want “just compile what I emit”

Emit **SIR-Core** only:

- Must pass: `sircc --verify-only your.sir.jsonl`
- Compile: `sircc --verify-strict your.sir.jsonl -o out`

### If you want the toolchain to do shared lowering

Emit **SIR-HL** with feature gates:

- Include: `{"k":"meta", "ext":{"features":["sem:v1", ...]}}`
- Debug lowering:  
  `sircc --lower-hl --lower-strict --emit-sir-core out.core.sir.jsonl in.sir.jsonl`

---

## Roadmap (short)

- Keep SIR-Core small and pinned (long-lived contract)
- Grow SIR-HL only via feature-gated, deterministic intent packs
- Expand “cookbook” emission patterns so frontends converge instead of diverging
- Treat any lowering rule change as an ABI-level decision unless proven semantics-preserving

---

## License

- Tools (`sircc`, `sirc`, `sem`) are **GPL-3.0-or-later**.
- The SIR format/spec + schemas are **MIT** (`schema/sir/**` + `LICENSE-LIB`).
