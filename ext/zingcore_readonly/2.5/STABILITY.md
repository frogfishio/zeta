# zABI 2.5 Stability Guarantees

**Status**: Golden (Production-Ready)  
**Declared**: January 29, 2026

This document defines the stability contract for zABI 2.5.

## Core Commitment

The **zABI 2.5 core syscall surface** is **frozen** and will not break within the 2.x family.

Core syscalls (always present):
- `zi_abi_version` → returns `0x00020005`
- `zi_ctl` (ZCL1 control-plane)
- `zi_read`, `zi_write`, `zi_end`
- `zi_alloc`, `zi_free`
- `zi_telemetry`

### What "Frozen" Means

Within zABI 2.x:
- Function names MUST NOT change.
- Argument meanings, return meanings, and error codes MUST NOT change.
- Guest memory pointer width (`zi_ptr_t` = `uint64_t`) is stable.
- Handle semantics (0=stdin, 1=stdout, 2=stderr, >=3 allocated) are stable.

### Allowed Evolution

New functionality MAY be added in minor versions (2.6, 2.7, ...) via:
- **New capabilities** (discovered via `zi_ctl` CAPS_LIST)
- **New async selectors** (discovered via `async/default` LIST op)
- **New optional syscalls** (prefixed with `zi_`, gated by capability registration)

Breaking changes (removing syscalls, changing pointer model) require a major version bump (e.g., 3.0).

## Golden Capabilities (v1)

The following capabilities are **production-ready** and **stable** within zABI 2.5:

| Capability | Identity | Sandbox | Status |
|------------|----------|---------|--------|
| Filesystem | `file/fs@v1` | `ZI_FS_ROOT` | Golden |
| Process Args | `proc/argv@v1` | read-only | Golden |
| Process Env | `proc/env@v1` | read-only | Golden |
| Hopper Arena | `proc/hopper@v1` | isolated instance | Golden |
| TCP Client | `net/tcp@v1` | `ZI_NET_ALLOW` | Golden |
| Async Invocation | `async/default@v1` | selector registry | Golden |
| Event Bus | `event/bus@v1` | in-process | Golden |
| System Info | `sys/info@v1` | best-effort, may redact | Golden |

Each golden capability:
- Has a normative specification (see `README.md` and `abi/ABI_V2_5.md`)
- Has comprehensive unit tests
- Has integration test coverage (`make itest`)
- Has documented sandboxing/security posture

### Capability Versioning

Capability versions are **independent** of zABI version:
- `file/fs@v1` remains stable even if zABI advances to 2.6+.
- New capability versions (e.g., `file/fs@v2`) are additive; old versions remain supported.

## Deprecation Policy

Deprecated features will:
1. Be marked in headers/docs as `deprecated` for at least one minor version cycle.
2. Emit runtime warnings (via `zi_telemetry`) when used.
3. Be removed only in a major version bump.

No deprecations are currently planned for zABI 2.5 core or golden caps v1.

## Compatibility Testing

All zABI 2.5 releases pass:
- `make test` (unit test suite)
- `make itest` (integration test suite: caps listing, smoke tests)
- Cross-runtime conformance tests (when available)

## Migration from 2.2_final

zABI 2.5 introduces **breaking changes** from `zingcore2.2_final`:
- Removed `zi_abi_features` (use `zi_ctl` CAPS_LIST instead)
- Capability registration is now explicit (no auto-registration)
- Async selectors require explicit registration
- ZCL1 protocol replaces ad-hoc control-plane mechanisms

Migration guide: see `README.md` § "Migration plan".

## References

- Normative ABI spec: `abi/ABI_V2_5.md`
- Wire contract header: `zingcore/include/zi_sysabi25.h`
- Capability implementations: `zingcore/src/zi_*25.c`
- Test suite: `zingcore/tests/test_*.c`

## Governance

Stability decisions are made by the zasm maintainers. Proposals for changes are discussed via:
- GitHub issues (feature requests, bug reports)
- Pull requests (implementation + tests)
- This document (normative stability contract updates)

**Last Updated**: January 29, 2026  
**Signatories**: zasm core team
