# Expects:
#   -DSIRCC=<path to sircc>
#   -DARGS=<cmake list of args>
#   -DOUT=<path to output file>
#   -DEXPECT=<substring to find in output file>
# Optional:
#   -DEXPECT2=<substring>
#   -DEXPECT3=<substring>
#   -DEXPECT4=<substring>
#   -DEXPECT5=<substring>
#   -DEXPECT_EXIT_NONZERO=ON (default OFF)

if(NOT DEFINED SIRCC)
  message(FATAL_ERROR "expect_output_file_contains.cmake: missing -DSIRCC")
endif()
if(NOT DEFINED ARGS)
  set(ARGS)
endif()
if(NOT DEFINED OUT)
  message(FATAL_ERROR "expect_output_file_contains.cmake: missing -DOUT")
endif()
if(NOT DEFINED EXPECT)
  message(FATAL_ERROR "expect_output_file_contains.cmake: missing -DEXPECT")
endif()
if(NOT DEFINED EXPECT_EXIT_NONZERO)
  set(EXPECT_EXIT_NONZERO OFF)
endif()

execute_process(
  COMMAND ${SIRCC} ${ARGS}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)

if(EXPECT_EXIT_NONZERO)
  if(rc EQUAL 0)
    message(FATAL_ERROR "expected non-zero exit code, got 0\nstdout:\n${out}\nstderr:\n${err}")
  endif()
else()
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "expected exit code 0, got ${rc}\nstdout:\n${out}\nstderr:\n${err}")
  endif()
endif()

if(NOT EXISTS "${OUT}")
  message(FATAL_ERROR "expected output file to exist: ${OUT}\nstdout:\n${out}\nstderr:\n${err}")
endif()

file(READ "${OUT}" blob)

foreach(k EXPECT EXPECT2 EXPECT3 EXPECT4 EXPECT5)
  if(DEFINED ${k})
    set(exp "${${k}}")
    string(REPLACE "\\\"" "\"" exp "${exp}")
    string(FIND "${blob}" "${exp}" idx)
    if(idx EQUAL -1)
      message(FATAL_ERROR "output file did not contain '${exp}'\nfile: ${OUT}")
    endif()
  endif()
endforeach()

