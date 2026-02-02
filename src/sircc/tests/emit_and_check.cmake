if(NOT DEFINED SIRCC)
  set(SIRCC "sircc")
endif()

if(NOT DEFINED INPUT)
  message(FATAL_ERROR "emit_and_check.cmake: missing -DINPUT=... (sir.jsonl)")
endif()
if(NOT DEFINED OUTPUT)
  message(FATAL_ERROR "emit_and_check.cmake: missing -DOUTPUT=... (.ll)")
endif()
if(NOT DEFINED PATTERNS)
  message(FATAL_ERROR "emit_and_check.cmake: missing -DPATTERNS=... (list)")
endif()

execute_process(
  COMMAND "${SIRCC}" "${INPUT}" -o "${OUTPUT}" --emit-llvm
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
  message(FATAL_ERROR "sircc failed (rc=${rc})\n${out}\n${err}")
endif()

file(READ "${OUTPUT}" contents)

foreach(p IN LISTS PATTERNS)
  string(FIND "${contents}" "${p}" idx)
  if(idx EQUAL -1)
    message(FATAL_ERROR "missing expected pattern in ${OUTPUT}: ${p}")
  endif()
endforeach()
