cmake_minimum_required(VERSION 3.20)

project(sir LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

# Project version (used by shipped binaries + dist bundle).
set(SIR_VERSION "0.0.0")
if(EXISTS ${CMAKE_SOURCE_DIR}/VERSION)
  file(READ ${CMAKE_SOURCE_DIR}/VERSION SIR_VERSION_RAW)
  string(STRIP "${SIR_VERSION_RAW}" SIR_VERSION)
endif()
set(SIR_VERSION "${SIR_VERSION}" CACHE STRING "Project version (from ./VERSION)")

add_subdirectory(src/sircore)
add_subdirectory(src/sircc)
add_subdirectory(src/sem)
add_subdirectory(src/zetacheck)

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
set(SIR_DIST_LIB ${SIR_DIST_ROOT}/lib)
set(SIR_DIST_PRELUDE ${SIR_DIST_LIB}/sircc/prelude)
set(SIR_DIST_TEST ${SIR_DIST_ROOT}/test)
set(SIR_DIST_TEST_EXAMPLES ${SIR_DIST_TEST}/examples)
set(SIR_DIST_TEST_SEM ${SIR_DIST_TEST}/sem)
set(SIR_DIST_TEST_SEM_EXAMPLES ${SIR_DIST_TEST_SEM}/examples)
set(SIR_DIST_TEST_SEM_RUN ${SIR_DIST_TEST_SEM}/run)
set(SIR_DIST_RT ${SIR_DIST_ROOT}/rt)

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
  DEPENDS sircc sem zetacheck
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_BIN_OS}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_DOC}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_PRELUDE}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_TEST_EXAMPLES}
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${SIR_DIST_TEST_SEM_EXAMPLES}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_TEST_SEM_EXAMPLES}
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${SIR_DIST_TEST_SEM_RUN}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_TEST_SEM_RUN}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_RT}

  # binaries
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:sircc> ${SIR_DIST_BIN_OS}/sircc
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:sem> ${SIR_DIST_BIN_OS}/sem
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:zetacheck> ${SIR_DIST_BIN_OS}/zetacheck
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/VERSION ${SIR_DIST_ROOT}/VERSION

  # docs
  COMMAND ${CMAKE_COMMAND} -E rm -f ${SIR_DIST_DOC}/src.md
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/docs/src.md ${SIR_DIST_DOC}/sircc.md
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/docs/mir_producer.md ${SIR_DIST_DOC}/mir_producer.md
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/docs/compiler_kit_cheatsheet.md ${SIR_DIST_DOC}/compiler_kit_cheatsheet.md
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/docs/ast_to_sir_cookbook.md ${SIR_DIST_DOC}/ast_to_sir_cookbook.md
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/docs/data_v1.md ${SIR_DIST_DOC}/data_v1.md
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/docs/interop_extern.md ${SIR_DIST_DOC}/interop_extern.md
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/docs/target_layout_contract.md ${SIR_DIST_DOC}/target_layout_contract.md
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/docs/diagnostics.md ${SIR_DIST_DOC}/diagnostics.md
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/docs/hl_core_contract.md ${SIR_DIST_DOC}/hl_core_contract.md
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sem/README.md ${SIR_DIST_DOC}/sem.md
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sem/docs/caps.md ${SIR_DIST_DOC}/sem_caps.md
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/docs/test.md ${SIR_DIST_TEST}/README.md
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/docs/QUICKSTART.md ${SIR_DIST_ROOT}/QUICKSTART.md
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DOUT=${SIR_DIST_DOC}/support.html
    "-DARGS_STR=--print-support --format html --full"
    -P ${CMAKE_SOURCE_DIR}/src/sircc/tests/run_and_write_stdout.cmake
  COMMAND ${CMAKE_COMMAND}
    -DSIRCC=$<TARGET_FILE:sircc>
    -DOUT=${SIR_DIST_DOC}/support.json
    "-DARGS_STR=--print-support --format json --full"
    -P ${CMAKE_SOURCE_DIR}/src/sircc/tests/run_and_write_stdout.cmake

  # preludes (compiler kit boilerplate)
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/src/sircc/prelude ${SIR_DIST_PRELUDE}

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
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/atomic_basic_i32.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/atomic_basic_i32.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/atomic_basic_i64.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/atomic_basic_i64.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/atomic_cmpxchg_i32.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/atomic_cmpxchg_i32.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/atomic_cmpxchg_i64.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/atomic_cmpxchg_i64.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/interop_export_global_i32.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/interop_export_global_i32.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/hello_zabi25_write.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/hello_zabi25_write.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/hello_zabi25_caps_list.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/hello_zabi25_caps_list.sir.jsonl

  # pack corpus (fun/closure/adt/sem)
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/fun_sym_call.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/fun_sym_call.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/closure_make_call.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/closure_make_call.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/adt_make_get.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/adt_make_get.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/sem_if_thunk_trap_not_taken.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/sem_if_thunk_trap_not_taken.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/sem_match_sum_option_i32.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/sem_match_sum_option_i32.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/simd_splat_extract.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/simd_splat_extract.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/simd_i32_add_extract_replace.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/simd_i32_add_extract_replace.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/simd_cmp_select_bool_mask.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/simd_cmp_select_bool_mask.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/simd_shuffle_two_inputs.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/simd_shuffle_two_inputs.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/simd_f32_mul_nan_canon_bits.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/simd_f32_mul_nan_canon_bits.sir.jsonl

  # SEM-runnable examples (subset; safe for `sem --check-run`)
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/add.sir.jsonl ${SIR_DIST_TEST_SEM_EXAMPLES}/add.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/mem_copy_fill.sir.jsonl ${SIR_DIST_TEST_SEM_EXAMPLES}/mem_copy_fill.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/cfg_if.sir.jsonl ${SIR_DIST_TEST_SEM_EXAMPLES}/cfg_if.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/cfg_switch.sir.jsonl ${SIR_DIST_TEST_SEM_EXAMPLES}/cfg_switch.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/hello_zabi25_write.sir.jsonl ${SIR_DIST_TEST_SEM_EXAMPLES}/hello_zabi25_write.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/ptr_layout.sir.jsonl ${SIR_DIST_TEST_SEM_EXAMPLES}/ptr_layout.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/call_indirect_ptrsym.sir.jsonl ${SIR_DIST_TEST_SEM_EXAMPLES}/call_indirect_ptrsym.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/atomic_basic_i32.sir.jsonl ${SIR_DIST_TEST_SEM_EXAMPLES}/atomic_basic_i32.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/atomic_basic_i64.sir.jsonl ${SIR_DIST_TEST_SEM_EXAMPLES}/atomic_basic_i64.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/atomic_cmpxchg_i32.sir.jsonl ${SIR_DIST_TEST_SEM_EXAMPLES}/atomic_cmpxchg_i32.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/atomic_cmpxchg_i64.sir.jsonl ${SIR_DIST_TEST_SEM_EXAMPLES}/atomic_cmpxchg_i64.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/interop_export_global_i32.sir.jsonl ${SIR_DIST_TEST_SEM_EXAMPLES}/interop_export_global_i32.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/misaligned_load_traps.sir.jsonl ${SIR_DIST_TEST_SEM_EXAMPLES}/misaligned_load_traps.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/hello_zabi25_caps_list.sir.jsonl ${SIR_DIST_TEST_SEM_EXAMPLES}/hello_zabi25_caps_list.sir.jsonl

  # SEM runnable examples (subset; safe for `sem --check --check-run`)
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/add.sir.jsonl ${SIR_DIST_TEST_SEM_RUN}/add.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/mem_copy_fill.sir.jsonl ${SIR_DIST_TEST_SEM_RUN}/mem_copy_fill.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/cfg_if.sir.jsonl ${SIR_DIST_TEST_SEM_RUN}/cfg_if.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/cfg_switch.sir.jsonl ${SIR_DIST_TEST_SEM_RUN}/cfg_switch.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/hello_zabi25_write.sir.jsonl ${SIR_DIST_TEST_SEM_RUN}/hello_zabi25_write.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/call_indirect_ptrsym.sir.jsonl ${SIR_DIST_TEST_SEM_RUN}/call_indirect_ptrsym.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/atomic_basic_i32.sir.jsonl ${SIR_DIST_TEST_SEM_RUN}/atomic_basic_i32.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/atomic_basic_i64.sir.jsonl ${SIR_DIST_TEST_SEM_RUN}/atomic_basic_i64.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/atomic_cmpxchg_i32.sir.jsonl ${SIR_DIST_TEST_SEM_RUN}/atomic_cmpxchg_i32.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/atomic_cmpxchg_i64.sir.jsonl ${SIR_DIST_TEST_SEM_RUN}/atomic_cmpxchg_i64.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/interop_export_global_i32.sir.jsonl ${SIR_DIST_TEST_SEM_RUN}/interop_export_global_i32.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/hello_zabi25_caps_list.sir.jsonl ${SIR_DIST_TEST_SEM_RUN}/hello_zabi25_caps_list.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sem/tests/fixtures/file_root/hello.txt ${SIR_DIST_TEST_SEM_RUN}/hello.txt

  # negative fixtures (verify-only; diagnostics)
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/bad_unknown_field.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/bad_unknown_field.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/bad_instr_operand.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/bad_instr_operand.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/bad_feature_gate_atomic.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/bad_feature_gate_atomic.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/bad_ptr_sym_undefined_extern_fn.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/bad_ptr_sym_undefined_extern_fn.sir.jsonl
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sircc/examples/cfg_bad_early_term.sir.jsonl ${SIR_DIST_TEST_EXAMPLES}/cfg_bad_early_term.sir.jsonl
)

# Optional: bundle zABI 2.5 integration-pack runtime for macOS/arm64 in dist/
set(SIR_ZABI25_PACK_ROOT ${CMAKE_SOURCE_DIR}/ext/integration-pack/macos-arm64)
set(SIR_DIST_ZABI25_ROOT ${SIR_DIST_RT}/zabi25/macos-arm64)
if(EXISTS ${SIR_ZABI25_PACK_ROOT}/include AND EXISTS ${SIR_ZABI25_PACK_ROOT}/lib/libzingcore25.a)
  add_custom_command(
    TARGET dist_sircc
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_ZABI25_ROOT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_ZABI25_ROOT}/lib
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_ZABI25_ROOT}/examples/host_shim
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${SIR_ZABI25_PACK_ROOT}/include ${SIR_DIST_ZABI25_ROOT}/include
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SIR_ZABI25_PACK_ROOT}/lib/libzingcore25.a ${SIR_DIST_ZABI25_ROOT}/lib/libzingcore25.a
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SIR_ZABI25_PACK_ROOT}/examples/host_shim/runner.c ${SIR_DIST_ZABI25_ROOT}/examples/host_shim/runner.c
  )
endif()

if(TARGET sirc)
  set(SIR_VSCODE_EXT_VSIX ${CMAKE_SOURCE_DIR}/sir-language-support/sir-language-support-0.0.2.vsix)
  set(SIR_VSCODE_EXT_README ${CMAKE_SOURCE_DIR}/sir-language-support/README.md)

  add_custom_target(dist_sirc
    DEPENDS sirc
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_BIN_OS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_DOC}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_TEST_EXAMPLES}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_DOC}/editors
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SIR_DIST_ROOT}/starter

    # binary + docs
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:sirc> ${SIR_DIST_BIN_OS}/sirc
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/README.md ${SIR_DIST_DOC}/sirc.md

    # starter project (package.toml + @include)
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/src/sirc/starter/hello_zeta ${SIR_DIST_ROOT}/starter/hello_zeta

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

    # pack examples (feature-gated; intended for sirc → sircc)
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/atomics_basic.sir ${SIR_DIST_TEST_EXAMPLES}/atomics_basic.sir
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/simd_basic.sir ${SIR_DIST_TEST_EXAMPLES}/simd_basic.sir
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/simd_shuffle_hsum.sir ${SIR_DIST_TEST_EXAMPLES}/simd_shuffle_hsum.sir
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/simd_load_store_vec.sir ${SIR_DIST_TEST_EXAMPLES}/simd_load_store_vec.sir
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/simd_atomic_mix.sir ${SIR_DIST_TEST_EXAMPLES}/simd_atomic_mix.sir
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/simd_masked_add.sir ${SIR_DIST_TEST_EXAMPLES}/simd_masked_add.sir
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/simd_gather_scatter_masked.sir ${SIR_DIST_TEST_EXAMPLES}/simd_gather_scatter_masked.sir
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/simd_atomic_per_lane.sir ${SIR_DIST_TEST_EXAMPLES}/simd_atomic_per_lane.sir
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/ptr_arith.sir ${SIR_DIST_TEST_EXAMPLES}/ptr_arith.sir
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/sirc/examples/float_cmp_select.sir ${SIR_DIST_TEST_EXAMPLES}/float_cmp_select.sir

    # VS Code extension (prebuilt .vsix, if present)
  )

  if(EXISTS ${SIR_VSCODE_EXT_VSIX} AND EXISTS ${SIR_VSCODE_EXT_README})
    add_custom_command(
      TARGET dist_sirc
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SIR_VSCODE_EXT_VSIX} ${SIR_DIST_DOC}/editors/sir-language-support-0.0.2.vsix
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SIR_VSCODE_EXT_README} ${SIR_DIST_DOC}/editors/sir-language-support.md
    )
  endif()
endif()

add_custom_target(dist DEPENDS dist_sircc)
if(TARGET sirc)
  add_dependencies(dist dist_sirc)
endif()
