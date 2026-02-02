cmake_minimum_required(VERSION 3.20)

add_executable(sircc
  main.c
  compiler.c
  sircc.c
  json.c
)

target_include_directories(sircc PRIVATE ${CMAKE_CURRENT_LIST_DIR})

find_package(LLVM REQUIRED CONFIG)

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
set_tests_properties(sircc_verify_error_uses_src_ref PROPERTIES WILL_FAIL TRUE)
