# sircc (SIR JSONL compiler)

`sircc` compiles `sir-v1.0` `*.sir.jsonl` programs to native code using LLVM.

This repo’s “shipable bundle” is produced by the build target `dist`, which creates:

- `dist/bin/<os>/sircc` (the compiler binary)
- `dist/bin/<os>/sirc` (a convenience copy of the same binary)
- `dist/test/` (a small normative example set you can run immediately)

## Quickstart

Build the bundle:

```sh
cmake -S . -B build
cmake --build build --target dist
```

Then run:

```sh
./dist/bin/<os>/sircc --help
./dist/bin/<os>/sircc --print-target
./dist/bin/<os>/sircc ./dist/test/examples/mem_copy_fill.sir.jsonl -o /tmp/mem_copy_fill && /tmp/mem_copy_fill; echo $?
```

## CLI

```text
sircc <input.sir.jsonl> -o <output> [--emit-llvm|--emit-obj] [--clang <path>] [--target-triple <triple>]
sircc --verify-only <input.sir.jsonl>
sircc --dump-records --verify-only <input.sir.jsonl>
sircc --print-target [--target-triple <triple>]
sircc --version
```

Notes:
- default output is a native executable (links via `clang`)
- `--emit-llvm` writes LLVM IR (`.ll`)
- `--emit-obj` writes an object file (`.o`)
- if `meta.ext.target.triple` is present, it is used unless `--target-triple` overrides it

