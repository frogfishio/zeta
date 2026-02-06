cmake_minimum_required(VERSION 3.20)

add_executable(sem
  sem.c
  zcl1.c
  sem_host.c
  zi_tape.c
  guest_mem.c
  handles.c
  semrt.c
)

target_compile_definitions(sem PRIVATE SIR_VERSION="${SIR_VERSION}")
target_include_directories(sem PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_compile_options(sem PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)

add_executable(sem_unit_caps_list
  tests/test_caps_list.c
  zcl1.c
  sem_host.c
)

target_compile_definitions(sem_unit_caps_list PRIVATE SIR_VERSION="${SIR_VERSION}")
target_include_directories(sem_unit_caps_list PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_compile_options(sem_unit_caps_list PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)

add_test(NAME sem_caps_list_empty COMMAND sem_unit_caps_list)

add_executable(sem_unit_semrt_write
  tests/test_semrt_write.c
  zcl1.c
  sem_host.c
  guest_mem.c
  handles.c
  semrt.c
)

target_compile_definitions(sem_unit_semrt_write PRIVATE SIR_VERSION="${SIR_VERSION}")
target_include_directories(sem_unit_semrt_write PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_compile_options(sem_unit_semrt_write PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)

add_test(NAME sem_semrt_write COMMAND sem_unit_semrt_write)
