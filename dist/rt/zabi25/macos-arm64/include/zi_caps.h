#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// zABI 2.5 capability model (by-the-book runtime, WIP).
//
// Design goals:
// - Explicit initialization (no constructor-based auto-registration)
// - Deterministic enumeration order
// - Stable, minimal structs suitable for embedding

typedef struct zi_cap_v1 {
  const char *kind;   // e.g. "exec"
  const char *name;   // e.g. "run"
  uint32_t version;   // selector/interface version for this cap
  uint32_t cap_flags; // bitmask; semantics TBD

  const uint8_t *meta;
  uint32_t meta_len;
} zi_cap_v1;

typedef struct zi_cap_registry_v1 {
  const zi_cap_v1 *const *caps;
  size_t cap_count;
} zi_cap_registry_v1;

// Initialize the process-global capability registry.
// Safe to call multiple times; returns 1 on success.
int zi_caps_init(void);

// Reset registry contents (test-only; not intended for production callers).
void zi_caps_reset_for_test(void);

// Register a capability in the process-global registry.
// Returns 1 on success, 0 on error (invalid cap, duplicate, full, or not init).
int zi_cap_register(const zi_cap_v1 *cap);

// Get a pointer to the process-global registry (or NULL if not initialized).
const zi_cap_registry_v1 *zi_cap_registry(void);

#ifdef __cplusplus
}
#endif
