if(NOT DEFINED SIRCC)
  set(SIRCC "sircc")
endif()

if(NOT DEFINED INPUT)
  message(FATAL_ERROR "lower_hl_emit_core_and_verify.cmake: missing -DINPUT=... (sir.jsonl)")
endif()
if(NOT DEFINED OUT)
  message(FATAL_ERROR "lower_hl_emit_core_and_verify.cmake: missing -DOUT=... (sir.core.jsonl)")
endif()

execute_process(
  COMMAND "${SIRCC}" --lower-hl --emit-sir-core "${OUT}" "${INPUT}"
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
  message(FATAL_ERROR "sircc --lower-hl failed (rc=${rc})\n${out}\n${err}")
endif()

execute_process(
  COMMAND "${SIRCC}" --verify-only "${OUT}"
  RESULT_VARIABLE vrc
  OUTPUT_VARIABLE vout
  ERROR_VARIABLE verr
)

if(NOT vrc EQUAL 0)
  message(FATAL_ERROR "sircc --verify-only failed on emitted core (rc=${vrc})\n${vout}\n${verr}")
endif()

file(READ "${OUT}" contents)

if(DEFINED CONTAINS)
  foreach(p IN LISTS CONTAINS)
    set(pp "${p}")
    string(REPLACE "\\\"" "\"" pp "${pp}")
    string(FIND "${contents}" "${pp}" idx)
    if(idx EQUAL -1)
      message(FATAL_ERROR "missing expected pattern in ${OUT}: ${pp}")
    endif()
  endforeach()
endif()

if(DEFINED NOT_CONTAINS)
  foreach(p IN LISTS NOT_CONTAINS)
    set(pp "${p}")
    string(REPLACE "\\\"" "\"" pp "${pp}")
    string(FIND "${contents}" "${pp}" idx)
    if(NOT idx EQUAL -1)
      message(FATAL_ERROR "found unexpected pattern in ${OUT}: ${pp}")
    endif()
  endforeach()
endif()
