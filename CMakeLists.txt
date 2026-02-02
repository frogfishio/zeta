cmake_minimum_required(VERSION 3.20)

project(sir LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

add_subdirectory(src/sircc)

option(SIR_ENABLE_SIRC "Build the experimental .sir text parser (flex/bison)" OFF)
if(SIR_ENABLE_SIRC)
  add_subdirectory(src/sirc)
endif()

#
# dist bundle (copy-pasteable for alpha users)
#
set(SIR_DIST_ROOT ${CMAKE_SOURCE_DIR}/dist)
set(SIR_DIST_BIN ${SIR_DIST_ROOT}/bin)
set(SIR_DIST_DOC ${SIR_DIST_ROOT}/doc)
set(SIR_DIST_TEST ${SIR_DIST_ROOT}/test)
set(SIR_DIST_TEST_EXAMPLES ${SIR_DIST_TEST}/examples)

set(SIR_DIST_OS "${CMAKE_SYSTEM_NAME}")
if(SIR_DIST_OS STREQUAL "Darwin")
  set(SIR_DIST_OS "macos")
elseif(SIR_DIST_OS STREQUAL "Linux")
  set(SIR_DIST_OS "linux")
elseif(SIR_DIST_OS STREQUAL "Windows")
  set(SIR_DIST_OS "windows")
endif()
set(SIR_DIST_BIN_OS ${SIR_DIST_BIN}/${SIR_DIST_OS})

add_custom_target(dist_sircc
  DEPENDS sircc
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_BIN_OS}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_DOC}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_TEST_EXAMPLES}

  # binaries
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:sircc> ${SIR_DIST_BIN_OS}/sircc

  # docs
  COMMAND ${CMAKE_COMMAND} -E rm -f ${SIR_DIST_DOC}/src.md
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/docs/src.md ${SIR_DIST_DOC}/sircc.md
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/docs/test.md ${SIR_DIST_TEST}/README.md

  # normative JSONL examples (subset)
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/add.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/add.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/mem_copy_fill.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/mem_copy_fill.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/cfg_if.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/cfg_if.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/cfg_switch.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/cfg_switch.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/hello_world_puts.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/hello_world_puts.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/float_nan_canon_ops.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/float_nan_canon_ops.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/call_indirect_ptrsym.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/call_indirect_ptrsym.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/misaligned_load_traps.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/misaligned_load_traps.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/ptr_layout.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/ptr_layout.sir.jsonl
)

if(TARGET sirc)
  add_custom_target(dist_sirc
    DEPENDS sirc
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_BIN_OS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_DOC}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_TEST_EXAMPLES}

    # binary + docs
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:sirc> ${SIR_DIST_BIN_OS}/sirc
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/README.md ${SIR_DIST_DOC}/sirc.md

    # sugar examples (.sir) that mirror the JSONL examples where possible
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/hello.sir ${SIR_DIST_TEST_EXAMPLES}/hello.sir
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/add.sir ${SIR_DIST_TEST_EXAMPLES}/add.sir
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/mem_copy_fill.sir ${SIR_DIST_TEST_EXAMPLES}/mem_copy_fill.sir
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/cfg_if.sir ${SIR_DIST_TEST_EXAMPLES}/cfg_if.sir
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/cfg_switch.sir ${SIR_DIST_TEST_EXAMPLES}/cfg_switch.sir
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/call_indirect_ptrsym.sir ${SIR_DIST_TEST_EXAMPLES}/call_indirect_ptrsym.sir
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/ptr_layout.sir ${SIR_DIST_TEST_EXAMPLES}/ptr_layout.sir
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/eff_fence.sir ${SIR_DIST_TEST_EXAMPLES}/eff_fence.sir
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/load_store_no_align.sir ${SIR_DIST_TEST_EXAMPLES}/load_store_no_align.sir
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/trap.sir ${SIR_DIST_TEST_EXAMPLES}/trap.sir
  )
endif()

add_custom_target(dist DEPENDS dist_sircc)
if(TARGET sirc)
  add_dependencies(dist dist_sirc)
endif()
