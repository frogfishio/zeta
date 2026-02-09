// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SEM_SOURCE_DIR
#define SEM_SOURCE_DIR "."
#endif

#ifndef SEM_EXE_PATH
#define SEM_EXE_PATH "sem"
#endif

#ifndef SIRCC_EXE_PATH
#define SIRCC_EXE_PATH "sircc"
#endif

static int run_cmd(const char* cmd, int* out_exit_code) {
  if (!cmd || !out_exit_code) return -1;
  *out_exit_code = -1;
  const int st = system(cmd);
  if (st == -1) return -1;
  if (WIFEXITED(st)) {
    *out_exit_code = WEXITSTATUS(st);
    return 0;
  }
  if (WIFSIGNALED(st)) {
    *out_exit_code = 128 + WTERMSIG(st);
    return 0;
  }
  return -1;
}

static int ensure_dir(const char* path) {
  if (!path || !*path) return -1;
  if (mkdir(path, 0700) == 0) return 0;
  if (errno == EEXIST) return 0;
  return -1;
}

static const char* path_basename(const char* path) {
  if (!path) return "";
  const char* last = strrchr(path, '/');
  return last ? (last + 1) : path;
}

static void basename_no_ext(const char* filename, char* out, size_t out_cap) {
  if (!out || out_cap == 0) return;
  out[0] = 0;
  if (!filename) return;
  const char* base = path_basename(filename);
  const char* dot = strrchr(base, '.');
  const size_t n = dot ? (size_t)(dot - base) : strlen(base);
  const size_t m = (n < (out_cap - 1)) ? n : (out_cap - 1);
  memcpy(out, base, m);
  out[m] = 0;
}

static int diff_one(const char* fixture_rel, const char* out_dir) {
  char fixture[PATH_MAX];
  if (snprintf(fixture, sizeof(fixture), "%s/%s", SEM_SOURCE_DIR, fixture_rel) >= (int)sizeof(fixture)) {
    fprintf(stderr, "fixture path too long\n");
    return 2;
  }

  char base[128];
  basename_no_ext(fixture_rel, base, sizeof(base));

  char exe_path[PATH_MAX];
  if (snprintf(exe_path, sizeof(exe_path), "%s/%s.bin", out_dir, base) >= (int)sizeof(exe_path)) {
    fprintf(stderr, "exe path too long\n");
    return 2;
  }

  int sem_rc = -1;
  {
    char cmd[PATH_MAX * 2];
    if (snprintf(cmd, sizeof(cmd), "\"%s\" --run \"%s\" > /dev/null", SEM_EXE_PATH, fixture) >= (int)sizeof(cmd)) {
      fprintf(stderr, "sem cmd too long\n");
      return 2;
    }
    if (run_cmd(cmd, &sem_rc) != 0) {
      fprintf(stderr, "failed to run sem\n");
      return 2;
    }
  }

  int sircc_rc = -1;
  {
    char cmd[PATH_MAX * 3];
    if (snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" -o \"%s\" > /dev/null", SIRCC_EXE_PATH, fixture, exe_path) >= (int)sizeof(cmd)) {
      fprintf(stderr, "sircc cmd too long\n");
      return 2;
    }
    if (run_cmd(cmd, &sircc_rc) != 0) {
      fprintf(stderr, "failed to run sircc\n");
      return 2;
    }
    if (sircc_rc != 0) {
      fprintf(stderr, "sircc failed for %s with exit code %d\n", fixture_rel, sircc_rc);
      return 1;
    }
  }

  int exe_rc = -1;
  {
    char cmd[PATH_MAX * 2];
    if (snprintf(cmd, sizeof(cmd), "\"%s\" > /dev/null", exe_path) >= (int)sizeof(cmd)) {
      fprintf(stderr, "exe cmd too long\n");
      return 2;
    }
    if (run_cmd(cmd, &exe_rc) != 0) {
      fprintf(stderr, "failed to run compiled binary\n");
      return 2;
    }
  }

  if (sem_rc != exe_rc) {
    fprintf(stderr, "sem vs sircc mismatch for %s: sem=%d exe=%d\n", fixture_rel, sem_rc, exe_rc);
    return 1;
  }

  return 0;
}

int main(void) {
  // Keep this suite small and deterministic: no file IO, no zi_* calls, no argv/env dependencies.
  // Add fixtures here once they pass sem and sircc deterministically.
  const char* fixtures[] = {
      "src/sircc/examples/atomic_basic_i32.sir.jsonl",
      "src/sircc/examples/mem_copy_fill.sir.jsonl",
      "src/sircc/examples/float_load_canon.sir.jsonl",
      "src/sircc/examples/sem_if_val_to_select.sir.jsonl",
      "src/sircc/examples/sem_and_sc_thunk_trap_not_taken.sir.jsonl",
      "src/sircc/examples/sem_or_sc_thunk_trap_not_taken.sir.jsonl",
      "src/sircc/examples/sem_switch_thunk_trap_not_taken.sir.jsonl",
      "src/sircc/examples/sem_scope_defer_runs_on_fallthrough.sir.jsonl",
      "src/sircc/examples/sem_while_global_counter.sir.jsonl",
      "src/sem/tests/fixtures/i16_store_load_zext.sir.jsonl",
  };

  const char* out_dir = "out_sem_diff_suite";
  if (ensure_dir(out_dir) != 0) {
    perror("mkdir out_sem_diff_suite");
    return 2;
  }

  for (size_t i = 0; i < (sizeof(fixtures) / sizeof(fixtures[0])); i++) {
    const int rc = diff_one(fixtures[i], out_dir);
    if (rc != 0) return rc;
  }

  return 0;
}
