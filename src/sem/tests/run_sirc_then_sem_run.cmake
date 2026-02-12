if(NOT DEFINED SIRC OR SIRC STREQUAL "")
  message(FATAL_ERROR "run_sirc_then_sem_run: missing -DSIRC")
endif()
if(NOT DEFINED SEM OR SEM STREQUAL "")
  message(FATAL_ERROR "run_sirc_then_sem_run: missing -DSEM")
endif()
if(NOT DEFINED INPUT OR INPUT STREQUAL "")
  message(FATAL_ERROR "run_sirc_then_sem_run: missing -DINPUT")
endif()
if(NOT DEFINED OUT OR OUT STREQUAL "")
  message(FATAL_ERROR "run_sirc_then_sem_run: missing -DOUT")
endif()

# Optional: extra args to pass to `sem --run` (CMake list).
if(NOT DEFINED SEM_ARGS)
  set(SEM_ARGS "")
endif()

# Optional: file to redirect to `sem --run` stdin.
if(NOT DEFINED SEM_STDIN_FILE)
  set(SEM_STDIN_FILE "")
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

if(SEM_STDIN_FILE STREQUAL "")
  execute_process(
    COMMAND ${SEM} --run ${OUT} ${SEM_ARGS}
    RESULT_VARIABLE rc2
    OUTPUT_VARIABLE out2
    ERROR_VARIABLE err2
  )
else()
  execute_process(
    COMMAND ${SEM} --run ${OUT} ${SEM_ARGS}
    INPUT_FILE ${SEM_STDIN_FILE}
    RESULT_VARIABLE rc2
    OUTPUT_VARIABLE out2
    ERROR_VARIABLE err2
  )
endif()

if(NOT rc2 EQUAL 0)
  message(STATUS "stdout:\n${out2}")
  message(STATUS "stderr:\n${err2}")
  message(FATAL_ERROR "sem --run failed with rc=${rc2}")
endif()
