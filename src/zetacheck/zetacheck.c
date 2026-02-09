// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SIR_VERSION
#define SIR_VERSION "0.0.0"
#endif

static void die(const char* msg) {
  fprintf(stderr, "zetacheck: %s\n", msg);
  exit(2);
}

static void usage(FILE* out) {
  fprintf(out,
          "zetacheck — dist bundle self-check runner\n"
          "\n"
          "Usage:\n"
          "  zetacheck [--json] [--verbose]\n"
          "\n"
          "Runs:\n"
          "  - sircc --check\n"
          "  - sem   --check --check-run <dist>/test/sem/run\n"
          "\n"
          "Options:\n"
          "  --help, -h  Show this help message\n"
          "  --version   Show version information\n"
          "  --json      Emit a single JSON summary to stdout\n"
          "  --verbose   Print invoked commands\n"
          "\n"
          "License: GPLv3+\n"
          "© 2026 Frogfish — Author: Alexander Croft\n");
}

static void version(FILE* out) { fprintf(out, "zetacheck %s\n", SIR_VERSION); }

static bool path_dirname_inplace(char* p) {
  if (!p || !*p) return false;
  size_t n = strlen(p);
  while (n && p[n - 1] == '/') p[--n] = 0;
  if (!n) return false;
  char* slash = strrchr(p, '/');
  if (!slash) return false;
  if (slash == p) {
    p[1] = 0;
    return true;
  }
  *slash = 0;
  return true;
}

static bool get_exe_dir(const char* argv0, char out[PATH_MAX]) {
  if (!argv0 || !*argv0) return false;

  char tmp[PATH_MAX];
  tmp[0] = 0;

  if (strchr(argv0, '/')) {
    if (!realpath(argv0, tmp)) {
      // Best-effort fallback; still lets relative invocation work.
      if (snprintf(tmp, sizeof(tmp), "%s", argv0) >= (int)sizeof(tmp)) return false;
    }
  } else {
    // argv0 has no '/', so try resolving relative to cwd.
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return false;
    char cand[PATH_MAX];
    if (snprintf(cand, sizeof(cand), "%s/%s", cwd, argv0) >= (int)sizeof(cand)) return false;
    if (!realpath(cand, tmp)) {
      if (snprintf(tmp, sizeof(tmp), "%s", cand) >= (int)sizeof(tmp)) return false;
    }
  }

  if (!path_dirname_inplace(tmp)) return false;
  if (snprintf(out, PATH_MAX, "%s", tmp) >= (int)PATH_MAX) return false;
  return true;
}

static int run_child(char* const argv[], bool verbose, bool quiet) {
  if (!argv || !argv[0]) return 2;
  if (verbose) {
    fprintf(stderr, "zetacheck: run:");
    for (size_t i = 0; argv[i]; i++) fprintf(stderr, " %s", argv[i]);
    fprintf(stderr, "\n");
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 2;
  }
  if (pid == 0) {
    if (quiet) {
      FILE* devnull = fopen("/dev/null", "w");
      if (devnull) {
        dup2(fileno(devnull), STDOUT_FILENO);
        dup2(fileno(devnull), STDERR_FILENO);
      }
    }
    execv(argv[0], argv);
    perror("execv");
    _exit(127);
  }

  int st = 0;
  if (waitpid(pid, &st, 0) < 0) {
    perror("waitpid");
    return 2;
  }
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
  return 2;
}

int main(int argc, char** argv) {
  bool json = false;
  bool verbose = false;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage(stdout);
      return 0;
    }
    if (strcmp(argv[i], "--version") == 0) {
      version(stdout);
      return 0;
    }
    if (strcmp(argv[i], "--json") == 0) {
      json = true;
      continue;
    }
    if (strcmp(argv[i], "--verbose") == 0) {
      verbose = true;
      continue;
    }
    fprintf(stderr, "zetacheck: unknown option: %s\n", argv[i]);
    usage(stderr);
    return 2;
  }

  char exe_dir[PATH_MAX];
  if (!get_exe_dir(argv[0], exe_dir)) die("failed to resolve executable directory");

  char sircc_path[PATH_MAX];
  char sem_path[PATH_MAX];
  if (snprintf(sircc_path, sizeof(sircc_path), "%s/sircc", exe_dir) >= (int)sizeof(sircc_path)) die("sircc path too long");
  if (snprintf(sem_path, sizeof(sem_path), "%s/sem", exe_dir) >= (int)sizeof(sem_path)) die("sem path too long");

  if (access(sircc_path, X_OK) != 0) {
    fprintf(stderr, "zetacheck: missing sircc executable: %s\n", sircc_path);
    return 2;
  }
  if (access(sem_path, X_OK) != 0) {
    fprintf(stderr, "zetacheck: missing sem executable: %s\n", sem_path);
    return 2;
  }

  char dist_root[PATH_MAX];
  if (snprintf(dist_root, sizeof(dist_root), "%s", exe_dir) >= (int)sizeof(dist_root)) die("dist root path too long");
  if (!path_dirname_inplace(dist_root)) die("failed to compute dist root");
  if (!path_dirname_inplace(dist_root)) die("failed to compute dist root");

  char sem_run_dir[PATH_MAX];
  if (snprintf(sem_run_dir, sizeof(sem_run_dir), "%s/test/sem/run", dist_root) >= (int)sizeof(sem_run_dir)) die("sem run dir path too long");

  const bool quiet = json && !verbose;

  int sircc_rc = 2;
  {
    char* const a[] = {sircc_path, (char*)"--check", NULL};
    sircc_rc = run_child(a, verbose, quiet);
  }

  int sem_rc = 2;
  {
    char* const a[] = {sem_path, (char*)"--check", (char*)"--check-run", sem_run_dir, NULL};
    sem_rc = run_child(a, verbose, quiet);
  }

  const int ok = (sircc_rc == 0 && sem_rc == 0);

  if (json) {
    printf("{\"k\":\"zetacheck\",\"version\":\"%s\",\"sircc\":{\"rc\":%d},\"sem\":{\"rc\":%d},\"ok\":%s}\n", SIR_VERSION, sircc_rc, sem_rc,
           ok ? "true" : "false");
  } else {
    fprintf(stderr, "sircc --check: %s (rc=%d)\n", sircc_rc == 0 ? "OK" : "FAIL", sircc_rc);
    fprintf(stderr, "sem   --check --check-run: %s (rc=%d)\n", sem_rc == 0 ? "OK" : "FAIL", sem_rc);
  }

  return ok ? 0 : 1;
}
