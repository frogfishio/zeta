cmake_minimum_required(VERSION 3.20)

add_executable(sem
  sem.c
  zi_tape.c
)

target_compile_definitions(sem PRIVATE SIR_VERSION="${SIR_VERSION}")
target_include_directories(sem PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/src/sircore)
target_link_libraries(sem PRIVATE sircore_hosted_zabi sircore_vm)

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
