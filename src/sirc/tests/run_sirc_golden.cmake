if(NOT DEFINED SIRC OR SIRC STREQUAL "")
  message(FATAL_ERROR "run_sirc_golden: missing -DSIRC")
endif()
if(NOT DEFINED INPUT OR INPUT STREQUAL "")
  message(FATAL_ERROR "run_sirc_golden: missing -DINPUT")
endif()
if(NOT DEFINED EXPECT OR EXPECT STREQUAL "")
  message(FATAL_ERROR "run_sirc_golden: missing -DEXPECT")
endif()
if(NOT DEFINED OUT OR OUT STREQUAL "")
  message(FATAL_ERROR "run_sirc_golden: missing -DOUT")
endif()

file(REMOVE "${OUT}")

execute_process(
  COMMAND ${SIRC} --emit-src both --ids stable ${INPUT} -o ${OUT}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
  message(STATUS "stdout:\n${out}")
  message(STATUS "stderr:\n${err}")
  message(FATAL_ERROR "sirc failed with rc=${rc}")
endif()

if(NOT EXISTS "${OUT}")
  message(FATAL_ERROR "expected output file to exist: ${OUT}")
endif()
if(NOT EXISTS "${EXPECT}")
  message(FATAL_ERROR "expected golden file to exist: ${EXPECT}")
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} -E compare_files ${OUT} ${EXPECT}
  RESULT_VARIABLE same
  OUTPUT_VARIABLE diff_out
  ERROR_VARIABLE diff_err
)

if(NOT same EQUAL 0)
  message(STATUS "compare_files stdout:\n${diff_out}")
  message(STATUS "compare_files stderr:\n${diff_err}")
  file(READ "${OUT}" got)
  file(READ "${EXPECT}" want)
  message(STATUS "---- got ----\n${got}")
  message(STATUS "---- want ----\n${want}")
  message(FATAL_ERROR "golden mismatch: ${INPUT}")
endif()
