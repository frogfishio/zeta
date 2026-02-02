# Migration Guide: zingcore2.2_final → zABI 2.5

**Target Audience**: Embedders and runtime integrators  
**Status**: Draft (January 2026)

This guide helps you migrate from `zingcore2.2_final` to the new zABI 2.5 runtime.

## Executive Summary

zABI 2.5 represents a **major version bump** with breaking changes. The core philosophy shifts from implicit auto-registration to explicit, deterministic initialization.

### Key Benefits of Migration

- **Explicit registration**: No constructor surprises; you control what's exposed.
- **Golden capabilities**: Production-ready sandboxed I/O (filesystem, network, async).
- **Improved testability**: Deterministic enumeration, stable error codes.
- **Better modularity**: Clean separation between core syscalls and capabilities.
- **Future-proof**: 2.5 is frozen; new features via caps/selectors only.

### Migration Effort

- **Small embedding**: ~2 hours (add explicit registration calls).
- **Medium integration**: ~1-2 days (update cap usage patterns, test coverage).
- **Large system**: ~1 week (audit all ABI touchpoints, conformance testing).

---

## Breaking Changes

### 1. Removed: `zi_abi_features`

**2.2_final**: used a `zi_abi_features()` bitset for discovery/negotiation.

**2.5 (replacement)**:
```c
// Use zi_ctl to enumerate capabilities
uint8_t req[24], res[4096];
build_zcl1_caps_list_req(req, 1);
int32_t n = zi_ctl((zi_ptr_t)(uintptr_t)req, 24, 
                    (zi_ptr_t)(uintptr_t)res, sizeof(res));
// Parse response to check for specific caps
```

**Rationale**: Feature bits don't scale; structured discovery via `zi_ctl` is more flexible.

### 2. Explicit Capability Registration

**2.2_final**:
```c
// Caps auto-registered via constructor attributes
int main() {
  zingcore_init();  // Magic happens
}
```

**2.5 (required)**:
```c
int main() {
  // 1. Init registries
  if (!zingcore25_init()) {
    fprintf(stderr, "init failed\n");
    return 1;
  }
  
  // 2. Register capabilities explicitly
  zi_file_fs25_register();
  zi_net_tcp25_register();
  zi_proc_argv25_register();
  zi_proc_env25_register();
  zi_proc_hopper25_register();
  zi_async_default25_register();
  
  // 3. Register selectors (if using async)
  zi_async_default25_register_selectors();
  
  // Now zi_cap_open, zi_ctl, etc. work
}
```

**Migration tip**: Create a `register_all_caps()` helper for your embeddings.

### 3. Async Selector Registration Required

**2.2_final**: Selectors auto-registered (if supported).

**2.5**:
```c
// After registering async/default cap:
const zi_async_selector my_selector = {
  .cap_kind = "exec",
  .cap_name = "run",
  .selector = "run.v1",
  .invoke = my_invoke_fn,
  .cancel = NULL,
};

// Must register after cap exists:
if (!zi_async_register(&my_selector)) {
  fprintf(stderr, "selector registration failed\n");
}
```

**Rationale**: Prevents dangling selectors; enforces coupling to registered caps.

### 4. ZCL1 Framing for Control Plane

**2.2_final**: Custom control-plane protocols (varied per op).

**2.5**: All control-plane ops use ZCL1 frames (24-byte header + payload).

**Example** (`zi_ctl` CAPS_LIST):

```c
// Build request frame
uint8_t req[24];
memcpy(req + 0, "ZCL1", 4);
write_u16le(req + 4, 1);        // version
write_u16le(req + 6, 1);        // op=CAPS_LIST
write_u32le(req + 8, 42);       // rid
write_u32le(req + 12, 0);       // status (request)
write_u32le(req + 16, 0);       // reserved
write_u32le(req + 20, 0);       // payload_len

uint8_t res[4096];
int32_t n = zi_ctl((zi_ptr_t)(uintptr_t)req, 24, 
                    (zi_ptr_t)(uintptr_t)res, sizeof(res));

// Parse response frame...
```

**Migration tip**: Use `zi_zcl1.h` helpers (`zi_zcl1_write_ok`, `zi_zcl1_parse`).

### 5. Capability Open Parameters

**2.2_final**: Some caps used implicit env vars or global state.

**2.5**: All open parameters are explicit in the open request blob.

**Example** (`file/fs`):

```c
uint8_t params[20];
write_u64le(params + 0, (uint64_t)(uintptr_t)"/demo.txt");
write_u32le(params + 8, 9);  // path_len
write_u32le(params + 12, ZI_FILE_O_READ);
write_u32le(params + 16, 0);  // mode

uint8_t open_req[40];
build_cap_open_req(open_req, "file", "fs", params, 20);

zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)open_req);
```

Sandboxing is still via env vars (`ZI_FS_ROOT`, `ZI_NET_ALLOW`), but the path is in params.

### 6. Handle Allocation Model

**2.2_final**: Handles 0/1/2 sometimes auto-wired to stdio.

**2.5**: Handles 0/1/2 are **reserved** but not auto-wired. The embedder must allocate them explicitly via the handle table:

```c
zi_handles25_init();

static fd_stream s_in = {.fd = 0, .close_on_end = 0};
static fd_stream s_out = {.fd = 1, .close_on_end = 0};
static fd_stream s_err = {.fd = 2, .close_on_end = 0};

zi_handle_t h_in = zi_handle25_alloc(&fd_ops, &s_in, ZI_H_READABLE);
zi_handle_t h_out = zi_handle25_alloc(&fd_ops, &s_out, ZI_H_WRITABLE);
zi_handle_t h_err = zi_handle25_alloc(&fd_ops, &s_err, ZI_H_WRITABLE);

// Now handles 0/1/2 are available for zi_read/zi_write
```

**Rationale**: Explicit wiring makes it clear what the guest sees; no magic.

---

## New Features Available in 2.5

### 1. Golden Capabilities

All golden caps are production-ready with tests, docs, and sandboxing:

- **file/fs**: Filesystem with `ZI_FS_ROOT` sandbox.
- **proc/argv, proc/env**: Read-only access to args/env.
- **proc/hopper**: Safe arena allocator with catalog + optional direct-call ABI.
- **net/tcp**: TCP client with DNS resolution, gated by `ZI_NET_ALLOW`.
- **async/default**: Async invocation with futures, cancellation, event streams.

### 2. Hopper Direct-Call ABI (`zi_hopabi25`)

For guests that need fast-path Hopper access without ZCL1 overhead:

```c
#include "zi_hopabi25.h"

// Guest code (native/wasm/jit):
zi_hopabi25_ctx_t *ctx = zi_hopabi25_open(1, 1024, 16, 0);
int32_t ref = zi_hopabi25_record(ctx, 1);
zi_hopabi25_field_set_bytes(ctx, ref, 0, (const uint8_t *)"hi", 2);
// ...
zi_hopabi25_close(ctx);
```

**Use case**: Language runtimes that need Hopper for object graphs but want native-speed calls.

### 3. Structured Errors (RFC 7807 Problem Details)

All errors now follow a standard JSON shape:

```json
{
  "type": "urn:zi-error:not_found",
  "title": "Not Found",
  "status": 404,
  "detail": "File '/missing.txt' does not exist",
  "trace": "t_file_fs_open_123",
  "chain": []
}
```

Emitted via `zi_telemetry` for observability.

### 4. Async Selector Model

Build your own async operations:

```c
static int my_async_run(const zi_async_emit *emit, void *ctx,
                        const uint8_t *params, uint32_t len,
                        uint64_t req_id, uint64_t future_id) {
  // 1. Ack immediately
  emit->ack(ctx, req_id, future_id);
  
  // 2. Do work (can be async/threaded)
  uint8_t result[] = "done";
  
  // 3. Complete the future
  emit->future_ok(ctx, future_id, result, 4);
  return 1;
}

const zi_async_selector sel = {
  .cap_kind = "exec",
  .cap_name = "run",
  .selector = "run.v1",
  .invoke = my_async_run,
  .cancel = NULL,
};

zi_async_register(&sel);
```

---

## Migration Checklist

### Phase 1: Preparation (1-2 hours)

- [ ] Read `STABILITY.md` and `abi/ABI_V2_5.md`.
- [ ] Review wire protocol specs (`ZCL1_PROTOCOL.md`, etc.).
- [ ] Audit your codebase for uses of:
  - `zi_abi_features`
  - Implicit cap registration
  - Custom control-plane protocols
- [ ] Identify which golden caps you need.

### Phase 2: Code Changes (2-8 hours)

- [ ] Update build to link `libzingcore25.a` instead of 2.2_final.
- [ ] Replace `#include "zi_abi_v2.h"` with `#include "zi_sysabi25.h"`.
- [ ] Add explicit `zingcore25_init()` call.
- [ ] Add cap registration calls (`zi_*_register()`).
- [ ] Add selector registration if using async.
- [ ] Update `zi_ctl` callsites to use ZCL1 frames.
- [ ] Wire stdio handles explicitly if needed.
- [ ] Update cap open callsites to use new params format.

### Phase 3: Testing (4-16 hours)

- [ ] Run existing test suite; fix failures.
- [ ] Add new tests for:
  - Cap enumeration (`zi_ctl` CAPS_LIST)
  - Each golden cap you use
  - Error cases (invalid params, sandbox violations)
- [ ] Run `make test` and `make itest` in zingcore 2.5.
- [ ] Smoke test your embedding end-to-end.

### Phase 4: Validation (2-4 hours)

- [ ] Verify deterministic behavior (same input → same output).
- [ ] Check sandboxing works (`ZI_FS_ROOT`, `ZI_NET_ALLOW`).
- [ ] Review telemetry output for unexpected errors.
- [ ] Run conformance tests (see `abi/conformance/`).

---

## Example: Minimal 2.5 Embedding

```c
#include "zingcore25.h"
#include "zi_file_fs25.h"
#include "zi_proc_argv25.h"
#include "zi_runtime25.h"
#include "zi_sysabi25.h"

#include <stdio.h>

int main(int argc, char **argv) {
  // 1. Init
  if (!zingcore25_init()) {
    fprintf(stderr, "zingcore25_init failed\n");
    return 1;
  }
  
  // 2. Set runtime hooks
  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);
  
  // 3. Register caps
  zi_file_fs25_register();
  zi_proc_argv25_register();
  
  // 4. Wire argv
  zi_runtime25_set_argv(argc, (const char *const *)argv);
  
  // 5. Use the ABI
  printf("zABI version: 0x%08x\n", zi_abi_version());
  
  // 6. Example: open file/fs
  uint8_t params[20];
  // ... build params ...
  uint8_t req[40];
  // ... build open request ...
  zi_handle_t h = zi_cap_open((zi_ptr_t)(uintptr_t)req);
  if (h >= 3) {
    // ... use handle ...
    zi_end(h);
  }
  
  return 0;
}
```

---

## Timeline & Support

- **2.2_final**: Frozen; no new features.
- **2.5**: Golden (stable); current recommendation.
- **2.6+**: Minor additions only (new caps/selectors).

### Deprecation Policy

- 2.2_final will remain available but is not recommended for new projects.
- No removal date is set; we maintain backward compatibility via separate build targets.

### Getting Help

- Documentation: `src/zingcore/2.5/README.md`
- Examples: `src/zingcore/2.5/zingcore/examples/`
- Tests: `src/zingcore/2.5/zingcore/tests/`
- Issues: GitHub issue tracker

---

## Common Pitfalls

### 1. Forgetting to Initialize

**Error**: Caps return `ZI_E_NOSYS` or crash.

**Fix**: Call `zingcore25_init()` before any ABI usage.

### 2. Registering Selectors Before Caps

**Error**: `zi_async_register` fails.

**Fix**: Register the cap first, then its selectors:

```c
zi_async_default25_register();  // Register cap
zi_async_default25_register_selectors();  // Then selectors
```

### 3. Wrong Pointer Model (WASM)

**Error**: `ZI_E_BOUNDS` when accessing guest memory.

**Fix**: Ensure `zi_ptr_t` values are treated as offsets in WASM (not native pointers). Use `zi_runtime25_set_mem()` with proper mapping callbacks.

### 4. Buffer Too Small for `zi_ctl`

**Error**: `zi_ctl` returns `ZI_E_BOUNDS`.

**Fix**: Allocate at least 4KB for response buffer; retry with larger buffer if needed.

---

## Performance Notes

- **Handle allocation**: O(n) scan; negligible for <100 handles.
- **Cap enumeration**: O(n); cached after first call.
- **ZCL1 framing**: ~50 bytes overhead per request/response; negligible for typical I/O.
- **Async future table**: 64 max per handle; sufficient for most use cases.

No significant performance regressions vs. 2.2_final for typical workloads.

---

## Next Steps

1. Try the minimal embedding example above.
2. Run `make test` and `make itest` in zingcore 2.5.
3. Review the golden cap docs for your use cases.
4. Ask questions via GitHub issues!

**Good luck with your migration!**
