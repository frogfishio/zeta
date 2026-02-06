#include "zi_caps.h"

#include <string.h>

#ifndef ZI_CAPS_MAX
#define ZI_CAPS_MAX 128
#endif

typedef struct {
  int initialized;
  const zi_cap_v1 *caps[ZI_CAPS_MAX];
  size_t cap_count;

  zi_cap_registry_v1 pub;
} zi_caps_state;

static zi_caps_state g_caps;

static int streq(const char *a, const char *b) {
  if (a == b) return 1;
  if (!a || !b) return 0;
  return strcmp(a, b) == 0;
}

static int cap_same_identity(const zi_cap_v1 *a, const zi_cap_v1 *b) {
  if (!a || !b) return 0;
  return streq(a->kind, b->kind) && streq(a->name, b->name) && a->version == b->version;
}

static int cap_cmp_identity(const zi_cap_v1 *a, const zi_cap_v1 *b) {
  if (a == b) return 0;
  if (!a) return -1;
  if (!b) return 1;
  int kc = strcmp(a->kind ? a->kind : "", b->kind ? b->kind : "");
  if (kc != 0) return kc;
  int nc = strcmp(a->name ? a->name : "", b->name ? b->name : "");
  if (nc != 0) return nc;
  if (a->version < b->version) return -1;
  if (a->version > b->version) return 1;
  return 0;
}

int zi_caps_init(void) {
  if (g_caps.initialized) return 1;
  g_caps.initialized = 1;
  g_caps.cap_count = 0;
  g_caps.pub.caps = (const zi_cap_v1 *const *)g_caps.caps;
  g_caps.pub.cap_count = 0;
  return 1;
}

void zi_caps_reset_for_test(void) {
  g_caps.initialized = 1;
  g_caps.cap_count = 0;
  g_caps.pub.caps = (const zi_cap_v1 *const *)g_caps.caps;
  g_caps.pub.cap_count = 0;
}

int zi_cap_register(const zi_cap_v1 *cap) {
  if (!g_caps.initialized) return 0;
  if (!cap || !cap->kind || !cap->name) return 0;
  if (cap->meta_len && !cap->meta) return 0;

  for (size_t i = 0; i < g_caps.cap_count; i++) {
    if (cap_same_identity(g_caps.caps[i], cap)) return 0;
  }

  if (g_caps.cap_count >= ZI_CAPS_MAX) return 0;

  // Insert deterministically by (kind,name,version).
  size_t pos = g_caps.cap_count;
  for (size_t i = 0; i < g_caps.cap_count; i++) {
    if (cap_cmp_identity(cap, g_caps.caps[i]) < 0) {
      pos = i;
      break;
    }
  }
  for (size_t j = g_caps.cap_count; j > pos; j--) {
    g_caps.caps[j] = g_caps.caps[j - 1];
  }
  g_caps.caps[pos] = cap;
  g_caps.cap_count++;
  g_caps.pub.cap_count = g_caps.cap_count;
  return 1;
}

const zi_cap_registry_v1 *zi_cap_registry(void) {
  if (!g_caps.initialized) return NULL;
  return &g_caps.pub;
}
