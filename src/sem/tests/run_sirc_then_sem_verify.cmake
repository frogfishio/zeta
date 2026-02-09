if(NOT DEFINED SIRC OR SIRC STREQUAL "")
  message(FATAL_ERROR "run_sirc_then_sem_verify: missing -DSIRC")
endif()
if(NOT DEFINED SEM OR SEM STREQUAL "")
  message(FATAL_ERROR "run_sirc_then_sem_verify: missing -DSEM")
endif()
if(NOT DEFINED INPUT OR INPUT STREQUAL "")
  message(FATAL_ERROR "run_sirc_then_sem_verify: missing -DINPUT")
endif()
if(NOT DEFINED OUT OR OUT STREQUAL "")
  message(FATAL_ERROR "run_sirc_then_sem_verify: missing -DOUT")
endif()

file(REMOVE "${OUT}")

execute_process(
  COMMAND ${SIRC} --ids stable ${INPUT} -o ${OUT}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
  message(STATUS "stdout:\n${out}")
  message(STATUS "stderr:\n${err}")
  message(FATAL_ERROR "sirc failed with rc=${rc}")
endif()

execute_process(
  COMMAND ${SEM} --verify ${OUT}
  RESULT_VARIABLE rc2
  OUTPUT_VARIABLE out2
  ERROR_VARIABLE err2
)

if(NOT rc2 EQUAL 0)
  message(STATUS "stdout:\n${out2}")
  message(STATUS "stderr:\n${err2}")
  message(FATAL_ERROR "sem --verify failed with rc=${rc2}")
endif()
