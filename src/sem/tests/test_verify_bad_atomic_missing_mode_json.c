#include "sir_jsonl.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>

static int fail(const char* msg) {
  fprintf(stderr, "sem_unit: %s\n", msg);
  return 1;
}

static bool file_read_line(const char* path, char* buf, size_t cap) {
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  const bool ok = (fgets(buf, cap, f) != NULL);
  fclose(f);
  return ok;
}

int main(void) {
  const char* input = SEM_SOURCE_DIR "/src/sem/tests/fixtures/bad_atomic_missing_mode.sir.jsonl";

  char diag_path[] = "/tmp/sem_verify_bad_atomic_missing_mode_json_XXXXXX";
  const int dfd = mkstemp(diag_path);
  if (dfd < 0) return fail("mkstemp diag failed");

  const int saved_stderr = dup(STDERR_FILENO);
  if (saved_stderr < 0) {
    close(dfd);
    unlink(diag_path);
    return fail("dup stderr failed");
  }
  if (dup2(dfd, STDERR_FILENO) < 0) {
    close(saved_stderr);
    close(dfd);
    unlink(diag_path);
    return fail("dup2 failed");
  }
  close(dfd);

  const int rc = sem_verify_sir_jsonl_ex(input, SEM_DIAG_JSON, true);

  fflush(stderr);
  (void)dup2(saved_stderr, STDERR_FILENO);
  close(saved_stderr);

  if (rc != 1) {
    unlink(diag_path);
    fprintf(stderr, "sem_unit: expected rc=1 got rc=%d\n", rc);
    return fail("unexpected return code");
  }

  char line[4096];
  if (!file_read_line(diag_path, line, sizeof(line))) {
    unlink(diag_path);
    return fail("expected JSON diagnostic line");
  }
  unlink(diag_path);

  if (strstr(line, "\"tool\":\"sem\"") == NULL) return fail("expected tool=sem");
  if (strstr(line, "\"code\":\"sem.parse.atomic.mode\"") == NULL) return fail("expected sem.parse.atomic.mode diagnostic");
  if (strstr(line, "\"path\":") == NULL) return fail("expected path field");
  if (strstr(line, "\"line\":") == NULL) return fail("expected line field");
  if (strstr(line, "\"node\":") == NULL) return fail("expected node field");
  return 0;
}

