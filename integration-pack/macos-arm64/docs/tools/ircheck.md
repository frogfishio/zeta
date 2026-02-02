<!-- SPDX-FileCopyrightText: 2026 Frogfish -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# ircheck (IR stream checker)

`ircheck` validates ZASM IR JSONL streams for **IR v1.1 conformance**.

It is intended as a single “certification authority” that third-party IR emitters can run in CI before handing IR to tools like `zem`, `zld`, or `lower`.

## What it checks

For each non-empty JSON line:

- JSONL record parses successfully.
- Record satisfies strict per-record constraints (typed operands, identifier grammar, directive set, required fields for `src`/`diag`, etc).
- Stream invariant: `src_ref` (if present) must reference a **prior** `src` record id.

By default, `ircheck` **requires** `ir: "zasm-v1.1"` on every record.

## Usage

### Stream mode (stdin)

```sh
bin/ircheck < program.jsonl
```

### Tool mode (one or more files)

```sh
bin/ircheck --tool file1.jsonl file2.jsonl
```

## Exit codes

- `0` — valid
- `2` — invalid input or validation failure

## CLI flags

- `--tool` validate one or more input files (instead of stdin)
- `--ir v1.1|v1.0|any` require an `ir` tag (default: `v1.1`)
- `--all` report all errors (don’t stop at first)
- `--verbose` emit extra diagnostics
- `--json` emit diagnostics as JSON lines (stderr)
- `--version` print version

## Notes

- `ircheck` is intentionally **not** a compiler and does not emit WAT/objects; it only validates.
- If you specifically want to validate as part of a compile-to-WAT pipeline, `zld --conform=strict --verify` remains useful.
