// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

static void usage(FILE* out) {
  fprintf(out,
          "Usage:\n"
          "  sircc <input.sir.jsonl> -o <output> [--emit-llvm|--emit-obj] [--clang <path>] [--target-triple <triple>]\n"
          "  sircc --verify-only <input.sir.jsonl>\n"
          "  sircc --dump-records --verify-only <input.sir.jsonl>\n"
          "  sircc --print-target [--target-triple <triple>]\n"
          "  sircc --version\n");
}

int main(int argc, char** argv) {
  SirccOptions opt = {
      .input_path = NULL,
      .output_path = NULL,
      .emit = SIRCC_EMIT_EXE,
      .clang_path = NULL,
      .target_triple = NULL,
      .verify_only = false,
      .dump_records = false,
      .print_target = false,
  };

  for (int i = 1; i < argc; i++) {
    const char* a = argv[i];

    if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage(stdout);
      return 0;
    }
    if (strcmp(a, "--version") == 0) {
      printf("sircc %s\n", SIRCC_VERSION);
      return 0;
    }
    if (strcmp(a, "--verify-only") == 0) {
      opt.verify_only = true;
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
    if (strcmp(a, "--emit-llvm") == 0) {
      opt.emit = SIRCC_EMIT_LLVM_IR;
      continue;
    }
    if (strcmp(a, "--emit-obj") == 0) {
      opt.emit = SIRCC_EMIT_OBJ;
      continue;
    }
    if (strcmp(a, "-o") == 0) {
      if (i + 1 >= argc) {
        usage(stderr);
        return 2;
      }
      opt.output_path = argv[++i];
      continue;
    }
    if (strcmp(a, "--clang") == 0) {
      if (i + 1 >= argc) {
        usage(stderr);
        return 2;
      }
      opt.clang_path = argv[++i];
      continue;
    }
    if (strcmp(a, "--target-triple") == 0) {
      if (i + 1 >= argc) {
        usage(stderr);
        return 2;
      }
      opt.target_triple = argv[++i];
      continue;
    }
    if (a[0] == '-') {
      fprintf(stderr, "sircc: unknown flag: %s\n", a);
      usage(stderr);
      return 2;
    }

    if (!opt.input_path) {
      opt.input_path = a;
      continue;
    }

    fprintf(stderr, "sircc: unexpected argument: %s\n", a);
    usage(stderr);
    return 2;
  }

  if (opt.print_target) {
    return sircc_print_target(opt.target_triple) ? 0 : 1;
  }

  if (!opt.input_path) {
    usage(stderr);
    return 2;
  }
  if (!opt.verify_only && !opt.output_path) {
    usage(stderr);
    return 2;
  }

  return sircc_compile(&opt) ? 0 : 1;
}
