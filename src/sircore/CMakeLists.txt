cmake_minimum_required(VERSION 3.20)

add_library(sircore_runtime
  guest_mem.c
  handles.c
  zcl1.c
)

target_include_directories(sircore_runtime PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(sircore_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(sircore_hosted_zabi
  hosted_zabi.c
  sem_host.c
)

target_include_directories(sircore_hosted_zabi PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(sircore_hosted_zabi PUBLIC sircore_runtime)
target_compile_options(sircore_hosted_zabi PRIVATE -Wall -Wextra -Wpedantic -Werror)

# Optional: integrate zingcore25 (golden caps like sys/loop, file/aio, net/tcp).
if(APPLE AND EXISTS "${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/lib/libzingcore25.a" AND EXISTS "${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/include/zi_sysabi25.h")
  target_compile_definitions(sircore_hosted_zabi PUBLIC SIR_HAVE_ZINGCORE25=1)
  target_include_directories(sircore_hosted_zabi PUBLIC ${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/include)
  target_link_libraries(sircore_hosted_zabi PUBLIC ${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64/lib/libzingcore25.a)
endif()

add_library(sircore_vm
  sircore_vm.c
)

target_include_directories(sircore_vm PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(sircore_vm PUBLIC sircore_runtime)
target_compile_options(sircore_vm PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(sircore_module
  sir_module.c
)

target_include_directories(sircore_module PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(sircore_module PUBLIC sircore_runtime)
target_compile_options(sircore_module PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(sircore_unit_vm_hello
  tests/test_vm_hello.c
)

target_include_directories(sircore_unit_vm_hello PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(sircore_unit_vm_hello PRIVATE sircore_vm sircore_hosted_zabi)
target_compile_options(sircore_unit_vm_hello PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sircore_vm_hello COMMAND sircore_unit_vm_hello)

add_executable(sircore_unit_module_hello
  tests/test_module_hello.c
)

target_include_directories(sircore_unit_module_hello PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(sircore_unit_module_hello PRIVATE sircore_module sircore_hosted_zabi)
target_compile_options(sircore_unit_module_hello PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sircore_module_hello COMMAND sircore_unit_module_hello)

add_executable(sircore_unit_module_negative
  tests/test_module_negative.c
)

target_include_directories(sircore_unit_module_negative PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(sircore_unit_module_negative PRIVATE sircore_module)
target_compile_options(sircore_unit_module_negative PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME sircore_module_negative COMMAND sircore_unit_module_negative)
