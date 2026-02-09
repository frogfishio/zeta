// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <limits.h>
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

int main(void) {
  const char* fixture_rel = "src/sem/tests/fixtures/atomic_basic_i32.sir.jsonl";

  char fixture[PATH_MAX];
  if (snprintf(fixture, sizeof(fixture), "%s/%s", SEM_SOURCE_DIR, fixture_rel) >= (int)sizeof(fixture)) {
    fprintf(stderr, "fixture path too long\n");
    return 2;
  }

  // Create output dir in the current working directory (CTest runs in a build dir).
  const char* out_dir = "out_sem_diff";
  if (ensure_dir(out_dir) != 0) {
    perror("mkdir out_sem_diff");
    return 2;
  }

  char exe_path[PATH_MAX];
  if (snprintf(exe_path, sizeof(exe_path), "%s/%s", out_dir, "atomic_basic_i32.bin") >= (int)sizeof(exe_path)) {
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
      fprintf(stderr, "sircc failed with exit code %d\n", sircc_rc);
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
    fprintf(stderr, "sem vs sircc mismatch: sem=%d exe=%d\n", sem_rc, exe_rc);
    return 1;
  }

  return 0;
}

