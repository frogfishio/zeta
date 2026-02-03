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

static void usage(FILE* out) {
  fprintf(out,
          "Usage:\n"
          "  sircc <input.sir.jsonl> -o <output> [--emit-llvm|--emit-obj|--emit-zasm] [--clang <path>] [--target-triple <triple>]\n"
          "  sircc --verify-only <input.sir.jsonl>\n"
          "  sircc --dump-records --verify-only <input.sir.jsonl>\n"
          "  sircc --print-target [--target-triple <triple>]\n"
          "  sircc --print-support [--format text|json] [--full]\n"
          "  sircc --check [--dist-root <path>|--examples-dir <path>] [--format text|json]\n"
          "  sircc [--runtime libc|zabi25] [--zabi25-root <path>] ...\n"
          "  sircc [--diagnostics text|json] [--color auto|always|never] [--diag-context N] [--verbose] [--strip]\n"
          "  sircc --require-pinned-triple ...\n"
          "  sircc --version\n");
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

  SirccOptions opt = {
      .argv0 = (argc > 0) ? argv[0] : NULL,
      .input_path = NULL,
      .output_path = NULL,
      .emit = SIRCC_EMIT_EXE,
      .clang_path = NULL,
      .target_triple = NULL,
      .runtime = SIRCC_RUNTIME_LIBC,
      .zabi25_root = NULL,
      .verify_only = false,
      .dump_records = false,
      .print_target = false,
      .verbose = false,
      .strip = false,
      .require_pinned_triple = false,
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
      printf("sircc %s (LLVM %u.%u.%u, default-triple=%s)\n", SIRCC_VERSION, maj, min, pat, triple ? triple : "(null)");
      if (triple) LLVMDisposeMessage(triple);
      return 0;
    }
    if (strcmp(a, "--verify-only") == 0) {
      opt.verify_only = true;
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
  if (!opt.verify_only && !opt.output_path) {
    usage(stderr);
    return SIRCC_EXIT_USAGE;
  }

  return sircc_compile(&opt);
}
