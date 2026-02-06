# sem CLI (conceptual spec, v0)

`sem` is the reference CLI frontend for running SIR through `sircore`.

It is responsible for:

- parsing input formats (JSONL today; CBOR later)
- building a structured `sircore` module
- providing host capabilities via a pure `zi_ctl` message ABI
- presenting diagnostics/events in text or JSONL

## 1. Modes

### 1.1 Stream mode

If no input files are provided, `sem` reads SIR from stdin.

### 1.2 Tool mode

`sem --tool` accepts multiple inputs and writes outputs to a specified path (e.g. trace/coverage files).

## 2. Host capability policy

Default is “deny all”.

Flags enable capabilities explicitly, e.g.:

- `--stdin PATH` (provide a readable stdin handle)
- `--inherit-env` / `--clear-env` / `--env KEY=VAL` (snapshot env)
- `--params ...` (guest argv)
- (later) `--fs-root PATH` (file capability sandbox)

## 3. Outputs

- `--diagnostics text|json`
- `--trace` / `--trace-jsonl-out PATH`
- `--coverage` / `--coverage-out PATH`

All JSON outputs should be JSONL records to allow streaming.

