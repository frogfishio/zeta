// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>

typedef struct SirProgram SirProgram;

// Lowers supported SIR-HL constructs into SIR-Core in-place.
//
// This is used by normal codegen (to avoid "split personality" lowering) and
// by `lower_hl_and_emit_sir_core` when producing a debug-normalized Core stream.
bool lower_hl_in_place(SirProgram* p);

// Rewrites supported SIR-HL constructs into Core SIR and emits a normalized
// `sir-v1.0` JSONL stream to `out_path`.
//
// Current MVP lowering:
// - `sem.if` (val/val branches) -> `select`
// - `sem.and_sc` / `sem.or_sc` (rhs kind=val) -> `bool.and` / `bool.or`
//
// If an unsupported `sem:*` form is encountered, this returns false and emits
// a diagnostic.
bool lower_hl_and_emit_sir_core(SirProgram* p, const char* out_path);
