#pragma once

#include <stdbool.h>
#include <stddef.h>

// Signature types for validating semantics constructor calls ("intrinsics")
// against a compiled, closed vocabulary.
//
// Notes:
// - `kind` is "tok" or "rule".
// - `type` is the token or rule name. `"*"` is a wildcard that matches any
//   token/rule name of the given `kind`.
// - `many=true` means list-valued. For list-valued fields, absence is represented
//   by an empty list; optionality is not semantically meaningful.

typedef struct Spec3FieldSig {
    const char *name;
    const char *kind; // "tok" | "rule"
    const char *type; // token name or rule name, or "*" wildcard
    bool optional;
    bool many;
} Spec3FieldSig;

typedef struct Spec3VariantSig {
    const Spec3FieldSig *fields;
    size_t field_count;
} Spec3VariantSig;

typedef struct Spec3IntrinsicSig {
    const char *name;
    const Spec3VariantSig *variants;
    size_t variant_count;
} Spec3IntrinsicSig;
