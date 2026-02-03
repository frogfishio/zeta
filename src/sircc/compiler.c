// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_internal.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int sircc_compile(const SirccOptions* opt) {
  if (!opt || !opt->input_path) return SIRCC_EXIT_USAGE;
  if (!opt->verify_only && !opt->output_path) return SIRCC_EXIT_USAGE;

  SirProgram p = {0};
  p.opt = opt;
  p.exit_code = SIRCC_EXIT_ERROR;
  arena_init(&p.arena);
  char* owned_triple = NULL;

  bool ok = parse_program(&p, opt, opt->input_path);
  if (!ok) goto done;

  ok = validate_program(&p);
  if (!ok) goto done;

  if (opt->verify_only) {
    ok = true;
    goto done;
  }

  if (opt->emit == SIRCC_EMIT_ZASM_IR) {
    ok = emit_zasm_v11(&p, opt->output_path);
    goto done;
  }

  // After parsing, clear record-local location so later errors don't point at the last JSONL line.
  p.cur_path = opt->input_path;
  p.cur_line = 0;
  p.cur_src_ref = -1;
  p.cur_loc.unit = NULL;
  p.cur_loc.line = 0;
  p.cur_loc.col = 0;

  const char* use_triple = opt->target_triple ? opt->target_triple : p.target_triple;
  if (!use_triple) {
    owned_triple = LLVMGetDefaultTargetTriple();
    use_triple = owned_triple;
  }

  if (opt->require_pinned_triple && !opt->target_triple && !p.target_triple) {
    errf(&p, "sircc: missing pinned target triple (set meta.ext.target.triple or pass --target-triple)");
    ok = false;
    goto done;
  }

  LLVMContextRef ctx = LLVMContextCreate();
  LLVMModuleRef mod = LLVMModuleCreateWithNameInContext("sir", ctx);

  if (!init_target_for_module(&p, mod, use_triple)) {
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    ok = false;
    goto done;
  }

  if (!lower_functions(&p, ctx, mod)) {
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    ok = false;
    goto done;
  }

  char* verr = NULL;
  if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &verr) != 0) {
    errf(&p, "sircc: LLVM verification failed: %s", verr ? verr : "(unknown)");
    LLVMDisposeMessage(verr);
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    ok = false;
    goto done;
  }

  if (opt->emit == SIRCC_EMIT_LLVM_IR) {
    ok = emit_module_ir(&p, mod, opt->output_path);
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    goto done;
  }

  if (opt->emit == SIRCC_EMIT_OBJ) {
    ok = emit_module_obj(&p, mod, use_triple, opt->output_path);
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    goto done;
  }

  char tmp_obj[4096];
  if (!make_tmp_obj(tmp_obj, sizeof(tmp_obj))) {
    bump_exit_code(&p, SIRCC_EXIT_INTERNAL);
    errf(&p, "sircc: failed to create temporary object path");
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    ok = false;
    goto done;
  }

  ok = emit_module_obj(&p, mod, use_triple, tmp_obj);
  LLVMDisposeModule(mod);
  LLVMContextDispose(ctx);
  if (!ok) {
    unlink(tmp_obj);
    goto done;
  }

  if (opt->runtime == SIRCC_RUNTIME_ZABI25) {
    ok = run_clang_link_zabi25(&p, opt->clang_path, tmp_obj, opt->output_path);
  } else {
    ok = run_clang_link(&p, opt->clang_path, tmp_obj, opt->output_path);
  }
  unlink(tmp_obj);
  if (ok) ok = run_strip(&p, opt->output_path);

done:
  if (owned_triple) LLVMDisposeMessage(owned_triple);
  free(p.srcs);
  free(p.syms);
  free(p.types);
  free(p.nodes);
  arena_free(&p.arena);
  return ok ? SIRCC_EXIT_OK : p.exit_code;
}
