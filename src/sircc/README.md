# sircc

`sircc` is a WIP compiler from SIR JSONL (one JSON object per line) to native binaries via LLVM.

## Status

- Parses `sir-v1.0` JSONL records using LLVM's JSON parser.
- Lowers a very small subset of `k:"type"` + `k:"node"` to LLVM IR.
- Emits an object file with LLVM and links with `clang`.

## Example

- `cmake -S . -B build -G Ninja -DLLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm`
- `cmake --build build`
- `./build/src/sircc/sircc src/sircc/examples/add.sir.jsonl -o add`

Note: this example’s `main` takes two `i32` arguments because we don’t yet model argv lowering; you can still inspect IR with `--emit-llvm`.
