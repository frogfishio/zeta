if(NOT DEFINED SIRCC)
  message(FATAL_ERROR "SIRCC is required")
endif()
if(NOT DEFINED OUT)
  message(FATAL_ERROR "OUT is required")
endif()

set(cmd_args "")
if(DEFINED ARGS_STR)
  # ARGS_STR is a single native-style command string (spaces/quotes allowed).
  separate_arguments(cmd_args NATIVE_COMMAND "${ARGS_STR}")
elseif(DEFINED ARGS)
  # ARGS is a CMake list (semicolon-separated).
  set(cmd_args ${ARGS})
endif()

execute_process(
  COMMAND ${SIRCC} ${cmd_args}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)

if(NOT rc EQUAL 0)
  message(STATUS "stderr:\n${stderr}")
  message(FATAL_ERROR "command failed (rc=${rc}): ${SIRCC} ${cmd_args}")
endif()

file(WRITE "${OUT}" "${stdout}")
