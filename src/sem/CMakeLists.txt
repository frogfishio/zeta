cmake_minimum_required(VERSION 3.20)

add_executable(sem
  sem.c
  zcl1.c
  sem_host.c
  zi_tape.c
)

target_compile_definitions(sem PRIVATE SIR_VERSION="${SIR_VERSION}")
target_include_directories(sem PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_compile_options(sem PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)

add_executable(sem_unit_tests
  tests/test_caps_list.c
  zcl1.c
  sem_host.c
)

target_compile_definitions(sem_unit_tests PRIVATE SIR_VERSION="${SIR_VERSION}")
target_include_directories(sem_unit_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_compile_options(sem_unit_tests PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)

add_test(NAME sem_caps_list_empty COMMAND sem_unit_tests)
