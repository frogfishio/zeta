if(NOT DEFINED SIRCC)
  set(SIRCC "sircc")
endif()

if(NOT DEFINED INPUT)
  message(FATAL_ERROR "diff_compile_run_hl_vs_core.cmake: missing -DINPUT=... (sir.jsonl)")
endif()
if(NOT DEFINED CORE_OUT)
  message(FATAL_ERROR "diff_compile_run_hl_vs_core.cmake: missing -DCORE_OUT=... (sir.core.jsonl)")
endif()
if(NOT DEFINED EXE_HL)
  message(FATAL_ERROR "diff_compile_run_hl_vs_core.cmake: missing -DEXE_HL=... (hl exe path)")
endif()
if(NOT DEFINED EXE_CORE)
  message(FATAL_ERROR "diff_compile_run_hl_vs_core.cmake: missing -DEXE_CORE=... (core exe path)")
endif()

if(NOT DEFINED ARGS_EXTRA)
  set(ARGS_EXTRA)
endif()

execute_process(
  COMMAND "${SIRCC}" ${ARGS_EXTRA} "${INPUT}" -o "${EXE_HL}"
  RESULT_VARIABLE hl_compile_rc
  OUTPUT_VARIABLE hl_compile_out
  ERROR_VARIABLE hl_compile_err
)
if(NOT hl_compile_rc EQUAL 0)
  message(FATAL_ERROR "sircc compile HL failed (rc=${hl_compile_rc})\n${hl_compile_out}\n${hl_compile_err}")
endif()

execute_process(
  COMMAND "${EXE_HL}"
  RESULT_VARIABLE hl_run_rc
  OUTPUT_VARIABLE hl_run_out
  ERROR_VARIABLE hl_run_err
)

execute_process(
  COMMAND "${SIRCC}" --lower-hl --emit-sir-core "${CORE_OUT}" "${INPUT}"
  RESULT_VARIABLE lower_rc
  OUTPUT_VARIABLE lower_out
  ERROR_VARIABLE lower_err
)
if(NOT lower_rc EQUAL 0)
  message(FATAL_ERROR "sircc --lower-hl failed (rc=${lower_rc})\n${lower_out}\n${lower_err}")
endif()

execute_process(
  COMMAND "${SIRCC}" ${ARGS_EXTRA} "${CORE_OUT}" -o "${EXE_CORE}"
  RESULT_VARIABLE core_compile_rc
  OUTPUT_VARIABLE core_compile_out
  ERROR_VARIABLE core_compile_err
)
if(NOT core_compile_rc EQUAL 0)
  message(FATAL_ERROR "sircc compile Core failed (rc=${core_compile_rc})\n${core_compile_out}\n${core_compile_err}")
endif()

execute_process(
  COMMAND "${EXE_CORE}"
  RESULT_VARIABLE core_run_rc
  OUTPUT_VARIABLE core_run_out
  ERROR_VARIABLE core_run_err
)

if(NOT hl_run_rc EQUAL core_run_rc)
  message(FATAL_ERROR "exit code mismatch (hl=${hl_run_rc}, core=${core_run_rc})")
endif()
if(NOT hl_run_out STREQUAL core_run_out)
  message(FATAL_ERROR "stdout mismatch\n-- HL --\n${hl_run_out}\n-- CORE --\n${core_run_out}")
endif()
if(NOT hl_run_err STREQUAL core_run_err)
  message(FATAL_ERROR "stderr mismatch\n-- HL --\n${hl_run_err}\n-- CORE --\n${core_run_err}")
endif()

