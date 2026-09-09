# Check both exit status and the summary: a crashed sanitizer/runtime must
# never be mistaken for a successful expected-failure regression test.
execute_process(COMMAND "${PROGRAM}" --quiet --no-color
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
if(NOT result STREQUAL "1" OR
   NOT output MATCHES "cases=8 subcases=0 checks=[0-9]+ failures=19" OR
   NOT output MATCHES "SOME FAILED")
    message(FATAL_ERROR "Expected 8 deliberate failing cases / 19 failures, got ${result}:\n${output}\n${errors}")
endif()
