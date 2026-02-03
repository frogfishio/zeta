if(NOT DEFINED SIRCC)
  set(SIRCC "sircc")
endif()

if(NOT DEFINED ZEM)
  message(FATAL_ERROR "emit_zasm_and_run_zem.cmake: missing -DZEM=... (path to zem)")
endif()

if(NOT DEFINED IRCHECK)
  message(FATAL_ERROR "emit_zasm_and_run_zem.cmake: missing -DIRCHECK=... (path to ircheck)")
endif()

if(NOT DEFINED INPUT)
  message(FATAL_ERROR "emit_zasm_and_run_zem.cmake: missing -DINPUT=... (sir.jsonl)")
endif()
if(NOT DEFINED OUTPUT)
  message(FATAL_ERROR "emit_zasm_and_run_zem.cmake: missing -DOUTPUT=... (.jsonl)")
endif()
if(NOT DEFINED EXPECT_STDOUT)
  message(FATAL_ERROR "emit_zasm_and_run_zem.cmake: missing -DEXPECT_STDOUT=... (substring)")
endif()

execute_process(
  COMMAND "${SIRCC}" "${INPUT}" -o "${OUTPUT}" --emit-zasm
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
  message(FATAL_ERROR "sircc --emit-zasm failed (rc=${rc})\n${out}\n${err}")
endif()

execute_process(
  COMMAND "${IRCHECK}" --tool --ir v1.1 "${OUTPUT}"
  RESULT_VARIABLE rc2
  OUTPUT_VARIABLE out2
  ERROR_VARIABLE err2
)

if(NOT rc2 EQUAL 0)
  message(FATAL_ERROR "ircheck failed (rc=${rc2})\n${out2}\n${err2}")
endif()

execute_process(
  COMMAND "${ZEM}" "${OUTPUT}"
  RESULT_VARIABLE rc3
  OUTPUT_VARIABLE out3
  ERROR_VARIABLE err3
)

if(NOT rc3 EQUAL 0)
  message(FATAL_ERROR "zem failed (rc=${rc3})\nstdout:\n${out3}\nstderr:\n${err3}")
endif()

string(FIND "${out3}" "${EXPECT_STDOUT}" idx)
if(idx EQUAL -1)
  message(FATAL_ERROR "zem stdout missing expected substring: ${EXPECT_STDOUT}\nstdout:\n${out3}\nstderr:\n${err3}")
endif()

