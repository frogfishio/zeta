<!-- SPDX-FileCopyrightText: 2025 Frogfish -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Integration Pack (Native + IR)

This pack is a **ready-to-use distribution** for third-party integrators.
It bundles the zasm tools plus the **zABI 2.5 host runtime archive** needed to link native outputs from `lower`.

It intentionally does **not** include legacy/experimental surfaces (e.g. old cloak/JIT reference snapshots).

## Layout

The pack is written to:

- `dist/integration-pack/<platform>/...`

Where `<platform>` is currently `macos-arm64`.

Inside a platform folder:

- `bin/` — tool binaries (`lower`, `zld`, `zem`, `ircheck`, ...)
- `lib/` — libraries (`libzingcore25.a`)
- `include/` — C/C++ headers (zasm public headers + zingcore25 headers)
- `docs/` — tool + spec docs
- `schema/` — JSON schemas for IR (ship IR **v1.1**)
- `conformance/` — JSONL-only conformance fixtures (schema/record validation)
- `examples/` — small C templates (host shim + cap usage)

## Build the pack

From the repo root:

- `make dist-integration-pack`

Or directly:

- `./docs/integration_pack/pack.sh ./dist/integration-pack macos-arm64`

## Quick starts

### Validate IR output (JSONL)

- `./dist/integration-pack/macos-arm64/bin/ircheck < your.jsonl`
- `./dist/integration-pack/macos-arm64/bin/ircheck --tool your.jsonl`

`zld --conform=strict --verify` is still useful as a compiler-side validator:

- `./dist/integration-pack/macos-arm64/bin/zld --conform=strict --verify < your.jsonl`
- `./dist/integration-pack/macos-arm64/conformance/conform_zld.sh`

### Link a native `lower` output against zABI 2.5

`libzingcore25.a` is a runtime archive: zABI syscalls are implemented in zingcore, but many of them forward to **host hooks** (stdio, alloc, env, etc). So you must install a host (shim) before calling your lowered entrypoint.

Given an object produced by `lower` (Mach-O arm64), link like:

- `cc -I./dist/integration-pack/macos-arm64/include \
    your.o \
    ./dist/integration-pack/macos-arm64/lib/libzingcore25.a \
    -o your_app`

If you want the “golden host wiring” helpers, include:

- `#include <zi_hostlib25.h>`

Then call `zi_hostlib25_init_all()` early in `main()`.

A minimal template is shipped in the pack at:

- `./dist/integration-pack/macos-arm64/examples/host_shim/`

That folder also includes a small `echo_zabi25_native.jsonl` sample so the pack is self-contained.

### Capability example: get a time value

The pack also includes a tiny host-side example that opens `sys/info@v1` and requests `TIME_NOW` (ZCL1) to retrieve timestamps:

- `./dist/integration-pack/macos-arm64/examples/sys_info_time_now/`

## IR version

For new integrations, emit `ir: "zasm-v1.1"` and validate against `schema/ir/v1.1/record.schema.json`.
