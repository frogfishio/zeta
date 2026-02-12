cmake_minimum_required(VERSION 3.20)

add_executable(sem
  sem.c
  sem_hosted.c
  sir_jsonl.c
  zi_tape.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem PRIVATE SIR_VERSION="${SIR_VERSION}")
target_include_directories(sem PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem PRIVATE sircore_hosted_zabi sircore_vm sircore_module)

target_compile_options(sem PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)

add_executable(sem_unit_caps_list
  tests/test_caps_list.c
)

target_compile_definitions(sem_unit_caps_list PRIVATE SIR_VERSION="${SIR_VERSION}")
target_include_directories(sem_unit_caps_list PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore)
target_link_libraries(sem_unit_caps_list PRIVATE sircore_hosted_zabi)

target_compile_options(sem_unit_caps_list PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)

add_test(NAME sem_caps_list_empty COMMAND sem_unit_caps_list)

add_executable(sem_unit_sem_host_env_argv
  tests/test_sem_host_env_argv.c
)

target_compile_definitions(sem_unit_sem_host_env_argv PRIVATE SIR_VERSION="${SIR_VERSION}")
target_include_directories(sem_unit_sem_host_env_argv PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore)
target_link_libraries(sem_unit_sem_host_env_argv PRIVATE sircore_hosted_zabi)

target_compile_options(sem_unit_sem_host_env_argv PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)

add_test(NAME sem_sem_host_env_argv COMMAND sem_unit_sem_host_env_argv)

add_executable(sem_unit_semrt_write
  tests/test_semrt_write.c
)

target_compile_definitions(sem_unit_semrt_write PRIVATE SIR_VERSION="${SIR_VERSION}")
target_include_directories(sem_unit_semrt_write PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore)
target_link_libraries(sem_unit_semrt_write PRIVATE sircore_hosted_zabi)

target_compile_options(sem_unit_semrt_write PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)

add_test(NAME sem_semrt_write COMMAND sem_unit_semrt_write)

# Optional: zingcore25 availability (mirrors sircore_hosted_zabi integration conditions).
set(SEM_HAVE_ZINGCORE25 OFF)
if(APPLE AND EXISTS "${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/lib/libzingcore25.a" AND EXISTS "${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/include/zi_sysabi25.h")
  set(SEM_HAVE_ZINGCORE25 ON)
endif()

if(SEM_HAVE_ZINGCORE25)
  add_executable(sem_unit_semrt_file_aio
    tests/test_semrt_file_aio.c
  )

  target_compile_definitions(sem_unit_semrt_file_aio PRIVATE SIR_VERSION="${SIR_VERSION}")
  target_include_directories(sem_unit_semrt_file_aio PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore)
  target_link_libraries(sem_unit_semrt_file_aio PRIVATE sircore_hosted_zabi sircore_runtime)

  target_compile_options(sem_unit_semrt_file_aio PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
  )

  add_test(NAME sem_semrt_file_aio COMMAND sem_unit_semrt_file_aio)
endif()

add_executable(sem_unit_semrt_proc_env_argv
  tests/test_semrt_proc_env_argv.c
)

target_compile_definitions(sem_unit_semrt_proc_env_argv PRIVATE SIR_VERSION="${SIR_VERSION}")
target_include_directories(sem_unit_semrt_proc_env_argv PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore)
target_link_libraries(sem_unit_semrt_proc_env_argv PRIVATE sircore_hosted_zabi)

target_compile_options(sem_unit_semrt_proc_env_argv PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)

add_test(NAME sem_semrt_proc_env_argv COMMAND sem_unit_semrt_proc_env_argv)

add_executable(sem_unit_semrt_sys_loop_timer
  tests/test_semrt_sys_loop_timer.c
)

target_compile_definitions(sem_unit_semrt_sys_loop_timer PRIVATE SIR_VERSION="${SIR_VERSION}")
target_include_directories(sem_unit_semrt_sys_loop_timer PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore)
target_link_libraries(sem_unit_semrt_sys_loop_timer PRIVATE sircore_hosted_zabi)

target_compile_options(sem_unit_semrt_sys_loop_timer PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)

add_test(NAME sem_semrt_sys_loop_timer COMMAND sem_unit_semrt_sys_loop_timer)

add_test(
  NAME sem_run_hello_zabi25_write
  COMMAND $<TARGET_FILE:sem> --run ${CMAKE_SOURCE_DIR}/src/sircc/examples/hello_zabi25_write.sir.jsonl
)

add_test(
  NAME sem_run_hello_zabi25_caps_list
  COMMAND $<TARGET_FILE:sem> --run ${CMAKE_SOURCE_DIR}/src/sircc/examples/hello_zabi25_caps_list.sir.jsonl
)

add_test(
  NAME sem_run_hello_zabi25_file_cat
  COMMAND $<TARGET_FILE:sem>
    --cat /hello.txt
    --fs-root ${CMAKE_SOURCE_DIR}/src/sem/tests/fixtures/file_root
)

# Regression: ensure sirc-emitted stable IDs for @include-heavy .sir inputs do not collide
# in a way that prevents sem from loading/verifying the program.
# Note: top-level CMake adds `src/sem` before `src/sirc`, so `if(TARGET sirc)`
# would always be false here. Gate on the option instead.
if(SIR_ENABLE_SIRC)
  add_test(
    NAME sem_verify_sirc_include_relpath_collision
    COMMAND ${CMAKE_COMMAND}
      -DSIRC=$<TARGET_FILE:sirc>
      -DSEM=$<TARGET_FILE:sem>
      -DINPUT=${CMAKE_SOURCE_DIR}/src/sirc/tests/fixtures/include_relpath_collision/root.sir
      -DOUT=${CMAKE_CURRENT_BINARY_DIR}/sem_verify_sirc_include_relpath_collision.sir.jsonl
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_sirc_then_sem_verify.cmake
  )

  add_test(
    NAME sem_run_guestlib_caps_list_demo
    COMMAND ${CMAKE_COMMAND}
      -DSIRC=$<TARGET_FILE:sirc>
      -DSEM=$<TARGET_FILE:sem>
      -DINPUT=${CMAKE_SOURCE_DIR}/src/guestlib/examples/caps_list_demo.sir
      -DOUT=${CMAKE_CURRENT_BINARY_DIR}/sem_run_guestlib_caps_list_demo.sir.jsonl
      -DSEM_ARGS=--fs-root\;${CMAKE_SOURCE_DIR}/src/sem/tests/fixtures/file_root
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_sirc_then_sem_run.cmake
  )

  add_test(
    NAME sem_run_guestlib_ctl_error_trace_demo
    COMMAND ${CMAKE_COMMAND}
      -DSIRC=$<TARGET_FILE:sirc>
      -DSEM=$<TARGET_FILE:sem>
      -DINPUT=${CMAKE_SOURCE_DIR}/src/guestlib/examples/ctl_error_trace_demo.sir
      -DOUT=${CMAKE_CURRENT_BINARY_DIR}/sem_run_guestlib_ctl_error_trace_demo.sir.jsonl
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_sirc_then_sem_run.cmake
  )

  add_test(
    NAME sem_run_guestlib_stdout_ctrl_demo
    COMMAND ${CMAKE_COMMAND}
      -DSIRC=$<TARGET_FILE:sirc>
      -DSEM=$<TARGET_FILE:sem>
      -DINPUT=${CMAKE_SOURCE_DIR}/src/guestlib/examples/stdout_ctrl_demo.sir
      -DOUT=${CMAKE_CURRENT_BINARY_DIR}/sem_run_guestlib_stdout_ctrl_demo.sir.jsonl
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_sirc_then_sem_run.cmake
  )

  add_test(
    NAME sem_run_guestlib_stdin_echo_demo
    COMMAND ${CMAKE_COMMAND}
      -DSIRC=$<TARGET_FILE:sirc>
      -DSEM=$<TARGET_FILE:sem>
      -DINPUT=${CMAKE_SOURCE_DIR}/src/guestlib/examples/stdin_echo_demo.sir
      -DOUT=${CMAKE_CURRENT_BINARY_DIR}/sem_run_guestlib_stdin_echo_demo.sir.jsonl
      -DSEM_STDIN_FILE=${CMAKE_SOURCE_DIR}/src/guestlib/examples/fixtures/stdin_abcde.txt
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_sirc_then_sem_run.cmake
  )

  add_test(
    NAME sem_run_guestlib_loop_sleep_demo
    COMMAND ${CMAKE_COMMAND}
      -DSIRC=$<TARGET_FILE:sirc>
      -DSEM=$<TARGET_FILE:sem>
      -DINPUT=${CMAKE_SOURCE_DIR}/src/guestlib/examples/loop_sleep_demo.sir
      -DOUT=${CMAKE_CURRENT_BINARY_DIR}/sem_run_guestlib_loop_sleep_demo.sir.jsonl
      -DSEM_ARGS=--enable\;sys:loop
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/run_sirc_then_sem_run.cmake
  )
endif()

add_executable(sem_unit_diff_sem_vs_sircc_atomic_basic_i32
  tests/test_diff_sem_vs_sircc_atomic_basic_i32.c
)

add_dependencies(sem_unit_diff_sem_vs_sircc_atomic_basic_i32 sem sircc)
target_compile_definitions(sem_unit_diff_sem_vs_sircc_atomic_basic_i32 PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_definitions(sem_unit_diff_sem_vs_sircc_atomic_basic_i32 PRIVATE SEM_EXE_PATH="$<TARGET_FILE:sem>")
target_compile_definitions(sem_unit_diff_sem_vs_sircc_atomic_basic_i32 PRIVATE SIRCC_EXE_PATH="$<TARGET_FILE:sircc>")
target_compile_options(sem_unit_diff_sem_vs_sircc_atomic_basic_i32 PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_diff_sem_vs_sircc_atomic_basic_i32 COMMAND sem_unit_diff_sem_vs_sircc_atomic_basic_i32)

add_executable(sem_unit_diff_sem_vs_sircc_suite
  tests/test_diff_sem_vs_sircc_suite.c
)

add_dependencies(sem_unit_diff_sem_vs_sircc_suite sem sircc)
target_compile_definitions(sem_unit_diff_sem_vs_sircc_suite PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_definitions(sem_unit_diff_sem_vs_sircc_suite PRIVATE SEM_EXE_PATH="$<TARGET_FILE:sem>")
target_compile_definitions(sem_unit_diff_sem_vs_sircc_suite PRIVATE SIRCC_EXE_PATH="$<TARGET_FILE:sircc>")
target_compile_options(sem_unit_diff_sem_vs_sircc_suite PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_diff_sem_vs_sircc_suite COMMAND sem_unit_diff_sem_vs_sircc_suite)

add_executable(sem_unit_run_call_indirect
  tests/test_run_call_indirect.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_call_indirect PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_call_indirect PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_call_indirect PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_call_indirect PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_call_indirect PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_call_indirect_ptrsym COMMAND sem_unit_run_call_indirect)

add_executable(sem_unit_run_cfg_if
  tests/test_run_cfg_if.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_cfg_if PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_cfg_if PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_cfg_if PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_cfg_if PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_cfg_if PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_cfg_if COMMAND sem_unit_run_cfg_if)

add_executable(sem_unit_run_mem_stack
  tests/test_run_mem_stack.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_mem_stack PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_mem_stack PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_mem_stack PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_mem_stack PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_mem_stack PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_mem_stack COMMAND sem_unit_run_mem_stack)

add_executable(sem_unit_run_cfg_join_phi
  tests/test_run_cfg_join_phi.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_cfg_join_phi PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_cfg_join_phi PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_cfg_join_phi PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_cfg_join_phi PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_cfg_join_phi PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_cfg_join_phi COMMAND sem_unit_run_cfg_join_phi)

add_executable(sem_unit_run_cfg_switch
  tests/test_run_cfg_switch.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_cfg_switch PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_cfg_switch PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_cfg_switch PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_cfg_switch PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_cfg_switch PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_cfg_switch COMMAND sem_unit_run_cfg_switch)

add_executable(sem_unit_run_term_trap
  tests/test_run_term_trap.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_term_trap PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_term_trap PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_term_trap PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_term_trap PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_term_trap PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_term_trap COMMAND sem_unit_run_term_trap)

add_executable(sem_unit_run_term_unreachable
  tests/test_run_term_unreachable.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_term_unreachable PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_term_unreachable PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_term_unreachable PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_term_unreachable PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_term_unreachable PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_term_unreachable COMMAND sem_unit_run_term_unreachable)

add_executable(sem_unit_run_bad_cfg_br_args_mismatch
  tests/test_run_bad_cfg_br_args_mismatch.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_bad_cfg_br_args_mismatch PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_bad_cfg_br_args_mismatch PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_bad_cfg_br_args_mismatch PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_bad_cfg_br_args_mismatch PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_bad_cfg_br_args_mismatch PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_bad_cfg_br_args_mismatch COMMAND sem_unit_run_bad_cfg_br_args_mismatch)

add_executable(sem_unit_run_bad_cfg_switch_case_lit_not_const
  tests/test_run_bad_cfg_switch_case_lit_not_const.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_bad_cfg_switch_case_lit_not_const PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_bad_cfg_switch_case_lit_not_const PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_bad_cfg_switch_case_lit_not_const PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_bad_cfg_switch_case_lit_not_const PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_bad_cfg_switch_case_lit_not_const PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_bad_cfg_switch_case_lit_not_const COMMAND sem_unit_run_bad_cfg_switch_case_lit_not_const)

add_executable(sem_unit_run_sem_mem_fill_i32
  tests/test_run_sem_mem_fill_i32.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_mem_fill_i32 PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_mem_fill_i32 PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_mem_fill_i32 PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_mem_fill_i32 PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_mem_fill_i32 PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_mem_fill_i32 COMMAND sem_unit_run_sem_mem_fill_i32)

add_executable(sem_unit_run_sem_mem_copy_i32
  tests/test_run_sem_mem_copy_i32.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_mem_copy_i32 PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_mem_copy_i32 PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_mem_copy_i32 PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_mem_copy_i32 PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_mem_copy_i32 PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_mem_copy_i32 COMMAND sem_unit_run_sem_mem_copy_i32)

add_executable(sem_unit_run_mem_copy_overlap_trap
  tests/test_run_mem_copy_overlap_trap.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_mem_copy_overlap_trap PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_mem_copy_overlap_trap PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_mem_copy_overlap_trap PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_mem_copy_overlap_trap PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_mem_copy_overlap_trap PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_mem_copy_overlap_trap COMMAND sem_unit_run_mem_copy_overlap_trap)

add_executable(sem_unit_run_global_i32_ptrsym
  tests/test_run_global_i32_ptrsym.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_global_i32_ptrsym PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_global_i32_ptrsym PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_global_i32_ptrsym PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_global_i32_ptrsym PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_global_i32_ptrsym PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_global_i32_ptrsym COMMAND sem_unit_run_global_i32_ptrsym)

add_executable(sem_unit_run_global_array_const
  tests/test_run_global_array_const.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_global_array_const PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_global_array_const PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_global_array_const PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_global_array_const PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_global_array_const PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_global_array_const COMMAND sem_unit_run_global_array_const)

add_executable(sem_unit_run_global_array_repeat
  tests/test_run_global_array_repeat.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_global_array_repeat PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_global_array_repeat PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_global_array_repeat PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_global_array_repeat PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_global_array_repeat PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_global_array_repeat COMMAND sem_unit_run_global_array_repeat)

add_executable(sem_unit_run_struct_layout
  tests/test_run_struct_layout.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_struct_layout PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_struct_layout PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_struct_layout PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_struct_layout PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_struct_layout PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_struct_layout COMMAND sem_unit_run_struct_layout)

add_executable(sem_unit_run_global_struct_const_struct_zero
  tests/test_run_global_struct_const_struct_zero.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_global_struct_const_struct_zero PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_global_struct_const_struct_zero PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_global_struct_const_struct_zero PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_global_struct_const_struct_zero PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_global_struct_const_struct_zero PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_global_struct_const_struct_zero COMMAND sem_unit_run_global_struct_const_struct_zero)

add_executable(sem_unit_run_call_direct_internal
  tests/test_run_call_direct_internal.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_call_direct_internal PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_call_direct_internal PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_call_direct_internal PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_call_direct_internal PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_call_direct_internal PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_call_direct_internal COMMAND sem_unit_run_call_direct_internal)

add_executable(sem_unit_hint_ptrsym_extern_decl_fn
  tests/test_hint_ptrsym_extern_decl_fn.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_hint_ptrsym_extern_decl_fn PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_hint_ptrsym_extern_decl_fn PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_hint_ptrsym_extern_decl_fn PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_hint_ptrsym_extern_decl_fn PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_hint_ptrsym_extern_decl_fn PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_hint_ptrsym_extern_decl_fn COMMAND sem_unit_hint_ptrsym_extern_decl_fn)

add_executable(sem_unit_run_fun_sym_call
  tests/test_run_fun_sym_call.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_fun_sym_call PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_fun_sym_call PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_fun_sym_call PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_fun_sym_call PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_fun_sym_call PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_fun_sym_call COMMAND sem_unit_run_fun_sym_call)

add_executable(sem_unit_run_closure_make_call
  tests/test_run_closure_make_call.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_closure_make_call PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_closure_make_call PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_closure_make_call PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_closure_make_call PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_closure_make_call PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_closure_make_call COMMAND sem_unit_run_closure_make_call)

add_executable(sem_unit_run_sem_ptr_add_sub_cmp
  tests/test_run_sem_ptr_add_sub_cmp.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_ptr_add_sub_cmp PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_ptr_add_sub_cmp PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_ptr_add_sub_cmp PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_ptr_add_sub_cmp PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_ptr_add_sub_cmp PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_ptr_add_sub_cmp COMMAND sem_unit_run_sem_ptr_add_sub_cmp)

add_executable(sem_unit_run_sem_ptr_cmp_ne
  tests/test_run_sem_ptr_cmp_ne.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_ptr_cmp_ne PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_ptr_cmp_ne PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_ptr_cmp_ne PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_ptr_cmp_ne PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_ptr_cmp_ne PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_ptr_cmp_ne COMMAND sem_unit_run_sem_ptr_cmp_ne)

add_executable(sem_unit_run_ptr_cmp
  tests/test_run_ptr_cmp.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_ptr_cmp PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_ptr_cmp PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_ptr_cmp PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_ptr_cmp PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_ptr_cmp PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_ptr_cmp COMMAND sem_unit_run_ptr_cmp)

add_executable(sem_unit_run_sem_bool_ops
  tests/test_run_sem_bool_ops.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_bool_ops PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_bool_ops PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_bool_ops PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_bool_ops PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_bool_ops PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_bool_ops COMMAND sem_unit_run_sem_bool_ops)

add_executable(sem_unit_run_sem_if_val_to_select
  tests/test_run_sem_if_val_to_select.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_if_val_to_select PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_if_val_to_select PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_if_val_to_select PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_if_val_to_select PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_if_val_to_select PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_if_val_to_select COMMAND sem_unit_run_sem_if_val_to_select)

add_executable(sem_unit_run_sem_if_thunk_trap_not_taken
  tests/test_run_sem_if_thunk_trap_not_taken.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_if_thunk_trap_not_taken PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_if_thunk_trap_not_taken PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_if_thunk_trap_not_taken PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_if_thunk_trap_not_taken PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_if_thunk_trap_not_taken PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_if_thunk_trap_not_taken COMMAND sem_unit_run_sem_if_thunk_trap_not_taken)

add_executable(sem_unit_run_sem_and_sc_thunk_trap_not_taken
  tests/test_run_sem_and_sc_thunk_trap_not_taken.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_and_sc_thunk_trap_not_taken PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_and_sc_thunk_trap_not_taken PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_and_sc_thunk_trap_not_taken PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_and_sc_thunk_trap_not_taken PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_and_sc_thunk_trap_not_taken PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_and_sc_thunk_trap_not_taken COMMAND sem_unit_run_sem_and_sc_thunk_trap_not_taken)

add_executable(sem_unit_run_sem_or_sc_thunk_trap_not_taken
  tests/test_run_sem_or_sc_thunk_trap_not_taken.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_or_sc_thunk_trap_not_taken PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_or_sc_thunk_trap_not_taken PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_or_sc_thunk_trap_not_taken PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_or_sc_thunk_trap_not_taken PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_or_sc_thunk_trap_not_taken PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_or_sc_thunk_trap_not_taken COMMAND sem_unit_run_sem_or_sc_thunk_trap_not_taken)

add_executable(sem_unit_run_sem_switch_thunk_trap_not_taken
  tests/test_run_sem_switch_thunk_trap_not_taken.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_switch_thunk_trap_not_taken PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_switch_thunk_trap_not_taken PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_switch_thunk_trap_not_taken PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_switch_thunk_trap_not_taken PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_switch_thunk_trap_not_taken PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_switch_thunk_trap_not_taken COMMAND sem_unit_run_sem_switch_thunk_trap_not_taken)

add_executable(sem_unit_run_sem_match_sum_option_i32
  tests/test_run_sem_match_sum_option_i32.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_match_sum_option_i32 PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_match_sum_option_i32 PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_match_sum_option_i32 PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_match_sum_option_i32 PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_match_sum_option_i32 PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_match_sum_option_i32 COMMAND sem_unit_run_sem_match_sum_option_i32)

add_executable(sem_unit_run_sem_match_sum_let_option_i32
  tests/test_run_sem_match_sum_let_option_i32.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_match_sum_let_option_i32 PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_match_sum_let_option_i32 PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_match_sum_let_option_i32 PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_match_sum_let_option_i32 PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_match_sum_let_option_i32 PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_match_sum_let_option_i32 COMMAND sem_unit_run_sem_match_sum_let_option_i32)

add_executable(sem_unit_run_sem_break_exits_loop
  tests/test_run_sem_break_exits_loop.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_break_exits_loop PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_break_exits_loop PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_break_exits_loop PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_break_exits_loop PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_break_exits_loop PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_break_exits_loop COMMAND sem_unit_run_sem_break_exits_loop)

add_executable(sem_unit_run_sem_while_body_bad_code_traps
  tests/test_run_sem_while_body_bad_code_traps.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_while_body_bad_code_traps PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_while_body_bad_code_traps PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_while_body_bad_code_traps PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_while_body_bad_code_traps PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_while_body_bad_code_traps PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_while_body_bad_code_traps COMMAND sem_unit_run_sem_while_body_bad_code_traps)

add_executable(sem_unit_run_sem_cond_thunk_trap_not_taken
  tests/test_run_sem_cond_thunk_trap_not_taken.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_cond_thunk_trap_not_taken PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_cond_thunk_trap_not_taken PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_cond_thunk_trap_not_taken PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_cond_thunk_trap_not_taken PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_cond_thunk_trap_not_taken PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_cond_thunk_trap_not_taken COMMAND sem_unit_run_sem_cond_thunk_trap_not_taken)

add_executable(sem_unit_run_fun_cmp_eq_true
  tests/test_run_fun_cmp_eq_true.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_fun_cmp_eq_true PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_fun_cmp_eq_true PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_fun_cmp_eq_true PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_fun_cmp_eq_true PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_fun_cmp_eq_true PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_fun_cmp_eq_true COMMAND sem_unit_run_fun_cmp_eq_true)

add_executable(sem_unit_run_fun_cmp_ne_true
  tests/test_run_fun_cmp_ne_true.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_fun_cmp_ne_true PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_fun_cmp_ne_true PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_fun_cmp_ne_true PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_fun_cmp_ne_true PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_fun_cmp_ne_true PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_fun_cmp_ne_true COMMAND sem_unit_run_fun_cmp_ne_true)

add_executable(sem_unit_verify_fun_cmp_sig_mismatch
  tests/test_verify_fun_cmp_sig_mismatch.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_verify_fun_cmp_sig_mismatch PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_verify_fun_cmp_sig_mismatch PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_verify_fun_cmp_sig_mismatch PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_verify_fun_cmp_sig_mismatch PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_verify_fun_cmp_sig_mismatch PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_verify_fun_cmp_sig_mismatch COMMAND sem_unit_verify_fun_cmp_sig_mismatch)

add_executable(sem_unit_verify_bad_closure_make_code_sig_mismatch
  tests/test_verify_bad_closure_make_code_sig_mismatch.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_verify_bad_closure_make_code_sig_mismatch PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_verify_bad_closure_make_code_sig_mismatch PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_verify_bad_closure_make_code_sig_mismatch PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_verify_bad_closure_make_code_sig_mismatch PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_verify_bad_closure_make_code_sig_mismatch PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_verify_bad_closure_make_code_sig_mismatch COMMAND sem_unit_verify_bad_closure_make_code_sig_mismatch)

add_executable(sem_unit_run_sem_while_global_counter
  tests/test_run_sem_while_global_counter.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_while_global_counter PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_while_global_counter PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_while_global_counter PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_while_global_counter PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_while_global_counter PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_while_global_counter COMMAND sem_unit_run_sem_while_global_counter)

add_executable(sem_unit_run_sem_defer_increments_global_before_ret
  tests/test_run_sem_defer_increments_global_before_ret.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_defer_increments_global_before_ret PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_defer_increments_global_before_ret PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_defer_increments_global_before_ret PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_defer_increments_global_before_ret PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_defer_increments_global_before_ret PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_defer_increments_global_before_ret COMMAND sem_unit_run_sem_defer_increments_global_before_ret)

add_executable(sem_unit_run_sem_scope_defer_runs_on_fallthrough
  tests/test_run_sem_scope_defer_runs_on_fallthrough.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_scope_defer_runs_on_fallthrough PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_scope_defer_runs_on_fallthrough PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_scope_defer_runs_on_fallthrough PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_scope_defer_runs_on_fallthrough PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_scope_defer_runs_on_fallthrough PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_scope_defer_runs_on_fallthrough COMMAND sem_unit_run_sem_scope_defer_runs_on_fallthrough)

add_executable(sem_unit_run_float_load_canon
  tests/test_run_float_load_canon.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_float_load_canon PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_float_load_canon PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_float_load_canon PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_float_load_canon PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_float_load_canon PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_float_load_canon COMMAND sem_unit_run_float_load_canon)

add_executable(sem_unit_run_i16_store_load_zext
  tests/test_run_i16_store_load_zext.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_i16_store_load_zext PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_i16_store_load_zext PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_i16_store_load_zext PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_i16_store_load_zext PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_i16_store_load_zext PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_i16_store_load_zext COMMAND sem_unit_run_i16_store_load_zext)

add_executable(sem_unit_run_f64_cmp_olt_to_i32
  tests/test_run_f64_cmp_olt_to_i32.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_f64_cmp_olt_to_i32 PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_f64_cmp_olt_to_i32 PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_f64_cmp_olt_to_i32 PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_f64_cmp_olt_to_i32 PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_f64_cmp_olt_to_i32 PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_f64_cmp_olt_to_i32 COMMAND sem_unit_run_f64_cmp_olt_to_i32)

add_executable(sem_unit_run_misaligned_load_traps
  tests/test_run_misaligned_load_traps.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_misaligned_load_traps PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_misaligned_load_traps PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_misaligned_load_traps PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_misaligned_load_traps PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_misaligned_load_traps PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_misaligned_load_traps COMMAND sem_unit_run_misaligned_load_traps)

add_executable(sem_unit_run_sem_i32_cmp_variants
  tests/test_run_sem_i32_cmp_variants.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_i32_cmp_variants PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_i32_cmp_variants PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_i32_cmp_variants PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_i32_cmp_variants PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_i32_cmp_variants PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_i32_cmp_variants COMMAND sem_unit_run_sem_i32_cmp_variants)

add_executable(sem_unit_run_sem_ptr_cast_roundtrip
  tests/test_run_sem_ptr_cast_roundtrip.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_ptr_cast_roundtrip PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_ptr_cast_roundtrip PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_ptr_cast_roundtrip PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_ptr_cast_roundtrip PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_ptr_cast_roundtrip PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_ptr_cast_roundtrip COMMAND sem_unit_run_sem_ptr_cast_roundtrip)

add_executable(sem_unit_run_sem_ptr_sizeof_array
  tests/test_run_sem_ptr_sizeof_array.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_ptr_sizeof_array PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_ptr_sizeof_array PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_ptr_sizeof_array PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_ptr_sizeof_array PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_ptr_sizeof_array PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_ptr_sizeof_array COMMAND sem_unit_run_sem_ptr_sizeof_array)

add_executable(sem_unit_run_sem_ptr_alignof_array
  tests/test_run_sem_ptr_alignof_array.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_ptr_alignof_array PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_ptr_alignof_array PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_ptr_alignof_array PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_ptr_alignof_array PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_ptr_alignof_array PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_ptr_alignof_array COMMAND sem_unit_run_sem_ptr_alignof_array)

add_executable(sem_unit_run_sem_i32_bitops
  tests/test_run_sem_i32_bitops.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_i32_bitops PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_i32_bitops PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_i32_bitops PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_i32_bitops PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_i32_bitops PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_i32_bitops COMMAND sem_unit_run_sem_i32_bitops)

add_executable(sem_unit_run_sem_i32_shift_divrem_sat
  tests/test_run_sem_i32_shift_divrem_sat.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_i32_shift_divrem_sat PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_i32_shift_divrem_sat PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_i32_shift_divrem_sat PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_i32_shift_divrem_sat PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_i32_shift_divrem_sat PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_i32_shift_divrem_sat COMMAND sem_unit_run_sem_i32_shift_divrem_sat)

add_executable(sem_unit_run_sem_i32_trunc_i64
  tests/test_run_sem_i32_trunc_i64.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_i32_trunc_i64 PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_i32_trunc_i64 PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_i32_trunc_i64 PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_i32_trunc_i64 PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_i32_trunc_i64 PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_i32_trunc_i64 COMMAND sem_unit_run_sem_i32_trunc_i64)

add_executable(sem_unit_run_sem_void_type_ignored
  tests/test_run_sem_void_type_ignored.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_void_type_ignored PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_void_type_ignored PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_void_type_ignored PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_void_type_ignored PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_void_type_ignored PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_void_type_ignored COMMAND sem_unit_run_sem_void_type_ignored)

add_executable(sem_unit_run_sem_ptr_kind_param
  tests/test_run_sem_ptr_kind_param.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_ptr_kind_param PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_ptr_kind_param PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_ptr_kind_param PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_ptr_kind_param PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_ptr_kind_param PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_ptr_kind_param COMMAND sem_unit_run_sem_ptr_kind_param)

add_executable(sem_unit_verify_ptr_layout
  tests/test_verify_ptr_layout.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_verify_ptr_layout PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_verify_ptr_layout PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_verify_ptr_layout PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_verify_ptr_layout PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_verify_ptr_layout PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_verify_ptr_layout COMMAND sem_unit_verify_ptr_layout)

add_executable(sem_unit_verify_bad_call_indirect_argc
  tests/test_verify_bad_call_indirect_argc.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_verify_bad_call_indirect_argc PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_verify_bad_call_indirect_argc PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_verify_bad_call_indirect_argc PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_verify_bad_call_indirect_argc PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_verify_bad_call_indirect_argc PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_verify_bad_call_indirect_argc COMMAND sem_unit_verify_bad_call_indirect_argc)

add_executable(sem_unit_verify_bad_ptr_offset_void
  tests/test_verify_bad_ptr_offset_void.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_verify_bad_ptr_offset_void PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_verify_bad_ptr_offset_void PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_verify_bad_ptr_offset_void PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_verify_bad_ptr_offset_void PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_verify_bad_ptr_offset_void PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_verify_bad_ptr_offset_void COMMAND sem_unit_verify_bad_ptr_offset_void)

add_executable(sem_unit_verify_bad_atomic_missing_mode_json
  tests/test_verify_bad_atomic_missing_mode_json.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_verify_bad_atomic_missing_mode_json PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_verify_bad_atomic_missing_mode_json PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_verify_bad_atomic_missing_mode_json PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_verify_bad_atomic_missing_mode_json PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_verify_bad_atomic_missing_mode_json PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_verify_bad_atomic_missing_mode_json COMMAND sem_unit_verify_bad_atomic_missing_mode_json)

add_executable(sem_unit_run_mem_copy_fill
  tests/test_run_mem_copy_fill.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_mem_copy_fill PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_mem_copy_fill PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_mem_copy_fill PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_mem_copy_fill PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_mem_copy_fill PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_mem_copy_fill COMMAND sem_unit_run_mem_copy_fill)

add_executable(sem_unit_run_sem_i32_div_s_trap_ok
  tests/test_run_sem_i32_div_s_trap_ok.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_i32_div_s_trap_ok PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_i32_div_s_trap_ok PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_i32_div_s_trap_ok PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_i32_div_s_trap_ok PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_i32_div_s_trap_ok PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_i32_div_s_trap_ok COMMAND sem_unit_run_sem_i32_div_s_trap_ok)

add_executable(sem_unit_run_sem_i32_div_s_trap_zero
  tests/test_run_sem_i32_div_s_trap_zero.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_sem_i32_div_s_trap_zero PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_sem_i32_div_s_trap_zero PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_sem_i32_div_s_trap_zero PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_sem_i32_div_s_trap_zero PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_sem_i32_div_s_trap_zero PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_sem_i32_div_s_trap_zero COMMAND sem_unit_run_sem_i32_div_s_trap_zero)

add_executable(sem_unit_trace_smoke
  tests/test_trace_smoke.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_trace_smoke PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_trace_smoke PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_trace_smoke PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_trace_smoke PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_trace_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_trace_smoke COMMAND sem_unit_trace_smoke)

add_executable(sem_unit_trace_filter_op_smoke
  tests/test_trace_filter_op_smoke.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_trace_filter_op_smoke PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_trace_filter_op_smoke PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_trace_filter_op_smoke PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_trace_filter_op_smoke PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_trace_filter_op_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_trace_filter_op_smoke COMMAND sem_unit_trace_filter_op_smoke)

add_executable(sem_unit_coverage_smoke
  tests/test_coverage_smoke.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_coverage_smoke PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_coverage_smoke PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_coverage_smoke PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_coverage_smoke PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_coverage_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_coverage_smoke COMMAND sem_unit_coverage_smoke)

add_executable(sem_unit_coverage_srcmap_smoke
  tests/test_coverage_srcmap_smoke.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_coverage_srcmap_smoke PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_coverage_srcmap_smoke PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_coverage_srcmap_smoke PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_coverage_srcmap_smoke PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_coverage_srcmap_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_coverage_srcmap_smoke COMMAND sem_unit_coverage_srcmap_smoke)

add_executable(sem_unit_verify_validate_diag_fields_json
  tests/test_verify_validate_diag_fields_json.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_verify_validate_diag_fields_json PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_verify_validate_diag_fields_json PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_verify_validate_diag_fields_json PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_verify_validate_diag_fields_json PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_verify_validate_diag_fields_json PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_verify_validate_diag_fields_json COMMAND sem_unit_verify_validate_diag_fields_json)

add_executable(sem_unit_exec_failure_diag_fields_json
  tests/test_exec_failure_diag_fields_json.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_exec_failure_diag_fields_json PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_exec_failure_diag_fields_json PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_exec_failure_diag_fields_json PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_exec_failure_diag_fields_json PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_exec_failure_diag_fields_json PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_exec_failure_diag_fields_json COMMAND sem_unit_exec_failure_diag_fields_json)

add_executable(sem_unit_run_atomic_cmpxchg_i32
  tests/test_run_atomic_cmpxchg_i32.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_atomic_cmpxchg_i32 PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_atomic_cmpxchg_i32 PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_atomic_cmpxchg_i32 PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_atomic_cmpxchg_i32 PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_atomic_cmpxchg_i32 PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_atomic_cmpxchg_i32 COMMAND sem_unit_run_atomic_cmpxchg_i32)

add_executable(sem_unit_run_atomic_basic_i64
  tests/test_run_atomic_basic_i64.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_atomic_basic_i64 PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_atomic_basic_i64 PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_atomic_basic_i64 PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_atomic_basic_i64 PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_atomic_basic_i64 PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_atomic_basic_i64 COMMAND sem_unit_run_atomic_basic_i64)

add_executable(sem_unit_run_atomic_cmpxchg_i64
  tests/test_run_atomic_cmpxchg_i64.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_atomic_cmpxchg_i64 PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_atomic_cmpxchg_i64 PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_atomic_cmpxchg_i64 PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_atomic_cmpxchg_i64 PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_atomic_cmpxchg_i64 PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_atomic_cmpxchg_i64 COMMAND sem_unit_run_atomic_cmpxchg_i64)

add_executable(sem_unit_run_simd_i32_add_extract_replace
  tests/test_run_simd_i32_add_extract_replace.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_simd_i32_add_extract_replace PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_simd_i32_add_extract_replace PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_simd_i32_add_extract_replace PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_simd_i32_add_extract_replace PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_simd_i32_add_extract_replace PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_simd_i32_add_extract_replace COMMAND sem_unit_run_simd_i32_add_extract_replace)

add_executable(sem_unit_run_simd_load_vec_misaligned_traps
  tests/test_run_simd_load_vec_misaligned_traps.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_simd_load_vec_misaligned_traps PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_simd_load_vec_misaligned_traps PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_simd_load_vec_misaligned_traps PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_simd_load_vec_misaligned_traps PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_simd_load_vec_misaligned_traps PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_simd_load_vec_misaligned_traps COMMAND sem_unit_run_simd_load_vec_misaligned_traps)

add_executable(sem_unit_run_simd_splat_extract_load_store_vec
  tests/test_run_simd_splat_extract_load_store_vec.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_simd_splat_extract_load_store_vec PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_simd_splat_extract_load_store_vec PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_simd_splat_extract_load_store_vec PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_simd_splat_extract_load_store_vec PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_simd_splat_extract_load_store_vec PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_simd_splat_extract_load_store_vec COMMAND sem_unit_run_simd_splat_extract_load_store_vec)

add_executable(sem_unit_run_simd_shuffle_two_inputs
  tests/test_run_simd_shuffle_two_inputs.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_simd_shuffle_two_inputs PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_simd_shuffle_two_inputs PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_simd_shuffle_two_inputs PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_simd_shuffle_two_inputs PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_simd_shuffle_two_inputs PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_simd_shuffle_two_inputs COMMAND sem_unit_run_simd_shuffle_two_inputs)

add_executable(sem_unit_run_simd_cmp_select_bool_mask
  tests/test_run_simd_cmp_select_bool_mask.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_simd_cmp_select_bool_mask PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_simd_cmp_select_bool_mask PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_simd_cmp_select_bool_mask PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_simd_cmp_select_bool_mask PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_simd_cmp_select_bool_mask PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_simd_cmp_select_bool_mask COMMAND sem_unit_run_simd_cmp_select_bool_mask)

add_executable(sem_unit_run_simd_extract_oob_traps
  tests/test_run_simd_extract_oob_traps.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_simd_extract_oob_traps PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_simd_extract_oob_traps PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_simd_extract_oob_traps PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_simd_extract_oob_traps PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_simd_extract_oob_traps PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_simd_extract_oob_traps COMMAND sem_unit_run_simd_extract_oob_traps)

add_executable(sem_unit_run_simd_replace_oob_traps
  tests/test_run_simd_replace_oob_traps.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_simd_replace_oob_traps PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_simd_replace_oob_traps PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_simd_replace_oob_traps PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_simd_replace_oob_traps PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_simd_replace_oob_traps PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_simd_replace_oob_traps COMMAND sem_unit_run_simd_replace_oob_traps)

add_executable(sem_unit_run_simd_shuffle_oob_traps
  tests/test_run_simd_shuffle_oob_traps.c
  sem_hosted.c
  sir_jsonl.c
  ${CMAKE_SOURCE_DIR}/src/sircc/json.c
  ${CMAKE_SOURCE_DIR}/src/sircc/sircc.c
)

target_compile_definitions(sem_unit_run_simd_shuffle_oob_traps PRIVATE SIR_VERSION="${SIR_VERSION}")
target_compile_definitions(sem_unit_run_simd_shuffle_oob_traps PRIVATE SEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(sem_unit_run_simd_shuffle_oob_traps PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore ${CMAKE_SOURCE_DIR}/src/sircc)
target_link_libraries(sem_unit_run_simd_shuffle_oob_traps PRIVATE sircore_hosted_zabi sircore_module)
target_compile_options(sem_unit_run_simd_shuffle_oob_traps PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sem_run_simd_shuffle_oob_traps COMMAND sem_unit_run_simd_shuffle_oob_traps)

add_test(
  NAME sem_check_smoke
  COMMAND $<TARGET_FILE:sem> --check
    ${CMAKE_SOURCE_DIR}/src/sircc/examples/hello_zabi25_write.sir.jsonl
    ${CMAKE_SOURCE_DIR}/src/sircc/examples/ptr_layout.sir.jsonl
)

add_test(
  NAME sem_check_run_smoke
  COMMAND $<TARGET_FILE:sem> --check --check-run
    ${CMAKE_SOURCE_DIR}/src/sircc/examples/hello_zabi25_write.sir.jsonl
    ${CMAKE_SOURCE_DIR}/src/sircc/examples/mem_copy_fill.sir.jsonl
)

add_test(
  NAME sem_check_run_smoke_json
  COMMAND $<TARGET_FILE:sem> --check --check-run --format json
    ${CMAKE_SOURCE_DIR}/src/sircc/examples/hello_zabi25_write.sir.jsonl
    ${CMAKE_SOURCE_DIR}/src/sircc/examples/mem_copy_fill.sir.jsonl
)
