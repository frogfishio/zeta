#include "zi_async.h"
#include "zi_caps.h"

#include <stdio.h>

static int dummy_invoke(const zi_async_emit *emit, void *ctx, const uint8_t *p,
                        uint32_t n, uint64_t req_id, uint64_t future_id) {
  (void)emit;
  (void)ctx;
  (void)p;
  (void)n;
  (void)req_id;
  (void)future_id;
  return 1;
}

static const zi_async_selector sel_exec_run_v1 = {
    .cap_kind = "exec",
    .cap_name = "run",
    .selector = "run.v1",
    .invoke = dummy_invoke,
    .cancel = NULL,
};

static const zi_async_selector sel_exec_run_fq_v1 = {
  .cap_kind = "exec",
  .cap_name = "run",
  // Fully-qualified forms are intentionally rejected in zingcore 2.5.
  .selector = "exec.run.v1",
  .invoke = dummy_invoke,
  .cancel = NULL,
};

static const zi_async_selector sel_exec_run_unversioned = {
  .cap_kind = "exec",
  .cap_name = "run",
  .selector = "run",
  .invoke = dummy_invoke,
  .cancel = NULL,
};

static const zi_async_selector sel_exec_run_badchar = {
  .cap_kind = "exec",
  .cap_name = "run",
  .selector = "run/v1",
  .invoke = dummy_invoke,
  .cancel = NULL,
};

static const zi_async_selector sel_async_ping_v1 = {
    .cap_kind = "async",
    .cap_name = "default",
    .selector = "ping.v1",
    .invoke = dummy_invoke,
    .cancel = NULL,
};

static const zi_cap_v1 cap_exec_run_v1 = {
  .kind = "exec",
  .name = "run",
  .version = 1,
  .cap_flags = 0,
  .meta = NULL,
  .meta_len = 0,
};

static const zi_cap_v1 cap_async_default_v1 = {
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
  if (!zi_async_init()) {
    fprintf(stderr, "zi_async_init failed\n");
    return 1;
  }

  zi_caps_reset_for_test();
  zi_async_reset_for_test();

  // Selectors must not register before the cap exists.
  if (zi_async_register(&sel_exec_run_v1)) {
    fprintf(stderr, "selector registered before cap existed\n");
    return 1;
  }

  if (!zi_cap_register(&cap_async_default_v1)) {
    fprintf(stderr, "register async/default cap failed\n");
    return 1;
  }
  if (!zi_cap_register(&cap_exec_run_v1)) {
    fprintf(stderr, "register exec/run cap failed\n");
    return 1;
  }

  if (zi_async_register(&sel_exec_run_fq_v1)) {
    fprintf(stderr, "fully-qualified selector name was accepted\n");
    return 1;
  }
  if (zi_async_register(&sel_exec_run_unversioned)) {
    fprintf(stderr, "unversioned selector name was accepted\n");
    return 1;
  }
  if (zi_async_register(&sel_exec_run_badchar)) {
    fprintf(stderr, "invalid-char selector name was accepted\n");
    return 1;
  }

  if (!zi_async_register(&sel_async_ping_v1)) {
    fprintf(stderr, "register async/default ping failed\n");
    return 1;
  }
  if (!zi_async_register(&sel_exec_run_v1)) {
    fprintf(stderr, "register exec/run run failed\n");
    return 1;
  }

  // Duplicate should fail.
  if (zi_async_register(&sel_exec_run_v1)) {
    fprintf(stderr, "duplicate register unexpectedly succeeded\n");
    return 1;
  }

  const zi_async_selector *found = zi_async_find(
      "exec", 4, "run", 3, "run.v1", 6);
  if (!found) {
    fprintf(stderr, "zi_async_find failed\n");
    return 1;
  }

  const zi_async_registry_v1 *reg = zi_async_registry();
  if (!reg) {
    fprintf(stderr, "zi_async_registry returned NULL\n");
    return 1;
  }
  if (reg->selector_count != 2) {
    fprintf(stderr, "expected 2 selectors, got %zu\n", reg->selector_count);
    return 1;
  }

  // Deterministic enumeration order: registration order.
  if (!reg->selectors[0] || !reg->selectors[1]) {
    fprintf(stderr, "selectors array contains NULL\n");
    return 1;
  }

  printf("ok\n");
  return 0;
}
