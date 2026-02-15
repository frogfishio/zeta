#include "sem2sir_profile.h"

#include "sem2sir_check.h"
#include "sem2sir_emit.h"

#include <stdio.h>
#include <string.h>

static void usage(const char *argv0) {
  fprintf(stderr, "usage:\n");
  fprintf(stderr, "  %s --dump-profile\n", argv0);
  fprintf(stderr, "  %s --check <stage4.ast.jsonl>\n", argv0);
  fprintf(stderr, "  %s --emit-sir <stage4.ast.jsonl> --out <out.sir.jsonl>\n", argv0);
}

static int dump_profile(void) {
  // Keep this simple: dump the full dictionary and assert roundtrips.

    for (int t = 1; t <= (int)SEM2SIR_TYPE_CSTR; t++) {
    sem2sir_type_id tid = (sem2sir_type_id)t;
    const char *s = sem2sir_type_to_string(tid);
    if (!s)
      continue;
    sem2sir_type_id back = sem2sir_type_parse(s, strlen(s));
    if (back != tid) {
      fprintf(stderr, "internal: type dictionary mismatch: id=%d string='%s' parsed=%d\n", t, s, (int)back);
      return 2;
    }
    printf("type %s\n", s);
  }

  for (int op = 1; op <= (int)SEM2SIR_OP_CORE_GTE; op++) {
    sem2sir_op_id opid = (sem2sir_op_id)op;
    const char *s = sem2sir_op_to_string(opid);
    if (!s)
      continue;
    sem2sir_op_id back = sem2sir_op_parse(s, strlen(s));
    if (back != opid) {
      fprintf(stderr, "internal: op dictionary mismatch: id=%d string='%s' parsed=%d\n", op, s, (int)back);
      return 2;
    }
    printf("op %s\n", s);
  }

  for (int k = 1; k < (int)SEM2SIR_INTRINSIC__MAX; k++) {
    sem2sir_intrinsic_id kid = (sem2sir_intrinsic_id)k;
    const char *s = sem2sir_intrinsic_to_string(kid);
    if (!s)
      continue;
    sem2sir_intrinsic_id back = sem2sir_intrinsic_parse(s, strlen(s));
    if (back != kid) {
      fprintf(stderr, "internal: intrinsic dictionary mismatch: id=%d string='%s' parsed=%d\n", k, s, (int)back);
      return 2;
    }
    printf("intrinsic %s\n", s);
  }

  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(argv[0]);
    return 2;
  }

  if (argc == 2 && strcmp(argv[1], "--dump-profile") == 0) {
    return dump_profile();
  }

  if (argc == 3 && strcmp(argv[1], "--check") == 0) {
    return sem2sir_check_stage4_file(argv[2]);
  }

  if (argc == 5 && strcmp(argv[1], "--emit-sir") == 0 && strcmp(argv[3], "--out") == 0) {
    return sem2sir_emit_sir_file(argv[2], argv[4]);
  }

  usage(argv[0]);
  return 2;
}
