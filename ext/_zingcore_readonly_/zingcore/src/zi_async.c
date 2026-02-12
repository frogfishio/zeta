#include "zi_async.h"

#include "zi_caps.h"

#include <string.h>

#ifndef ZI_ASYNC_SELECTORS_MAX
#define ZI_ASYNC_SELECTORS_MAX 256
#endif

typedef struct {
  int initialized;
  const zi_async_selector *selectors[ZI_ASYNC_SELECTORS_MAX];
  size_t selector_count;
  zi_async_registry_v1 pub;
} zi_async_state;

static zi_async_state g_async;

static int streq(const char *a, const char *b) {
  if (a == b) return 1;
  if (!a || !b) return 0;
  return strcmp(a, b) == 0;
}

static int selector_same_identity(const zi_async_selector *a,
                                 const zi_async_selector *b) {
  if (!a || !b) return 0;
  return streq(a->cap_kind, b->cap_kind) && streq(a->cap_name, b->cap_name) &&
         streq(a->selector, b->selector);
}

static int bytes_eq_str(const char *s, const char *b, size_t n) {
  if (!s || !b) return 0;
  size_t slen = strlen(s);
  if (slen != n) return 0;
  return memcmp(s, b, n) == 0;
}

static int cap_exists(const char *kind, const char *name) {
  const zi_cap_registry_v1 *reg = zi_cap_registry();
  if (!reg || !reg->caps) return 0;
  for (size_t i = 0; i < reg->cap_count; i++) {
    const zi_cap_v1 *c = reg->caps[i];
    if (!c || !c->kind || !c->name) continue;
    if (streq(c->kind, kind) && streq(c->name, name)) return 1;
  }
  return 0;
}

static int is_ascii_space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
}

static int selector_is_valid_relative(const zi_async_selector *sel) {
  if (!sel || !sel->cap_kind || !sel->cap_name || !sel->selector) return 0;
  const char *s = sel->selector;
  size_t slen = strlen(s);
  if (slen == 0) return 0;

  // No whitespace/control, no path separators.
  for (size_t i = 0; i < slen; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (ch < 0x20 || ch == 0x7F) return 0;
    if (is_ascii_space((char)ch)) return 0;
    if (ch == '/' || ch == '\\') return 0;
  }

  // Reject fully-qualified forms like "exec.run.v1".
  size_t klen = strlen(sel->cap_kind);
  if (klen > 0 && slen > klen && memcmp(s, sel->cap_kind, klen) == 0 && s[klen] == '.') {
    return 0;
  }

  // Require version suffix: ".v<digits>".
  const char *dotv = strstr(s, ".v");
  if (!dotv) return 0;
  dotv += 2;
  if (*dotv == '\0') return 0;
  for (const char *p = dotv; *p; p++) {
    if (*p < '0' || *p > '9') return 0;
  }
  return 1;
}

int zi_async_init(void) {
  if (g_async.initialized) return 1;
  g_async.initialized = 1;
  g_async.selector_count = 0;
  g_async.pub.selectors = (const zi_async_selector *const *)g_async.selectors;
  g_async.pub.selector_count = 0;
  return 1;
}

void zi_async_reset_for_test(void) {
  g_async.initialized = 1;
  g_async.selector_count = 0;
  g_async.pub.selectors = (const zi_async_selector *const *)g_async.selectors;
  g_async.pub.selector_count = 0;
}

int zi_async_register(const zi_async_selector *sel) {
  if (!g_async.initialized) return 0;
  if (!sel || !sel->cap_kind || !sel->cap_name || !sel->selector) return 0;
  if (!sel->invoke) return 0;

  // By-the-book coupling: selectors may only be registered for an existing cap.
  if (!cap_exists(sel->cap_kind, sel->cap_name)) return 0;

  // By-the-book naming: selector is relative + versioned (no fully-qualified kind prefix).
  if (!selector_is_valid_relative(sel)) return 0;

  for (size_t i = 0; i < g_async.selector_count; i++) {
    if (selector_same_identity(g_async.selectors[i], sel)) return 0;
  }

  if (g_async.selector_count >= ZI_ASYNC_SELECTORS_MAX) return 0;
  g_async.selectors[g_async.selector_count++] = sel;
  g_async.pub.selector_count = g_async.selector_count;
  return 1;
}

const zi_async_selector *zi_async_find(const char *kind, size_t kind_len,
                                       const char *name, size_t name_len,
                                       const char *selector,
                                       size_t selector_len) {
  if (!g_async.initialized) return NULL;
  if (!kind || !name || !selector) return NULL;

  for (size_t i = 0; i < g_async.selector_count; i++) {
    const zi_async_selector *s = g_async.selectors[i];
    if (!s) continue;
    if (!bytes_eq_str(s->cap_kind, kind, kind_len)) continue;
    if (!bytes_eq_str(s->cap_name, name, name_len)) continue;
    if (!bytes_eq_str(s->selector, selector, selector_len)) continue;
    return s;
  }
  return NULL;
}

const zi_async_registry_v1 *zi_async_registry(void) {
  if (!g_async.initialized) return NULL;
  return &g_async.pub;
}
