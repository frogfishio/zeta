// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

bool run_clang_link(SirProgram* p, const char* clang_path, const char* obj_path, const char* out_path) {
  const char* clang = clang_path ? clang_path : "clang";
  const SirccOptions* opt = p ? p->opt : NULL;

  char* const argv[] = {(char*)clang, (char*)"-o", (char*)out_path, (char*)obj_path, NULL};

  if (opt && opt->verbose) {
    fprintf(stderr, "sircc: link: %s -o %s %s\n", clang, out_path, obj_path);
  }

  pid_t pid = fork();
  if (pid < 0) {
    bump_exit_code(p, SIRCC_EXIT_INTERNAL);
    err_codef(p, "sircc.proc.fork_failed", "sircc: fork failed: %s", strerror(errno));
    return false;
  }
  if (pid == 0) {
    execvp(clang, argv);
    fprintf(stderr, "sircc: failed to exec '%s': %s\n", clang, strerror(errno));
    _exit(127);
  }
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) {
    bump_exit_code(p, SIRCC_EXIT_INTERNAL);
    err_codef(p, "sircc.proc.waitpid_failed", "sircc: waitpid failed: %s", strerror(errno));
    return false;
  }
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    if (code == 127) bump_exit_code(p, SIRCC_EXIT_TOOLCHAIN);
    err_codef(p, "sircc.tool.clang.failed", "sircc: clang failed (exit=%d)", code);
    return false;
  }
  return true;
}

static bool file_exists(const char* path) {
  if (!path) return false;
  struct stat st;
  if (stat(path, &st) != 0) return false;
  return S_ISREG(st.st_mode);
}

static bool dir_exists(const char* path) {
  if (!path) return false;
  struct stat st;
  if (stat(path, &st) != 0) return false;
  return S_ISDIR(st.st_mode);
}

static bool path_dirname(const char* path, char* out, size_t out_cap) {
  if (!path || !out || out_cap == 0) return false;
  const char* last = strrchr(path, '/');
  if (!last) {
    snprintf(out, out_cap, "%s", ".");
    return true;
  }
  size_t n = (size_t)(last - path);
  if (n == 0) n = 1; // keep "/"
  if (n >= out_cap) return false;
  memcpy(out, path, n);
  out[n] = 0;
  return true;
}

static bool path_join2(char* out, size_t out_cap, const char* a, const char* b) {
  if (!out || !out_cap || !a || !b) return false;
  size_t alen = strlen(a);
  const char* sep = (alen > 0 && a[alen - 1] == '/') ? "" : "/";
  int n = snprintf(out, out_cap, "%s%s%s", a, sep, b);
  return n > 0 && (size_t)n < out_cap;
}

static bool zabi25_root_is_valid(const char* root) {
  if (!root) return false;
  char lib[PATH_MAX];
  char inc[PATH_MAX];
  char runner[PATH_MAX];
  if (!path_join2(lib, sizeof(lib), root, "lib/libzingcore25.a")) return false;
  if (!path_join2(inc, sizeof(inc), root, "include/zi_sysabi25.h")) return false;
  if (!path_join2(runner, sizeof(runner), root, "examples/host_shim/runner.c")) return false;
  return file_exists(lib) && file_exists(inc) && file_exists(runner);
}

static bool resolve_zabi25_root(const SirccOptions* opt, char* out, size_t out_cap) {
  if (!out || out_cap == 0) return false;

  const char* env = getenv("SIRCC_ZABI25_ROOT");
  const char* try_roots[8] = {0};
  size_t n = 0;
  if (opt && opt->zabi25_root) try_roots[n++] = opt->zabi25_root;
  if (env && *env) try_roots[n++] = env;
  try_roots[n++] = "dist/rt/zabi25/macos-arm64";
  try_roots[n++] = "ext/integration-pack/macos-arm64";
  try_roots[n++] = "integration-pack/macos-arm64";

  // Relative to argv0 if available.
  char d0[PATH_MAX];
  if (opt && opt->argv0 && path_dirname(opt->argv0, d0, sizeof(d0))) {
    static char rel1[PATH_MAX];
    static char rel2[PATH_MAX];
    static char rel3[PATH_MAX];
    if (path_join2(rel1, sizeof(rel1), d0, "../../rt/zabi25/macos-arm64")) try_roots[n++] = rel1;
    if (path_join2(rel2, sizeof(rel2), d0, "../rt/zabi25/macos-arm64")) try_roots[n++] = rel2;
    if (path_join2(rel3, sizeof(rel3), d0, "rt/zabi25/macos-arm64")) try_roots[n++] = rel3;
  }

  for (size_t i = 0; i < n; i++) {
    const char* r = try_roots[i];
    if (!r) continue;
    if (!dir_exists(r)) continue;
    if (zabi25_root_is_valid(r)) {
      snprintf(out, out_cap, "%s", r);
      return true;
    }
  }
  return false;
}

static bool run_clang_compile_c(SirProgram* p, const char* clang_path, const char* c_path, const char* include_dir,
                                const char* out_obj_path) {
  const char* clang = clang_path ? clang_path : "clang";
  const SirccOptions* opt = p ? p->opt : NULL;

  char inc_arg[PATH_MAX + 4];
  inc_arg[0] = 0;
  if (include_dir) snprintf(inc_arg, sizeof(inc_arg), "-I%s", include_dir);

  char* argv[16];
  int ai = 0;
  argv[ai++] = (char*)clang;
  argv[ai++] = (char*)"-std=c11";
  argv[ai++] = (char*)"-c";
  argv[ai++] = (char*)c_path;
  if (include_dir && inc_arg[0]) argv[ai++] = inc_arg;
  argv[ai++] = (char*)"-o";
  argv[ai++] = (char*)out_obj_path;
  argv[ai++] = NULL;

  if (opt && opt->verbose) {
    fprintf(stderr, "sircc: cc: %s -std=c11 -c %s %s -o %s\n", clang, c_path, include_dir ? inc_arg : "", out_obj_path);
  }

  pid_t pid = fork();
  if (pid < 0) {
    bump_exit_code(p, SIRCC_EXIT_INTERNAL);
    err_codef(p, "sircc.proc.fork_failed", "sircc: fork failed: %s", strerror(errno));
    return false;
  }
  if (pid == 0) {
    execvp(clang, argv);
    fprintf(stderr, "sircc: failed to exec '%s': %s\n", clang, strerror(errno));
    _exit(127);
  }
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) {
    bump_exit_code(p, SIRCC_EXIT_INTERNAL);
    err_codef(p, "sircc.proc.waitpid_failed", "sircc: waitpid failed: %s", strerror(errno));
    return false;
  }
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    if (code == 127) bump_exit_code(p, SIRCC_EXIT_TOOLCHAIN);
    err_codef(p, "sircc.tool.clang.failed", "sircc: clang failed (exit=%d)", code);
    return false;
  }
  return true;
}

bool run_clang_link_zabi25(SirProgram* p, const char* clang_path, const char* guest_obj_path, const char* out_path) {
  const SirccOptions* opt = p ? p->opt : NULL;
  char root[PATH_MAX];
  if (!resolve_zabi25_root(opt, root, sizeof(root))) {
    bump_exit_code(p, SIRCC_EXIT_TOOLCHAIN);
    err_codef(p, "sircc.runtime.zabi25.not_found",
              "sircc: zabi25 runtime not found (set --zabi25-root or env SIRCC_ZABI25_ROOT)");
    return false;
  }

  char include_dir[PATH_MAX];
  char lib_path[PATH_MAX];
  char runner_c[PATH_MAX];
  if (!path_join2(include_dir, sizeof(include_dir), root, "include")) return false;
  if (!path_join2(lib_path, sizeof(lib_path), root, "lib/libzingcore25.a")) return false;
  if (!path_join2(runner_c, sizeof(runner_c), root, "examples/host_shim/runner.c")) return false;

  char runner_obj[PATH_MAX];
  if (!make_tmp_obj(runner_obj, sizeof(runner_obj))) {
    bump_exit_code(p, SIRCC_EXIT_INTERNAL);
    err_codef(p, "sircc.io.tmp_obj_failed", "sircc: failed to create temp obj for zabi runner");
    return false;
  }

  bool ok = run_clang_compile_c(p, clang_path, runner_c, include_dir, runner_obj);
  if (!ok) {
    unlink(runner_obj);
    return false;
  }

  const char* clang = clang_path ? clang_path : "clang";
  if (opt && opt->verbose) {
    fprintf(stderr, "sircc: link(zabi25): %s -o %s %s %s %s\n", clang, out_path, runner_obj, guest_obj_path, lib_path);
  }

  char* const argv[] = {(char*)clang, (char*)"-o", (char*)out_path, runner_obj, (char*)guest_obj_path, (char*)lib_path, NULL};
  pid_t pid = fork();
  if (pid < 0) {
    unlink(runner_obj);
    bump_exit_code(p, SIRCC_EXIT_INTERNAL);
    err_codef(p, "sircc.proc.fork_failed", "sircc: fork failed: %s", strerror(errno));
    return false;
  }
  if (pid == 0) {
    execvp(clang, argv);
    fprintf(stderr, "sircc: failed to exec '%s': %s\n", clang, strerror(errno));
    _exit(127);
  }
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) {
    unlink(runner_obj);
    bump_exit_code(p, SIRCC_EXIT_INTERNAL);
    err_codef(p, "sircc.proc.waitpid_failed", "sircc: waitpid failed: %s", strerror(errno));
    return false;
  }

  unlink(runner_obj);

  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    if (code == 127) bump_exit_code(p, SIRCC_EXIT_TOOLCHAIN);
    err_codef(p, "sircc.tool.clang.failed", "sircc: clang failed (exit=%d)", code);
    return false;
  }
  return true;
}

bool run_strip(SirProgram* p, const char* exe_path) {
  const SirccOptions* opt = p ? p->opt : NULL;
  if (!opt || !opt->strip) return true;

  const char* strip = "strip";
  if (opt->verbose) {
    fprintf(stderr, "sircc: strip: %s %s\n", strip, exe_path);
  }

  pid_t pid = fork();
  if (pid < 0) {
    bump_exit_code(p, SIRCC_EXIT_INTERNAL);
    err_codef(p, "sircc.proc.fork_failed", "sircc: fork failed: %s", strerror(errno));
    return false;
  }
  if (pid == 0) {
    execlp(strip, strip, exe_path, (char*)NULL);
    fprintf(stderr, "sircc: failed to exec '%s': %s\n", strip, strerror(errno));
    _exit(127);
  }

  int st = 0;
  if (waitpid(pid, &st, 0) < 0) {
    bump_exit_code(p, SIRCC_EXIT_INTERNAL);
    err_codef(p, "sircc.proc.waitpid_failed", "sircc: waitpid failed: %s", strerror(errno));
    return false;
  }
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    if (code == 127) bump_exit_code(p, SIRCC_EXIT_TOOLCHAIN);
    err_codef(p, "sircc.tool.strip.failed", "sircc: strip failed (exit=%d)", code);
    return false;
  }
  return true;
}

bool make_tmp_obj(char* out, size_t out_cap) {
  const char* dir = getenv("TMPDIR");
  if (!dir) dir = "/tmp";
  if (snprintf(out, out_cap, "%s/sircc-XXXXXX.o", dir) >= (int)out_cap) return false;
  int fd = mkstemps(out, 2);
  if (fd < 0) return false;
  close(fd);
  return true;
}
