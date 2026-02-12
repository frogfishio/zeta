#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// RFC 7807 "Problem Details" inspired error payload for zingcore 2.5.
//
// Key design points:
// - `trace` is a human-entered unique identifier (often ~20 chars) placed at the
//   error creation site to make whole-repo grep pinpoint the origin.
// - No runtime dependencies and no heap allocation required.

#define ZI_PROBLEM_JSON "application/problem+json"

#ifndef ZI_PROBLEM_CHAIN_MAX
#define ZI_PROBLEM_CHAIN_MAX 8
#endif

// Stable, machine-readable error identifiers.
//
// These are intended to be emitted in telemetry and serialized into Problem Details.
// They are not tied to HTTP transport, but use HTTP status codes as a familiar
// severity/classification scheme.
//
// NOTE: Additions are allowed; renames are NOT.
typedef enum {
  ZI_ERR_VALIDATION_ERROR = 1,
  ZI_ERR_INVALID_REQUEST,
  ZI_ERR_ALREADY_EXISTS,
  ZI_ERR_INVALID_TOKEN,
  ZI_ERR_TOKEN_EXPIRED,
  ZI_ERR_AUTH_ERROR,
  ZI_ERR_INSUFFICIENT_SCOPE,
  ZI_ERR_NOT_FOUND,
  ZI_ERR_UNSUPPORTED_METHOD,
  ZI_ERR_SYSTEM_ERROR,
  ZI_ERR_CONFIGURATION_ERROR,
  ZI_ERR_SERVICE_ERROR,
  ZI_ERR_BAD_REQUEST,
  ZI_ERR_PAYMENT_REQUIRED,
  ZI_ERR_CONFLICT,
  ZI_ERR_UNAUTHORIZED,
  ZI_ERR_FORBIDDEN,
  ZI_ERR_TOO_MANY_REQUESTS,
  ZI_ERR_NOT_IMPLEMENTED,
  ZI_ERR_BAD_GATEWAY,
  ZI_ERR_SERVICE_UNAVAILABLE,
  ZI_ERR_GATEWAY_TIMEOUT,
} zi_problem_error;

// Returns the stable string identifier for an error (e.g. "not_found").
const char *zi_problem_error_id(zi_problem_error e);

// Returns the canonical status code for an error (e.g. 404 for NOT_FOUND).
uint32_t zi_problem_status(zi_problem_error e);

// Returns a short human title (e.g. "Not Found").
// Title is intended for humans; it is stable-ish but not a compatibility surface.
const char *zi_problem_title(zi_problem_error e);

typedef struct {
  zi_problem_error error;
  const char *error_description;
  const char *stage; // optional
  uint64_t at_ms;
} zi_problem_chain_item;

typedef struct {
  zi_problem_error error;
  uint32_t status;
  const char *detail;

  // `trace` is a greppable origin identifier set at creation.
  const char *trace; // optional

  zi_problem_chain_item chain[ZI_PROBLEM_CHAIN_MAX];
  uint32_t chain_count;
} zi_problem_details;

// Initialize a problem details value.
void zi_problem_init(zi_problem_details *p, zi_problem_error e, const char *detail,
                     const char *trace);

// Append a semantic chain hop (mutates p). Returns 1 on success, 0 on failure.
int zi_problem_chain_push(zi_problem_details *p, zi_problem_error e,
                          const char *error_description, const char *stage,
                          uint64_t at_ms);

// Serialize a public RFC7807-compatible JSON object.
//
// Output shape:
// {
//   "type": "urn:zi-error:<id>",
//   "title": "...",
//   "status": 400,
//   "detail": "...",
//   "trace": "...",            // omitted if NULL
//   "chain": [ { ... }, ... ]
// }
//
// Returns number of bytes written (excluding NUL) on success.
// Returns 0 on failure (buffer too small or invalid inputs).
size_t zi_problem_to_json(const zi_problem_details *p, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
