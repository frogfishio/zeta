// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct Arena {
  struct ArenaBlock* head;
  struct ArenaBlock* cur;
} Arena;

void arena_init(Arena* a);
void arena_free(Arena* a);
void* arena_alloc(Arena* a, size_t size);
char* arena_strdup(Arena* a, const char* s);

typedef struct StrView {
  const char* ptr;
  size_t len;
} StrView;

StrView sv_from_cstr(const char* s);
bool sv_eq(StrView a, const char* b);
