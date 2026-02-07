// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>
#include <stdio.h>

typedef enum {
  SIRCC_SUPPORT_TEXT = 0,
  SIRCC_SUPPORT_JSON,
  SIRCC_SUPPORT_HTML,
} SirccSupportFormat;

bool sircc_print_support(FILE* out, SirccSupportFormat fmt, bool full);
