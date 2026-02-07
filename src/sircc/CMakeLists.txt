cmake_minimum_required(VERSION 3.20)

add_executable(sircc
  main.c
  compiler.c
  compiler_ids.c
  compiler_diag.c
  compiler_emit.c
  compiler_lower_hl.c
  compiler_link.c
  compiler_lower_cfg.c
  compiler_lower_expr_a.c
  compiler_lower_expr_b.c
  compiler_lower_simd.c
  compiler_lower_util.c
  compiler_parse.c
  compiler_tables.c
  compiler_types.c
  compiler_validate.c
  compiler_zasm_backend.c
  compiler_zasm_backend_cfg.c
  compiler_zasm_backend_names.c
  compiler_zasm_backend_ops.c
  compiler_zasm_backend_stmt.c
  compiler_zasm_backend_util.c
  compiler_zasm_addr.c
  compiler_zasm_addr_emit.c
  compiler_zasm_collect.c
  compiler_zasm_diag.c
  compiler_zasm_emit.c
  compiler_zasm_regcache.c
  compiler_zasm_lower_stmt.c
  compiler_zasm_lower_value.c
  support.c
  check.c
  sircc.c
  json.c
)

target_compile_definitions(sircc PRIVATE SIR_VERSION="${SIR_VERSION}")

target_include_directories(sircc PRIVATE ${CMAKE_CURRENT_LIST_DIR})

find_package(LLVM REQUIRED CONFIG)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

option(SIRCC_ENABLE_LOWER_COMPARE_TESTS "Enable LLVM-vs-lower comparison tests (requires ext/integration-pack/macos-arm64/bin/lower; may fail while lower is WIP)" OFF)

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
  NAME sircc_verify_const7_string_ids
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/const7_string_ids.sir.jsonl
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
  NAME sircc_verify_fun_sym_call
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/fun_sym_call.sir.jsonl
)

add_test(
  NAME sircc_verify_closure_make_call
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/closure_make_call.sir.jsonl
)

add_test(
  NAME sircc_verify_adt_make_get
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/adt_make_get.sir.jsonl
)

add_test(
  NAME sircc_verify_sem_match_sum_option_i32
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/sem_match_sum_option_i32.sir.jsonl
)

add_test(
  NAME sircc_verify_feature_gate_late_meta
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/feature_gate_late_meta.sir.jsonl
)

add_test(
  NAME sircc_verify_forward_refs
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/forward_refs.sir.jsonl
)

add_test(
  NAME sircc_diag_bad_fun_missing_feature
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--verify-only\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_fun_missing_feature.sir.jsonl
    "-DEXPECT=requires feature fun:v1"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_closure_missing_feature
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--verify-only\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_closure_missing_feature.sir.jsonl
    "-DEXPECT=requires feature closure:v1"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_adt_missing_feature
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--verify-only\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_adt_missing_feature.sir.jsonl
    "-DEXPECT=requires feature adt:v1"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_adt_get_nullary
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=${CMAKE_CURRENT_LIST_DIR}/examples/bad_adt_get_nullary.sir.jsonl\\;-o\\;${CMAKE_CURRENT_BINARY_DIR}/bad_adt_get_nullary.ll\\;--emit-llvm
    "-DEXPECT=variant 0 is nullary (no payload)"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_sem_missing_feature
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--verify-only\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_sem_missing_feature.sir.jsonl
    "-DEXPECT=requires feature sem:v1"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_simd_missing_feature
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--verify-only\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_simd_missing_feature.sir.jsonl
    "-DEXPECT=requires feature simd:v1"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_simd_vec_lane_unsupported
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--verify-only\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_simd_vec_lane_unsupported.sir.jsonl
    "-DEXPECT=sircc.type.vec.lane.unsupported"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_simd_vec_lanes_zero
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--verify-only\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_simd_vec_lanes_zero.sir.jsonl
    "-DEXPECT=sircc.type.vec.lanes.bad"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_simd_shuffle_idx_len
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--diagnostics\\;json\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_simd_shuffle_idx_len.sir.jsonl\\;-o\\;${CMAKE_CURRENT_BINARY_DIR}/bad_simd_shuffle_idx_len.ll\\;--emit-llvm
    "-DEXPECT=\\\"code\\\":\\\"sircc.vec.shuffle.idx.len_bad\\\""
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_simd_splat_missing_type_ref
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--diagnostics\\;json\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_simd_splat_missing_type_ref.sir.jsonl\\;-o\\;${CMAKE_CURRENT_BINARY_DIR}/bad_simd_splat_missing_type_ref.ll\\;--emit-llvm
    "-DEXPECT=\\\"code\\\":\\\"sircc.vec.splat.missing_type\\\""
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_simd_splat_wrong_type_ref
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--diagnostics\\;json\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_simd_splat_wrong_type_ref.sir.jsonl\\;-o\\;${CMAKE_CURRENT_BINARY_DIR}/bad_simd_splat_wrong_type_ref.ll\\;--emit-llvm
    "-DEXPECT=\\\"code\\\":\\\"sircc.vec.splat.type.bad\\\""
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_ptr_sym_undefined_extern_fn
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    "-DARGS=--diagnostics;json;--verify-only;${CMAKE_CURRENT_LIST_DIR}/examples/bad_ptr_sym_undefined_extern_fn.sir.jsonl"
    "-DEXPECT=\\\"code\\\":\\\"sircc.ptr.sym.unknown\\\""
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_ptr_sym_undefined_extern_fn_mentions_decl_fn
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    "-DARGS=--diagnostics;json;--verify-only;${CMAKE_CURRENT_LIST_DIR}/examples/bad_ptr_sym_undefined_extern_fn.sir.jsonl"
    "-DEXPECT=decl.fn"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_ptr_add_fun
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=${CMAKE_CURRENT_LIST_DIR}/examples/bad_ptr_add_fun.sir.jsonl\\;-o\\;${CMAKE_CURRENT_BINARY_DIR}/bad_ptr_add_fun.ll\\;--emit-llvm
    "-DEXPECT=cannot operate on opaque fun values"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_ptr_to_i64_closure
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=${CMAKE_CURRENT_LIST_DIR}/examples/bad_ptr_to_i64_closure.sir.jsonl\\;-o\\;${CMAKE_CURRENT_BINARY_DIR}/bad_ptr_to_i64_closure.ll\\;--emit-llvm
    "-DEXPECT=cannot operate on opaque closure values"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_fun_sym_conflict_global
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--diagnostics\\;json\\;--verify-only\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_fun_sym_conflict_global.sir.jsonl
    "-DEXPECT=\\\"code\\\":\\\"sircc.fun.sym.conflict_sym\\\""
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_fun_sym_sig_mismatch_vs_fn
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--diagnostics\\;json\\;--verify-only\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_fun_sym_sig_mismatch_vs_fn.sir.jsonl
    "-DEXPECT=\\\"code\\\":\\\"sircc.fun.sym.sig_mismatch\\\""
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_fun_sym_undefined
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--diagnostics\\;json\\;--verify-only\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_fun_sym_undefined.sir.jsonl
    "-DEXPECT=\\\"code\\\":\\\"sircc.fun.sym.undefined\\\""
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_store_ptr_fun
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=${CMAKE_CURRENT_LIST_DIR}/examples/bad_store_ptr_fun.sir.jsonl\\;-o\\;${CMAKE_CURRENT_BINARY_DIR}/bad_store_ptr_fun.ll\\;--emit-llvm
    "-DEXPECT=cannot store opaque fun values"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_closure_make_code_sig_mismatch
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--diagnostics\\;json\\;--verify-only\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_closure_make_code_sig_mismatch.sir.jsonl
    "-DEXPECT=\\\"code\\\":\\\"sircc.closure.make.code.sig_mismatch\\\""
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_sem_if_branch_kind
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--diagnostics\\;json\\;--verify-only\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_sem_if_branch_kind.sir.jsonl
    "-DEXPECT=\\\"code\\\":\\\"sircc.sem.branch.kind.bad\\\""
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_diag_bad_sem_match_sum_thunk_param_ty
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--diagnostics\\;json\\;--verify-only\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_sem_match_sum_thunk_param_ty.sir.jsonl
    "-DEXPECT=\\\"code\\\":\\\"sircc.sem.match_sum.thunk.param.bad\\\""
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_lower_hl_sem_if_thunk_to_cfg
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/sem_if_thunk_trap_not_taken.sir.jsonl
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/sem_if_thunk.core.sir.jsonl
    -DCONTAINS=\\\"tag\\\":\\\"term.cbr\\\"\\;\\\"tag\\\":\\\"bparam\\\"\\;\\\"tag\\\":\\\"call.fun\\\"
    -DNOT_CONTAINS=\\\"tag\\\":\\\"sem.if\\\"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/lower_hl_emit_core_and_verify.cmake
)

add_test(
  NAME sircc_lower_hl_sem_and_sc_thunk_to_cfg
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/sem_and_sc_thunk_trap_not_taken.sir.jsonl
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/sem_and_sc_thunk.core.sir.jsonl
    -DCONTAINS=\\\"tag\\\":\\\"term.cbr\\\"\\;\\\"tag\\\":\\\"bparam\\\"
    -DNOT_CONTAINS=\\\"tag\\\":\\\"sem.and_sc\\\"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/lower_hl_emit_core_and_verify.cmake
)

add_test(
  NAME sircc_lower_hl_sem_or_sc_thunk_to_cfg
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/sem_or_sc_thunk_trap_not_taken.sir.jsonl
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/sem_or_sc_thunk.core.sir.jsonl
    -DCONTAINS=\\\"tag\\\":\\\"term.cbr\\\"\\;\\\"tag\\\":\\\"bparam\\\"
    -DNOT_CONTAINS=\\\"tag\\\":\\\"sem.or_sc\\\"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/lower_hl_emit_core_and_verify.cmake
)

add_test(
  NAME sircc_cinterop_export_add2
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/interop_export_add2.sir.jsonl
    -DHARNESS_C=${CMAKE_CURRENT_LIST_DIR}/tests/interop_export_add2_harness.c
    -DOBJ=${CMAKE_CURRENT_BINARY_DIR}/interop_export_add2.o
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/interop_export_add2.exe
    -DEXPECT=0
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/link_obj_with_c_and_expect_exit.cmake
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
  NAME sircc_diag_bad_fn_linkage_json
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--diagnostics\\;json\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_fn_linkage.sir.jsonl\\;-o\\;${CMAKE_CURRENT_BINARY_DIR}/bad_fn_linkage.ll\\;--emit-llvm
    "-DEXPECT=unsupported linkage"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_require_target_contract_missing
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--require-target-contract\\;--verify-only\\;${CMAKE_CURRENT_LIST_DIR}/examples/add.sir.jsonl
    "-DEXPECT=missing explicit target contract fields"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
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

add_test(
  NAME sircc_lower_hl_sem_if_val_to_select
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/sem_if_val_to_select.core.sir.jsonl
    -DARGS=--lower-hl\\;--emit-sir-core\\;${CMAKE_CURRENT_BINARY_DIR}/sem_if_val_to_select.core.sir.jsonl\\;${CMAKE_CURRENT_LIST_DIR}/examples/sem_if_val_to_select.sir.jsonl
    "-DEXPECT=\\\"tag\\\":\\\"select\\\""
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_output_file_contains.cmake
)

add_test(
  NAME sircc_lower_hl_sem_if_val_strips_sem
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/sem_if_val_to_select.core.sir.jsonl
    -DARGS=--lower-hl\\;--emit-sir-core\\;${CMAKE_CURRENT_BINARY_DIR}/sem_if_val_to_select.core.sir.jsonl\\;${CMAKE_CURRENT_LIST_DIR}/examples/sem_if_val_to_select.sir.jsonl
    "-DNOT_EXPECT=\\\"tag\\\":\\\"sem.if\\\""
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_output_file_not_contains.cmake
)

add_test(
  NAME sircc_verify_simd_splat_extract
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/simd_splat_extract.sir.jsonl
)

add_test(
  NAME sircc_verify_simd_i32_add_extract_replace
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/simd_i32_add_extract_replace.sir.jsonl
)

add_test(
  NAME sircc_verify_simd_cmp_select_bool_mask
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/simd_cmp_select_bool_mask.sir.jsonl
)

add_test(
  NAME sircc_verify_simd_shuffle_two_inputs
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/simd_shuffle_two_inputs.sir.jsonl
)

add_test(
  NAME sircc_verify_simd_f32_mul_nan_canon_bits
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/simd_f32_mul_nan_canon_bits.sir.jsonl
)

add_test(
  NAME sircc_run_global_i32_ptrsym
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/global_i32_ptrsym.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/global_i32_ptrsym.exe
    -DEXPECT=12
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_and_expect_exit.cmake
)

add_test(
  NAME sircc_run_fun_sym_call
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/fun_sym_call.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/fun_sym_call.exe
    -DEXPECT=7
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_and_expect_exit.cmake
)

add_test(
  NAME sircc_run_closure_make_call
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/closure_make_call.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/closure_make_call.exe
    -DEXPECT=12
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_and_expect_exit.cmake
)

add_test(
  NAME sircc_run_adt_make_get
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/adt_make_get.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/adt_make_get.exe
    -DEXPECT=12
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_and_expect_exit.cmake
)

add_test(
  NAME sircc_run_sem_if_thunk_trap_not_taken
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/sem_if_thunk_trap_not_taken.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/sem_if_thunk_trap_not_taken.exe
    -DEXPECT=7
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_and_expect_exit.cmake
)

add_test(
  NAME sircc_run_sem_match_sum_option_i32
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/sem_match_sum_option_i32.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/sem_match_sum_option_i32.exe
    -DEXPECT=12
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_and_expect_exit.cmake
)

add_test(
  NAME sircc_run_global_array_const
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/global_array_const.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/global_array_const.exe
    -DEXPECT=10
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_and_expect_exit.cmake
)

add_test(
  NAME sircc_run_global_array_repeat
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/global_array_repeat.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/global_array_repeat.exe
    -DEXPECT=28
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_and_expect_exit.cmake
)

add_test(
  NAME sircc_run_struct_layout
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/struct_layout.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/struct_layout.exe
    -DEXPECT=12
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_and_expect_exit.cmake
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
  NAME sircc_run_const7_string_ids
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/const7_string_ids.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/const7_string_ids.exe
    -DEXPECT=7
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
  NAME sircc_run_simd_splat_extract
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/simd_splat_extract.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/simd_splat_extract.exe
    -DEXPECT=7
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_and_expect_exit.cmake
)

add_test(
  NAME sircc_run_simd_i32_add_extract_replace
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/simd_i32_add_extract_replace.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/simd_i32_add_extract_replace.exe
    -DEXPECT=9
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_and_expect_exit.cmake
)

add_test(
  NAME sircc_run_simd_cmp_select_bool_mask
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/simd_cmp_select_bool_mask.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/simd_cmp_select_bool_mask.exe
    -DEXPECT=7
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_and_expect_exit.cmake
)

add_test(
  NAME sircc_run_simd_shuffle_two_inputs
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/simd_shuffle_two_inputs.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/simd_shuffle_two_inputs.exe
    -DEXPECT=6
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_and_expect_exit.cmake
)

add_test(
  NAME sircc_run_simd_f32_mul_nan_canon_bits
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/simd_f32_mul_nan_canon_bits.sir.jsonl
    -DEXE=${CMAKE_CURRENT_BINARY_DIR}/simd_f32_mul_nan_canon_bits.exe
    -DEXPECT=7
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
  NAME sircc_emit_llvm_adt_layout_i64_payload_has_pad
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=${CMAKE_CURRENT_LIST_DIR}/examples/adt_layout_i64_payload.sir.jsonl\\;-o\\;${CMAKE_CURRENT_BINARY_DIR}/adt_layout_i64_payload.ll\\;--emit-llvm
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/adt_layout_i64_payload.ll
    "-DEXPECT={ i32, [4 x i8], [8 x i8] }"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_output_file_contains.cmake
)

add_test(
  NAME sircc_emit_llvm_adt_wrong_variant_traps_has_trap
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=${CMAKE_CURRENT_LIST_DIR}/examples/adt_wrong_variant_traps.sir.jsonl\\;-o\\;${CMAKE_CURRENT_BINARY_DIR}/adt_wrong_variant_traps.ll\\;--emit-llvm
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/adt_wrong_variant_traps.ll
    "-DEXPECT=llvm.trap"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_output_file_contains.cmake
)

add_test(
  NAME sircc_emit_llvm_adt_make_variant_oob_traps_has_trap
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=${CMAKE_CURRENT_LIST_DIR}/examples/adt_make_variant_oob_traps.sir.jsonl\\;-o\\;${CMAKE_CURRENT_BINARY_DIR}/adt_make_variant_oob_traps.ll\\;--emit-llvm
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/adt_make_variant_oob_traps.ll
    "-DEXPECT=llvm.trap"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_output_file_contains.cmake
)

add_test(
  NAME sircc_emit_llvm_simd_extract_oob_traps_has_trap
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=${CMAKE_CURRENT_LIST_DIR}/examples/simd_extract_oob_traps.sir.jsonl\\;-o\\;${CMAKE_CURRENT_BINARY_DIR}/simd_extract_oob_traps.ll\\;--emit-llvm
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/simd_extract_oob_traps.ll
    "-DEXPECT=llvm.trap"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_output_file_contains.cmake
)

add_test(
  NAME sircc_emit_llvm_simd_replace_oob_traps_has_trap
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=${CMAKE_CURRENT_LIST_DIR}/examples/simd_replace_oob_traps.sir.jsonl\\;-o\\;${CMAKE_CURRENT_BINARY_DIR}/simd_replace_oob_traps.ll\\;--emit-llvm
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/simd_replace_oob_traps.ll
    "-DEXPECT=llvm.trap"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_output_file_contains.cmake
)

add_test(
  NAME sircc_emit_llvm_simd_shuffle_oob_traps_has_trap
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=${CMAKE_CURRENT_LIST_DIR}/examples/simd_shuffle_oob_traps.sir.jsonl\\;-o\\;${CMAKE_CURRENT_BINARY_DIR}/simd_shuffle_oob_traps.ll\\;--emit-llvm
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/simd_shuffle_oob_traps.ll
    "-DEXPECT=llvm.trap"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_output_file_contains.cmake
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

  add_test(
    NAME sircc_emit_zasm_hello_zabi25_write_map
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/hello_zabi25_write.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/hello_zabi25_write.map.zasm.jsonl
      -DMAP=${CMAKE_CURRENT_BINARY_DIR}/hello_zabi25_write.zasm.map.jsonl
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_map.cmake
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
    NAME sircc_emit_zasm_run_zem_struct_layout_exit12
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_struct_layout.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_struct_layout.zem.zasm.jsonl
      "-DEXPECT_STDOUT=************"
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

  add_test(
    NAME sircc_emit_zasm_run_zem_load_store_name_prints_Z
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_load_store_name_print.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_load_store_name_print.zem.zasm.jsonl
      "-DEXPECT_STDOUT=Z"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_ptr_add_disp_prints_Z
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_ptr_add_disp_print.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_ptr_add_disp_print.zem.zasm.jsonl
      "-DEXPECT_STDOUT=Z"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_i32_add_len_prints_two_stars
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_i32_add_len_print.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_i32_add_len_print.zem.zasm.jsonl
      "-DEXPECT_STDOUT=**"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_cfg_condbr_prints_F
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_cfg_condbr_print.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_cfg_condbr_print.zem.zasm.jsonl
      "-DEXPECT_STDOUT=F"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_cfg_join_args_prints_F
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_cfg_join_args_print.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_cfg_join_args_print.zem.zasm.jsonl
      "-DEXPECT_STDOUT=F"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_cfg_store_load_let_prints_Z
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_cfg_store_load_let_print.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_cfg_store_load_let_print.zem.zasm.jsonl
      "-DEXPECT_STDOUT=Z"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_cfg_condbr_ne_prints_T
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_cfg_condbr_ne_print_T.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_cfg_condbr_ne_print_T.zem.zasm.jsonl
      "-DEXPECT_STDOUT=T"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_cfg_condbr_ult_prints_T
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_cfg_condbr_ult_print_T.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_cfg_condbr_ult_print_T.zem.zasm.jsonl
      "-DEXPECT_STDOUT=T"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_cfg_condbr_sgt_slot_prints_T
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_cfg_condbr_sgt_slot_print_T.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_cfg_condbr_sgt_slot_print_T.zem.zasm.jsonl
      "-DEXPECT_STDOUT=T"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_ptr_add_dynamic_prints_C
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_ptr_add_dynamic_print_C.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_ptr_add_dynamic_print_C.zem.zasm.jsonl
      "-DEXPECT_STDOUT=C"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_ptr_offset_dynamic_prints_D
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_ptr_offset_dynamic_print_D.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_ptr_offset_dynamic_print_D.zem.zasm.jsonl
      "-DEXPECT_STDOUT=D"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_mem_fill_copy_ptrs_prints_G
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_mem_fill_copy_ptrs_print_G.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_mem_fill_copy_ptrs_print_G.zem.zasm.jsonl
      "-DEXPECT_STDOUT=G"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_i32_mul_len_prints_MM
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_i32_mul_len_print_MM.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_i32_mul_len_print_MM.zem.zasm.jsonl
      "-DEXPECT_STDOUT=MM"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_i32_and_len_prints_AA
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_i32_and_len_print_AA.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_i32_and_len_print_AA.zem.zasm.jsonl
      "-DEXPECT_STDOUT=AA"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_i32_shl_len_prints_SS
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_i32_shl_len_print_SS.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_i32_shl_len_print_SS.zem.zasm.jsonl
      "-DEXPECT_STDOUT=SS"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_i32_div_len_prints_DD
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_i32_div_len_print_DD.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_i32_div_len_print_DD.zem.zasm.jsonl
      "-DEXPECT_STDOUT=DD"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_i32_rem_len_prints_RR
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_i32_rem_len_print_RR.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_i32_rem_len_print_RR.zem.zasm.jsonl
      "-DEXPECT_STDOUT=RR"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_i32_clz_len_prints_CC
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_i32_clz_len_print_CC.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_i32_clz_len_print_CC.zem.zasm.jsonl
      "-DEXPECT_STDOUT=CC"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_i32_ctz_len_prints_TT
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_i32_ctz_len_print_TT.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_i32_ctz_len_print_TT.zem.zasm.jsonl
      "-DEXPECT_STDOUT=TT"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_i32_popc_len_prints_PP
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_i32_popc_len_print_PP.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_i32_popc_len_print_PP.zem.zasm.jsonl
      "-DEXPECT_STDOUT=PP"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_cfg_loop_fill3_prints_XXX
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_cfg_loop_fill3_print.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_cfg_loop_fill3_print.zem.zasm.jsonl
      "-DEXPECT_STDOUT=XXX"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_i64_clz_ptr_add_prints_E
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_i64_clz_ptr_add_print_E.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_i64_clz_ptr_add_print_E.zem.zasm.jsonl
      "-DEXPECT_STDOUT=E"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_i64_ctz_ptr_add_prints_D
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_i64_ctz_ptr_add_print_D.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_i64_ctz_ptr_add_print_D.zem.zasm.jsonl
      "-DEXPECT_STDOUT=D"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_i64_popc_ptr_add_prints_Z
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_i64_popc_ptr_add_print_Z.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_i64_popc_ptr_add_print_Z.zem.zasm.jsonl
      "-DEXPECT_STDOUT=Z"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )

  add_test(
    NAME sircc_emit_zasm_run_zem_regcache_store_invalidate_prints_B
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_regcache_store_invalidate_print_B.sir.jsonl
      -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_regcache_store_invalidate_print_B.zem.zasm.jsonl
      "-DEXPECT_STDOUT=B"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_run_zem.cmake
  )
endif()

add_test(
  NAME sircc_emit_zasm_name_const_return_binds
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_name_const_return.sir.jsonl
    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_name_const_return.zasm.jsonl
    "-DEXPECT=\"m\":\"LD\",\"ops\":[{\"t\":\"reg\",\"v\":\"HL\"},{\"t\":\"num\",\"v\":7}]"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_expect_contains.cmake
)

add_test(
  NAME sircc_emit_zasm_name_call_result_spills
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/zasm_name_call_result_return.sir.jsonl
    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/zasm_name_call_result_return.zasm.jsonl
    "-DEXPECT=\"m\":\"ST64\""
    "-DEXPECT2=\"m\":\"LD64\""
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/emit_zasm_and_expect_contains.cmake
)

add_test(
  NAME sircc_emit_zasm_diag_unsupported_value_node_json
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--diagnostics\\;json\\;${CMAKE_CURRENT_LIST_DIR}/examples/zasm_bad_unsupported_value_node_diag.sir.jsonl\\;-o\\;${CMAKE_CURRENT_BINARY_DIR}/zasm_bad_unsupported_value_node_diag.zasm.jsonl\\;--emit-zasm
    -DEXPECT=\\\"k\\\":\\\"diag\\\"
    -DEXPECT2=\\\"about\\\":{\\\"k\\\":\\\"node\\\"
    -DEXPECT3=\\\"code\\\":\\\"sircc.zasm.value.unsupported\\\"
    -DEXPECT4=\\\"tag\\\":\\\"i32.add\\\"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_emit_zasm_diag_ptr_offset_missing_ty_json
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--diagnostics\\;json\\;${CMAKE_CURRENT_LIST_DIR}/examples/zasm_bad_ptr_offset_missing_ty.sir.jsonl\\;-o\\;${CMAKE_CURRENT_BINARY_DIR}/zasm_bad_ptr_offset_missing_ty.zasm.jsonl\\;--emit-zasm
    -DEXPECT=\\\"k\\\":\\\"diag\\\"
    -DEXPECT2=\\\"about\\\":{\\\"k\\\":\\\"node\\\"
    -DEXPECT3=\\\"tag\\\":\\\"ptr.offset\\\"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

add_test(
  NAME sircc_emit_zasm_diag_call_too_many_args_json
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--diagnostics\\;json\\;${CMAKE_CURRENT_LIST_DIR}/examples/zasm_bad_call_too_many_args.sir.jsonl\\;-o\\;${CMAKE_CURRENT_BINARY_DIR}/zasm_bad_call_too_many_args.zasm.jsonl\\;--emit-zasm
    -DEXPECT=\\\"k\\\":\\\"diag\\\"
    -DEXPECT2=\\\"about\\\":{\\\"k\\\":\\\"node\\\"
    -DEXPECT3=\\\"tag\\\":\\\"call.indirect\\\"
    -P ${CMAKE_CURRENT_LIST_DIR}/tests/expect_stderr_contains.cmake
)

if(SIRCC_ENABLE_LOWER_COMPARE_TESTS AND APPLE AND EXISTS "${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/lower" AND EXISTS "${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck" AND EXISTS "${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/lib/libzingcore25.a")
  add_test(
    NAME sircc_compare_lower_vs_llvm_hello_zabi25_write
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DLOWER=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/lower
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DZABI25_ROOT=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/hello_zabi25_write.sir.jsonl
      -DOUT_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/compare_lower_hello_zabi25_write
      "-DEXPECT_STDOUT=hello from zABI25"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/compare_llvm_vs_lower.cmake
  )

  add_test(
    NAME sircc_compare_lower_vs_llvm_mem_copy_fill_zir_main
    COMMAND ${CMAKE_COMMAND}
      -DSIRCC=$<TARGET_FILE:sircc>
      -DLOWER=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/lower
      -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
      -DZABI25_ROOT=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/mem_copy_fill_zir_main.sir.jsonl
      -DOUT_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/compare_lower_mem_copy_fill_zir_main
      "-DEXPECT_STDOUT=*"
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/compare_llvm_vs_lower.cmake
  )

  if(EXISTS "${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem")
    add_test(
      NAME sircc_compare_lower_vs_llvm_hello_zabi25_write_pgo_len
      COMMAND ${CMAKE_COMMAND}
        -DSIRCC=$<TARGET_FILE:sircc>
        -DLOWER=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/lower
        -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
        -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
        -DZABI25_ROOT=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64
        -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/hello_zabi25_write.sir.jsonl
        -DOUT_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/compare_lower_hello_zabi25_write_pgo_len
        "-DEXPECT_STDOUT=hello from zABI25"
        -P ${CMAKE_CURRENT_LIST_DIR}/tests/compare_llvm_vs_lower.cmake
    )

    add_test(
      NAME sircc_compare_lower_vs_llvm_mem_copy_fill_zir_main_pgo_len
      COMMAND ${CMAKE_COMMAND}
        -DSIRCC=$<TARGET_FILE:sircc>
        -DLOWER=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/lower
        -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
        -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
        -DZABI25_ROOT=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64
        -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/mem_copy_fill_zir_main.sir.jsonl
        -DOUT_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/compare_lower_mem_copy_fill_zir_main_pgo_len
        "-DEXPECT_STDOUT=*"
        -P ${CMAKE_CURRENT_LIST_DIR}/tests/compare_llvm_vs_lower.cmake
    )

    add_test(
      NAME sircc_compare_lower_vs_llvm_mem_copy_fill_zir_main_strip_uncovered
      COMMAND ${CMAKE_COMMAND}
        -DSIRCC=$<TARGET_FILE:sircc>
        -DLOWER=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/lower
        -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
        -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
        -DZABI25_ROOT=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64
        -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/mem_copy_fill_zir_main.sir.jsonl
        -DOUT_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/compare_lower_mem_copy_fill_zir_main_strip_uncovered
        "-DEXPECT_STDOUT=*"
        -DSTRIP_MODE=uncovered-delete
        -P ${CMAKE_CURRENT_LIST_DIR}/tests/compare_llvm_vs_lower.cmake
    )
  endif()
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
  NAME sircc_diag_json_bad_operand_has_code
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DARGS=--diagnostics\\;json\\;--verify-only\\;${CMAKE_CURRENT_LIST_DIR}/examples/bad_instr_operand.sir.jsonl
    -DEXPECT=\\\"code\\\":\\\"sircc.schema.value.num.bad\\\"
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
