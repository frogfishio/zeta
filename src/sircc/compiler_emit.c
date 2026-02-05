// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_internal.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static void llvm_init_targets_once(void) {
  static int inited = 0;
  if (inited) return;
  // Avoid forcing linkage against every LLVM target backend. For the
  // "product" path (Milestone 3), initializing the native target is enough.
  // If/when we want a true cross-compiler build, we can add an opt-in mode
  // that links + initializes all targets.
  if (LLVMInitializeNativeTarget() != 0) {
    fprintf(stderr, "sircc: failed to initialize native LLVM target\n");
    exit(2);
  }
  if (LLVMInitializeNativeAsmPrinter() != 0) {
    fprintf(stderr, "sircc: failed to initialize native LLVM asm printer\n");
    exit(2);
  }
  // The parser isn't strictly required for object/exe emission, but is a cheap
  // init and keeps future tooling options open.
  (void)LLVMInitializeNativeAsmParser();
  inited = 1;
}

bool emit_module_ir(SirProgram* p, LLVMModuleRef mod, const char* out_path) {
  char* err = NULL;
  if (LLVMPrintModuleToFile(mod, out_path, &err) != 0) {
    err_codef(p, "sircc.llvm.emit_ir_failed", "sircc: failed to write LLVM IR: %s", err ? err : "(unknown)");
    LLVMDisposeMessage(err);
    return false;
  }
  return true;
}

bool init_target_for_module(SirProgram* p, LLVMModuleRef mod, const char* triple) {
  if (!p || !mod || !triple) return false;

  llvm_init_targets_once();

  char* err = NULL;
  LLVMTargetRef target = NULL;
  if (LLVMGetTargetFromTriple(triple, &target, &err) != 0) {
    err_codef(p, "sircc.llvm.triple.unsupported", "sircc: target triple '%s' unsupported: %s", triple, err ? err : "(unknown)");
    LLVMDisposeMessage(err);
    return false;
  }

  LLVMTargetMachineRef tm =
      LLVMCreateTargetMachine(target, triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocDefault, LLVMCodeModelDefault);
  if (!tm) {
    err_codef(p, "sircc.llvm.target_machine.create_failed", "sircc: failed to create target machine");
    return false;
  }

  LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
  char* dl_str = LLVMCopyStringRepOfTargetData(td);
  LLVMSetTarget(mod, triple);
  LLVMSetDataLayout(mod, dl_str);

  p->ptr_bytes = LLVMPointerSize(td);
  p->ptr_bits = p->ptr_bytes * 8u;

  LLVMDisposeMessage(dl_str);
  LLVMDisposeTargetData(td);
  LLVMDisposeTargetMachine(tm);
  return true;
}

bool emit_module_obj(SirProgram* p, LLVMModuleRef mod, const char* triple, const char* out_path) {
  llvm_init_targets_once();

  char* err = NULL;
  const char* use_triple = triple ? triple : LLVMGetDefaultTargetTriple();
  LLVMTargetRef target = NULL;
  if (LLVMGetTargetFromTriple(use_triple, &target, &err) != 0) {
    err_codef(p, "sircc.llvm.triple.unsupported", "sircc: target triple '%s' unsupported: %s", use_triple, err ? err : "(unknown)");
    LLVMDisposeMessage(err);
    if (!triple) LLVMDisposeMessage((char*)use_triple);
    return false;
  }

  LLVMTargetMachineRef tm =
      LLVMCreateTargetMachine(target, use_triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocDefault,
                              LLVMCodeModelDefault);
  if (!tm) {
    err_codef(p, "sircc.llvm.target_machine.create_failed", "sircc: failed to create target machine");
    if (!triple) LLVMDisposeMessage((char*)use_triple);
    return false;
  }

  LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
  char* dl_str = LLVMCopyStringRepOfTargetData(td);
  LLVMSetTarget(mod, use_triple);
  LLVMSetDataLayout(mod, dl_str);
  LLVMDisposeMessage(dl_str);
  LLVMDisposeTargetData(td);

  if (LLVMTargetMachineEmitToFile(tm, mod, (char*)out_path, LLVMObjectFile, &err) != 0) {
    err_codef(p, "sircc.llvm.emit_obj_failed", "sircc: failed to emit object: %s", err ? err : "(unknown)");
    LLVMDisposeMessage(err);
    LLVMDisposeTargetMachine(tm);
    if (!triple) LLVMDisposeMessage((char*)use_triple);
    return false;
  }

  LLVMDisposeTargetMachine(tm);
  if (!triple) LLVMDisposeMessage((char*)use_triple);
  return true;
}

bool sircc_print_target(const char* triple) {
  llvm_init_targets_once();

  char* err = NULL;
  const char* use_triple = triple ? triple : LLVMGetDefaultTargetTriple();
  LLVMTargetRef target = NULL;
  if (LLVMGetTargetFromTriple(use_triple, &target, &err) != 0) {
    errf(NULL, "sircc: target triple '%s' unsupported: %s", use_triple, err ? err : "(unknown)");
    LLVMDisposeMessage(err);
    if (!triple) LLVMDisposeMessage((char*)use_triple);
    return false;
  }

  LLVMTargetMachineRef tm =
      LLVMCreateTargetMachine(target, use_triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocDefault, LLVMCodeModelDefault);
  if (!tm) {
    errf(NULL, "sircc: failed to create target machine");
    if (!triple) LLVMDisposeMessage((char*)use_triple);
    return false;
  }

  LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
  char* dl_str = LLVMCopyStringRepOfTargetData(td);

  unsigned ptr_bytes = LLVMPointerSize(td);
  unsigned ptr_bits = ptr_bytes * 8u;
  const char* endian = (dl_str && dl_str[0] == 'E') ? "big" : "little";

  printf("triple: %s\n", use_triple);
  printf("data_layout: %s\n", dl_str ? dl_str : "(null)");
  printf("endianness: %s\n", endian);
  printf("ptrBits: %u\n", ptr_bits);

  LLVMDisposeMessage(dl_str);
  LLVMDisposeTargetData(td);
  LLVMDisposeTargetMachine(tm);
  if (!triple) LLVMDisposeMessage((char*)use_triple);
  return true;
}
