if(NOT DEFINED SIRCC)
  set(SIRCC "sircc")
endif()

if(NOT DEFINED IRCHECK)
  message(FATAL_ERROR "emit_zasm_and_ircheck.cmake: missing -DIRCHECK=... (path to ircheck)")
endif()

if(NOT DEFINED INPUT)
  message(FATAL_ERROR "emit_zasm_and_ircheck.cmake: missing -DINPUT=... (sir.jsonl)")
endif()
if(NOT DEFINED OUTPUT)
  message(FATAL_ERROR "emit_zasm_and_ircheck.cmake: missing -DOUTPUT=... (.jsonl)")
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

