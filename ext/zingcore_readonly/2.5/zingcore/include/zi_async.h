#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// zABI 2.5 async selector registry (by-the-book runtime, WIP).
//
// Key properties:
// - Explicit initialization (no constructor-based auto-registration)
// - Deterministic enumeration order
// - Public enumeration API (no probing hacks)

typedef struct {
  int (*ack)(void *ctx, uint64_t req_id, uint64_t future_id);
  int (*fail)(void *ctx, uint64_t req_id, const char *code, const char *msg);
  int (*future_ok)(void *ctx, uint64_t future_id, const uint8_t *val,
                   uint32_t val_len);
  int (*future_fail)(void *ctx, uint64_t future_id, const char *code,
                     const char *msg);
  int (*future_cancel)(void *ctx, uint64_t future_id);
} zi_async_emit;

typedef int (*zi_async_invoke)(const zi_async_emit *emit, void *emit_ctx,
                               const uint8_t *params, uint32_t params_len,
                               uint64_t req_id, uint64_t future_id);

typedef int (*zi_async_cancel_cb)(void *emit_ctx, uint64_t future_id);

typedef struct zi_async_selector {
  const char *cap_kind;  // e.g. "exec"
  const char *cap_name;  // e.g. "run"
  // Selector names are relative and versioned, e.g. "run.v1" or "ping.v1".
  // Fully-qualified forms like "exec.run.v1" are intentionally rejected in 2.5.
  const char *selector;

  zi_async_invoke invoke;
  zi_async_cancel_cb cancel; // optional
} zi_async_selector;

typedef struct zi_async_registry_v1 {
  const zi_async_selector *const *selectors;
  size_t selector_count;
} zi_async_registry_v1;

// Initialize the process-global selector registry.
// Safe to call multiple times; returns 1 on success.
int zi_async_init(void);

// Reset registry contents (test-only; not intended for production callers).
void zi_async_reset_for_test(void);

// Register a selector in the process-global registry.
// Returns 1 on success, 0 on error (invalid selector, duplicate, full, or not init).
int zi_async_register(const zi_async_selector *sel);

// Find a selector by (cap_kind, cap_name, selector) bytes.
const zi_async_selector *zi_async_find(const char *kind, size_t kind_len,
                                       const char *name, size_t name_len,
                                       const char *selector,
                                       size_t selector_len);

// Enumerate all selectors.
const zi_async_registry_v1 *zi_async_registry(void);

#ifdef __cplusplus
}
#endif
