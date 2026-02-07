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

## 4. Strings (current behavior)

SIR itself does not define a first-class “string” type in the core subset; frontends represent strings as **bytes in memory** plus whatever metadata they need (len, encoding, ownership).

In `sem` today, the supported literal is:

- `cstr` node: `{"tag":"cstr","fields":{"value":"..."}}`

Semantics:

- `fields.value` is a JSON string (Unicode). `sem` currently treats this as **UTF-8 bytes** (the JSON parser produces a UTF-8 `char*`).
- The emitted bytes are **not** implicitly NUL-terminated.
- `cstr` evaluates to a **`ptr`** (address of the first byte). `sem` also materializes the byte length internally (an `i64` slot) but does not return it as part of the value.

Implications:

- For `zi_write(fd, ptr, len)`-style APIs (explicit length), `cstr` is directly usable once you also compute/provide `len`.
- For C APIs that expect NUL-terminated strings, the producer must explicitly include the terminator in memory (or avoid those APIs in `sem` runs).
