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
  char sir_path[] = "/tmp/sem_verify_fun_cmp_sig_mismatch_XXXXXX";
  const int fd = mkstemp(sir_path);
  if (fd < 0) return fail("mkstemp failed");
  FILE* out = fdopen(fd, "wb");
  if (!out) {
    close(fd);
    unlink(sir_path);
    return fail("fdopen failed");
  }

  // fun.cmp.eq requires operands with the same sig.
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"meta\",\"producer\":\"sem-unit\",\"unit\":\"verify_fun_cmp_sig_mismatch\"}\n"))
    return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":1,\"kind\":\"prim\",\"prim\":\"i32\"}\n")) return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":2,\"kind\":\"prim\",\"prim\":\"bool\"}\n")) return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":3,\"kind\":\"fn\",\"params\":[],\"ret\":2}\n")) return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":4,\"kind\":\"fun\",\"sig\":3}\n")) return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":5,\"kind\":\"fn\",\"params\":[],\"ret\":1}\n")) return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":6,\"kind\":\"fun\",\"sig\":5}\n")) return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":10,\"kind\":\"fn\",\"params\":[],\"ret\":1}\n")) return fail("write failed");

  // Define two functions just so fun.sym can resolve them.
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":100,\"tag\":\"const.bool\",\"type_ref\":2,\"fields\":{\"value\":1}}\n"))
    return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":101,\"tag\":\"return\",\"fields\":{\"value\":{\"t\":\"ref\",\"id\":100}}}\n"))
    return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":102,\"tag\":\"block\",\"fields\":{\"stmts\":[{\"t\":\"ref\",\"id\":101}]}}\n"))
    return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":103,\"tag\":\"fn\",\"type_ref\":3,\"fields\":{\"name\":\"foo\",\"linkage\":\"local\",\"params\":[],\"body\":{\"t\":\"ref\",\"id\":102}}}\n"))
    return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":104,\"tag\":\"fn\",\"type_ref\":5,\"fields\":{\"name\":\"bar\",\"linkage\":\"local\",\"params\":[],\"body\":{\"t\":\"ref\",\"id\":102}}}\n"))
    return fail("write failed");

  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":110,\"tag\":\"fun.sym\",\"type_ref\":4,\"fields\":{\"name\":\"foo\"}}\n"))
    return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":111,\"tag\":\"fun.sym\",\"type_ref\":6,\"fields\":{\"name\":\"bar\"}}\n"))
    return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":112,\"tag\":\"fun.cmp.eq\",\"type_ref\":2,\"fields\":{\"args\":[{\"t\":\"ref\",\"id\":110},{\"t\":\"ref\",\"id\":111}]}}\n"))
    return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":120,\"tag\":\"term.ret\",\"fields\":{\"value\":{\"t\":\"ref\",\"id\":112}}}\n"))
    return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":121,\"tag\":\"block\",\"fields\":{\"stmts\":[{\"t\":\"ref\",\"id\":120}]}}\n"))
    return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":122,\"tag\":\"fn\",\"type_ref\":10,\"fields\":{\"name\":\"main\",\"params\":[],\"body\":{\"t\":\"ref\",\"id\":121}}}\n"))
    return fail("write failed");
  fclose(out);

  char diag_path[] = "/tmp/sem_verify_fun_cmp_sig_mismatch_diag_XXXXXX";
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

  if (strstr(buf, "sig") == NULL) return fail("expected sig mismatch diagnostic");
  if (strstr(buf, "fun.cmp") == NULL) return fail("expected fun.cmp mention in diagnostic");
  return 0;
}

