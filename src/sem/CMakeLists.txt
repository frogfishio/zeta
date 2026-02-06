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

add_executable(sem_unit_semrt_file_fs
  tests/test_semrt_file_fs.c
)

target_compile_definitions(sem_unit_semrt_file_fs PRIVATE SIR_VERSION="${SIR_VERSION}")
target_include_directories(sem_unit_semrt_file_fs PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore)
target_link_libraries(sem_unit_semrt_file_fs PRIVATE sircore_hosted_zabi)

target_compile_options(sem_unit_semrt_file_fs PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)

add_test(NAME sem_semrt_file_fs COMMAND sem_unit_semrt_file_fs)

add_test(
  NAME sem_run_hello_zabi25_write
  COMMAND $<TARGET_FILE:sem> --run ${CMAKE_SOURCE_DIR}/src/sircc/examples/hello_zabi25_write.sir.jsonl
)

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
