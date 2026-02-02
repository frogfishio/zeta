// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>

typedef enum SirccEmitKind {
  SIRCC_EMIT_EXE = 0,
  SIRCC_EMIT_LLVM_IR,
  SIRCC_EMIT_OBJ,
} SirccEmitKind;

typedef struct SirccOptions {
  const char* input_path;
  const char* output_path;
  SirccEmitKind emit;
  const char* clang_path;
  const char* target_triple;
} SirccOptions;

bool sircc_compile(const SirccOptions* opt);

