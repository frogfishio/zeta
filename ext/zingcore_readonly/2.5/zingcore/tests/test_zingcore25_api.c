#include "zingcore25.h"

#include <stdio.h>

static int dummy_invoke(const zi_async_emit *emit, void *emit_ctx,
                        const uint8_t *params, uint32_t params_len,
                        uint64_t req_id, uint64_t future_id) {
  (void)emit;
  (void)emit_ctx;
  (void)params;
  (void)params_len;
  (void)req_id;
  (void)future_id;
  return 0;
}

static const zi_cap_v1 cap_exec_run_v1 = {
    .kind = "exec",
    .name = "run",
    .version = 1,
    .cap_flags = 0,
    .meta = NULL,
    .meta_len = 0,
};

static const zi_async_selector sel_exec_run_v1 = {
    .cap_kind = "exec",
    .cap_name = "run",
    .selector = "run.v1",
    .invoke = dummy_invoke,
    .cancel = NULL,
};

int main(void) {
  if (zingcore25_zabi_version() != ZINGCORE25_ZABI_VERSION) {
    fprintf(stderr, "zingcore25_zabi_version mismatch\n");
    return 1;
  }

  if (!zingcore25_init()) {
    fprintf(stderr, "zingcore25_init failed\n");
    return 1;
  }

  // Wrapper accessors should be live after init.
  if (!zingcore25_cap_registry()) {
    fprintf(stderr, "zingcore25_cap_registry returned NULL\n");
    return 1;
  }
  if (!zingcore25_async_registry()) {
    fprintf(stderr, "zingcore25_async_registry returned NULL\n");
    return 1;
  }

  // Demonstrate explicit registration using the low-level APIs.
  zingcore25_reset_for_test();

  if (!zi_cap_register(&cap_exec_run_v1)) {
    fprintf(stderr, "zi_cap_register failed\n");
    return 1;
  }
  if (!zi_async_register(&sel_exec_run_v1)) {
    fprintf(stderr, "zi_async_register failed\n");
    return 1;
  }

  const zi_async_selector *found = zi_async_find("exec", 4, "run", 3, "run.v1", 6);
  if (!found) {
    fprintf(stderr, "zi_async_find failed\n");
    return 1;
  }

  return 0;
}
