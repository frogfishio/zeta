# Zeta

Zeta is a **low-level programming language** *and* a **compiler kit**.

It gives you a single, versioned contract you can:

- **author directly** (as readable `.sir` source),
- **emit from other languages** (as `sir-v1.0` JSONL),
- then **verify**, **run under explicit capabilities**, and/or **compile to native binaries**.

The hook is the language: Zeta is “IR-shaped”, but it’s a genuinely usable systems language—close to C in power, more explicit in semantics, and designed to be **verifier-first** and **capability-first**.

---

## Quick taste (the language)

Hello world (calling an external symbol):

```sir
unit hello target host

extern fn puts(s:ptr) -> i32

fn main() -> i32 public
  puts("hello world")
  return 0:i32
end
```

Zeta’s surface (`.sir`, via `sirc`) is intentionally close to the underlying IR:

- one line of `.sir` becomes one JSONL record (1:1),
- flags are explicit (`+flag` / `+key=value`),
- types are explicit (`i32`, `ptr`, `vec(i32, 4)`, `fn(...) -> ...`, etc),
- the toolchain rejects unsupported or ambiguous constructs (by design).

---

## What Zeta is (big picture)

Zeta has grown into a full “platform in a box”:

- **Language + IR contract (SIR)**: a strict, versioned interchange format with a stable codegen boundary (**SIR‑Core**) and feature‑gated higher forms (**SIR‑HL**) that are deterministically lowered.
- **Verifier**: `sircc --verify-only` gives producers a hard pass/fail contract, with structured diagnostics.
- **Emulator / test harness**: `sem` runs a supported subset deterministically under explicit host capabilities, supports recording/replay (“tapes”), and is meant to be the integration test engine for frontends.
- **Zero‑trust execution core (library)**: `sircore` executes structured modules with *all* host interaction via a capability ABI (`zi_*` + `zi_ctl`).
- **Multi-backend direction**: compile via LLVM today; also emit `zasm-v1.1` JSONL (`sircc --emit-zasm`) as a backend-neutral lowering target.
- **A VM tier (in-tree)**: `svm` is the “full VM” direction (JIT/runtime services) and is expected to remain compatible with `sircore`’s reference semantics.

If LLVM is “IR + optimizer + codegen”, Zeta aims to be:

> **language + IR + verifier + emulator + capability ABI + multi-backend toolchain**

---

## Why this is compelling (vs “lower to C”)

Zeta’s value isn’t just code generation—it’s the whole correctness loop:

- **Determinism as a contract**: same program + same inputs/caps ⇒ same outputs.
- **Verifier-enforced structure**: types, references, CFG forms, and feature gates are validated; failures are explicit and stable.
- **No ambient authority**: programs can be written against `zi_*` capabilities instead of implicitly inheriting “the whole OS”.
- **Emulation-first DX**: frontends can do `AST → SIR → sem` to prove “it *runs correctly*”, not just “it compiles”.
- **Shared lowering (compiler kit)**: intent packs (e.g. `sem:v1`) stop every frontend from reinventing CFG plumbing, short-circuiting, switch lowering, and scoped defers.

---

## Why learn Zeta?

- **It’s a real low-level language.** Write kernels/drivers/embedded-style code *and* normal applications with explicit control over types, memory, and calling conventions.
- **It’s a better “lowering target” than “lower to C”.** You get a strict verifier, stable diagnostic context (`src`/`src_ref`/`loc`), and an execution harness (`sem`) that can prove a lowering is correct by running it deterministically under explicit capabilities.
- **It scales from hand-written code to compiler pipelines.** You can author `.sir` directly for fast iteration, or emit `sir-v1.0` JSONL from a frontend/DSL and rely on Zeta’s shared lowering and codegen.

---

## Components (repo)

### SIR (the contract)

- **SIR‑Core**: small, stable, executable subset (the codegen boundary).
- **SIR‑HL**: feature-gated higher-level forms, deterministically lowered to Core.
  - Today, the blessed HL subset is `sem:v1` intent nodes (examples):
    - `sem.if`, `sem.cond`, `sem.and_sc`, `sem.or_sc`
    - `sem.switch`, `sem.match_sum`
    - `sem.while`, `sem.break`, `sem.continue`
    - `sem.defer`, `sem.scope`

### Tools

- **`sircc`**: verifier + compiler + lowerer (LLVM backend; optional zasm emission).
- **`sirc`**: the human-friendly language front-end (`.sir` → `*.sir.jsonl`).
- **`sem`**: verifier + deterministic runner under explicit capabilities (built on `sircore`).

### Libraries / runtime tiers

- **`sircore`**: zero-trust execution core (structured module in; capability calls out).
- **`svm`**: VM tier (JIT/runtime direction; early, but the intent is explicit).

---

## Quickstart (build + run)

Build the copy/pasteable bundle in `./dist/`:

```sh
cmake -S . -B build
cmake --build build --target dist
```

Run the normative suite (alpha-user friendly):

```sh
./dist/bin/<os>/sircc --check
```

Compile an example:

```sh
./dist/bin/<os>/sircc ./dist/test/examples/mem_copy_fill.sir.jsonl -o /tmp/mem_copy_fill
/tmp/mem_copy_fill; echo $?
```

If `sirc` is bundled, try the full language pipeline:

```sh
./dist/bin/<os>/sirc ./dist/test/examples/hello.sir -o /tmp/hello.sir.jsonl
./dist/bin/<os>/sircc /tmp/hello.sir.jsonl -o /tmp/hello
/tmp/hello
```

Support surface (authoritative; matches the implementation):

```sh
./dist/bin/<os>/sircc --print-support --format html --full > /tmp/sircc_support.html
```

Bundled docs/examples live under:

- `dist/doc/`
- `dist/test/examples/`

---

## “Platform in a box” (modes)

Zeta is intentionally usable in multiple modes:

1) **Verify + compile to native** (LLVM):
- `sircc --verify-only your.sir.jsonl`
- `sircc your.sir.jsonl -o your_exe`

2) **Verify + run under explicit capabilities** (emulation / harness):
- `sem --verify your.sir.jsonl`
- `sem --run your.sir.jsonl --cap ... [--tape-out run.tape]`

3) **Lower to a backend-neutral IR** (alternate backends / emulator-driven workflows):
- `sircc your.sir.jsonl --emit-zasm -o out.zasm.jsonl`

These aren’t different products: they’re different *lenses* over the same contract.

---

## Versioning

- Project version lives in `./VERSION` and is reported by `sircc --version`, `sirc --version`, and `sem --version`.
- Convenience wrappers: `make build`, `make dist`, `make bump`.

## Status / expectations

- Zeta is under active development; the **verifier and support surface** are treated as the truth.
- Use `sircc --print-support` to see exactly what’s implemented today.
- The dist bundle is designed to be copy/pasteable for alpha users.

---

## License

- Tools (`sircc`, `sirc`, `sem`) and implementation code are **GPL-3.0-or-later**.
- The SIR format/spec + schemas + example IR programs are **MIT** (`schema/sir/**` + `LICENSE-LIB`).
