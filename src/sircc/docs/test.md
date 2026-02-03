# dist/test (normative examples)

These `*.sir.jsonl` files are intended to be “try immediately” smoke tests for alpha users.

From the repo root after building `dist/`:

```sh
SIRCC=./dist/bin/<os>/sircc

$SIRCC --verify-only ./dist/test/examples/add.sir.jsonl
$SIRCC ./dist/test/examples/mem_copy_fill.sir.jsonl -o /tmp/mem_copy_fill && /tmp/mem_copy_fill; echo $?
$SIRCC ./dist/test/examples/cfg_if.sir.jsonl -o /tmp/cfg_if && /tmp/cfg_if; echo $?
$SIRCC ./dist/test/examples/cfg_switch.sir.jsonl -o /tmp/cfg_switch && /tmp/cfg_switch; echo $?
$SIRCC ./dist/test/examples/hello_world_puts.sir.jsonl -o /tmp/hello && /tmp/hello; echo $?
```

If you also built `sirc`, `dist/test/examples/` includes matching `.sir` sources for many examples:

```sh
SIRC=./dist/bin/<os>/sirc

$SIRC ./dist/test/examples/hello.sir -o /tmp/hello.sir.jsonl
$SIRCC /tmp/hello.sir.jsonl -o /tmp/hello && /tmp/hello
```

Expected exit codes:

- `mem_copy_fill.sir.jsonl` → `42`
- `cfg_if.sir.jsonl` → `222` (with no CLI args; the program’s `main(x)` receives `argc` as `x`, so `argc==1` ⇒ returns `222`)
- `cfg_switch.sir.jsonl` → `20` (with no CLI args; `argc==1` selects case `1` ⇒ returns `20`)
- `hello_world_puts.sir.jsonl` → `0` (also prints a line via `puts`)

## Mnemonic coverage (dev)

`sircc` includes a Milestone 3 mnemonic coverage gate against the normative table in `schema/sir/v1.0/mnemonics.html`:

```sh
ctest --test-dir build -R sircc_mnemonic_coverage_m3 --output-on-failure
```

Or run it directly:

```sh
python3 src/sircc/tools/mnemonic_coverage.py --enforce-m3
```

## Self-check (alpha user)

If you’re using the `dist/` bundle, `sircc` can run a small suite over `dist/test/examples`:

```sh
./dist/bin/<os>/sircc --check
```

Use `--format json` to get a machine-readable summary:

```sh
./dist/bin/<os>/sircc --check --format json
```
