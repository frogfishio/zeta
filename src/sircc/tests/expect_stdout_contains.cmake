# Expects:
#   -DSIRCC=<path to sircc>
#   -DARGS=<cmake list of args>
#   -DEXPECT=<substring to find in stdout>
# Optional:
#   -DEXPECT2=<substring>
#   -DEXPECT3=<substring>
#   -DEXPECT_EXIT_NONZERO=OFF (default OFF)

if(NOT DEFINED SIRCC)
  message(FATAL_ERROR "expect_stdout_contains.cmake: missing -DSIRCC")
endif()
if(NOT DEFINED ARGS)
  set(ARGS)
endif()
if(NOT DEFINED EXPECT)
  message(FATAL_ERROR "expect_stdout_contains.cmake: missing -DEXPECT")
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

foreach(k EXPECT EXPECT2 EXPECT3)
  if(DEFINED ${k})
    set(exp "${${k}}")
    # allow callers to pass escaped quotes (e.g. \\\"k\\\":\\\"diag\\\")
    string(REPLACE "\\\"" "\"" exp "${exp}")
    string(FIND "${out}" "${exp}" idx)
    if(idx EQUAL -1)
      message(FATAL_ERROR "stdout did not contain '${exp}'\nstdout:\n${out}\nstderr:\n${err}")
    endif()
  endif()
endforeach()

