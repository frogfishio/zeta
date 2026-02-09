# Quickstart (Zeta toolchain bundle)

This folder is designed to be copy/pasted to another machine for alpha users and integrators.

## Build

From the repo root:

```sh
cmake -S . -B build
cmake --build build
cmake --build build --target dist
```

Your bundle is now in `./dist/`.

## Smoke test (sircc)

```sh
SIRCC=./dist/bin/<os>/sircc

$SIRCC --check
```

## Smoke test (sem)

```sh
SEM=./dist/bin/<os>/sem

$SEM --check ./dist/test/sem/examples
$SEM --check --check-run ./dist/test/sem/run
```

## Authoring flow (sirc → sircc)

If `sirc` is included in the bundle:

```sh
SIRC=./dist/bin/<os>/sirc

$SIRC ./dist/test/examples/hello.sir -o /tmp/hello.sir.jsonl
$SIRCC /tmp/hello.sir.jsonl -o /tmp/hello && /tmp/hello
```

## Starter project (package.toml + @include)

```sh
$SIRC ./dist/starter/hello_zeta/src/main.sir -o /tmp/hello_zeta.sir.jsonl
$SIRCC /tmp/hello_zeta.sir.jsonl -o /tmp/hello_zeta && /tmp/hello_zeta
```

## “What does sircc support?”

```sh
$SIRCC --print-support --format text --full
$SIRCC --print-support --format json --full > /tmp/sircc_support.json
```

The dist bundle also includes:

- `dist/doc/support.html`
- `dist/doc/support.json`

## VS Code (syntax highlighting)

The bundle includes a `.vsix` for the `.sir` language and `.sir.jsonl` streams:

- `dist/doc/editors/sir-language-support-0.0.2.vsix`

Install via VS Code: “Extensions: Install from VSIX…”.
