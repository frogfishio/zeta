#include "sir_jsonl.h"

#include <stdio.h>

static int fail(const char* msg) {
  fprintf(stderr, "sem_unit: %s\n", msg);
  return 1;
}

int main(void) {
  const int rc = sem_run_sir_jsonl(SEM_SOURCE_DIR "/src/sircc/examples/sem_scope_defer_runs_on_fallthrough.sir.jsonl", NULL, 0, NULL);
  if (rc != 42) {
    fprintf(stderr, "sem_unit: expected rc=42 got rc=%d\n", rc);
    return fail("unexpected return code");
  }
  return 0;
}

