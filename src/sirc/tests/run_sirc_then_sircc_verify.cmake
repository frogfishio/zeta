if(NOT DEFINED SIRC)
  message(FATAL_ERROR "SIRC not set")
endif()
if(NOT DEFINED SIRCC)
  message(FATAL_ERROR "SIRCC not set")
endif()
if(NOT DEFINED INPUT)
  message(FATAL_ERROR "INPUT not set")
endif()
if(NOT DEFINED OUT)
  message(FATAL_ERROR "OUT not set")
endif()

execute_process(
  COMMAND "${SIRC}" "${INPUT}" -o "${OUT}"
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "sirc failed (${rc}):\n${err}")
endif()

execute_process(
  COMMAND "${SIRCC}" --verify-only "${OUT}"
  RESULT_VARIABLE rc2
  OUTPUT_VARIABLE out2
  ERROR_VARIABLE err2
)
if(NOT rc2 EQUAL 0)
  message(FATAL_ERROR "sircc --verify-only failed (${rc2}):\n${err2}")
endif()

