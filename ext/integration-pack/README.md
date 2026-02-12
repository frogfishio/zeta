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

Zingcore-specific docs shipped in `docs/zingcore25/` include:

- `CAP_READINESS_MATRIX.md` — what to `WATCH` for TCP + file/aio + sys/loop (would-block = `ZI_E_AGAIN`)

Zingcore protocol specifications are shipped in `docs/zingcore25/abi/` (ZCL1 framing, `zi_ctl`, and the golden caps like `sys/loop`, `sys/info`, `net/tcp`, `file/aio`, `event/bus`, `async/default`, `proc/hopper`, plus an overall zABI 2.5 overview).

## Sandboxing and feature knobs (env vars)

Several “golden” capabilities are sandboxed and/or feature-gated via environment variables.
If you see unexpected `ZI_E_DENIED` failures, check these first:

- `ZI_FS_ROOT` (filesystem sandbox)
    - If set: guest paths must be absolute and are resolved under this root (escape-resistant; `..` rejected).
    - Affects: `file/aio@v1`.
- `ZI_NET_ALLOW` (outbound TCP allowlist)
    - If unset/empty: only loopback is allowed.
    - If `any`: any host:port is allowed.
    - Otherwise: comma-separated tokens (see `docs/zingcore25/abi/TCP_PROTOCOL.md`).
    - Affects: `net/tcp@v1` outbound connects.
- `ZI_NET_LISTEN_ALLOW` (inbound TCP listen allowlist)
    - Same syntax as `ZI_NET_ALLOW`.
    - Affects: `net/tcp@v1` listener opens (bind+listen).
- `ZI_ENABLE_HTTP_CAP` (optional HTTP cap registration)
    - If set to a non-empty value other than `0`: registers `net/http`.
    - By default HTTP is disabled (intentionally “sugar”, not required for TCP/file/loop-based integrations).

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
