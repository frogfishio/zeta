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
- Build the copy-pasteable bundle in `./dist/`: `cmake --build build --target dist`

### Run

- Emit an executable: `./build/src/sircc/sircc src/sircc/examples/add.sir.jsonl -o add`
- Emit LLVM IR: `./build/src/sircc/sircc src/sircc/examples/add.sir.jsonl --emit-llvm -o add.ll`
- Emit object only: `./build/src/sircc/sircc src/sircc/examples/add.sir.jsonl --emit-obj -o add.o`
- Verify only (no codegen): `./build/src/sircc/sircc --verify-only src/sircc/examples/add.sir.jsonl`

Optional flags:

- Pick a linker: `--clang clang` (default: `clang`)
- Override the LLVM target triple: `--target-triple <triple>`

## Build `sirc` (experimental .sir parser)

`sirc` is an experimental flex/bison-based translator for a textual `.sir` syntax. It translates `.sir` to `sir-v1.0` `*.sir.jsonl`.

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
