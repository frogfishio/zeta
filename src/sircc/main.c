// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler.h"
#include "check.h"
#include "support.h"
#include "version.h"

#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool file_exists(const char* p) {
  if (!p) return false;
  FILE* f = fopen(p, "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

static bool path_join(char* out, size_t out_cap, const char* a, const char* b) {
  if (!out || !out_cap || !a || !b) return false;
  size_t alen = strlen(a);
  const char* sep = (alen > 0 && a[alen - 1] == '/') ? "" : "/";
  int n = snprintf(out, out_cap, "%s%s%s", a, sep, b);
  return n > 0 && (size_t)n < out_cap;
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

static const char* builtin_prelude_file(const char* name) {
  if (!name) return NULL;
  if (strcmp(name, "data_v1") == 0) return "data_v1.sir.jsonl";
  if (strcmp(name, "zabi25") == 0) return "zabi25_min.sir.jsonl";
  if (strcmp(name, "zabi25_min") == 0) return "zabi25_min.sir.jsonl";
  return NULL;
}

static bool resolve_prelude_builtin_path(const char* builtin, const char* argv0, char* out, size_t out_cap) {
  const char* file = builtin_prelude_file(builtin);
  if (!file || !out || !out_cap) return false;

  const char* env = getenv("SIRCC_PRELUDE_ROOT");
  const char* roots[8] = {0};
  size_t n = 0;
  if (env && *env) roots[n++] = env;
  roots[n++] = "dist/lib/sircc/prelude";
  roots[n++] = "src/sircc/prelude";

  // Relative to argv0 if available (covers build tree + dist layout).
  char d0[4096];
  if (path_dirname(argv0, d0, sizeof(d0))) {
    static char rel1[4096];
    static char rel2[4096];
    static char rel3[4096];
    static char rel4[4096];
    // build/src/sircc/sircc -> ../../../src/sircc/prelude
    if (path_join(rel1, sizeof(rel1), d0, "../../../src/sircc/prelude")) roots[n++] = rel1;
    // dist/bin/<os>/sircc -> ../lib/sircc/prelude (best-effort)
    if (path_join(rel2, sizeof(rel2), d0, "../lib/sircc/prelude")) roots[n++] = rel2;
    // dist/bin/<os>/sircc -> ../../lib/sircc/prelude (best-effort)
    if (path_join(rel3, sizeof(rel3), d0, "../../lib/sircc/prelude")) roots[n++] = rel3;
    // installed layouts sometimes place preludes next to the binary
    if (path_join(rel4, sizeof(rel4), d0, "../prelude")) roots[n++] = rel4;
  }

  char dist_root[4096];
  if (infer_dist_root_from_argv0(argv0, dist_root, sizeof(dist_root))) {
    static char rel[4096];
    if (path_join(rel, sizeof(rel), dist_root, "lib/sircc/prelude")) roots[n++] = rel;
  }

  for (size_t i = 0; i < n; i++) {
    char cand[4096];
    if (!roots[i]) continue;
    if (!path_join(cand, sizeof(cand), roots[i], file)) continue;
    if (file_exists(cand)) {
      snprintf(out, out_cap, "%s", cand);
      return true;
    }
  }
  return false;
}

static void usage(FILE* out) {
  fprintf(out,
          "sircc — SIR JSONL compiler\n"
          "\n"
          "Usage:\n"
          "  sircc <input.sir.jsonl> -o <output> [--emit-llvm|--emit-obj|--emit-zasm] [--clang <path>] [--target-triple <triple>]\n"
          "  sircc <input.sir.jsonl> -o <output.zasm.jsonl> --emit-zasm [--emit-zasm-map <map.jsonl>]\n"
          "  sircc [--prelude <prelude.sir.jsonl>]... <input.sir.jsonl> ...\n"
          "  sircc [--prelude-builtin data_v1|zabi25_min]... <input.sir.jsonl> ...\n"
          "  sircc --verify-only <input.sir.jsonl>\n"
          "  sircc --verify-strict --verify-only <input.sir.jsonl>\n"
          "  sircc --lower-hl --emit-sir-core <output.sir.jsonl> <input.sir.jsonl>\n"
          "  sircc --dump-records --verify-only <input.sir.jsonl>\n"
          "  sircc --print-target [--target-triple <triple>]\n"
          "  sircc --print-support [--format text|json] [--full]\n"
          "  sircc --check [--dist-root <path>|--examples-dir <path>] [--format text|json]\n"
          "  sircc [--runtime libc|zabi25] [--zabi25-root <path>] ...\n"
          "  sircc [--diagnostics text|json] [--color auto|always|never] [--diag-context N] [--verbose] [--strip]\n"
          "  sircc --deterministic ...\n"
          "  sircc --require-pinned-triple ...\n"
          "  sircc --require-target-contract ...\n"
          "  sircc --version\n"
          "\n"
          "Lowering:\n"
          "  --lower-hl         Lower supported SIR-HL into Core SIR (no codegen)\n"
          "  --lower-strict     Tighten lowering/verification rules (implies --verify-strict)\n"
          "  --emit-sir-core P  Write lowered Core SIR JSONL to P (requires --lower-hl)\n"
          "\n"
          "License: GPLv3+\n"
          "© 2026 Frogfish — Author: Alexander Croft\n");
}

static bool streq(const char* a, const char* b) { return a && b && strcmp(a, b) == 0; }

static bool parse_enum_value(const char* v, const char* a, const char* b, const char* c) {
  return streq(v, a) || streq(v, b) || (c && streq(v, c));
}

int main(int argc, char** argv) {
  bool print_support = false;
  bool support_full = false;
  bool check = false;
  const char* dist_root = NULL;
  const char* examples_dir = NULL;
  const char* format = "text";
  const char* runtime = "libc";
  const char* zabi25_root = NULL;
  const char* prelude_paths[32];
  size_t prelude_paths_len = 0;
  char prelude_builtin_bufs[32][4096];
  size_t prelude_builtin_bufs_len = 0;

  SirccOptions opt = {
      .argv0 = (argc > 0) ? argv[0] : NULL,
      .prelude_paths = NULL,
      .prelude_paths_len = 0,
      .input_path = NULL,
      .output_path = NULL,
      .emit = SIRCC_EMIT_EXE,
      .clang_path = NULL,
      .target_triple = NULL,
      .runtime = SIRCC_RUNTIME_LIBC,
      .zabi25_root = NULL,
      .zasm_map_path = NULL,
      .lower_hl = false,
      .emit_sir_core_path = NULL,
      .lower_strict = false,
      .verify_only = false,
      .verify_strict = false,
      .dump_records = false,
      .print_target = false,
      .verbose = false,
      .strip = false,
      .require_pinned_triple = false,
      .require_target_contract = false,
      .diagnostics = SIRCC_DIAG_TEXT,
      .color = SIRCC_COLOR_AUTO,
      .diag_context = 0,
  };

  for (int i = 1; i < argc; i++) {
    const char* a = argv[i];

    if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage(stdout);
      return 0;
    }
    if (strcmp(a, "--version") == 0) {
      unsigned maj = 0, min = 0, pat = 0;
      LLVMGetVersion(&maj, &min, &pat);
      char* triple = LLVMGetDefaultTargetTriple();
      printf("sircc %s\n", SIRCC_VERSION);
      printf("LLVM %u.%u.%u (default-triple=%s)\n", maj, min, pat, triple ? triple : "(null)");
      printf("License: GPLv3+\n");
      printf("© 2026 Frogfish — Author: Alexander Croft\n");
      if (triple) LLVMDisposeMessage(triple);
      return 0;
    }
    if (strcmp(a, "--verify-only") == 0) {
      opt.verify_only = true;
      continue;
    }
    if (strcmp(a, "--verify-strict") == 0) {
      opt.verify_strict = true;
      continue;
    }
    if (strcmp(a, "--prelude") == 0) {
      if (i + 1 >= argc) {
        usage(stderr);
        return SIRCC_EXIT_USAGE;
      }
      if (prelude_paths_len >= (sizeof(prelude_paths) / sizeof(prelude_paths[0]))) {
        fprintf(stderr, "sircc: too many --prelude files (max=%zu)\n", sizeof(prelude_paths) / sizeof(prelude_paths[0]));
        return SIRCC_EXIT_USAGE;
      }
      prelude_paths[prelude_paths_len++] = argv[++i];
      continue;
    }
    if (strcmp(a, "--prelude-builtin") == 0) {
      if (i + 1 >= argc) {
        usage(stderr);
        return SIRCC_EXIT_USAGE;
      }
      const char* builtin = argv[++i];
      if (prelude_paths_len >= (sizeof(prelude_paths) / sizeof(prelude_paths[0]))) {
        fprintf(stderr, "sircc: too many preludes (max=%zu)\n", sizeof(prelude_paths) / sizeof(prelude_paths[0]));
        return SIRCC_EXIT_USAGE;
      }
      if (prelude_builtin_bufs_len >= (sizeof(prelude_builtin_bufs) / sizeof(prelude_builtin_bufs[0]))) {
        fprintf(stderr, "sircc: too many --prelude-builtin uses (max=%zu)\n",
                sizeof(prelude_builtin_bufs) / sizeof(prelude_builtin_bufs[0]));
        return SIRCC_EXIT_USAGE;
      }
      char* buf = prelude_builtin_bufs[prelude_builtin_bufs_len++];
      buf[0] = 0;
      if (!resolve_prelude_builtin_path(builtin, opt.argv0, buf, sizeof(prelude_builtin_bufs[0]))) {
        fprintf(stderr, "sircc: unknown or missing prelude builtin '%s' (known: data_v1, zabi25_min)\n", builtin ? builtin : "(null)");
        return SIRCC_EXIT_USAGE;
      }
      prelude_paths[prelude_paths_len++] = buf;
      continue;
    }
    if (strcmp(a, "--lower-hl") == 0) {
      opt.lower_hl = true;
      continue;
    }
    if (strcmp(a, "--lower-strict") == 0) {
      opt.lower_strict = true;
      opt.verify_strict = true;
      continue;
    }
    if (strcmp(a, "--emit-sir-core") == 0) {
      if (i + 1 >= argc) {
        usage(stderr);
        return SIRCC_EXIT_USAGE;
      }
      opt.emit_sir_core_path = argv[++i];
      continue;
    }
    if (strcmp(a, "--print-support") == 0) {
      print_support = true;
      continue;
    }
    if (strcmp(a, "--full") == 0) {
      support_full = true;
      continue;
    }
    if (strcmp(a, "--check") == 0) {
      check = true;
      continue;
    }
    if (strcmp(a, "--runtime") == 0) {
      if (i + 1 >= argc) {
        usage(stderr);
        return SIRCC_EXIT_USAGE;
      }
      const char* v = argv[++i];
      if (!parse_enum_value(v, "libc", "zabi25", NULL)) {
        fprintf(stderr, "sircc: invalid --runtime value: %s\n", v);
        return SIRCC_EXIT_USAGE;
      }
      runtime = v;
      continue;
    }
    if (strcmp(a, "--zabi25-root") == 0) {
      if (i + 1 >= argc) {
        usage(stderr);
        return SIRCC_EXIT_USAGE;
      }
      zabi25_root = argv[++i];
      continue;
    }
    if (strcmp(a, "--dist-root") == 0) {
      if (i + 1 >= argc) {
        usage(stderr);
        return SIRCC_EXIT_USAGE;
      }
      dist_root = argv[++i];
      continue;
    }
    if (strcmp(a, "--examples-dir") == 0) {
      if (i + 1 >= argc) {
        usage(stderr);
        return SIRCC_EXIT_USAGE;
      }
      examples_dir = argv[++i];
      continue;
    }
    if (strcmp(a, "--dump-records") == 0) {
      opt.dump_records = true;
      continue;
    }
    if (strcmp(a, "--print-target") == 0) {
      opt.print_target = true;
      continue;
    }
    if (strcmp(a, "--format") == 0) {
      if (i + 1 >= argc) {
        usage(stderr);
        return SIRCC_EXIT_USAGE;
      }
      const char* v = argv[++i];
      if (!parse_enum_value(v, "text", "json", NULL)) {
        fprintf(stderr, "sircc: invalid --format value: %s\n", v);
        return SIRCC_EXIT_USAGE;
      }
      format = v;
      continue;
    }
    if (strcmp(a, "--emit-llvm") == 0) {
      opt.emit = SIRCC_EMIT_LLVM_IR;
      continue;
    }
    if (strcmp(a, "--emit-obj") == 0) {
      opt.emit = SIRCC_EMIT_OBJ;
      continue;
    }
    if (strcmp(a, "--emit-zasm") == 0) {
      opt.emit = SIRCC_EMIT_ZASM_IR;
      continue;
    }
    if (strcmp(a, "--emit-zasm-map") == 0) {
      if (i + 1 >= argc) {
        usage(stderr);
        return SIRCC_EXIT_USAGE;
      }
      opt.zasm_map_path = argv[++i];
      continue;
    }
    if (strcmp(a, "-o") == 0) {
      if (i + 1 >= argc) {
        usage(stderr);
        return SIRCC_EXIT_USAGE;
      }
      opt.output_path = argv[++i];
      continue;
    }
    if (strcmp(a, "--clang") == 0) {
      if (i + 1 >= argc) {
        usage(stderr);
        return SIRCC_EXIT_USAGE;
      }
      opt.clang_path = argv[++i];
      continue;
    }
    if (strcmp(a, "--target-triple") == 0) {
      if (i + 1 >= argc) {
        usage(stderr);
        return SIRCC_EXIT_USAGE;
      }
      opt.target_triple = argv[++i];
      continue;
    }
    if (strcmp(a, "--verbose") == 0) {
      opt.verbose = true;
      continue;
    }
    if (strcmp(a, "--strip") == 0) {
      opt.strip = true;
      continue;
    }
    if (strcmp(a, "--require-pinned-triple") == 0) {
      opt.require_pinned_triple = true;
      continue;
    }
    if (strcmp(a, "--require-target-contract") == 0) {
      opt.require_target_contract = true;
      continue;
    }
    if (strcmp(a, "--deterministic") == 0) {
      // Best-effort reproducibility: require explicit target triple (meta.ext.target.triple or --target-triple).
      opt.require_pinned_triple = true;
      // Stronger reproducibility: require explicit ABI contract fields (ptrBits/endian/*Align/structAlign).
      opt.require_target_contract = true;
      continue;
    }
    if (strcmp(a, "--diagnostics") == 0) {
      if (i + 1 >= argc) {
        usage(stderr);
        return SIRCC_EXIT_USAGE;
      }
      const char* v = argv[++i];
      if (!parse_enum_value(v, "text", "json", NULL)) {
        fprintf(stderr, "sircc: invalid --diagnostics value: %s\n", v);
        return SIRCC_EXIT_USAGE;
      }
      opt.diagnostics = streq(v, "json") ? SIRCC_DIAG_JSON : SIRCC_DIAG_TEXT;
      continue;
    }
    if (strcmp(a, "--diag-context") == 0) {
      if (i + 1 >= argc) {
        usage(stderr);
        return SIRCC_EXIT_USAGE;
      }
      const char* v = argv[++i];
      char* end = NULL;
      long n = strtol(v, &end, 10);
      if (!end || *end != 0 || n < 0 || n > 1000000) {
        fprintf(stderr, "sircc: invalid --diag-context value: %s\n", v);
        return SIRCC_EXIT_USAGE;
      }
      opt.diag_context = (int)n;
      continue;
    }
    if (strncmp(a, "--color", 7) == 0) {
      const char* v = NULL;
      if (strcmp(a, "--color") == 0) {
        if (i + 1 >= argc) {
          usage(stderr);
          return SIRCC_EXIT_USAGE;
        }
        v = argv[++i];
      } else if (strncmp(a, "--color=", 8) == 0) {
        v = a + 8;
      }
      if (!v || !parse_enum_value(v, "auto", "always", "never")) {
        fprintf(stderr, "sircc: invalid --color value: %s\n", v ? v : "(missing)");
        return SIRCC_EXIT_USAGE;
      }
      if (streq(v, "auto")) opt.color = SIRCC_COLOR_AUTO;
      else if (streq(v, "always")) opt.color = SIRCC_COLOR_ALWAYS;
      else opt.color = SIRCC_COLOR_NEVER;
      continue;
    }
    if (a[0] == '-') {
      fprintf(stderr, "sircc: unknown flag: %s\n", a);
      usage(stderr);
      return SIRCC_EXIT_USAGE;
    }

    if (!opt.input_path) {
      opt.input_path = a;
      continue;
    }

    fprintf(stderr, "sircc: unexpected argument: %s\n", a);
    usage(stderr);
    return SIRCC_EXIT_USAGE;
  }

  if (opt.print_target) {
    return sircc_print_target(opt.target_triple) ? 0 : 1;
  }

  if (streq(runtime, "zabi25")) {
    opt.runtime = SIRCC_RUNTIME_ZABI25;
    opt.zabi25_root = zabi25_root;
  } else {
    opt.runtime = SIRCC_RUNTIME_LIBC;
  }

  if (prelude_paths_len) {
    opt.prelude_paths = prelude_paths;
    opt.prelude_paths_len = prelude_paths_len;
  }

  if (print_support) {
    SirccSupportFormat sf = streq(format, "json") ? SIRCC_SUPPORT_JSON : SIRCC_SUPPORT_TEXT;
    return sircc_print_support(stdout, sf, support_full) ? 0 : SIRCC_EXIT_INTERNAL;
  }

  if (check) {
    SirccCheckOptions chk = {
        .argv0 = opt.argv0,
        .dist_root = dist_root,
        .examples_dir = examples_dir,
        .format = streq(format, "json") ? SIRCC_CHECK_JSON : SIRCC_CHECK_TEXT,
    };
    return sircc_run_check(stdout, &opt, &chk);
  }

  if (!opt.input_path) {
    usage(stderr);
    return SIRCC_EXIT_USAGE;
  }
  if (!opt.verify_only && !opt.lower_hl && !opt.output_path) {
    usage(stderr);
    return SIRCC_EXIT_USAGE;
  }

  return sircc_compile(&opt);
}
