<!-- SPDX-FileCopyrightText: 2026 Frogfish -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Contributing to SIR

Thanks for contributing. This repo is intentionally small, stream-first, and hostile to bloat.

## Project shape

This repo is a small toolchain around **SIR** (Semantic IR):

- **`sirc`** — human-readable `.sir` → `sir-v1.0` `*.sir.jsonl` (translator; does not emit binaries)
- **`sircc`** — `sir-v1.0` `*.sir.jsonl` → native executables / object files / LLVM IR (via LLVM)

Design rule: **stage boundaries are stable**. If you change the JSONL IR, you are changing the contract.

## License and trademarks

- Tooling code (the compilers/translators) is **GPL-3.0-or-later** (see `LICENSE`).
- The SIR spec/schema and example IR programs are **MIT** (see `LICENSE-LIB`).
- By submitting a PR, you agree your contribution is licensed under the license of the files you change/add (GPL for tooling, MIT for spec/schema/examples, unless a file header says otherwise).
- **Trademarks:** “sir” and related marks are not granted under the GPL. Don’t use the project name/logo to market forks.

## Setup (macOS)

### Required tools
- CMake >= 3.20
- LLVM (Homebrew recommended) + a system `clang` for linking
- Optional (for `sirc`): flex + bison

## Build

Configure (Homebrew LLVM):

- `cmake -S . -B build -G Ninja -DLLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm`

Build:

- `cmake --build build`

Build a copy-pasteable bundle (binaries + docs + examples) into `./dist/`:

- `cmake --build build --target dist`

## Quick pipeline

- `.sir` → `.sir.jsonl` (requires `-DSIR_ENABLE_SIRC=ON` at configure time):
  - `./build/src/sirc/sirc src/sirc/examples/hello.sir -o /tmp/hello.sir.jsonl`
- `.sir.jsonl` → native executable:
  - `./build/src/sircc/sircc /tmp/hello.sir.jsonl -o /tmp/hello && /tmp/hello`

## Run examples (local harness)

The repo’s examples are intended to be runnable directly from the `dist` bundle:

- `./dist/bin/<os>/sircc ./dist/test/examples/hello_world_puts.sir.jsonl -o /tmp/hello && /tmp/hello`

## Tests

We prefer **golden tests**:

- input fixture → expected output bytes
- keep tests deterministic (no timestamps, no randomness)

Run tests:

- `ctest --test-dir build --output-on-failure`

## IR changes (JSONL contract)

If you need to change the IR schema:

- Update the schema in `schema/`.
- Update both `sirc` and `sircc` in the same PR.
- Add a migration note in the PR description.

Strong preference: **extend** rather than break. New fields should be optional when possible.

## Style

- C is C11.
- Keep code boring and readable.
- Avoid hidden global state.
- Errors should include `tool: message` and a source line if available.

## PR checklist

- [ ] Builds: `cmake --build build`
- [ ] Tests: `ctest --test-dir build --output-on-failure`
- [ ] Added/updated examples if behavior changes
- [ ] IR/schema updated if needed
- [ ] No gratuitous refactors (separate PRs)

## Security

If you find a security issue (especially around parsing or bounds handling), please open a private report instead of posting a public issue.

---

Welcome aboard.
