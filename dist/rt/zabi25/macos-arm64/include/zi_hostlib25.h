#pragma once

#include "zi_runtime25.h"

#ifdef __cplusplus
extern "C" {
#endif

// Host-side convenience layer for native embedding of the zABI 2.5 syscall surface.
//
// This is intentionally a *host library*: it wires zingcore25 for a native process,
// enables a permissive "all access" environment, and registers the currently
// implemented golden capabilities.
//
// Key properties:
// - Guest pointers are treated as native pointers (via zi_mem_v1_native_init).
// - Standard streams are exposed as handles 0/1/2 (stdin/stdout/stderr).
// - All built-in caps are registered (async/default, event/bus, file/fs, net/tcp,
//   proc/{argv,env,hopper}, sys/info).

// Initializes zingcore25 + host wiring with a permissive configuration.
//
// Safe to call multiple times.
// Returns 1 on success, 0 on failure.
int zi_hostlib25_init_all(int argc, const char *const *argv, const char *const *envp);

#ifdef __cplusplus
} // extern "C"
#endif
