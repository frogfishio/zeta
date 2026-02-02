// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "json.h"
#include "sircc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ArenaBlock {
  struct ArenaBlock* next;
  size_t cap;
  size_t len;
  unsigned char data[];
} ArenaBlock;

void arena_init(Arena* a) {
  a->head = NULL;
  a->cur = NULL;
}

void arena_free(Arena* a) {
  ArenaBlock* b = (ArenaBlock*)a->head;
  while (b) {
    ArenaBlock* next = b->next;
    free(b);
    b = next;
  }
  a->head = NULL;
  a->cur = NULL;
}

static size_t align_up(size_t n, size_t align) {
  size_t rem = n % align;
  return rem ? (n + (align - rem)) : n;
}

void* arena_alloc(Arena* a, size_t size) {
  if (size == 0) size = 1;

  ArenaBlock* b = (ArenaBlock*)a->cur;
  if (!b) {
    size_t cap = 4096;
    while (cap < size) cap *= 2;
    b = (ArenaBlock*)malloc(sizeof(ArenaBlock) + cap);
    if (!b) return NULL;
    b->next = NULL;
    b->cap = cap;
    b->len = 0;
    a->head = b;
    a->cur = b;
  }

  size_t aligned = align_up(b->len, sizeof(void*));
  if (aligned + size > b->cap) {
    size_t cap = b->cap * 2;
    while (cap < size) cap *= 2;
    ArenaBlock* n = (ArenaBlock*)malloc(sizeof(ArenaBlock) + cap);
    if (!n) return NULL;
    n->next = NULL;
    n->cap = cap;
    n->len = 0;
    b->next = n;
    a->cur = n;
    b = n;
    aligned = 0;
  }

  void* p = b->data + aligned;
  b->len = aligned + size;
  memset(p, 0, size);
  return p;
}

char* arena_strdup(Arena* a, const char* s) {
  size_t n = strlen(s);
  char* out = (char*)arena_alloc(a, n + 1);
  if (!out) return NULL;
  memcpy(out, s, n + 1);
  return out;
}

StrView sv_from_cstr(const char* s) {
  return (StrView){.ptr = s, .len = s ? strlen(s) : 0};
}

bool sv_eq(StrView a, const char* b) {
  if (!b) return false;
  size_t blen = strlen(b);
  return a.len == blen && memcmp(a.ptr, b, blen) == 0;
}
