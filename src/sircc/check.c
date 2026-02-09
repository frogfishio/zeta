// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "check.h"

#include "json.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum {
  CHECK_VERIFY = 0,
  CHECK_RUN,
  CHECK_VERIFY_FAIL,
} CheckKind;

typedef struct {
  const char* name;
  const char* file;
  CheckKind kind;
  int expect_exit;
  const char* expect_code; // for CHECK_VERIFY_FAIL (JSON diagnostics)
} CheckCase;

static bool path_join(char* out, size_t out_cap, const char* a, const char* b) {
  if (!out || !out_cap || !a || !b) return false;
  size_t alen = strlen(a);
  const char* sep = (alen > 0 && a[alen - 1] == '/') ? "" : "/";
  int n = snprintf(out, out_cap, "%s%s%s", a, sep, b);
  return n > 0 && (size_t)n < out_cap;
}

static bool is_dir(const char* p) {
  struct stat st;
  if (!p) return false;
  if (stat(p, &st) != 0) return false;
  return S_ISDIR(st.st_mode);
}

static int run_exe(const char* exe_path) {
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    // Keep check output concise by default (examples may print to stdout).
    FILE* devnull = fopen("/dev/null", "wb");
    if (devnull) {
      int fd = fileno(devnull);
      if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
      }
      fclose(devnull);
    }
    char* const argv[] = {(char*)exe_path, NULL};
    execv(exe_path, argv);
    _exit(127);
  }
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) return -1;
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  return 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

static bool infer_dist_root_from_argv0(const char* argv0, char* out, size_t out_cap) {
  if (!argv0 || !out || !out_cap) return false;
  const char* p = strstr(argv0, "/bin/");
  if (!p) return false;
  size_t prefix = (size_t)(p - argv0);
  if (prefix >= out_cap) return false;
  memcpy(out, argv0, prefix);
  out[prefix] = 0;
  if (out[0] == 0) snprintf(out, out_cap, "%s", ".");
  return true;
}

static bool resolve_examples_dir(const SirccCheckOptions* chk, char* out, size_t out_cap) {
  if (!chk || !out || !out_cap) return false;

  if (chk->examples_dir) {
    if (!is_dir(chk->examples_dir)) return false;
    snprintf(out, out_cap, "%s", chk->examples_dir);
    return true;
  }

  if (chk->dist_root) {
    char tmp[PATH_MAX];
    if (!path_join(tmp, sizeof(tmp), chk->dist_root, "test/examples")) return false;
    if (!is_dir(tmp)) return false;
    snprintf(out, out_cap, "%s", tmp);
    return true;
  }

  // If run from within a dist/ folder: ./bin/<os>/sircc
  if (chk->argv0) {
    char dist_root[PATH_MAX];
    if (infer_dist_root_from_argv0(chk->argv0, dist_root, sizeof(dist_root))) {
      char tmp[PATH_MAX];
      if (path_join(tmp, sizeof(tmp), dist_root, "test/examples") && is_dir(tmp)) {
        snprintf(out, out_cap, "%s", tmp);
        return true;
      }
    }
  }

  // If invoked from inside dist/bin/<os>, these common relative paths work:
  if (is_dir("../../test/examples")) {
    snprintf(out, out_cap, "%s", "../../test/examples");
    return true;
  }
  if (is_dir("../test/examples")) {
    snprintf(out, out_cap, "%s", "../test/examples");
    return true;
  }
  if (is_dir("test/examples")) {
    snprintf(out, out_cap, "%s", "test/examples");
    return true;
  }

  // Try cwd: ./dist/test/examples
  if (is_dir("dist/test/examples")) {
    snprintf(out, out_cap, "%s", "dist/test/examples");
    return true;
  }

  // Fallback for dev/build trees: src/sircc/examples
  if (is_dir("src/sircc/examples")) {
    snprintf(out, out_cap, "%s", "src/sircc/examples");
    return true;
  }

  return false;
}

static bool mk_tmpdir(char* out_dir, size_t out_cap) {
  if (!out_dir || out_cap < 16) return false;
  const char* base = getenv("TMPDIR");
  if (!base || !*base) base = "/tmp";
  char tmpl[PATH_MAX];
  if (!path_join(tmpl, sizeof(tmpl), base, "sircc-check.XXXXXX")) return false;
  if (!path_join(out_dir, out_cap, tmpl, "")) return false; // normalize copy

  // mkdtemp needs a writable buffer.
  char buf[PATH_MAX];
  snprintf(buf, sizeof(buf), "%s", tmpl);
  char* made = mkdtemp(buf);
  if (!made) return false;
  snprintf(out_dir, out_cap, "%s", made);
  return true;
}

static bool extract_json_string_field(const char* line, const char* key, char* out, size_t out_cap) {
  if (!line || !key || !out || !out_cap) return false;
  out[0] = 0;
  char pat[64];
  int n = snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  if (n <= 0 || (size_t)n >= sizeof(pat)) return false;
  const char* p = strstr(line, pat);
  if (!p) return false;
  p += (size_t)n;
  // Codes are expected to be simple ASCII without escapes; parse until next quote.
  const char* end = strchr(p, '"');
  if (!end) return false;
  size_t len = (size_t)(end - p);
  if (len + 1 > out_cap) return false;
  memcpy(out, p, len);
  out[len] = 0;
  return true;
}

static bool extract_first_diag_code_from_stderr(const char* buf, char* out_code, size_t out_cap) {
  if (!buf || !out_code || !out_cap) return false;
  out_code[0] = 0;
  const char* p = buf;
  while (*p) {
    const char* eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    if (len > 0) {
      // Look for a diag record line (sircc emits compact JSON without whitespace).
      if (strstr(p, "\"k\":\"diag\"")) {
        char code[128];
        if (extract_json_string_field(p, "code", code, sizeof(code))) {
          snprintf(out_code, out_cap, "%s", code);
          return true;
        }
      }
    }
    if (!eol) break;
    p = eol + 1;
  }
  return false;
}

static bool capture_sircc_compile_stderr(const SirccOptions* opt, int* out_rc, char** out_buf) {
  if (!opt || !out_rc || !out_buf) return false;
  *out_rc = -1;
  *out_buf = NULL;

  int pipefd[2] = {-1, -1};
  if (pipe(pipefd) != 0) return false;

  int saved = dup(STDERR_FILENO);
  if (saved < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return false;
  }

  if (dup2(pipefd[1], STDERR_FILENO) < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    close(saved);
    return false;
  }
  close(pipefd[1]);

  int rc = sircc_compile(opt);

  // Restore stderr before reading to avoid confusing output if the read blocks.
  (void)dup2(saved, STDERR_FILENO);
  close(saved);

  // Read captured stderr (bounded).
  size_t cap = 64 * 1024;
  size_t len = 0;
  char* buf = (char*)malloc(cap + 1);
  if (!buf) {
    close(pipefd[0]);
    *out_rc = rc;
    return true;
  }
  for (;;) {
    ssize_t r = read(pipefd[0], buf + len, cap - len);
    if (r <= 0) break;
    len += (size_t)r;
    if (len == cap) break;
  }
  close(pipefd[0]);
  buf[len] = 0;

  *out_rc = rc;
  *out_buf = buf;
  return true;
}

int sircc_run_check(FILE* out, const SirccOptions* base_opt, const SirccCheckOptions* chk) {
  if (!out || !base_opt || !chk) return SIRCC_EXIT_USAGE;

  char examples[PATH_MAX];
  if (!resolve_examples_dir(chk, examples, sizeof(examples))) {
    fprintf(stderr, "sircc: --check: could not find examples dir (try --dist-root ./dist or --examples-dir ...)\n");
    return SIRCC_EXIT_USAGE;
  }

  // Suite is designed to match dist/test/examples and remain small.
  const CheckCase suite[] = {
      {.name = "add", .file = "add.sir.jsonl", .kind = CHECK_VERIFY, .expect_exit = 0},
      {.name = "call_indirect_ptrsym", .file = "call_indirect_ptrsym.sir.jsonl", .kind = CHECK_VERIFY, .expect_exit = 0},
      {.name = "ptr_layout", .file = "ptr_layout.sir.jsonl", .kind = CHECK_VERIFY, .expect_exit = 0},
      {.name = "misaligned_load_traps", .file = "misaligned_load_traps.sir.jsonl", .kind = CHECK_VERIFY, .expect_exit = 0},
      {.name = "atomic_basic_i32", .file = "atomic_basic_i32.sir.jsonl", .kind = CHECK_RUN, .expect_exit = 124},
      {.name = "mem_copy_fill", .file = "mem_copy_fill.sir.jsonl", .kind = CHECK_RUN, .expect_exit = 42},
      {.name = "cfg_if", .file = "cfg_if.sir.jsonl", .kind = CHECK_RUN, .expect_exit = 222},
      {.name = "cfg_switch", .file = "cfg_switch.sir.jsonl", .kind = CHECK_RUN, .expect_exit = 20},
      {.name = "hello_world_puts", .file = "hello_world_puts.sir.jsonl", .kind = CHECK_RUN, .expect_exit = 0},
      {.name = "simd_splat_extract", .file = "simd_splat_extract.sir.jsonl", .kind = CHECK_RUN, .expect_exit = 7},
      {.name = "simd_i32_add_extract_replace", .file = "simd_i32_add_extract_replace.sir.jsonl", .kind = CHECK_RUN, .expect_exit = 9},
      {.name = "simd_cmp_select_bool_mask", .file = "simd_cmp_select_bool_mask.sir.jsonl", .kind = CHECK_RUN, .expect_exit = 7},
      {.name = "simd_shuffle_two_inputs", .file = "simd_shuffle_two_inputs.sir.jsonl", .kind = CHECK_RUN, .expect_exit = 6},
      {.name = "simd_f32_mul_nan_canon_bits", .file = "simd_f32_mul_nan_canon_bits.sir.jsonl", .kind = CHECK_RUN, .expect_exit = 7},
      {.name = "fun_sym_call", .file = "fun_sym_call.sir.jsonl", .kind = CHECK_RUN, .expect_exit = 7},
      {.name = "closure_make_call", .file = "closure_make_call.sir.jsonl", .kind = CHECK_RUN, .expect_exit = 12},
      {.name = "adt_make_get", .file = "adt_make_get.sir.jsonl", .kind = CHECK_RUN, .expect_exit = 12},
      {.name = "sem_if_thunk_trap_not_taken", .file = "sem_if_thunk_trap_not_taken.sir.jsonl", .kind = CHECK_RUN, .expect_exit = 7},
      {.name = "sem_match_sum_option_i32", .file = "sem_match_sum_option_i32.sir.jsonl", .kind = CHECK_RUN, .expect_exit = 12},

      // Negative fixtures (verify-only): ensure stable diagnostic codes for integrators.
      {.name = "bad_unknown_field", .file = "bad_unknown_field.sir.jsonl", .kind = CHECK_VERIFY_FAIL, .expect_exit = 0, .expect_code = "sircc.schema.unknown_field"},
      {.name = "bad_instr_operand", .file = "bad_instr_operand.sir.jsonl", .kind = CHECK_VERIFY_FAIL, .expect_exit = 0, .expect_code = "sircc.schema.value.num.bad"},
      {.name = "bad_feature_gate_atomic", .file = "bad_feature_gate_atomic.sir.jsonl", .kind = CHECK_VERIFY_FAIL, .expect_exit = 0, .expect_code = "sircc.feature.gate"},
      {.name = "cfg_bad_early_term", .file = "cfg_bad_early_term.sir.jsonl", .kind = CHECK_VERIFY_FAIL, .expect_exit = 0, .expect_code = "sircc.cfg.block.term.not_last"},
  };

  char tmpdir[PATH_MAX];
  if (!mk_tmpdir(tmpdir, sizeof(tmpdir))) {
    fprintf(stderr, "sircc: --check: failed to create tmp dir: %s\n", strerror(errno));
    return SIRCC_EXIT_INTERNAL;
  }

  bool ok_all = true;
  size_t passed = 0;
  size_t total = sizeof(suite) / sizeof(suite[0]);

  if (chk->format == SIRCC_CHECK_JSON) {
    fprintf(out, "{");
    fprintf(out, "\"tool\":\"sircc\",\"k\":\"check\"");
    fprintf(out, ",\"examples_dir\":");
    json_write_escaped(out, examples);
    fprintf(out, ",\"tmp_dir\":");
    json_write_escaped(out, tmpdir);
    fprintf(out, ",\"tests\":[");
  } else {
    fprintf(out, "sircc --check\n");
    fprintf(out, "  examples: %s\n", examples);
  }

  for (size_t i = 0; i < total; i++) {
    const CheckCase* tc = &suite[i];

    char input[PATH_MAX];
    if (!path_join(input, sizeof(input), examples, tc->file)) {
      ok_all = false;
      if (chk->format == SIRCC_CHECK_JSON) {
        if (i) fprintf(out, ",");
        fprintf(out, "{\"name\":");
        json_write_escaped(out, tc->name);
        fprintf(out, ",\"ok\":false,\"error\":\"bad path\"}");
      } else {
        fprintf(out, "  FAIL %s: bad path\n", tc->name);
      }
      continue;
    }

    SirccOptions opt = *base_opt;
    opt.input_path = input;
    opt.dump_records = false;
    // Self-check suite is currently libc-based (dist/test/examples). Don't
    // inherit a caller's custom runtime/link configuration.
    opt.runtime = SIRCC_RUNTIME_LIBC;
    opt.zabi25_root = NULL;

    char exe[PATH_MAX];
    if (tc->kind == CHECK_VERIFY || tc->kind == CHECK_VERIFY_FAIL) {
      opt.verify_only = true;
      opt.output_path = NULL;
    } else {
      opt.verify_only = false;
      if (!path_join(exe, sizeof(exe), tmpdir, tc->name)) {
        ok_all = false;
        if (chk->format == SIRCC_CHECK_JSON) {
          if (i) fprintf(out, ",");
          fprintf(out, "{\"name\":");
          json_write_escaped(out, tc->name);
          fprintf(out, ",\"ok\":false,\"error\":\"bad exe path\"}");
        } else {
          fprintf(out, "  FAIL %s: bad exe path\n", tc->name);
        }
        continue;
      }
      opt.output_path = exe;
      opt.emit = SIRCC_EMIT_EXE;
      opt.strip = false;
    }

    int rc = 0;
    bool ok = false;
    char diag_code[128];
    diag_code[0] = 0;

    if (tc->kind == CHECK_VERIFY_FAIL) {
      opt.diagnostics = SIRCC_DIAG_JSON;
      opt.color = SIRCC_COLOR_NEVER;
      opt.diag_context = 0;

      char* errbuf = NULL;
      if (!capture_sircc_compile_stderr(&opt, &rc, &errbuf)) {
        ok = false;
      } else {
        bool want_fail = (rc != 0);
        bool have_code = errbuf && extract_first_diag_code_from_stderr(errbuf, diag_code, sizeof(diag_code));
        ok = want_fail && have_code && tc->expect_code && strcmp(diag_code, tc->expect_code) == 0;
      }
      free(errbuf);
    } else {
      rc = sircc_compile(&opt);
      ok = (rc == 0);
    }

    int run_rc = 0;
    if (ok && tc->kind == CHECK_RUN) {
      run_rc = run_exe(exe);
      ok = (run_rc == tc->expect_exit);
    }

    if (ok) passed++;
    else ok_all = false;

    if (chk->format == SIRCC_CHECK_JSON) {
      if (i) fprintf(out, ",");
      fprintf(out, "{");
      fprintf(out, "\"name\":");
      json_write_escaped(out, tc->name);
      fprintf(out, ",\"file\":");
      json_write_escaped(out, tc->file);
      fprintf(out, ",\"kind\":");
      json_write_escaped(out, (tc->kind == CHECK_VERIFY) ? "verify" : (tc->kind == CHECK_VERIFY_FAIL) ? "verify_fail" : "run");
      fprintf(out, ",\"ok\":%s", ok ? "true" : "false");
      fprintf(out, ",\"compile_rc\":%d", rc);
      if (tc->kind == CHECK_VERIFY_FAIL) {
        fprintf(out, ",\"expect_code\":");
        json_write_escaped(out, tc->expect_code ? tc->expect_code : "");
        fprintf(out, ",\"code\":");
        json_write_escaped(out, diag_code);
      }
      if (tc->kind == CHECK_RUN) {
        fprintf(out, ",\"expect_exit\":%d", tc->expect_exit);
        fprintf(out, ",\"exit\":%d", run_rc);
      }
      fprintf(out, "}");
    } else {
      if (tc->kind == CHECK_VERIFY) {
        fprintf(out, "  %s %s\n", ok ? "OK  " : "FAIL", tc->name);
      } else if (tc->kind == CHECK_VERIFY_FAIL) {
        fprintf(out, "  %s %s (code %s, expect %s)\n", ok ? "OK  " : "FAIL", tc->name, diag_code[0] ? diag_code : "(none)",
                tc->expect_code ? tc->expect_code : "(none)");
      } else {
        fprintf(out, "  %s %s (exit %d, expect %d)\n", ok ? "OK  " : "FAIL", tc->name, run_rc, tc->expect_exit);
      }
    }
  }

  if (chk->format == SIRCC_CHECK_JSON) {
    fprintf(out, "]");
    fprintf(out, ",\"passed\":%zu,\"total\":%zu,\"ok\":%s", passed, total, ok_all ? "true" : "false");
    fprintf(out, "}\n");
  } else {
    fprintf(out, "  result: %zu/%zu %s\n", passed, total, ok_all ? "OK" : "FAIL");
  }

  // best-effort cleanup
  for (size_t i = 0; i < total; i++) {
    if (suite[i].kind != CHECK_RUN) continue;
    char exe[PATH_MAX];
    if (path_join(exe, sizeof(exe), tmpdir, suite[i].name)) unlink(exe);
  }
  rmdir(tmpdir);

  return ok_all ? 0 : SIRCC_EXIT_ERROR;
}
