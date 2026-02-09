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

If you pass a sandbox root, `sem` ensures `file:fs` is present in CAPS_LIST:

- `sem --caps --fs-root PATH`
- `sem --run prog.sir.jsonl --fs-root PATH`

In this build, `--fs-root` adds the cap entry `file:fs:open,block` if it wasn’t already provided.

### 2) Convenience enablement: `--enable`

`sem --enable WHAT` toggles built-in features/caps.

Recognized values in this build:

- `--enable file:fs`  (adds `file:fs:open,block`)
- `--enable async:default` (adds `async:default:open,block`)
- `--enable sys:info` (adds `sys:info:pure`)
- `--enable env` (enables `zi_ctl` env ops; see “Not caps: env/argv”)
- `--enable argv` (enables `zi_ctl` argv ops; see “Not caps: env/argv”)

### 3) Explicit cap entries: `--cap`

Add one cap entry directly:

- `sem --cap KIND:NAME[:FLAGS]`

Examples:

- `sem --cap file:fs:open,block`
- `sem --cap sys:info:pure`

You can specify multiple `--cap` arguments.

### 4) Shorthand flags: `--cap-*`

These are equivalent to adding the matching `--cap` entry:

- `--cap-file-fs`       → `--cap file:fs:open,block`
- `--cap-async-default` → `--cap async:default:open,block`
- `--cap-sys-info`      → `--cap sys:info:pure`

## Caps recognized by this build

This section lists the cap *names* the `sem` CLI knows about (can be added to CAPS_LIST), and whether `zi_cap_open` can currently open them.

### `file:fs`

- Enable with:
  - `--fs-root PATH` (recommended; also configures the sandbox)
  - or `--enable file:fs` / `--cap-file-fs` / `--cap file:fs:open,block`
- `zi_cap_open` support: **yes**
  - Open currently supports only `kind="file"` + `name="fs"`
  - Open is sandboxed under `--fs-root`

### `async:default`

- Enable with `--enable async:default` / `--cap-async-default` / `--cap async:default:open,block`
- `zi_cap_open` support: **not yet** (will be listed but open will be denied)

### `sys:info`

- Enable with `--enable sys:info` / `--cap-sys-info` / `--cap sys:info:pure`
- `zi_cap_open` support: **not yet** (will be listed but open will be denied)

## Not caps: `env` and `argv`

`env` and `argv` are not `zi_cap_open` capabilities in this build.

They control whether `zi_ctl` exposes these host protocol ops:

- `SEM_ARGV_COUNT`, `SEM_ARGV_GET`
- `SEM_ENV_COUNT`, `SEM_ENV_GET`

Enable them via either:

- `--enable argv` and/or `--params ARG` (adds argv entries)
- `--enable env` and/or `--inherit-env` / `--env KEY=VAL` / `--clear-env`

## Quick recipes

- Show enabled caps:
  - `sem --caps --json`

- Enable file sandbox + confirm it’s listed:
  - `sem --caps --json --fs-root ./sandbox`

- Run the shipped examples from `dist/`:
  - `./dist/bin/<os>/sem --run ./dist/test/sem/run/hello_zabi25_caps_list.sir.jsonl`
  - `./dist/bin/<os>/sem --run ./dist/test/sem/run/hello_zabi25_file_cat.sir.jsonl --fs-root ./dist/test/sem/run`
