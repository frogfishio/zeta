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
  char diag_path[] = "/tmp/sem_verify_bad_closure_make_code_sig_mismatch_diag_XXXXXX";
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

  const int rc = sem_verify_sir_jsonl_ex(SEM_SOURCE_DIR "/src/sircc/examples/bad_closure_make_code_sig_mismatch.sir.jsonl", SEM_DIAG_TEXT, false);

  fflush(stderr);
  (void)dup2(saved_stderr, STDERR_FILENO);
  close(saved_stderr);

  if (rc == 0) {
    unlink(diag_path);
    return fail("expected verify to fail");
  }

  FILE* f = fopen(diag_path, "rb");
  if (!f) {
    unlink(diag_path);
    return fail("failed to open diag output");
  }
  char buf[4096];
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  unlink(diag_path);
  if (n == 0) return fail("expected diagnostic output");
  buf[n] = '\0';

  if (strstr(buf, "closure.make") == NULL && strstr(buf, "closure") == NULL) return fail("expected closure diagnostic");
  if (strstr(buf, "sig") == NULL && strstr(buf, "signature") == NULL) return fail("expected sig mismatch diagnostic");
  return 0;
}

