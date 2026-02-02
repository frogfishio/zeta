// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <stdio.h>
#include <string.h>

extern int yyparse(void);
extern FILE* yyin;

static void usage(FILE* out) { fprintf(out, "Usage: sirc <input.sir>\n"); }

int main(int argc, char** argv) {
  const char* path = NULL;
  for (int i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage(stdout);
      return 0;
    }
    if (a[0] == '-') {
      fprintf(stderr, "sirc: unknown flag: %s\n", a);
      usage(stderr);
      return 2;
    }
    if (!path) {
      path = a;
      continue;
    }
    fprintf(stderr, "sirc: unexpected argument: %s\n", a);
    usage(stderr);
    return 2;
  }
  if (!path) {
    usage(stderr);
    return 2;
  }

  FILE* f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "sirc: failed to open %s: %s\n", path, strerror(errno));
    return 1;
  }
  yyin = f;
  int rc = yyparse();
  fclose(f);
  return rc == 0 ? 0 : 1;
}

