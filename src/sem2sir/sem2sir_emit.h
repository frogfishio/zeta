#pragma once

// Strict Stage4 AST -> SIR v1.0 emitter.
//
// Policy: no defaults, no implicitness.
// If a required type/operator choice is not explicitly defined, this fails.

int sem2sir_emit_sir_file(const char *in_stage4_jsonl_path, const char *out_sir_jsonl_path);
