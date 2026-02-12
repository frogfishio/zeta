#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Minimal telemetry helper for zingcore 2.5.
//
// This is intentionally small and host-friendly: it provides a best-effort way
// to emit one JSON object per line (JSONL/NDJSON) to stderr for development.
//
// It is NOT a required "capability"; hosts may replace this entirely.

typedef struct {
  uint64_t ts_ms;
} zi_telemetry_clock;

// Optional injection point: if you want stable timestamps in tests.
// If NULL is passed to emit functions, ts_ms defaults to 0.

// Emit one JSONL line to stderr:
// {"ts":...,"topic":"...","body":<json or string>}
//
// Behavior:
// - `topic` is encoded as a JSON string (bytes are treated as UTF-8-ish and escaped).
// - `body` is embedded raw if it "looks like" JSON (best-effort, no full validation).
// - Otherwise, `body` is emitted as a JSON string.
//
// Returns number of bytes written to the output buffer on success.
// Returns 0 on failure (e.g. buffer too small).
size_t zi_telemetry_format_jsonl(const zi_telemetry_clock *clock, const uint8_t *topic,
                                uint32_t topic_len, const uint8_t *body,
                                uint32_t body_len, char *out, size_t out_cap);

// Convenience: format into an internal stack buffer and write to stderr.
// Best-effort: returns 1 if it wrote something, 0 otherwise.
int zi_telemetry_stderr_jsonl(const zi_telemetry_clock *clock, const uint8_t *topic,
                             uint32_t topic_len, const uint8_t *body,
                             uint32_t body_len);

#ifdef __cplusplus
}
#endif
