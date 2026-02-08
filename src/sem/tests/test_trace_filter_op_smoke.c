#include "sir_jsonl.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>

static int fail(const char* msg) {
  fprintf(stderr, "sem_unit: %s\n", msg);
  return 1;
}

int main(void) {
  char path[] = "/tmp/sem_trace_filter_op_XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0) return fail("mkstemp failed");
  close(fd);

  // Filter to a specific opcode that must appear in cfg_if.
  const char* op = "term.cbr";
  const int rc = sem_run_sir_jsonl_events_ex(SEM_SOURCE_DIR "/src/sircc/examples/cfg_if.sir.jsonl", NULL, 0, NULL, SEM_DIAG_TEXT, false, path,
                                             NULL, NULL, op);
  if (rc != 111) {
    fprintf(stderr, "sem_unit: expected rc=111 got rc=%d\n", rc);
    unlink(path);
    return fail("unexpected return code");
  }

  FILE* f = fopen(path, "rb");
  if (!f) {
    unlink(path);
    return fail("failed to open trace output");
  }
  char line[512];
  bool saw_step = false;
  while (fgets(line, sizeof(line), f) != NULL) {
    if (strstr(line, "\"k\":\"trace_step\"") != NULL) {
      saw_step = true;
      if (strstr(line, "\"op\":\"term.cbr\"") == NULL) {
        fclose(f);
        unlink(path);
        return fail("trace_step record did not match op filter");
      }
    }
  }
  fclose(f);
  unlink(path);
  if (!saw_step) return fail("trace output contained no trace_step records");
  return 0;
}

