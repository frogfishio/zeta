<!-- SPDX-FileCopyrightText: 2025 Frogfish -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# SIR — Semantic IR (and SIRC, the human-friendly surface)

SIR is a **streaming semantic intermediate representation** designed for compilers and build tools that care about **correctness, debuggability, and controllable lowering**.

Most IRs are great for machines and painful for humans. SIR is intentionally **both**:

- **Machine form:** strict, line-delimited **JSONL** records (`sir-v1.0`) that are tooling-friendly and easy to generate.
- **Human form (SIRC):** a small, readable **text syntax** that compiles 1:1 into the same JSONL shapes.

That “1:1 human/machine” property is the killer feature:

- You can **see** exactly what a high-level abstraction lowers to.
- You can **tweak** the lowering when performance/correctness matters.
- You can **patternize** and teach the compiler new lowering strategies.
- You can diff/grep/stream the IR like logs, and still keep semantic identity.

SIR sits in the sweet spot: **semantic enough** to be analyzable, but **flat enough** to be streamable and predictable.

---

## Quick taste (SIRC)

```sir
unit u0 target wasm32 +agg:v1

fn add(a:i32, b:i32) -> i32 public
  return i32.add(a, b)
end
```

The above is a tiny human-friendly program. `sirc` lowers it into `sir-v1.0` JSONL; `sircc` can then verify and compile it.

---

## What you get

### Streaming by default
SIR is JSONL: **one record per line**, in order. Tools can:

- read incrementally,
- attach diagnostics as they go,
- merge streams,
- or process massive units without building a giant in-memory AST.

### Semantic identity (not span-based)
SIR prefers **stable numeric IDs** for semantic entities (symbols/types/nodes) rather than “identity-by-source-span”.

- Spans exist (via `src` + `src_ref` + `loc`) for great error messages.
- Identity lives in explicit records (`sym`/`type`/`node` with `id`).

This makes transformations, caching, and cross-tool pipelines far more reliable.

### Extensible without schema looseness
Instead of “anything goes” objects, SIR stays strict and grows via explicit extension records:

- `ext` records carry producer/tool payloads.
- Consumers can ignore unknown extensions without losing validation.

---

## Tools in this repo

### `sircc` — SIR JSONL → native
- verifies `sir-v1.0`
- emits native executables, object files, or LLVM IR

### `sirc` — SIRC text → SIR JSONL (experimental)
- flex/bison-based translator
- compiles readable `.sir` into `*.sir.jsonl` (`sir-v1.0`)

---

## Build `sircc` (SIR JSONL → native)

### Prerequisites (macOS)

- CMake >= 3.20
- LLVM (with CMake package config)
- A system `clang` for linking

If you installed LLVM with Homebrew:

- `brew install llvm cmake ninja`
- Configure with `-DLLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm`

### Configure + build

- `cmake -S . -B build -G Ninja -DLLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm`
- `cmake --build build`
- Build the copy-pasteable bundle in `./dist/`: `cmake --build build --target dist`

### Versioning

- Project version lives in `./VERSION` and is embedded into `sircc --version` / `sirc --version`.
- Convenience wrapper: `make bump` (increments patch version) and `make dist` (builds the bundle).

### Run

- Emit an executable: `./build/src/sircc/sircc src/sircc/examples/add.sir.jsonl -o add`
- Emit LLVM IR: `./build/src/sircc/sircc src/sircc/examples/add.sir.jsonl --emit-llvm -o add.ll`
- Emit object only: `./build/src/sircc/sircc src/sircc/examples/add.sir.jsonl --emit-obj -o add.o`
- Verify only (no codegen): `./build/src/sircc/sircc --verify-only src/sircc/examples/add.sir.jsonl`

Optional flags:

- Pick a linker: `--clang clang` (default: `clang`)
- Override the LLVM target triple: `--target-triple <triple>`
- More tooling: `--verbose`, `--strip`, `--diagnostics json`, `--color auto|always|never`, `--diag-context N`

---

## Build `sirc` (experimental .sir parser)

`sirc` translates SIRC source (`.sir`) into `sir-v1.0` `*.sir.jsonl`.

- Configure with: `cmake -S . -B build -G Ninja -DSIR_ENABLE_SIRC=ON -DLLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm`
- Build: `cmake --build build`
- Bundle both `sircc` + `sirc` into `./dist/`: `cmake --build build --target dist`
- Translate: `./build/src/sirc/sirc src/sirc/examples/hello.sir -o /tmp/hello.sir.jsonl`
- Compile: `./build/src/sircc/sircc /tmp/hello.sir.jsonl -o /tmp/hello && /tmp/hello`

### Using the dist bundle

After building `dist`, you can hand the whole `./dist/` folder to alpha users. It contains:

- `dist/bin/<os>/sircc` (JSONL → native)
- `dist/bin/<os>/sirc` (optional, if built with `-DSIR_ENABLE_SIRC=ON`) (`.sir` → JSONL)
- `dist/doc/sircc.md` and `dist/doc/sirc.md`
- `dist/test/examples/` (paired `.sir` and `.sir.jsonl` examples)

Quick run (macOS example path):

- `./dist/bin/macos/sirc ./dist/test/examples/hello.sir -o /tmp/hello.sir.jsonl`
- `./dist/bin/macos/sircc /tmp/hello.sir.jsonl -o /tmp/hello && /tmp/hello`

---

## SIRC encoding model (how text maps to JSONL)

SIRC is “pretty sugar” over the same JSONL record shapes that `sircc` accepts.

**Rule:**
- Positional operands go in `(...)`.
- Everything else is a flat attribute tail:
  - flags: `+flag`
  - keyed fields: `key <value>`

Examples:

```sir
mem.copy(dst, src, len) +volatile +nontemporal
call.indirect(fp, a, b) sig add
let p: ptr = alloca(i32) count 4:i64 align 16:i64 +zero
term.trap code "bounds" msg "index out of range"
```

This mirrors the `unit ... target wasm32 +agg:v1` style: readable, brace-free, and maps cleanly to `fields.*` / `fields.flags`.

---

## Why write SIR (instead of only generating it)?

- **Lowering is where performance lives.** SIR lets you express and iterate on lowering directly.
- **Debugging is cheaper.** The representation is explicit, typed, and streamable.
- **Tooling is straightforward.** JSONL makes it easy to build linters, verifiers, reducers, minimizers, and fuzzers.
- **It’s actually a usable surface language.** SIRC reads more like a small systems language than “IR soup”.

---

## Status

- SIR JSONL (`sir-v1.0`) is the stable interchange target.
- SIRC is experimental and evolving quickly (DX-first).

---

## Contributing

PRs welcome. If you’re building a producer/consumer, please:

- keep streams valid JSONL (one record per line),
- preserve numeric IDs for semantic identity,
- put tool-only payloads in `ext` records.

---

## License

The repo is intentionally split so the **tooling is copyleft**, while the **format/spec is permissive**:

- **GPL-3.0-or-later**: compiler tooling (`sircc`, `sirc`) and the rest of the program code (see `LICENSE` and per-file SPDX headers).
- **MIT**: the SIR specification/schema and example IR programs used for testing/interoperability (see `LICENSE-LIB` and per-file SPDX headers, e.g. `schema/sir/**`).
- Third-party code, if present, is licensed as marked by its own headers.

Note: the GPL in this repository applies to the **compiler programs we ship**. It does not, by itself, place the GPL on the binaries you compile with `sircc` (unless you intentionally incorporate GPL-covered code/components into your output).
