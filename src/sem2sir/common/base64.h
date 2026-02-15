#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// GRIT-specific base64 helpers.
//
// Purpose: allow exact byte roundtripping in JSON artifacts (ParseTree, Pack, etc)
// without assuming UTF-8.

// Returns the maximum base64 string length (excluding NUL) needed to encode `n` bytes.
size_t grit_base64_encode_bound(size_t n);

// Encodes `in[0..in_len)` into a newly-allocated NUL-terminated base64 string.
// Caller owns `*out_str` and must free it with `free`.
bool grit_base64_encode_alloc(const uint8_t *in, size_t in_len, char **out_str);

// Decodes base64 text into newly-allocated bytes.
// Accepts standard base64 with '=' padding. Rejects invalid characters.
// Caller owns `*out_bytes` and must free it with `free`.
bool grit_base64_decode_alloc(const char *in, size_t in_len, uint8_t **out_bytes, size_t *out_len);
