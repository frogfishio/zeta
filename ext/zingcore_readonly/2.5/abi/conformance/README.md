# zABI 2.5 Conformance Test Suite

**Status**: Draft  
**Purpose**: Cross-runtime validation of zABI 2.5 implementations

This directory contains wire-level conformance tests for zABI 2.5. These tests are **implementation-agnostic** and can be run against any runtime claiming zABI 2.5 compliance.

## Test Categories

### 1. Core Syscalls (`core/`)
- `zi_abi_version` returns `0x00020005`
- `zi_alloc/zi_free` behavior (zero-size, alignment, OOM)
- `zi_read/zi_write/zi_end` handle semantics
- `zi_telemetry` emission (best-effort)

### 2. Control Plane (`ctl/`)
- `zi_ctl` ZCL1 frame parsing
- CAPS_LIST enumeration order
- Error response format

### 3. Handle Lifecycle (`handles/`)
- Reserved handles 0/1/2
- Handle allocation >=3
- Double-end detection
- Invalid handle rejection

### 4. Golden Capabilities (`caps/`)
- `file/fs` sandbox enforcement (`ZI_FS_ROOT`)
- `proc/argv` stream encoding
- `proc/env` stream encoding  
- `proc/hopper` ZCL1 protocol
- `net/tcp` DNS + sandbox (`ZI_NET_ALLOW`)
- `async/default` LIST/INVOKE/CANCEL protocol

### 5. Error Contracts (`errors/`)
- Error code stability (`ZI_E_*` values)
- ZCL1 error response payload format
- Deterministic error messages (where specified)

## Running Conformance Tests

### Against zingcore 2.5 Reference Implementation

```bash
cd /path/to/zasm/src/zingcore/2.5
make conformance
```

This compiles and runs all conformance tests against the reference implementation.

### Against a Custom Runtime

Conformance tests are C programs that:
1. Link against your runtime's `libzingcore*.a` or equivalent.
2. Call zABI 2.5 syscalls and validate behavior.
3. Exit with status 0 on success, non-zero on failure.

To run against your runtime:

```bash
cd abi/conformance
export ZINGCORE_LIB=/path/to/your/libzingcore.a
export ZINGCORE_INCLUDE=/path/to/your/include
make test
```

## Test Format

Each test is a standalone C program (`test_*.c`) that:
- Includes only `zi_sysabi25.h` and standard headers.
- Sets up minimal runtime context (memory mapping, etc.).
- Performs one or more validation checks.
- Prints diagnostic info to stderr on failure.
- Exits 0 (pass) or 1 (fail).

Example:
```c
#include "zi_sysabi25.h"
#include <stdio.h>

int main(void) {
  uint32_t ver = zi_abi_version();
  if (ver != 0x00020005u) {
    fprintf(stderr, "FAIL: expected 0x00020005, got 0x%08x\n", ver);
    return 1;
  }
  printf("PASS\n");
  return 0;
}
```

## Test Inventory

### Implemented

- `test_abi_version.c` — Validate `zi_abi_version` returns correct constant.
- `test_ctl_caps_list.c` — Validate ZCL1 framing and caps list structure.
- `test_handle_reserved.c` — Validate handles 0/1/2 are reserved.
- `test_alloc_zero.c` — Validate `zi_alloc(0)` behavior.

### Planned

- `test_alloc_alignment.c` — Validate allocation alignment guarantees.
- `test_free_null.c` — Validate `zi_free(0)` or `zi_free(NULL)`.
- `test_read_bounds.c` — Validate `zi_read` bounds checking.
- `test_write_bounds.c` — Validate `zi_write` bounds checking.
- `test_cap_open_invalid.c` — Validate `zi_cap_open` error paths.
- `test_fs_sandbox.c` — Validate `file/fs` rejects paths outside `ZI_FS_ROOT`.
- `test_tcp_sandbox.c` — Validate `net/tcp` rejects non-allowed hosts.
- `test_async_future_lifecycle.c` — Validate async future table behavior.

## Adding New Tests

1. Create `test_<name>.c` in the appropriate subdirectory.
2. Follow the test format (standalone, minimal deps).
3. Add to `Makefile` CONFORMANCE_TESTS list.
4. Document expected behavior in test comments.
5. Submit with a passing run against reference implementation.

## Conformance Certification

A runtime is **zABI 2.5 conformant** if:
1. All conformance tests pass.
2. Wire protocol formats match specs (`ZCL1_PROTOCOL.md`, etc.).
3. Error codes match `zi_sysabi25.h` definitions.
4. Capability enumeration is deterministic and stable.

## References

- Normative ABI spec: `../ABI_V2_5.md`
- Wire format specs: `../ZCL1_PROTOCOL.md`, `../ZI_CTL_PROTOCOL.md`, etc.
- Stability guarantees: `../STABILITY.md`
