if(NOT DEFINED SIRC OR SIRC STREQUAL "")
  message(FATAL_ERROR "run_sirc_diag_expect: missing -DSIRC")
endif()
if(NOT DEFINED INPUT OR INPUT STREQUAL "")
  message(FATAL_ERROR "run_sirc_diag_expect: missing -DINPUT")
endif()
if(NOT DEFINED EXPECT_CODE OR EXPECT_CODE STREQUAL "")
  message(FATAL_ERROR "run_sirc_diag_expect: missing -DEXPECT_CODE")
endif()

set(extra_args)
if(DEFINED EXTRA_ARGS AND NOT EXTRA_ARGS STREQUAL "")
  set(extra_args ${EXTRA_ARGS})
endif()

execute_process(
  COMMAND ${SIRC} --lint --diagnostics json ${extra_args} ${INPUT}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)

# Expect: lint fails, rc=1, and stderr contains JSONL diags.
if(NOT rc EQUAL 1)
  message(STATUS "stdout:\n${out}")
  message(STATUS "stderr:\n${err}")
  message(FATAL_ERROR "expected rc=1 for bad input, got rc=${rc}")
endif()

string(FIND "${err}" "{\"k\":\"diag\"" has_diag)
if(has_diag EQUAL -1)
  message(STATUS "stderr:\n${err}")
  message(FATAL_ERROR "expected JSON diagnostics on stderr")
endif()

string(FIND "${err}" "\"code\":\"${EXPECT_CODE}\"" has_code)
if(has_code EQUAL -1)
  message(STATUS "stderr:\n${err}")
  message(FATAL_ERROR "expected diagnostic code: ${EXPECT_CODE}")
endif()

if(DEFINED EXPECT_LINE AND NOT EXPECT_LINE STREQUAL "")
  string(FIND "${err}" "\"line\":${EXPECT_LINE}" has_line)
  if(has_line EQUAL -1)
    message(STATUS "stderr:\n${err}")
    message(FATAL_ERROR "expected line: ${EXPECT_LINE}")
  endif()
endif()

if(DEFINED EXPECT_COL AND NOT EXPECT_COL STREQUAL "")
  string(FIND "${err}" "\"col\":${EXPECT_COL}" has_col)
  if(has_col EQUAL -1)
    message(STATUS "stderr:\n${err}")
    message(FATAL_ERROR "expected col: ${EXPECT_COL}")
  endif()
endif()

if(DEFINED EXPECT_MSG_SUBSTR AND NOT EXPECT_MSG_SUBSTR STREQUAL "")
  string(FIND "${err}" "${EXPECT_MSG_SUBSTR}" has_msg)
  if(has_msg EQUAL -1)
    message(STATUS "stderr:\n${err}")
    message(FATAL_ERROR "expected msg substring: ${EXPECT_MSG_SUBSTR}")
  endif()
endif()
