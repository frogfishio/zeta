#include "handles.h"

#include <stdlib.h>
#include <string.h>

bool sem_handles_init(sem_handles_t* hs, uint32_t cap) {
  if (!hs) return false;
  if (cap < 4) return false;
  memset(hs, 0, sizeof(*hs));

  sem_handle_entry_t* entries = (sem_handle_entry_t*)calloc(cap, sizeof(*entries));
  if (!entries) return false;

  hs->entries = entries;
  hs->cap = cap;
  hs->next = 3;
  return true;
}

void sem_handles_dispose(sem_handles_t* hs) {
  if (!hs) return;
  free(hs->entries);
  memset(hs, 0, sizeof(*hs));
}

static bool is_valid_index(const sem_handles_t* hs, zi_handle_t h, uint32_t* out_i) {
  if (!hs || !hs->entries) return false;
  if (h < 0) return false;
  const uint32_t i = (uint32_t)h;
  if (i >= hs->cap) return false;
  if (out_i) *out_i = i;
  return true;
}

bool sem_handle_install(sem_handles_t* hs, zi_handle_t h, sem_handle_entry_t e) {
  uint32_t i = 0;
  if (!is_valid_index(hs, h, &i)) return false;
  hs->entries[i] = e;
  return true;
}

zi_handle_t sem_handle_alloc(sem_handles_t* hs, sem_handle_entry_t e) {
  if (!hs || !hs->entries) return -10;

  for (uint32_t attempt = 0; attempt < hs->cap; attempt++) {
    const zi_handle_t h = hs->next;
    hs->next++;
    if (hs->next >= (zi_handle_t)hs->cap) hs->next = 3;

    uint32_t i = 0;
    if (!is_valid_index(hs, h, &i)) continue;
    if (hs->entries[i].ops != NULL) continue;
    hs->entries[i] = e;
    return h;
  }
  return -8;
}

bool sem_handle_lookup(const sem_handles_t* hs, zi_handle_t h, sem_handle_entry_t* out) {
  if (!out) return false;
  uint32_t i = 0;
  if (!is_valid_index(hs, h, &i)) return false;
  if (!hs->entries[i].ops) return false;
  *out = hs->entries[i];
  return true;
}

bool sem_handle_release(sem_handles_t* hs, zi_handle_t h) {
  uint32_t i = 0;
  if (!is_valid_index(hs, h, &i)) return false;
  if (!hs->entries[i].ops) return false;
  if (h == 0 || h == 1 || h == 2) return false;
  memset(&hs->entries[i], 0, sizeof(hs->entries[i]));
  return true;
}

uint32_t sem_handle_hflags(const sem_handles_t* hs, zi_handle_t h) {
  uint32_t i = 0;
  if (!is_valid_index(hs, h, &i)) return 0;
  if (!hs->entries[i].ops) return 0;
  return hs->entries[i].hflags;
}

