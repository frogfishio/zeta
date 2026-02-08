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
  char sir_path[] = "/tmp/sem_verify_validate_diag_json_XXXXXX";
  const int fd = mkstemp(sir_path);
  if (fd < 0) return fail("mkstemp failed");
  FILE* out = fdopen(fd, "wb");
  if (!out) {
    close(fd);
    unlink(sir_path);
    return fail("fdopen failed");
  }

  // Reproducer: decl.fn signature expects 3 args, but call.indirect has only 2 args
  // and omits `sig`, so SEM will lower to call_extern with arg_count mismatch, which
  // is caught by sircore validation (not SEM parse validation).
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"meta\",\"producer\":\"sem-unit\",\"unit\":\"bad_validate\"}\n")) return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":1,\"kind\":\"prim\",\"prim\":\"i32\"}\n")) return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":2,\"kind\":\"prim\",\"prim\":\"i64\"}\n")) return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":3,\"kind\":\"prim\",\"prim\":\"ptr\"}\n")) return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":10,\"kind\":\"fn\",\"params\":[1,2,1],\"ret\":1}\n")) return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":11,\"kind\":\"fn\",\"params\":[],\"ret\":2}\n")) return fail("write failed");

  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":100,\"tag\":\"decl.fn\",\"type_ref\":10,\"fields\":{\"name\":\"zi_write\"}}\n"))
    return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":110,\"tag\":\"const.i32\",\"type_ref\":1,\"fields\":{\"value\":1}}\n"))
    return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":111,\"tag\":\"const.i32\",\"type_ref\":1,\"fields\":{\"value\":18}}\n"))
    return fail("write failed");

  // Wrong argc=2 (missing ptr + len); omit sig so SEM does not validate it early.
  if (!write_all(out,
                 "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":120,\"tag\":\"call.indirect\",\"type_ref\":1,\"fields\":{\"args\":[{\"t\":\"ref\",\"id\":100},{\"t\":\"ref\",\"id\":110},{\"t\":\"ref\",\"id\":111}]}}\n"))
    return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":121,\"tag\":\"let\",\"fields\":{\"name\":\"_\",\"value\":{\"t\":\"ref\",\"id\":120}}}\n"))
    return fail("write failed");

  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":130,\"tag\":\"const.i64\",\"type_ref\":2,\"fields\":{\"value\":0}}\n"))
    return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":131,\"tag\":\"term.ret\",\"fields\":{\"value\":{\"t\":\"ref\",\"id\":130}}}\n"))
    return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":140,\"tag\":\"block\",\"fields\":{\"stmts\":[{\"t\":\"ref\",\"id\":121},{\"t\":\"ref\",\"id\":131}]}}\n"))
    return fail("write failed");
  if (!write_all(out, "{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":150,\"tag\":\"fn\",\"type_ref\":11,\"fields\":{\"name\":\"zir_main\",\"params\":[],\"body\":{\"t\":\"ref\",\"id\":140}}}\n"))
    return fail("write failed");

  fclose(out);

  // Capture stderr JSON diagnostic.
  char diag_path[] = "/tmp/sem_verify_validate_diag_json_out_XXXXXX";
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

  const int rc = sem_verify_sir_jsonl_ex(sir_path, SEM_DIAG_JSON, false);

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
  char line[2048];
  const bool ok = (fgets(line, sizeof(line), f) != NULL);
  fclose(f);
  unlink(diag_path);
  if (!ok) return fail("expected JSON diagnostic line");

  if (strstr(line, "\"fid\":") == NULL) return fail("missing fid field in JSON diagnostic");
  if (strstr(line, "\"ip\":") == NULL) return fail("missing ip field in JSON diagnostic");
  if (strstr(line, "\"op\":\"call.extern\"") == NULL) return fail("missing op field in JSON diagnostic");
  return 0;
}

