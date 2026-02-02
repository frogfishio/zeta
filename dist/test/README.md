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
