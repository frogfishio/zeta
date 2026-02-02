if(NOT DEFINED SIRCC)
  set(SIRCC "sircc")
endif()

if(NOT DEFINED INPUT)
  message(FATAL_ERROR "run_and_expect_exit.cmake: missing -DINPUT=... (sir.jsonl)")
endif()
if(NOT DEFINED EXE)
  message(FATAL_ERROR "run_and_expect_exit.cmake: missing -DEXE=... (output exe path)")
endif()
if(NOT DEFINED EXPECT)
  message(FATAL_ERROR "run_and_expect_exit.cmake: missing -DEXPECT=... (expected exit code)")
endif()

execute_process(
  COMMAND "${SIRCC}" "${INPUT}" -o "${EXE}"
  RESULT_VARIABLE compile_rc
  OUTPUT_VARIABLE compile_out
  ERROR_VARIABLE compile_err
)

if(NOT compile_rc EQUAL 0)
  message(FATAL_ERROR "sircc compile failed (rc=${compile_rc})\n${compile_out}\n${compile_err}")
endif()

execute_process(
  COMMAND "${EXE}"
  RESULT_VARIABLE run_rc
)

if(NOT run_rc EQUAL EXPECT)
  message(FATAL_ERROR "unexpected exit code for ${EXE}: got ${run_rc}, want ${EXPECT}")
endif()
