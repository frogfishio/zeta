#include "sir_jsonl.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>

static int fail(const char* msg) {
  fprintf(stderr, "sem_unit: %s\n", msg);
  return 1;
}

static bool write_all(FILE* f, const char* s) {
  return f && s && fputs(s, f) >= 0;
}

int main(void) {
  char sir_path[] = "/tmp/sem_hint_ptrsym_extern_XXXXXX";
  const int fd = mkstemp(sir_path);
  if (fd < 0) return fail("mkstemp failed");
  FILE* out = fdopen(fd, "wb");
  if (!out) {
    close(fd);
    unlink(sir_path);
    return fail("fdopen failed");
  }

  // Reproducer: call.indirect through ptr.sym to a name that is not declared in-module.
  // Expect: SEM diagnostic suggests using decl.fn + call.indirect for extern calls.
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"meta\",\"producer\":\"sem-unit\",\"unit\":\"hint_ptrsym_extern\"}\n")) return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":1,\"kind\":\"prim\",\"prim\":\"i32\"}\n")) return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":2,\"kind\":\"fn\",\"params\":[],\"ret\":1}\n")) return fail("write failed");

  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":10,\"tag\":\"ptr.sym\",\"fields\":{\"name\":\"hello\",\"args\":[]}}\n"))
    return fail("write failed");
  if (!write_all(out,
                 "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":11,\"tag\":\"call.indirect\",\"type_ref\":1,"
                 "\"fields\":{\"sig\":{\"t\":\"ref\",\"id\":2},\"args\":[{\"t\":\"ref\",\"id\":10}]}}\n"))
    return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":12,\"tag\":\"term.ret\",\"fields\":{\"value\":{\"t\":\"ref\",\"id\":11}}}\n"))
    return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":13,\"tag\":\"block\",\"fields\":{\"stmts\":[{\"t\":\"ref\",\"id\":12}]}}\n"))
    return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":14,\"tag\":\"fn\",\"type_ref\":2,\"fields\":{\"name\":\"zir_main\",\"params\":[],\"body\":{\"t\":\"ref\",\"id\":13}}}\n"))
    return fail("write failed");
  fclose(out);

  char diag_path[] = "/tmp/sem_hint_ptrsym_extern_diag_XXXXXX";
  const int dfd = mkstemp(diag_path);
  if (dfd < 0) {
    unlink(sir_path);
    return fail("mkstemp diag failed");
  }

  const int saved_stderr = dup(STDERR_FILENO);
  if (saved_stderr < 0) {
    close(dfd);
    unlink(diag_path);
    unlink(sir_path);
    return fail("dup stderr failed");
  }
  if (dup2(dfd, STDERR_FILENO) < 0) {
    close(saved_stderr);
    close(dfd);
    unlink(diag_path);
    unlink(sir_path);
    return fail("dup2 failed");
  }
  close(dfd);

  const int rc = sem_verify_sir_jsonl_ex(sir_path, SEM_DIAG_TEXT, false);

  fflush(stderr);
  (void)dup2(saved_stderr, STDERR_FILENO);
  close(saved_stderr);

  unlink(sir_path);

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
  const bool ok = (fread(buf, 1, sizeof(buf) - 1, f) > 0);
  fclose(f);
  unlink(diag_path);
  if (!ok) return fail("expected diagnostic output");
  buf[sizeof(buf) - 1] = '\0';

  if (strstr(buf, "decl.fn") == NULL) return fail("expected decl.fn hint in diagnostic");
  return 0;
}

