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
  // Keep this simple: just prove the dictionary is wired up.
  const char *types[] = {"i32", "bool", "u8", "u32", "u64", "i64", "f64", "void", "ptr", "slice", "string.utf8"};
  const char *ops[] = {
      "core.assign",
      "core.bool.or_sc",
      "core.bool.and_sc",
      "core.add",
      "core.sub",
      "core.mul",
      "core.div",
      "core.rem",
      "core.shl",
      "core.shr",
      "core.bitand",
      "core.bitor",
      "core.bitxor",
      "core.eq",
      "core.ne",
      "core.lt",
      "core.lte",
      "core.gt",
      "core.gte",
  };
  const char *ks[] = {
      "Unit", "Proc", "Block", "Var", "ExprStmt", "Return", "If", "While",
      "Param", "Call", "Args",
      "Name", "TypeRef", "Int", "True", "False", "Nil", "Paren", "Bin",
  };

  for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
    const char *s = types[i];
    sem2sir_type_id t = sem2sir_type_parse(s, strlen(s));
    const char *back = sem2sir_type_to_string(t);
    if (t == SEM2SIR_TYPE_INVALID || !back) {
      fprintf(stderr, "internal: type not in dictionary: %s\n", s);
      return 2;
    }
    printf("type %s\n", back);
  }

  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
    const char *s = ops[i];
    sem2sir_op_id op = sem2sir_op_parse(s, strlen(s));
    const char *back = sem2sir_op_to_string(op);
    if (op == SEM2SIR_OP_INVALID || !back) {
      fprintf(stderr, "internal: op not in dictionary: %s\n", s);
      return 2;
    }
    printf("op %s\n", back);
  }

  for (size_t i = 0; i < sizeof(ks) / sizeof(ks[0]); i++) {
    const char *s = ks[i];
    sem2sir_intrinsic_id k = sem2sir_intrinsic_parse(s, strlen(s));
    const char *back = sem2sir_intrinsic_to_string(k);
    if (k == SEM2SIR_INTRINSIC_INVALID || !back) {
      fprintf(stderr, "internal: intrinsic not in dictionary: %s\n", s);
      return 2;
    }
    printf("intrinsic %s\n", back);
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
