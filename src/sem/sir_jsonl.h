#pragma once

#include <stdint.h>

#include "sem_host.h"

// Parse a small SIR JSONL subset and run it under the hosted zABI runtime.
// Returns process exit code (0..255-ish), or 1/2 for tool errors.
int sem_run_sir_jsonl(const char* path, const sem_cap_t* caps, uint32_t cap_count, const char* fs_root);

