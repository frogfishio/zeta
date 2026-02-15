#pragma once

// Strictly validates a Stage 4 `.ast.*.jsonl` document against the sem2sir
// closed vocabulary (intrinsics + normalized ops + normalized types).
//
// Returns 0 on success, non-zero on failure.
int sem2sir_check_stage4_file(const char *path);
