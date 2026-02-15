#pragma once

#include <stddef.h>

#include "semcall_sig.h"

// GL (Grit Language): canonical, source-level semantics vocabulary.
//
// This is intended to eventually replace language-/surface-specific semantics
// dictionaries used by Stage 3.

extern const Spec3IntrinsicSig GL_SIG[];
extern const size_t GL_SIG_COUNT;
