#include "sir_jsonl.h"

#include <stdio.h>

static int fail(const char* msg) {
  fprintf(stderr, "sem_unit: %s\n", msg);
  return 1;
}

int main(void) {
  const int rc = sem_run_sir_jsonl(SEM_SOURCE_DIR "/src/sircc/examples/closure_make_call.sir.jsonl", NULL, 0, NULL);
  if (rc != 12) {
    fprintf(stderr, "sem_unit: expected rc=12 got rc=%d\n", rc);
    return fail("unexpected return code");
  }
  return 0;
}

