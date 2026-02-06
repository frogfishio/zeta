#include "zi_caps.h"

#include <stdio.h>
#include <string.h>

static zi_cap_v1 cap_exec_run_v1 = {
    .kind = "exec",
    .name = "run",
    .version = 1,
    .cap_flags = 0,
    .meta = NULL,
    .meta_len = 0,
};

static zi_cap_v1 cap_async_default_v1 = {
    .kind = "async",
    .name = "default",
    .version = 1,
    .cap_flags = 0,
    .meta = NULL,
    .meta_len = 0,
};

int main(void) {
  if (!zi_caps_init()) {
    fprintf(stderr, "zi_caps_init failed\n");
    return 1;
  }

  zi_caps_reset_for_test();

  if (!zi_cap_register(&cap_async_default_v1)) {
    fprintf(stderr, "register async/default failed\n");
    return 1;
  }
  if (!zi_cap_register(&cap_exec_run_v1)) {
    fprintf(stderr, "register exec/run failed\n");
    return 1;
  }

  // Duplicate should fail.
  if (zi_cap_register(&cap_exec_run_v1)) {
    fprintf(stderr, "duplicate register unexpectedly succeeded\n");
    return 1;
  }

  const zi_cap_registry_v1 *reg = zi_cap_registry();
  if (!reg) {
    fprintf(stderr, "zi_cap_registry returned NULL\n");
    return 1;
  }
  if (reg->cap_count != 2) {
    fprintf(stderr, "expected 2 caps, got %zu\n", reg->cap_count);
    return 1;
  }

  // Deterministic enumeration order: lexicographic by (kind,name,version).
  if (!reg->caps[0] || !reg->caps[1]) {
    fprintf(stderr, "caps array contains NULL\n");
    return 1;
  }
  if (!(reg->caps[0]->kind && reg->caps[0]->name && reg->caps[1]->kind &&
        reg->caps[1]->name)) {
    fprintf(stderr, "cap identity fields missing\n");
    return 1;
  }

  if (strcmp(reg->caps[0]->kind, "async") != 0 || strcmp(reg->caps[0]->name, "default") != 0) {
    fprintf(stderr, "unexpected cap[0] identity\n");
    return 1;
  }
  if (strcmp(reg->caps[1]->kind, "exec") != 0 || strcmp(reg->caps[1]->name, "run") != 0) {
    fprintf(stderr, "unexpected cap[1] identity\n");
    return 1;
  }

  printf("ok\n");
  return 0;
}
