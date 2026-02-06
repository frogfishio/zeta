#pragma once

#include <stdint.h>

#include "zi_async.h"
#include "zi_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

// zingcore 25-family runtime entrypoints (wiring/convenience API).
// The wire/system ABI remains the `zi_*` surface (see zi_sysabi25.h).

// zABI version for this runtime (2.5).
#define ZINGCORE25_ZABI_VERSION 0x00020005u

const char *zingcore25_version(void);

// zABI version as an integer (matches ZINGCORE25_ZABI_VERSION).
uint32_t zingcore25_zabi_version(void);

// Initialize zingcore 2.5 process-global state.
// This currently initializes the caps and async selector registries.
// Safe to call multiple times; returns 1 on success.
int zingcore25_init(void);

// Convenience accessors for process-global registries.
const zi_cap_registry_v1 *zingcore25_cap_registry(void);
const zi_async_registry_v1 *zingcore25_async_registry(void);

// Test-only reset (not intended for production callers).
void zingcore25_reset_for_test(void);

#ifdef __cplusplus
}
#endif
