#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "sem_host.h"

typedef enum sem_diag_format {
  SEM_DIAG_TEXT = 0,
  SEM_DIAG_JSON = 1,
} sem_diag_format_t;

// Parse a small SIR JSONL subset and run it under the hosted zABI runtime.
// Returns process exit code (0..255-ish), or 1/2 for tool errors.
int sem_run_sir_jsonl(const char* path, const sem_cap_t* caps, uint32_t cap_count, const char* fs_root);

// Same as sem_run_sir_jsonl, but with explicit diagnostics formatting.
int sem_run_sir_jsonl_ex(const char* path, const sem_cap_t* caps, uint32_t cap_count, const char* fs_root, sem_diag_format_t diag_format,
                         bool diag_all);
