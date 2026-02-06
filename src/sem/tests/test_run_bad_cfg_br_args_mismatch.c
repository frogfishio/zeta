#include "sir_jsonl.h"

#include <stdio.h>

static int fail(const char* msg) {
  fprintf(stderr, "sem_unit: %s\n", msg);
  return 1;
}

int main(void) {
  const int rc =
      sem_run_sir_jsonl_ex(SEM_SOURCE_DIR "/src/sircc/examples/bad_cfg_br_args_mismatch.sir.jsonl", NULL, 0, NULL, SEM_DIAG_JSON, true);
  if (rc != 1) {
    fprintf(stderr, "sem_unit: expected rc=1 got rc=%d\n", rc);
    return fail("unexpected return code");
  }
  return 0;
}
