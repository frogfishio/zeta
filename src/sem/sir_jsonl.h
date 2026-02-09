#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "sem_host.h"

typedef enum sem_diag_format {
  SEM_DIAG_TEXT = 0,
  SEM_DIAG_JSON = 1,
} sem_diag_format_t;

typedef struct sem_run_host_cfg {
  const sem_cap_t* caps;
  uint32_t cap_count;
  const char* fs_root;

  bool argv_enabled;
  const char* const* argv;
  uint32_t argv_count;

  bool env_enabled;
  const sem_env_kv_t* env;
  uint32_t env_count;
} sem_run_host_cfg_t;

// Parse a small SIR JSONL subset and run it under the hosted zABI runtime.
// Returns process exit code (0..255-ish), or 1/2 for tool errors.
int sem_run_sir_jsonl(const char* path, const sem_cap_t* caps, uint32_t cap_count, const char* fs_root);

// Same as sem_run_sir_jsonl, but with explicit diagnostics formatting.
int sem_run_sir_jsonl_ex(const char* path, const sem_cap_t* caps, uint32_t cap_count, const char* fs_root, sem_diag_format_t diag_format,
                         bool diag_all);

// Run and capture the program exit code separately from tool error codes.
// Returns 0 on successful execution and writes the program exit code to `out_prog_rc`.
// Returns 1/2 on parse/verify/lower/runtime failure (and prints diagnostics like `sem --run`).
int sem_run_sir_jsonl_capture_ex(const char* path, const sem_cap_t* caps, uint32_t cap_count, const char* fs_root, sem_diag_format_t diag_format,
                                 bool diag_all, int* out_prog_rc);

// Run and emit an instruction-level trace as JSONL to the given path.
// Trace output is written to the file only (never mixed with program stdout/stderr).
int sem_run_sir_jsonl_trace_ex(const char* path, const sem_cap_t* caps, uint32_t cap_count, const char* fs_root, sem_diag_format_t diag_format,
                               bool diag_all, const char* trace_jsonl_out_path);

// Run and emit an instruction-level coverage report as JSONL to the given path.
// Coverage output is written to the file only (never mixed with program stdout/stderr).
int sem_run_sir_jsonl_coverage_ex(const char* path, const sem_cap_t* caps, uint32_t cap_count, const char* fs_root, sem_diag_format_t diag_format,
                                  bool diag_all, const char* coverage_jsonl_out_path);

// Run and emit trace and/or coverage JSONL sidecars (single execution).
// Pass NULL for any sidecar you don't want.
int sem_run_sir_jsonl_events_ex(const char* path, const sem_cap_t* caps, uint32_t cap_count, const char* fs_root, sem_diag_format_t diag_format,
                                bool diag_all, const char* trace_jsonl_out_path, const char* coverage_jsonl_out_path, const char* trace_func_filter,
                                const char* trace_op_filter);

// Like sem_run_sir_jsonl_events_ex, but also configures argv/env snapshots.
int sem_run_sir_jsonl_events_host_ex(const char* path, sem_run_host_cfg_t host_cfg, sem_diag_format_t diag_format, bool diag_all,
                                    const char* trace_jsonl_out_path, const char* coverage_jsonl_out_path, const char* trace_func_filter,
                                    const char* trace_op_filter);

// Like sem_run_sir_jsonl_capture_ex, but also configures argv/env snapshots.
int sem_run_sir_jsonl_capture_host_ex(const char* path, sem_run_host_cfg_t host_cfg, sem_diag_format_t diag_format, bool diag_all, int* out_prog_rc);

// Parse + lower + validate (but do not execute) a small SIR JSONL subset.
// Returns 0 on success, or 1/2 for tool errors.
int sem_verify_sir_jsonl(const char* path, sem_diag_format_t diag_format);

// Same as sem_verify_sir_jsonl, but with explicit diagnostics settings.
int sem_verify_sir_jsonl_ex(const char* path, sem_diag_format_t diag_format, bool diag_all);
