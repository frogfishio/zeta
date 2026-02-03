// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "compiler.h"

#include <stdbool.h>
#include <stdio.h>

typedef enum {
  SIRCC_CHECK_TEXT = 0,
  SIRCC_CHECK_JSON,
} SirccCheckFormat;

typedef struct {
  const char* argv0;         // optional; used to infer dist root (best-effort)
  const char* dist_root;     // optional; if set, uses <dist_root>/test/examples
  const char* examples_dir;  // optional; if set, uses this directory directly
  SirccCheckFormat format;
} SirccCheckOptions;

int sircc_run_check(FILE* out, const SirccOptions* base_opt, const SirccCheckOptions* chk);
