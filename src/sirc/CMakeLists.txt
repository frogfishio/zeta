cmake_minimum_required(VERSION 3.20)

# Allow building this directory standalone (useful for quick iteration):
#   cmake -S src/sirc -B build/sirc && cmake --build build/sirc
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(sirc LANGUAGES C)
  set(CMAKE_C_STANDARD 11)
  set(CMAKE_C_STANDARD_REQUIRED ON)
  set(CMAKE_C_EXTENSIONS OFF)
  include(CTest)
endif()

if(NOT DEFINED SIR_VERSION OR SIR_VERSION STREQUAL "")
  set(SIR_VERSION "0.0.0")
endif()

# Prefer Homebrew bison/flex on macOS (Apple's system bison is often very old).
find_program(SIRC_BISON
  NAMES bison
  HINTS
    /opt/homebrew/opt/bison/bin
    /usr/local/opt/bison/bin
)
find_program(SIRC_FLEX
  NAMES flex
  HINTS
    /opt/homebrew/opt/flex/bin
    /usr/local/opt/flex/bin
)

if(NOT SIRC_BISON)
  message(FATAL_ERROR "sirc: bison not found (install via Homebrew: brew install bison)")
endif()
if(NOT SIRC_FLEX)
  message(FATAL_ERROR "sirc: flex not found (install via Homebrew: brew install flex)")
endif()

set(SIRC_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR})
set(SIRC_BISON_C ${SIRC_GEN_DIR}/sir.tab.c)
set(SIRC_BISON_H ${SIRC_GEN_DIR}/sir.tab.h)
set(SIRC_FLEX_C  ${SIRC_GEN_DIR}/sir.yy.c)

add_custom_command(
  OUTPUT ${SIRC_BISON_C} ${SIRC_BISON_H}
  COMMAND ${SIRC_BISON} -d -o ${SIRC_BISON_C} ${CMAKE_CURRENT_LIST_DIR}/sir.y
  DEPENDS ${CMAKE_CURRENT_LIST_DIR}/sir.y
  VERBATIM
)

add_custom_command(
  OUTPUT ${SIRC_FLEX_C}
  COMMAND ${SIRC_FLEX} -o ${SIRC_FLEX_C} ${CMAKE_CURRENT_LIST_DIR}/sir.l
  DEPENDS ${CMAKE_CURRENT_LIST_DIR}/sir.l ${SIRC_BISON_H}
  VERBATIM
)

add_executable(sirc
  sirc.c
  ${SIRC_BISON_C}
  ${SIRC_FLEX_C}
)

target_compile_definitions(sirc PRIVATE SIR_VERSION="${SIR_VERSION}")

set_source_files_properties(
  ${SIRC_BISON_C}
  ${SIRC_FLEX_C}
  PROPERTIES
    COMPILE_OPTIONS "-Wno-error;-Wno-unused-function;-Wno-unneeded-internal-declaration"
)

target_include_directories(sirc PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${SIRC_GEN_DIR}
)

target_compile_options(sirc PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)

#
# Tests (require sircc)
#
if(TARGET sircc)
  function(sirc_add_verify_test name input_rel)
    add_test(
      NAME ${name}
      COMMAND ${CMAKE_COMMAND}
        -DSIRC=$<TARGET_FILE:sirc>
        -DSIRCC=$<TARGET_FILE:sircc>
        -DINPUT=${CMAKE_CURRENT_LIST_DIR}/${input_rel}
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/${name}.sir.jsonl
        -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_sirc_then_sircc_verify.cmake
    )
  endfunction()

  sirc_add_verify_test(sirc_verify_hello examples/hello.sir)
  sirc_add_verify_test(sirc_verify_add examples/add.sir)
	  sirc_add_verify_test(sirc_verify_mem_stack examples/mem_stack.sir)
	  sirc_add_verify_test(sirc_verify_mem_copy_fill examples/mem_copy_fill.sir)
	  sirc_add_verify_test(sirc_verify_eff_fence examples/eff_fence.sir)
	  sirc_add_verify_test(sirc_verify_abs_i32 examples/abs_i32.sir)
	  sirc_add_verify_test(sirc_verify_float_trunc examples/float_trunc.sir)
	  sirc_add_verify_test(sirc_verify_features examples/features.sir)
	  sirc_add_verify_test(sirc_verify_ptr_layout examples/ptr_layout.sir)
	  sirc_add_verify_test(sirc_verify_alloca_array examples/alloca_array.sir)
	  sirc_add_verify_test(sirc_verify_call_indirect_ptrsym examples/call_indirect_ptrsym.sir)
	  sirc_add_verify_test(sirc_verify_load_store_no_align examples/load_store_no_align.sir)
	  sirc_add_verify_test(sirc_verify_trap examples/trap.sir)
  sirc_add_verify_test(sirc_verify_cfg_if examples/cfg_if.sir)
  sirc_add_verify_test(sirc_verify_cfg_switch examples/cfg_switch.sir)
  sirc_add_verify_test(sirc_verify_cfg_join_phi examples/cfg_join_phi.sir)
  sirc_add_verify_test(sirc_verify_fun_sym_call examples/fun_sym_call.sir)
  sirc_add_verify_test(sirc_verify_closure_make_call examples/closure_make_call.sir)
  sirc_add_verify_test(sirc_verify_adt_make_get examples/adt_make_get.sir)
  sirc_add_verify_test(sirc_verify_sem_if_val_val examples/sem_if_val_val.sir)
  sirc_add_verify_test(sirc_verify_sem_if_thunk_trap_not_taken examples/sem_if_thunk_trap_not_taken.sir)
  sirc_add_verify_test(sirc_verify_sem_and_or_sc examples/sem_and_or_sc.sir)
  sirc_add_verify_test(sirc_verify_sem_switch_thunk examples/sem_switch_thunk.sir)
  sirc_add_verify_test(sirc_verify_sem_match_sum_payload_thunk examples/sem_match_sum_payload_thunk.sir)
  sirc_add_verify_test(sirc_verify_sem_while_break examples/sem_while_break.sir)
  sirc_add_verify_test(sirc_verify_sem_defer_void examples/sem_defer_void.sir)
  sirc_add_verify_test(sirc_verify_sem_scope_cfg examples/sem_scope_cfg.sir)
  sirc_add_verify_test(sirc_verify_sendloop_mono examples/sendloop_mono.sir)
  sirc_add_verify_test(sirc_verify_sendloop_uniform examples/sendloop_uniform.sir)

  add_test(
    NAME sirc_lint_add
    COMMAND ${CMAKE_COMMAND}
      -DSIRC=$<TARGET_FILE:sirc>
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/examples/add.sir
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_sirc_lint.cmake
  )

  add_test(
    NAME sirc_diag_json_bad_syntax
    COMMAND ${CMAKE_COMMAND}
      -DSIRC=$<TARGET_FILE:sirc>
      -DINPUT=${CMAKE_CURRENT_LIST_DIR}/tests/fixtures/bad_syntax.sir
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_sirc_diag_json.cmake
  )

  add_test(
    NAME sirc_tool_multi_hello_add
    COMMAND ${CMAKE_COMMAND}
      -DSIRC=$<TARGET_FILE:sirc>
      -DINPUT1=${CMAKE_CURRENT_LIST_DIR}/examples/hello.sir
      -DINPUT2=${CMAKE_CURRENT_LIST_DIR}/examples/add.sir
      -DOUT=${CMAKE_CURRENT_BINARY_DIR}/sirc_tool_multi_hello_add.jsonl
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_sirc_tool_multi.cmake
  )

  if(APPLE AND EXISTS "${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem" AND EXISTS "${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck")
    function(sirc_add_zem_test name input_rel expect_stdout)
      add_test(
        NAME ${name}
        COMMAND ${CMAKE_COMMAND}
          -DSIRC=$<TARGET_FILE:sirc>
          -DSIRCC=$<TARGET_FILE:sircc>
          -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
          -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
          -DINPUT_SIR=${CMAKE_CURRENT_LIST_DIR}/${input_rel}
          -DOUT_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/${name}
          "-DEXPECT_STDOUT=${expect_stdout}"
          -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_sirc_then_emit_zasm_and_run_zem.cmake
      )
    endfunction()

    sirc_add_zem_test(sirc_zem_sendloop_mono examples/sendloop_mono.sir "A")
    sirc_add_zem_test(sirc_zem_sendloop_uniform examples/sendloop_uniform.sir "D")
  endif()

  if(SIRCC_ENABLE_LOWER_COMPARE_TESTS AND APPLE AND EXISTS "${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/lower" AND EXISTS "${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck" AND EXISTS "${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/lib/libzingcore25.a")
    function(sirc_add_compare_lower_test name input_rel expect_stdout)
      add_test(
        NAME ${name}
        COMMAND ${CMAKE_COMMAND}
          -DSIRC=$<TARGET_FILE:sirc>
          -DSIRCC=$<TARGET_FILE:sircc>
          -DLOWER=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/lower
          -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
          -DZABI25_ROOT=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64
          -DINPUT_SIR=${CMAKE_CURRENT_LIST_DIR}/${input_rel}
          -DOUT_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/${name}
          "-DEXPECT_STDOUT=${expect_stdout}"
          -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_sirc_then_compare_lower_vs_llvm.cmake
      )
    endfunction()

    sirc_add_compare_lower_test(sirc_compare_lower_vs_llvm_sendloop_mono examples/sendloop_mono.sir "A")
    sirc_add_compare_lower_test(sirc_compare_lower_vs_llvm_sendloop_uniform examples/sendloop_uniform.sir "D")

    if(EXISTS "${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem")
      function(sirc_add_compare_lower_pgo_test name input_rel expect_stdout)
        add_test(
          NAME ${name}
          COMMAND ${CMAKE_COMMAND}
            -DSIRC=$<TARGET_FILE:sirc>
            -DSIRCC=$<TARGET_FILE:sircc>
            -DLOWER=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/lower
            -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
            -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
            -DZABI25_ROOT=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64
            -DINPUT_SIR=${CMAKE_CURRENT_LIST_DIR}/${input_rel}
            -DOUT_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/${name}
            "-DEXPECT_STDOUT=${expect_stdout}"
            -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_sirc_then_compare_lower_vs_llvm.cmake
        )
      endfunction()

      sirc_add_compare_lower_pgo_test(sirc_compare_lower_vs_llvm_sendloop_mono_pgo_len examples/sendloop_mono.sir "A")
      sirc_add_compare_lower_pgo_test(sirc_compare_lower_vs_llvm_sendloop_uniform_pgo_len examples/sendloop_uniform.sir "D")

      function(sirc_add_compare_lower_strip_test name input_rel expect_stdout)
        add_test(
          NAME ${name}
          COMMAND ${CMAKE_COMMAND}
            -DSIRC=$<TARGET_FILE:sirc>
            -DSIRCC=$<TARGET_FILE:sircc>
            -DLOWER=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/lower
            -DIRCHECK=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/ircheck
            -DZEM=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/bin/zem
            -DZABI25_ROOT=${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64
            -DINPUT_SIR=${CMAKE_CURRENT_LIST_DIR}/${input_rel}
            -DOUT_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/${name}
            "-DEXPECT_STDOUT=${expect_stdout}"
            -DSTRIP_MODE=uncovered-delete
            -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_sirc_then_compare_lower_vs_llvm.cmake
        )
      endfunction()

      sirc_add_compare_lower_strip_test(sirc_compare_lower_vs_llvm_sendloop_mono_strip_uncovered examples/sendloop_mono.sir "A")
      sirc_add_compare_lower_strip_test(sirc_compare_lower_vs_llvm_sendloop_uniform_strip_uncovered examples/sendloop_uniform.sir "D")
    endif()
  endif()
endif()
