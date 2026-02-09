#include "sir_jsonl.h"

#include <stdio.h>

static int fail(const char* msg) {
  fprintf(stderr, "sem_unit: %s\n", msg);
  return 1;
}

int main(void) {
  const int rc = sem_run_sir_jsonl(SEM_SOURCE_DIR "/src/sircc/examples/simd_i32_add_extract_replace.sir.jsonl", NULL, 0, NULL);
  if (rc != 9) {
    fprintf(stderr, "sem_unit: expected rc=9 got rc=%d\n", rc);
    return fail("unexpected return code");
  }
  return 0;
}

