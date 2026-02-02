## lower — JSON IR (zasm-v1.0/v1.1) → macOS arm64 Mach-O

`lower` consumes ZASM JSONL IR (`schema/ir/v1/record.schema.json` or `schema/ir/v1.1/record.schema.json`) and produces a Mach-O object suitable for linking with `clang` on macOS arm64. It includes dump, audit, and tracing helpers for debugging IR, symbols, relocations, and layout.

### Usage

```
lower --input <input.jsonl> [--o <out.o>] [flags...]
lower --tool --o <out.o> <input.jsonl>... [flags...]
lower --help
lower --version
```

### Core flags

- `--input <path>`: Input JSONL IR. Required in non-tool mode (single file).
- `--o <path>`: Output Mach-O object path (default: `src/lower/arm64/out/out.o`).
- `--tool`: Filelist mode; allow multiple inputs, requires `-o`.
- `--version`: Print version (read from `VERSION`).
- `--help`: Show help.

### Diagnostics and dumps

- `--debug`/`--trace`: Verbose audit (counts of labels/externs/refs; missing/extra symbol notices).
- `--dump-syms`: Print symbol table (name, offset, section).
- `--dump-relocs`: Print relocations (offset, type, symbol, line, ir_id).
- `--dump-layout`: Print code/data sizes and per-symbol offsets (data-relative shown).
- `--dump-asm`: Print 32-bit word hex dump with symbol labels for code offsets.
- `--dump-ir`: Echo parsed IR with ids/src_ref.
- `--with-src`: Include `src` records in the `--dump-ir` output.
- `--strict`: Treat missing symbol declarations as errors (halts before emit).
- `--json`: Emit the same JSON as `--emit-map`, but to stdout (symbols, relocs with ir_id, refs, layout, audit counts).
- `--emit-pc-map <path>`: Write a JSON array mapping code offsets to IR ids/lines (PC→IR correlation).

### Machine-readable outputs

- `--emit-map <path>`: Write JSON map with:
  - `code_len`, `data_len`, `data_off`
  - `symbols`: name/off/section
  - `relocs`: off/type/sym/line/ir_id
  - `refs`: relocs grouped by symbol name
  - `audit`: missing/extra symbol counts
- `--json`: Same schema as `--emit-map` but written to stdout for piping.
- `--emit-pc-map <path>`: JSON array `{off, ir_id, line}` for each instruction offset (useful to map crashes back to IR).

### LLDB helper generation

- `--emit-lldb <path>`: Emit an LLDB script that sets breakpoints, dumps registers/symbols, and runs.
- `--trace-func <sym>`: Function symbol to break on (default: `main`).
- `--trace-syms <list>`: Comma-separated symbols to dump (address + 4 qword read).
- `--trace-regs <list>`: Comma-separated registers to read (`w0,x0` etc.).

LLDB script notes:
- Breaks at function entry and a broad `ret` catch; refine inside LLDB if needed.
- Uses `&sym` so it remains valid post-link even if section layout shifts.
- Invoke with `lldb <binary> -s <script>`.

### Exit codes

- `0`: Success
- `1`: Usage/IO error
- `2`: Parse/validation error
- `3`: Codegen error
- `4`: Mach-O emit error

### Examples

- Lower and inspect syms/relocs/layout:
  ```
  lower --debug --dump-syms --dump-relocs --dump-layout \
        --input src/lower/arm64/testdata/hello_libzingcore.zir.jsonl \
        --o /tmp/hello.o
  ```

- Emit machine-readable map to file:
  ```
  lower --emit-map /tmp/map.json \
        --input src/lower/arm64/testdata/test_combo.zir.jsonl \
        --o /tmp/test_combo.o
  ```

- Stream JSON dump to stdout for tooling:
  ```
  lower --json --input src/lower/arm64/testdata/test_combo.zir.jsonl \
        --o /tmp/test_combo.o > /tmp/map.json
  ```

- Dump IR and annotated code:
  ```
  lower --dump-ir --dump-asm \
        --input src/lower/arm64/testdata/test_combo.zir.jsonl \
        --o /tmp/test_combo.o
  ```

- Generate an LLDB trace helper script for a canary:
  ```
  lower --emit-lldb /tmp/trace.lldb --trace-func fn_Main__withoutLocals_28a_2Cb_29 \
        --trace-syms var_fn_Main__withoutLocals_28a_2Cb_29_a,var_fn_Main__withoutLocals_28a_2Cb_29_b,tmp_0 \
        --trace-regs w0,w1 \
        --input src/lower/arm64/testdata/test_combo.zir.jsonl \
        --o /tmp/test_combo.o
  # usage: lldb /tmp/a.out -s /tmp/trace.lldb
  ```

  - Native zABI 2.5 “echo” (no legacy `_in/_out`):
    ```
    # 1) Lower IR to an object
    lower --input examples/echo_zabi25_native.jsonl --o /tmp/echo_zabi25_native.o

    # 2) Link a tiny C runner + zingcore25 hostlib
    cc -Isrc/zingcore/2.5/zingcore/include \
      examples/echo_zabi25_native_runner.c \
      /tmp/echo_zabi25_native.o \
      build/zingcore25/libzingcore25.a \
      -o /tmp/echo_zabi25_native

    # 3) Smoke
    printf 'hello\n' | /tmp/echo_zabi25_native
    ```

### Notes

- IR `id`/`src_ref` are preserved; relocs carry `ir_id` for cross-reference.
- Symbol table dedupes by name; `--strict` currently errors on missing decls.
- Avoid branching back to a function entry label: `lower` injects a prologue before the first instruction, so if your loop label shares the same address as the function label you’ll effectively re-run the prologue every iteration (stack growth). Put loop labels after at least one real instruction.
- `--dump-asm` is a raw hex word dump; use `otool -tV <bin>` for full disassembly.
- `refs` in JSON outputs are de-duplicated per symbol for easier tooling.
- The Mach-O writer emits deterministic objects (no timestamps).
