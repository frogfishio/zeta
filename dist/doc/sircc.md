# sircc (SIR JSONL compiler)

`sircc` compiles `sir-v1.0` `*.sir.jsonl` programs to native code using LLVM.

This repo’s “shipable bundle” is produced by the build target `dist`, which creates:

- `dist/bin/<os>/sircc` (the compiler binary)
- `dist/bin/<os>/sirc` (optional: `.sir` → `.sir.jsonl`, when built with `-DSIR_ENABLE_SIRC=ON`)
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

If you also built `sirc`, you can do the full pipeline from `.sir`:

```sh
./dist/bin/<os>/sirc ./dist/test/examples/hello.sir -o /tmp/hello.sir.jsonl
./dist/bin/<os>/sircc /tmp/hello.sir.jsonl -o /tmp/hello && /tmp/hello
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
