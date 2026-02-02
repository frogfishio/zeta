# sir

This repository contains the SIR (Semantic IR) draft specification and tooling.

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

### Run

- Emit an executable: `./build/src/sircc/sircc src/sircc/examples/add.sir.jsonl -o add`
- Emit LLVM IR: `./build/src/sircc/sircc src/sircc/examples/add.sir.jsonl --emit-llvm -o add.ll`
- Emit object only: `./build/src/sircc/sircc src/sircc/examples/add.sir.jsonl --emit-obj -o add.o`
- Verify only (no codegen): `./build/src/sircc/sircc --verify-only src/sircc/examples/add.sir.jsonl`

Optional flags:

- Pick a linker: `--clang clang` (default: `clang`)
- Override the LLVM target triple: `--target-triple <triple>`

## Build `sirc` (experimental .sir parser)

`sirc` is an experimental flex/bison-based parser for a textual `.sir` syntax. It is currently a syntax checker only (it does not emit JSONL yet).

- Configure with: `cmake -S . -B build -G Ninja -DSIR_ENABLE_SIRC=ON -DLLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm`
- Build: `cmake --build build`
- Run: `./build/sirc/sirc sirc/hello.sir`

Current lowering support is intentionally small (enough to smoke-test the pipeline):

- `type` records: `prim`, `fn`, `ptr`
- `node` tags: `fn`, `param`, `block`, `return`, `name`, `binop.add`, `const.i32`
