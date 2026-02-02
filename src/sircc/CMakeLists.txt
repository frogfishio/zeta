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
  NAME sircc_verify_call_mnemonic_tags
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/call_mnemonic_tags.sir.jsonl
)

add_test(
  NAME sircc_verify_cfg_if
  COMMAND sircc --verify-only ${CMAKE_CURRENT_LIST_DIR}/examples/cfg_if.sir.jsonl
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
