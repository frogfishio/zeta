# hello_zeta (SIRC starter)

This starter shows a tiny multi-file SIRC project using:

- `package.toml` (optional project marker)
- `@include` (top-level fragment include)
- `@mod` (stable id scope)

## Build (sirc → sircc)

From the Zeta repo root (or from a dist bundle that includes `sirc` + `sircc`):

```sh
SIRC=./dist/bin/<os>/sirc
SIRCC=./dist/bin/<os>/sircc

$SIRC ./dist/starter/hello_zeta/src/main.sir -o /tmp/hello_zeta.sir.jsonl
$SIRCC /tmp/hello_zeta.sir.jsonl -o /tmp/hello_zeta && /tmp/hello_zeta
```

## Layout

- `src/main.sir` is the entry file.
- `include/defs.sir` is a fragment included via `@include "include/defs.sir"`.

