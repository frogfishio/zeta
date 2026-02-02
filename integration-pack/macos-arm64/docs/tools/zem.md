<!-- SPDX-FileCopyrightText: 2025 Frogfish -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# zem

IR JSONL emulator + debugger for ZASM IR v1.1.

`zem` is a self-contained compiled tool. Using it does not require Python or any scripting runtime.

`zem` executes ZASM IR JSONL directly (the output of `zas`, and an input to tools like `zld`/`zir`). It is also used as a developer-experience tool: it can trace execution, expose a CLI debugger, and emit machine-readable debugger stop events suitable for feeding into external tooling (e.g. a DAP adapter).

## Usage

```sh
bin/zem /tmp/program.jsonl
```

Inputs are one or more IR JSONL files:

```sh
bin/zem file1.jsonl file2.jsonl
```

Or stdin (useful for pipes):

```sh
compiler | bin/zem --source-name program.jsonl
```

Notes:

- If you omit input files, `zem` reads program IR JSONL from stdin (stream mode).
- `-` explicitly means “read program IR JSONL from stdin” (same as stream mode).
- `--source-name` controls the `source.name` reported in `dbg_stop` events for stdin inputs.
- `--debug-script -` reads debugger commands from stdin, so it cannot be combined with reading program IR from stdin (either stream mode or `-`).

## Integration

`zem` is designed to be easy to embed in pipelines. The key is to keep the streams straight:

- **stdout**: the guest program’s stdout (what the emulated program writes via `_out` and similar).
- **stderr**: `zem` diagnostics and (optionally) JSONL event streams like `dbg_stop`, `trace`, `mem_read`, `mem_write`.

If you want to consume machine-readable events, use `--debug-events-only` so stderr is clean JSONL.

### Recipe 1: file input (program from file)

Use this when you want the guest program to read runtime stdin (e.g. echo/cat programs):

```sh
printf 'hello\n' | bin/zem examples/echo.jsonl > /tmp/program.out
```

- Read the emulated program’s output from stdout (`/tmp/program.out` above).
- Unless you enable tracing/debug events, stderr is just `zem` diagnostics.

### Recipe 2: pipe input (program IR from stdin)

Use this when another tool produces IR JSONL and you want to run it immediately:

```sh
compiler | bin/zem --source-name program.jsonl
```

Notes:

- When you use stream mode (or `-`), `zem` consumes stdin to read the program IR JSONL. That means the guest program effectively has no runtime stdin available.
- If the guest needs runtime stdin, prefer Recipe 1 (program from a file) so stdin can be used for program input.

### Recipe 3: pipe + breakpoints + events-only (tooling mode)

Use this when you want a pipeline-friendly run that produces only JSONL debugger stop events on stderr.
Because stdin is used for the program IR (`-`), drive the debugger from a script file:

```sh
cat > /tmp/zem.script <<'EOF'
blabel main
c
quit
EOF

compiler | bin/zem --debug-events-only --source-name program.jsonl --debug-script /tmp/zem.script - \
  2> /tmp/zem.events.jsonl \
  > /tmp/program.out
```

- Read debugger events from stderr (`/tmp/zem.events.jsonl`).
- Read the guest program output from stdout (`/tmp/program.out`).

## Common flags

## Tools

These are not separate binaries. They are built into `zem`.

These flags select a different top-level action than “run the emulator”. In these modes, `zem` is operating on IR files and/or running external commands, not executing an IR program.

Which one should you use?

- Use `--irdiff` when you have two IR files and you want to know whether they match (and why not).
- Use `--min-ir` when you have one “bad” IR file and you want the smallest IR that still triggers a failure in some command.
- Use `--triage` when you have many IR files and you want to run a command over all of them and group failures by signature.
- Use `--duel` when you want to compare two commands (A vs B) across inputs and find cases where they diverge (optionally minimizing a divergent case).

- `zem --irdiff ...` — compare two IR JSONL files
- `zem --min-ir ...` — delta-minimize an IR JSONL file against a predicate
- `zem --triage ...` — run a command across many inputs and group failures
- `zem --duel ...` — differential runner (A/B) with optional minimization

Examples:

```sh
bin/zem --irdiff a.ir.jsonl b.ir.jsonl
bin/zem --min-ir input.ir.jsonl -- bin/zir --canon {}
bin/zem --triage --summary corpus/*.jsonl -- bin/zld {}
bin/zem --duel --corpus corpus --a bin/zir --canon --b bin/zir --canon --assign-ids
```

### `--irdiff`

Compares two IR JSONL files for semantic-ish equality (record-by-record after filtering).

- Inputs: exactly two `.jsonl` files.
- Output: no output on success; prints a short mismatch report on stderr when different.
- Exit codes: `0` equal, `1` different, `2` error.

Useful options:

- `--include-ids` include v1.1 stable record ids in comparison.
- `--include-src` include v1.1 `src_ref` in comparison.
- `--include-loc` include `loc.line` in comparison.

Example:

```sh
bin/zem --irdiff --include-ids a.ir.jsonl b.ir.jsonl
```

### `--min-ir`

Delta-minimizes a single IR JSONL file against a predicate command.

You provide:

- an input IR JSONL file, and
- a predicate command (after `--`) that is run repeatedly.

Command templating:

- If any predicate arg is exactly `{}`, it is replaced with the candidate path.
- Otherwise the candidate path is appended as the last arg.

Predicate selection:

- `--want-exit N` predicate is satisfied iff the command exits with code `N`.
- `--want-nonzero` predicate is satisfied iff the command exits nonzero (default).

Example (minimize while still failing `zir --canon`):

```sh
bin/zem --min-ir --want-nonzero -o /tmp/min.jsonl input.ir.jsonl -- bin/zir --canon {}
```

### `--triage`

Runs an external command across many input IR files and groups failures by a stderr signature.

- Inputs: one or more `.jsonl` files, followed by `-- <cmd> ...`.
- Output:
  - A per-input JSONL stream (to stdout, or `--jsonl PATH`).
  - Optional human summary to stderr (`--summary`).

The output JSONL records include (at minimum):

- `k: "triage"`
- `path`: input path
- `exit`: command exit code
- `fail`: boolean
- `sig`: captured stderr prefix (see `--max-stderr`)

Example:

```sh
bin/zem --triage --want-nonzero --summary --jsonl /tmp/triage.jsonl corpus/*.jsonl -- bin/zld {}
```

### `--duel`

Runs two external commands (A and B) over inputs and checks whether they diverge.

Two input styles are supported:

- Explicit inputs:
  `bin/zem --duel --a <cmd...> --b <cmd...> -- <inputs...>`
- Corpus directory:
  `bin/zem --duel --corpus <dir> --a <cmd...> --b <cmd...>` (runs over `*.jsonl`)

Comparison modes:

- `--compare exit|stdout|stderr|both` (default: `both`)

Common workflows:

- `--check` is for a single input: exit `1` iff divergent.
- `--out DIR` writes per-case artifacts (captured stdout/stderr/metadata).
- `--minimize` minimizes a divergent case by invoking `zem --min-ir`.
  - `--zem PATH` can override which `zem` executable is used for that re-run.

Example (A/B compare `zir --canon`):

```sh
bin/zem --duel --corpus corpus --compare stdout --a bin/zir --canon --b bin/zir --canon --assign-ids
```

### Tracing

- `--trace` emits per-instruction JSONL events to stderr.
- `--trace-mem` adds `mem_read`/`mem_write` JSONL events to stderr.

### Process snapshot (argv/env)

`zem` can provide a process-like snapshot (guest argv + environment variables) to the emulated program via the `zi_*` proc/env syscalls.

Guest argv:

- Use `--params` to stop option parsing; remaining args become guest argv.
  - `--` is accepted as an alias for `--params`.

```sh
bin/zem /tmp/program.jsonl --params hello world
```

Environment snapshot:

- Default is an empty environment.
- `--inherit-env` snapshots the host environment (best effort) for `zi_env_get_*`.
- `--clear-env` clears the snapshot.
- `--env KEY=VAL` adds/overrides a single entry (repeatable).

```sh
bin/zem --clear-env --env MODE=debug --env USER=alice /tmp/program.jsonl
```

Supported proc/env syscalls:

- `CALL zi_argc` → `HL=argc`
- `CALL zi_argv_len` → `HL=i, HL=len_or_err`
- `CALL zi_argv_copy` → `HL=i, DE=out_ptr64, BC=cap, HL=written_or_err`
- `CALL zi_env_get_len` → `HL=key_ptr64, DE=key_len, HL=len_or_err`
- `CALL zi_env_get_copy` → `HL=key_ptr64, DE=key_len, BC=out_ptr64, IX=cap, HL=written_or_err`

### Coverage

`zem` can record per-PC instruction hit counts and write them as JSONL.

```sh
bin/zem --coverage --coverage-out /tmp/zem.coverage.jsonl /tmp/program.jsonl
```

Print a quick “black holes” summary (labels with uncovered instructions):

```sh
bin/zem --coverage --coverage-blackholes 20 --coverage-out /tmp/zem.coverage.jsonl /tmp/program.jsonl
```

Merge multiple runs (useful for CI shards or multi-phase pipelines):

```sh
bin/zem --coverage --coverage-merge /tmp/zem.coverage.jsonl \
  --coverage-out /tmp/zem.coverage.merged.jsonl \
  /tmp/program.jsonl
```

Notes:

- Coverage is per IR record index (`pc`). Only instruction records (`kind == instr`) are reported.
- If the IR provides stable record ids (`id` in v1.1), coverage records also include `ir_id` for cross-tool correlation.
- The JSONL report also includes per-label aggregates (`k == "zem_cov_label"`) to support black-hole analysis.
- When `--debug-events-only` is used, `--coverage` requires `--coverage-out` (to keep stderr clean JSONL).
- When `--debug-events-only` is used, `--coverage-blackholes` is rejected (since it prints a human summary to stderr).

### Debugging (CLI)

- `--debug` starts the interactive CLI debugger (starts paused).
- `--break-pc N` breaks when `pc == N` (where `pc` is the IR record index).
- `--break-label L` breaks at label `L`.
- `--break FILE:LINE` breaks at the first instruction mapped to `FILE:LINE` via v1.1 `src`/`src_ref`.
- `--debug-script PATH` runs debugger commands from PATH (no prompt; exit on EOF). Use `-` for stdin.

If you want to run a piped program _and_ drive the debugger with a script, put the script in a file:

```sh
printf 'blabel main\nc\nquit\n' > /tmp/zem.script
compiler | bin/zem --debug-events-only --source-name program.jsonl --debug-script /tmp/zem.script -
```

### Debugger stop events (JSONL)

- `--debug-events` emits JSONL `dbg_stop` events to stderr on each debugger stop.
- `--debug-events-only` like `--debug-events`, but suppresses human-oriented debugger output (prompt/help/regs/disasm/etc) and suppresses zem lifecycle telemetry.

### Diagnostics (trap reports)

When `zem` fails (e.g. out-of-bounds memory access), it prints a human-oriented trap report to stderr including:

- the failing IR record (`pc`, label/line if present)
- stable record identity (`ir_id` / `src_ref`) when present in the IR
- register dump + backtrace
- a short “recent instruction” history
- for instructions that dereference memory, the base register value and its provenance (where that register was last written)

In some cases `zem` can also emit a targeted diagnosis if it recognizes a high-signal signature (e.g. return-slot loaded with `LD32`, sign-extended, then used as an address and traps out-of-bounds).

This is intended to make “trap, identify, explain” workflows fast when debugging compiler lowering bugs.

### Sniffer mode (proactive warnings)

- `--sniff` enables heuristic warnings for high-signal bug signatures (currently: return-slot pointer truncation patterns).
- `--sniff-fatal` like `--sniff`, but turns a warning into a failing trap.

In addition, `--sniff` warns when common ABI v2 (`zi_*`) call arguments look invalid at the boundary (e.g. a sign-extended i32 used where the ABI expects a u32 pointer/length, or obvious out-of-bounds spans). This is meant to surface “will crash later” issues early, with provenance.

The warning includes the detected pattern PCs, the suspect register’s current value, and its provenance.

### Shake mode (deterministic perturbations)

`--shake` runs the program multiple times with small, deterministic perturbations to help surface latent bugs (e.g. uninitialized reads, reliance on a stable heap layout).

Typical usage:

```sh
bin/zem --shake --shake-iters 50 /tmp/program.jsonl
```

To replay a specific failing run (use the values printed in the shake header):

```sh
bin/zem --shake --shake-seed 123 --shake-start 17 --shake-iters 1 /tmp/program.jsonl
```

Useful flags:

- `--shake-heap-pad N` / `--shake-heap-pad-max N` vary the base heap address (alignment-safe padding).
- `--shake-poison-heap` fills newly allocated heap bytes with deterministic non-zero data to surface zero-init assumptions.

### Fuzzing (coverage-guided stdin)

`zem` includes a simple in-process, coverage-guided fuzzer for programs that read from guest stdin.

Key properties:

- Single binary (no external fuzzers/tools).
- Deterministic by default (seeded RNG).
- Mutates only guest stdin bytes.

Basic usage:

```sh
bin/zem --fuzz --fuzz-iters 1000 --fuzz-len 64 --fuzz-mutations 4 --fuzz-seed 1 \
  --fuzz-out /tmp/zem.best.bin \
  --fuzz-crash-out /tmp/zem.crash.bin \
  /tmp/program.jsonl
```

Notes:

- `--fuzz` requires exactly one program input file (not stream mode / `-`).
- If you pass `--stdin PATH`, that file is used as the seed input (and also makes replay easy).
- If you omit `--fuzz-len`, it defaults to the `--stdin` file size when provided, otherwise `64`.
- `--fuzz-out` writes the best coverage input found so far.
- `--fuzz-crash-out` writes the first failing input found (if any).

Replay workflow:

```sh
# Fuzz:
bin/zem --fuzz --fuzz-iters 10000 --fuzz-crash-out /tmp/crash.bin /tmp/program.jsonl

# Replay the failure:
bin/zem --stdin /tmp/crash.bin /tmp/program.jsonl
```

When a crash is found and `--fuzz-crash-out` is set, `zem` prints a one-line repro command.

#### Concolic-lite branch unlocker

For some programs, random mutation stalls on hard-to-guess branches. Enable the unlocker:

```sh
bin/zem --fuzz --fuzz-unlock --fuzz-unlock-tries 4 --fuzz-mutations 4 /tmp/program.jsonl
```

To see one-line predicate traces when the unlocker emits suggestions, add:

```sh
bin/zem --fuzz --fuzz-unlock-trace /tmp/program.jsonl
```

This prints lines like `zem: unlock: pc=... cond=... take=... stdin_off=... rhs=... suggest=...` to stderr.

The unlocker is best-effort and currently strongest for branches that compare a single stdin byte against an immediate constant (e.g. `CP A, #66` followed by `JR eq,label`).

To test the unlocker in isolation (no random mutation), set:

```sh
bin/zem --fuzz --fuzz-mutations 0 --fuzz-unlock /tmp/program.jsonl
```

## Debugger REPL quickstart

Run, then interact:

```sh
bin/zem --debug /tmp/program.jsonl
```

Typical commands:

- `help` list commands
- `continue` / `c` run until breakpoint/exit
- `step` / `s` execute one instruction
- `next` / `n` step over CALL (best-effort)
- `finish` run until returning from the current frame
- `regs` show registers
- `bt` show call stack
- `pc` show current pc/label
- `bpc N` add breakpoint at pc N
- `blabel NAME` add breakpoint at label NAME
- `bp` list breakpoints

Scripted debugger (useful for automation):

```sh
printf 'bp\ncontinue\n' | bin/zem --debug-script - /tmp/program.jsonl
```

## `dbg_stop` schema (JSONL)

When `--debug-events` (or `--debug-events-only`) is enabled, `zem` writes one JSON object per line to stderr.

Stop events have `k == "dbg_stop"` and include:

- `k`: always `"dbg_stop"`
- `v`: schema version number (currently `1`)
- `reason`: stop reason string (e.g. `"paused"`, `"breakpoint"`, `"step"`, `"next"`, `"finish"`)
- `frame`: stable frame object
  - `pc`: IR record index (0-based)
  - `id`: frame id (0 is the current frame)
  - `ir_id`: stable IR record id from v1.1 `id` (or `null`)
  - `label`: label at `pc` (or `null`)
  - `line`: source line (or `null` if unavailable)
  - `col`: source column (currently always `1`)
  - `kind`: record kind (`"instr"`, `"dir"`, `"label"`, ...)
  - plus one of: `m` (mnemonic), `d` (directive), `name` (label/dir name) when applicable
  - `source`: source identity object
    - `name`: display name (filename or `"<stdin>"`)
    - `path`: path if known (null for stdin)
  - `src_ref`: v1.1 source mapping (or `null`)
    - `ref`: numeric `src_ref` from the current record
    - `src`: resolved source record (or `null` if not found)
      - `id`: source record id
      - `file`: source file path (or `null`)
      - `line`: source line (or `null`)
      - `col`: source column (or `null`)
      - `text`: source line text (or `null`)
- `sp`: call stack depth
- `bp`: matched breakpoint metadata (or `null`)
  - `pc`: breakpoint pc
  - `cond`: breakpoint condition expression (or `null`)
  - `cond_ok`: condition parsed/evaluated successfully
  - `result`: condition result (true/false)
- `bps`: array of active breakpoint PCs (numbers)
- `frames`: call stack frames, for DAP/tooling
  - `id`: frame id (stable within a stop event)
  - `pc`: frame pc (current frame first)
  - `ir_id`: stable IR record id from v1.1 `id` (or `null`)
  - `name`: nearest label at-or-before `pc` (or `null`)
  - `label`: label exactly at `pc` (or `null`)
  - `line`: source line (or `null`)
  - `col`: source column (currently always `1`)
  - `m`: mnemonic at `pc` if `pc` points to an instruction (or `null`)
  - `source`: `{name,path}` as above
  - `src_ref`: same structure as `frame.src_ref`
- `regs`: register snapshot (`HL`, `DE`, `BC`, `IX`, `A`)
- `regprov`: register provenance map (register -> provenance object or `null`)
- `watches`: watch values (empty unless watches are configured)
  - each watch may include `written_by` with `{pc,label,line,op}`

Notes:

- Fields under `frame` and the top-level `k/reason/pc/label/sp/bps/regs` are intended to be stable for DAP/tooling.
- `rec` is included as a best-effort mirror of the current IR record and may evolve.

Example (pretty-printed; actual output is one line):

```json
{
  "k": "dbg_stop",
  "v": 1,
  "reason": "paused",
  "frame": {"pc": 0, "id": 0, "ir_id": null, "label": null, "line": null, "col": 1, "kind": "dir", "d": "EXTERN"},
  "pc": 0,
  "label": null,
  "sp": 0,
  "bp": null,
  "bps": [0],
  "frames": [{"id": 0, "pc": 0, "ir_id": null, "name": null, "label": null, "line": null, "col": 1, "m": null}],
  "regs": {"HL": 0, "DE": 0, "BC": 0, "IX": 0, "A": 0},
  "regprov": {"HL": null, "DE": null, "BC": null, "IX": null, "A": null},
  "watches": []
}
```
