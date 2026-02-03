cmake_minimum_required(VERSION 3.20)

add_executable(sircc
  main.c
  compiler.c
  compiler_diag.c
  compiler_emit.c
  compiler_link.c
  compiler_lower_cfg.c
  compiler_lower_expr_a.c
  compiler_lower_expr_b.c
  compiler_lower_util.c
  compiler_parse.c
  compiler_tables.c
  compiler_types.c
  compiler_validate.c
  compiler_zasm_backend.c
  compiler_zasm_collect.c
  compiler_zasm_emit.c
  compiler_zasm_lower_stmt.c
  compiler_zasm_lower_value.c
  support.c
  check.c
  sircc.c
  json.c
)

target_include_directories(sircc PRIVATE ${CMAKE_CURRENT_LIST_DIR})

find_package(LLVM REQUIRED CONFIG)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(SIRCC_SUPPORT_GEN_C ${CMAKE_CURRENT_BINARY_DIR}/sircc_support_table.generated.c)
set(SIRCC_SUPPORT_GEN_H ${CMAKE_CURRENT_BINARY_DIR}/sircc_support_table.generated.h)

# Keep the generated support table in sync with the current compiler sources.
file(GLOB SIRCC_COMPILER_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/compiler*.c
  ${CMAKE_CURRENT_LIST_DIR}/compiler*.h
)
add_custom_command(
  OUTPUT ${SIRCC_SUPPORT_GEN_C} ${SIRCC_SUPPORT_GEN_H}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/mnemonic_coverage.py
    --emit-support-c ${SIRCC_SUPPORT_GEN_C}
    --emit-support-h ${SIRCC_SUPPORT_GEN_H}
    --spec ${CMAKE_CURRENT_LIST_DIR}/../../schema/sir/v1.0/mnemonics.html
    --compiler ${CMAKE_CURRENT_LIST_DIR}
  DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/tools/mnemonic_coverage.py
    ${SIRCC_COMPILER_SOURCES}
    ${CMAKE_CURRENT_LIST_DIR}/../../schema/sir/v1.0/mnemonics.html
  VERBATIM
)

target_sources(sircc PRIVATE ${SIRCC_SUPPORT_GEN_C})
target_include_directories(sircc PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_include_directories(sircc PRIVATE ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
target_compile_options(sircc PRIVATE ${LLVM_DEFINITIONS_LIST})

# Keep the component list small; add more as the compiler grows.
llvm_map_components_to_libnames(SIRCC_LLVM_LIBS
  core
  support
  analysis
  target
  mc
  native
  nativecodegen
)

target_link_libraries(sircc PRIVATE ${SIRCC_LLVM_LIBS})

# LLVM is implemented in C++; when linking from C, explicitly pull in a C++ stdlib.
if(APPLE)
  target_link_libraries(sircc PRIVATE c++)
else()
  target_link_libraries(sircc PRIVATE stdc++)
endif()

target_compile_options(sircc PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)

#
# dist bundle is defined at repo root (see top-level CMakeLists.txt)
#

add_test(
  NAME sircc_verify_add
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/add.sir.jsonl
)

add_test(
  NAME sircc_verify_add_mnemonic_tags
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/add_mnemonic_tags.sir.jsonl
)

add_test(
  NAME sircc_verify_cmp_mnemonic_tags
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/cmp_mnemonic_tags.sir.jsonl
)

add_test(
  NAME sircc_verify_float_mnemonic_tags
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/float_mnemonic_tags.sir.jsonl
)

add_test(
  NAME sircc_verify_float_cmp
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/float_cmp.sir.jsonl
)

add_test(
  NAME sircc_verify_call_mnemonic_tags
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/call_mnemonic_tags.sir.jsonl
)

add_test(
  NAME sircc_verify_call_indirect_ptrsym
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/call_indirect_ptrsym.sir.jsonl
)

add_test(
  NAME sircc_verify_cfg_if
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/cfg_if.sir.jsonl
)

add_test(
  NAME sircc_verify_cfg_if_condbr
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/cfg_if_condbr.sir.jsonl
)

add_test(
  NAME sircc_verify_cfg_switch
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/cfg_switch.sir.jsonl
)

add_test(
  NAME sircc_verify_cfg_join_phi
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/cfg_join_phi.sir.jsonl
)

add_test(
  NAME sircc_verify_mem_stack
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/mem_stack.sir.jsonl
)

add_test(
  NAME sircc_verify_ptr_cmp
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/ptr_cmp.sir.jsonl
)

add_test(
  NAME sircc_verify_mem_stack_widths
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/mem_stack_widths.sir.jsonl
)

add_test(
  NAME sircc_verify_mem_copy_fill
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/mem_copy_fill.sir.jsonl
)

add_test(
  NAME sircc_verify_hello_world_puts
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/hello_world_puts.sir.jsonl
)

if(APPLE AND EXISTS ${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/lib/libzingcore25.a)
  add_test(
    NAME sircc_run_hello_zabi25_write
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/hello_zabi25_write.sir.jsonl
      -DEXE=${CMAKE_CURRENT_BINARY_DIR}/hello_zabi25_write.exe
      -DEXPECT=0
      "-DARGS_EXTRA=--runtime;zabi25;--zabi25-root;${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_and_expect_exit.cmake
  )
endif()

add_test(
  NAME sircc_require_pinned_triple_missing_fails
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    "-DARGS=--require-pinned-triple;${CMAKE_CURRENT_LIST_DIR}/examples/add.sir.jsonl;-o;${CMAKE_CURRENT_BINARY_DIR}/should_not_exist.exe"
    "-DEXPECT=missing pinned target triple"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_mnemonic_coverage_m3
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/mnemonic_coverage.py
    --enforce-m3
    --spec ${CMAKE_CURRENT_LIST_DIR}/../../schema/sir/v1.0/mnemonics.html
    --compiler ${CMAKE_CURRENT_LIST_DIR}
)

add_test(
  NAME sircc_print_support_json
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    "-DARGS=--print-support;--format;json"
    "-DEXPECT=\\\"tool\\\":\\\"sircc\\\""
    "-DEXPECT2=\\\"missing\\\""
    "-DEXPECT3=\\\"milestone3\\\""
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stdout_contains.cmake
)

add_test(
  NAME sircc_diag_json_includes_context
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    "-DARGS=--diagnostics;json;--diag-context;1;--verify-only;${CMAKE_CURRENT_LIST_DIR}/examples/bad_unknown_field.sir.jsonl"
    "-DEXPECT=\\\"context\\\""
    "-DEXPECT2=\\\"context_line\\\""
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_check_smoke
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    "-DARGS=--check;--examples-dir;${CMAKE_CURRENT_LIST_DIR}/examples;--format;json"
    "-DEXPECT=\\\"k\\\":\\\"check\\\""
    "-DEXPECT2=\\\"ok\\\":true"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stdout_contains.cmake
)

add_test(
  NAME sircc_run_mem_copy_fill
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/mem_copy_fill.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/mem_copy_fill.exe
    -DEXPECT=42
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_and_expect_exit.cmake
)

add_test(
  NAME sircc_run_mem_stack
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/mem_stack.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/mem_stack.exe
    -DEXPECT=2
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_and_expect_exit.cmake
)

add_test(
  NAME sircc_run_cfg_if
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/cfg_if.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/cfg_if.exe
    -DEXPECT=222
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_and_expect_exit.cmake
)

add_test(
  NAME sircc_run_cfg_switch
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/cfg_switch.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/cfg_switch.exe
    -DEXPECT=20
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_and_expect_exit.cmake
)

add_test(
  NAME sircc_run_hello_world_puts
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/hello_world_puts.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/hello_world_puts.exe
    -DEXPECT=0
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_and_expect_exit.cmake
)

add_test(
  NAME sircc_emit_llvm_ptr_layout
  COMMAND sircc ${CMAKE_CURRENT_LIST_DIR}/examples/ptr_layout.sir.jsonl -o ${CMAKE_CURRENT_BINARY_DIR}/ptr_layout.ll --emit-llvm
)

add_test(
  NAME sircc_emit_llvm_rot_mnemonic_tags
  COMMAND sircc ${CMAKE_CURRENT_LIST_DIR}/examples/rot_mnemonic_tags.sir.jsonl -o ${CMAKE_CURRENT_BINARY_DIR}/rot_mnemonic_tags.ll --emit-llvm
)

add_test(
  NAME sircc_emit_llvm_divrem_mnemonic_tags
  COMMAND sircc ${CMAKE_CURRENT_LIST_DIR}/examples/divrem_mnemonic_tags.sir.jsonl -o ${CMAKE_CURRENT_BINARY_DIR}/divrem_mnemonic_tags.ll --emit-llvm
)

add_test(
  NAME sircc_emit_llvm_conv_mnemonic_tags
  COMMAND sircc ${CMAKE_CURRENT_LIST_DIR}/examples/conv_mnemonic_tags.sir.jsonl -o ${CMAKE_CURRENT_BINARY_DIR}/conv_mnemonic_tags.ll --emit-llvm
)

add_test(
  NAME sircc_emit_llvm_eqz_mnemonic_tags
  COMMAND sircc ${CMAKE_CURRENT_LIST_DIR}/examples/eqz_mnemonic_tags.sir.jsonl -o ${CMAKE_CURRENT_BINARY_DIR}/eqz_mnemonic_tags.ll --emit-llvm
)

add_test(
  NAME sircc_emit_llvm_mem_copy_overlap_trap
  COMMAND sircc ${CMAKE_CURRENT_LIST_DIR}/examples/mem_copy_overlap_trap.sir.jsonl -o ${CMAKE_CURRENT_BINARY_DIR}/mem_copy_overlap_trap.ll --emit-llvm
)

add_test(
  NAME sircc_emit_llvm_float_load_canon
  COMMAND sircc ${CMAKE_CURRENT_LIST_DIR}/examples/float_load_canon.sir.jsonl -o ${CMAKE_CURRENT_BINARY_DIR}/float_load_canon.ll --emit-llvm
)

add_test(
  NAME sircc_emit_check_llvm_float_nan_canon_ops
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/float_nan_canon_ops.sir.jsonl
    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/float_nan_canon_ops.ll
    -DPATTERNS=fcmp\ uno\ float;fcmp\ uno\ double;select\ i1;0x7FF8000000000000
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_and_check.cmake
)

add_test(
  NAME sircc_emit_llvm_alloca_op
  COMMAND sircc ${CMAKE_CURRENT_LIST_DIR}/examples/alloca_op.sir.jsonl -o ${CMAKE_CURRENT_BINARY_DIR}/alloca_op.ll --emit-llvm
)

add_test(
  NAME sircc_emit_llvm_alloca_count_ref
  COMMAND sircc ${CMAKE_CURRENT_LIST_DIR}/examples/alloca_count_ref.sir.jsonl -o ${CMAKE_CURRENT_BINARY_DIR}/alloca_count_ref.ll --emit-llvm
)

add_test(
  NAME sircc_emit_llvm_eff_fence
  COMMAND sircc ${CMAKE_CURRENT_LIST_DIR}/examples/eff_fence.sir.jsonl -o ${CMAKE_CURRENT_BINARY_DIR}/eff_fence.ll --emit-llvm
)

add_test(
  NAME sircc_emit_llvm_load_store_no_align
  COMMAND sircc ${CMAKE_CURRENT_LIST_DIR}/examples/load_store_no_align.sir.jsonl -o ${CMAKE_CURRENT_BINARY_DIR}/load_store_no_align.ll --emit-llvm
)

add_test(
  NAME sircc_emit_llvm_call_indirect_ptrsym
  COMMAND sircc ${CMAKE_CURRENT_LIST_DIR}/examples/call_indirect_ptrsym.sir.jsonl -o ${CMAKE_CURRENT_BINARY_DIR}/call_indirect_ptrsym.ll --emit-llvm
)

add_test(
  NAME sircc_emit_llvm_misaligned_load_traps
  COMMAND sircc ${CMAKE_CURRENT_LIST_DIR}/examples/misaligned_load_traps.sir.jsonl -o ${CMAKE_CURRENT_BINARY_DIR}/misaligned_load_traps.ll --emit-llvm
)

if(APPLE AND EXISTS "${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck")
  add_test(
    NAME sircc_emit_zasm_hello_zabi25_write_ircheck
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/hello_zabi25_write.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/hello_zabi25_write.zasm.jsonl
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_ircheck.cmake
  )
endif()

if(APPLE AND EXISTS "${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem" AND EXISTS "${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck")
  add_test(
    NAME sircc_emit_zasm_run_zem_hello_zabi25_write
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/hello_zabi25_write.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/hello_zabi25_write.zem.zasm.jsonl
      "-DEXPECT_STDOUT=hello from zABI25"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_mem_copy_fill_prints_star
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/mem_copy_fill_zir_main.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/mem_copy_fill_zir_main.zem.zasm.jsonl
      "-DEXPECT_STDOUT=*"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )
endif()

add_test(
  NAME sircc_verify_reject_cfg_early_term
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/cfg_bad_early_term.sir.jsonl
)
set_tests_properties(sircc_verify_reject_cfg_early_term PROPERTIES WILL_FAIL TRUE)

add_test(
  NAME sircc_verify_reject_unknown_field
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/bad_unknown_field.sir.jsonl
)
set_tests_properties(sircc_verify_reject_unknown_field PROPERTIES WILL_FAIL TRUE)

add_test(
  NAME sircc_verify_reject_bad_operand
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/bad_instr_operand.sir.jsonl
)
set_tests_properties(sircc_verify_reject_bad_operand PROPERTIES WILL_FAIL TRUE)

add_test(
  NAME sircc_verify_reject_missing_feature_gate
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/bad_feature_gate_atomic.sir.jsonl
)
set_tests_properties(sircc_verify_reject_missing_feature_gate PROPERTIES WILL_FAIL TRUE)

add_test(
  NAME sircc_verify_error_uses_src_ref
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/bad_src_ref.sir.jsonl
)

add_test(
  NAME sircc_diag_json_unknown_field
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--diagnostics\\;json\\;--verify-only\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_unknown_field.sir.jsonl
    -DEXPECT=\\\"k\\\":\\\"diag\\\"
    -DEXPECT2=\\\"level\\\":\\\"error\\\"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_context_snippet_unknown_field
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--diag-context\\;1\\;--verify-only\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_unknown_field.sir.jsonl
    -DEXPECT=record:
    -DEXPECT2=k=type
    -DEXPECT3=oops
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)
set_tests_properties(sircc_verify_error_uses_src_ref PROPERTIES WILL_FAIL TRUE)
