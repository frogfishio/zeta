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
} CheckKind;

typedef struct {
  const char* name;
  const char* file;
  CheckKind kind;
  int expect_exit;
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
      {.name = "mem_copy_fill", .file = "mem_copy_fill.sir.jsonl", .kind = CHECK_RUN, .expect_exit = 42},
      {.name = "cfg_if", .file = "cfg_if.sir.jsonl", .kind = CHECK_RUN, .expect_exit = 222},
      {.name = "cfg_switch", .file = "cfg_switch.sir.jsonl", .kind = CHECK_RUN, .expect_exit = 20},
      {.name = "hello_world_puts", .file = "hello_world_puts.sir.jsonl", .kind = CHECK_RUN, .expect_exit = 0},
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
    if (tc->kind == CHECK_VERIFY) {
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

    int rc = sircc_compile(&opt);
    bool ok = (rc == 0);
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
      json_write_escaped(out, (tc->kind == CHECK_VERIFY) ? "verify" : "run");
      fprintf(out, ",\"ok\":%s", ok ? "true" : "false");
      fprintf(out, ",\"compile_rc\":%d", rc);
      if (tc->kind == CHECK_RUN) {
        fprintf(out, ",\"expect_exit\":%d", tc->expect_exit);
        fprintf(out, ",\"exit\":%d", run_rc);
      }
      fprintf(out, "}");
    } else {
      if (tc->kind == CHECK_VERIFY) {
        fprintf(out, "  %s %s\n", ok ? "OK  " : "FAIL", tc->name);
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
