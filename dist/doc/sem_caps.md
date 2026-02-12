# sem capabilities (caps) and enablement

`sem` exposes a minimal capability model to guest code via:

- `zi_ctl` op `CAPS_LIST` (discover which caps are present)
- `zi_cap_open` (open an *openable* cap into a handle)

This document describes:

- how to enable caps from the `sem` CLI
- which caps are recognized by this build
- what “enabled” means (listed vs openable)

## Inspect the current caps list

Text format:

- `sem --caps`

JSON format (easy to parse):

- `sem --caps --json`

Example:

- `sem --caps --json --fs-root ./sandbox`

The returned `caps[].flags` is a bitset:

- `open` (1): cap may be opened via `zi_cap_open`
- `pure` (2): cap is declared pure (no side effects)
- `block` (4): cap may block

## Ways to enable caps

### 1) Automatic cap: `--fs-root`

If you pass a sandbox root, `sem` configures `file/aio` sandboxing and ensures the golden caps needed for `sem --cat` are present in CAPS_LIST:

- `sem --caps --fs-root PATH`
- `sem --run prog.sir.jsonl --fs-root PATH`

In this build, `--fs-root` sets `ZI_FS_ROOT` and adds (if missing):

- `sys:loop:open,block`
- `file:aio:open,block`

### 2) Convenience enablement: `--enable`

`sem --enable WHAT` toggles built-in features/caps.

Recognized values in this build:

- `--enable sys:loop` (adds `sys:loop:open,block`)
- `--enable file:aio` (adds `file:aio:open,block`)
- `--enable net:tcp`  (adds `net:tcp:open,block`)
- `--enable proc:env` (enables an openable `proc:env` stream cap)
- `--enable proc:argv` (enables an openable `proc:argv` stream cap)
- `--enable sys:info` (adds `sys:info:pure`) *(legacy/experimental)*
- `--enable env` (alias for `--enable proc:env`)
- `--enable argv` (alias for `--enable proc:argv`)

### 3) Explicit cap entries: `--cap`

Add one cap entry directly:

- `sem --cap KIND:NAME[:FLAGS]`

Examples:

- `sem --cap sys:info:pure`
- `sem --cap file:aio:open,block`

You can specify multiple `--cap` arguments.

### 4) Shorthand flags: `--cap-*`

These are equivalent to adding the matching `--cap` entry:

- `--cap-sys-info`      → `--cap sys:info:pure`

## Caps recognized by this build

This section lists the cap *names* the `sem` CLI knows about (can be added to CAPS_LIST), and whether `zi_cap_open` can currently open them.

### `sys:loop`

- Enable with `--enable sys:loop` / `--cap sys:loop:open,block`
- `zi_cap_open` support:
  - **hosted-only**: yes (minimal; timers + `POLL` timer events)
  - **zingcore25 builds**: yes (supports `WATCH`/`UNWATCH` and readiness)

### `file:aio`

- Enable with `--enable file:aio` / `--cap file:aio:open,block`
- `zi_cap_open` support:
  - **hosted-only**: stub (open succeeds, requests return ZCL1 error)
  - **zingcore25 builds**: yes (requests are ZCL1-framed; completions delivered as `op=100` DONE events)

### `net:tcp`

- Enable with `--enable net:tcp` / `--cap net:tcp:open,block`
- `zi_cap_open` support: **stub only**
  - Open succeeds, but `zi_read`/`zi_write` return `ZI_E_NOSYS`.

### `sys:info`

- Enable with `--enable sys:info` / `--cap-sys-info` / `--cap sys:info:pure`
- `zi_cap_open` support: **not yet** (will be listed but open will be denied)

## `proc:env` and `proc:argv`

This build exposes environment and argv via standard zABI 2.5 proc caps:

- `proc:env` (openable) yields a read-only stream containing:
  - `u32 version` (currently 1)
  - `u32 envc`
  - repeated `envc` times: `u32 len`, then `len` raw bytes (typically `KEY=VALUE`)

- `proc:argv` (openable) yields a read-only stream containing:
  - `u32 version` (currently 1)
  - `u32 argc`
  - repeated `argc` times: `u32 len`, then `len` raw bytes

Enable them via either:

- `--enable proc:env` and/or `--inherit-env` / `--env KEY=VAL` / `--clear-env`
- `--enable proc:argv` and/or `--params ARG`

## Quick recipes

- Show enabled caps:
  - `sem --caps --json`

- Enable file sandbox + confirm it’s listed:
  - `sem --caps --json --fs-root ./sandbox`

- Run the shipped examples from `dist/`:
  - `./dist/bin/<os>/sem --run ./dist/test/sem/run/hello_zabi25_caps_list.sir.jsonl`
  - `./dist/bin/<os>/sem --cat /hello.txt --fs-root ./dist/test/sem/run`
