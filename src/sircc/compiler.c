// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_internal.h"
#include "compiler_lower_hl.h"
#include "compiler_zasm_internal.h"

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
  if (!opt->verify_only && !opt->lower_hl && !opt->output_path) return SIRCC_EXIT_USAGE;

  SirProgram p = {0};
  p.opt = opt;
  p.exit_code = SIRCC_EXIT_ERROR;
  arena_init(&p.arena);
  sir_idmaps_init(&p);
  char* owned_triple = NULL;

  bool ok = parse_program(&p, opt, opt->input_path);
  if (!ok) goto done;

  ok = validate_program(&p);
  if (!ok) goto done;

  const char* use_triple = opt->target_triple ? opt->target_triple : p.target_triple;
  if (opt->require_pinned_triple && !use_triple) {
    err_codef(&p, "sircc.target.triple.missing",
              "sircc: missing pinned target triple (set meta.ext.target.triple or pass --target-triple)");
    ok = false;
    goto done;
  }

  if (opt->require_target_contract) {
    const char* missing[32];
    size_t missing_len = 0;

    if (!p.target_ptrbits_override) missing[missing_len++] = "meta.ext.target.ptrBits";
    if (!p.target_endian_override) missing[missing_len++] = "meta.ext.target.endian";
    if (!p.target_structalign_override) missing[missing_len++] = "meta.ext.target.structAlign";

    if (!p.target_intalign_override) {
      missing[missing_len++] = "meta.ext.target.intAlign.{i8,i16,i32,i64,ptr}";
    } else {
      if (!p.align_i8) missing[missing_len++] = "meta.ext.target.intAlign.i8";
      if (!p.align_i16) missing[missing_len++] = "meta.ext.target.intAlign.i16";
      if (!p.align_i32) missing[missing_len++] = "meta.ext.target.intAlign.i32";
      if (!p.align_i64) missing[missing_len++] = "meta.ext.target.intAlign.i64";
      if (!p.align_ptr) missing[missing_len++] = "meta.ext.target.intAlign.ptr";
    }

    if (!p.target_floatalign_override) {
      missing[missing_len++] = "meta.ext.target.floatAlign.{f32,f64}";
    } else {
      if (!p.align_f32) missing[missing_len++] = "meta.ext.target.floatAlign.f32";
      if (!p.align_f64) missing[missing_len++] = "meta.ext.target.floatAlign.f64";
    }

    if (missing_len) {
      char buf[1024];
      size_t off = 0;
      off += (size_t)snprintf(buf + off, sizeof(buf) - off, "sircc: missing explicit target contract fields:");
      for (size_t i = 0; i < missing_len && off + 4 < sizeof(buf); i++) {
        off += (size_t)snprintf(buf + off, sizeof(buf) - off, "%s%s", (i == 0) ? " " : ", ", missing[i]);
      }
      err_codef(&p, "sircc.target.contract.missing", "%s", buf);
      ok = false;
      goto done;
    }
  }

  if (opt->lower_hl) {
    if (!opt->emit_sir_core_path || !*opt->emit_sir_core_path) {
      err_codef(&p, "sircc.lower_hl.missing_output", "sircc: --lower-hl requires --emit-sir-core <output.sir.jsonl>");
      ok = false;
      goto done;
    }
    ok = lower_hl_and_emit_sir_core(&p, opt->emit_sir_core_path);
    goto done;
  }

  if (opt->verify_only) {
    ok = true;
    goto done;
  }

  // Avoid "split personality" lowering: normal codegen runs the same HL→Core
  // pipeline as `sircc --lower-hl --emit-sir-core`, just without emitting.
  if (p.feat_sem_v1) {
    ok = lower_hl_in_place(&p);
    if (!ok) goto done;
  }

  if (opt->emit == SIRCC_EMIT_ZASM_IR) {
    if (!use_triple) {
      owned_triple = LLVMGetDefaultTargetTriple();
      use_triple = owned_triple;
    }
    if (!init_target_info(&p, use_triple)) {
      ok = false;
      goto done;
    }

    FILE* map_out = NULL;
    if (opt->zasm_map_path && *opt->zasm_map_path) {
      map_out = fopen(opt->zasm_map_path, "wb");
      if (!map_out) {
        err_codef(&p, "sircc.io.open_failed", "sircc: failed to open zasm map output: %s", strerror(errno));
        ok = false;
        goto done;
      }
      zasm_set_map_output(map_out);
    }
    zasm_clear_about();
    ok = emit_zasm_v11(&p, opt->output_path);
    zasm_set_map_output(NULL);
    if (map_out) fclose(map_out);
    goto done;
  }

  // After parsing, clear record-local location so later errors don't point at the last JSONL line.
  p.cur_path = opt->input_path;
  p.cur_line = 0;
  p.cur_src_ref = -1;
  p.cur_loc.unit = NULL;
  p.cur_loc.line = 0;
  p.cur_loc.col = 0;

  if (!use_triple) {
    owned_triple = LLVMGetDefaultTargetTriple();
    use_triple = owned_triple;
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
    err_codef(&p, "sircc.llvm.verify_failed", "sircc: LLVM verification failed: %s", verr ? verr : "(unknown)");
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
    err_codef(&p, "sircc.tmp_obj.create_failed", "sircc: failed to create temporary object path");
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
  free(p.pending_features);
  sir_idmaps_free(&p);
  arena_free(&p.arena);
  return ok ? SIRCC_EXIT_OK : p.exit_code;
}
