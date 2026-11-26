if(NOT DEFINED COMMAND_PATH)
    message(FATAL_ERROR "COMMAND_PATH is required")
endif()

if(NOT DEFINED EXPECTED_ERROR)
    message(FATAL_ERROR "EXPECTED_ERROR is required")
endif()

execute_process(
    COMMAND "${COMMAND_PATH}" ${COMMAND_ARGS}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)

set(combined_output "${output}${error}")

if(result EQUAL 0)
    message(FATAL_ERROR "expected command to fail, but it succeeded\n${combined_output}")
endif()

string(FIND "${combined_output}" "${EXPECTED_ERROR}" match_index)
if(match_index EQUAL -1)
    message(FATAL_ERROR "expected error was not found: ${EXPECTED_ERROR}\n${combined_output}")
endif()
