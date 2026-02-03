// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>

typedef enum SirccEmitKind {
  SIRCC_EMIT_EXE = 0,
  SIRCC_EMIT_LLVM_IR,
  SIRCC_EMIT_OBJ,
} SirccEmitKind;

typedef enum SirccExitCode {
  SIRCC_EXIT_OK = 0,
  SIRCC_EXIT_ERROR = 1,        // invalid input / validation / codegen / link failure
  SIRCC_EXIT_INTERNAL = 2,     // internal error (OOM, invariants)
  SIRCC_EXIT_TOOLCHAIN = 3,    // missing external tools (clang/strip)
  SIRCC_EXIT_USAGE = 4,        // bad CLI args
} SirccExitCode;

typedef enum SirccDiagnosticsFormat {
  SIRCC_DIAG_TEXT = 0,
  SIRCC_DIAG_JSON = 1,
} SirccDiagnosticsFormat;

typedef enum SirccColorMode {
  SIRCC_COLOR_AUTO = 0,
  SIRCC_COLOR_ALWAYS = 1,
  SIRCC_COLOR_NEVER = 2,
} SirccColorMode;

typedef enum SirccRuntimeKind {
  SIRCC_RUNTIME_LIBC = 0,
  SIRCC_RUNTIME_ZABI25 = 1,
} SirccRuntimeKind;

typedef struct SirccOptions {
  const char* argv0; // optional; used for best-effort path inference
  const char* input_path;
  const char* output_path;
  SirccEmitKind emit;
  const char* clang_path;
  const char* target_triple;
  SirccRuntimeKind runtime;
  const char* zabi25_root; // optional; default probes repo and dist paths
  bool verify_only;
  bool dump_records;
  bool print_target;
  bool verbose;
  bool strip;
  bool require_pinned_triple;
  SirccDiagnosticsFormat diagnostics;
  SirccColorMode color;
  int diag_context; // number of surrounding JSONL lines to print on error (also embedded in JSON diagnostics)
} SirccOptions;

int sircc_compile(const SirccOptions* opt);
bool sircc_print_target(const char* triple);
