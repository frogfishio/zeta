#include "sir_jsonl.h"

#include <stdio.h>

static int fail(const char* msg) {
  fprintf(stderr, "sem_unit: %s\n", msg);
  return 1;
}

int main(void) {
  const int rc = sem_verify_sir_jsonl(SEM_SOURCE_DIR "/src/sircc/examples/bad_ptr_offset_void.sir.jsonl", SEM_DIAG_TEXT);
  if (rc != 1) {
    fprintf(stderr, "sem_unit: expected rc=1 got rc=%d\n", rc);
    return fail("unexpected return code");
  }
  return 0;
}
