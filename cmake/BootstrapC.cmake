if(NOT DEFINED MIKO_EXE)
    message(FATAL_ERROR "MIKO_EXE is required")
endif()
if(NOT DEFINED C_COMPILER)
    message(FATAL_ERROR "C_COMPILER is required")
endif()
if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()
# On Windows, inject MSVC/SDK include+lib flags so the C compiler finds <stdio.h>.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/MikoCCFlags.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/MikoCCFlags.cmake")
endif()
if(NOT DEFINED BOOTSTRAP_NAME)
    set(BOOTSTRAP_NAME "functions")
endif()
if(NOT DEFINED BOOTSTRAP_SOURCE)
    set(BOOTSTRAP_SOURCE "examples/${BOOTSTRAP_NAME}.miko")
endif()
if(DEFINED EXPECTED_OUTPUT)
    set(expected "${EXPECTED_OUTPUT}")
else()
    set(expected "30")
endif()

if(DEFINED BOOTSTRAP_OUT_DIR)
    set(out_dir "${BOOTSTRAP_OUT_DIR}")
else()
    set(out_dir "${SOURCE_DIR}/out")
endif()
file(MAKE_DIRECTORY "${out_dir}")

set(output_prefix "${out_dir}/bootstrap_${BOOTSTRAP_NAME}")
message(STATUS "bootstrap C generate: ${BOOTSTRAP_SOURCE} -> ${output_prefix}.c")
execute_process(
    COMMAND "${MIKO_EXE}" "${SOURCE_DIR}/compiler/main.miko" --run -- "${BOOTSTRAP_SOURCE}" "${output_prefix}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE gen_result
    OUTPUT_VARIABLE gen_output
    ERROR_VARIABLE gen_error
)
if(NOT gen_result EQUAL 0)
    message(FATAL_ERROR "bootstrap C generation failed for ${BOOTSTRAP_SOURCE}: ${gen_error}\n${gen_output}")
endif()

set(c_file "${output_prefix}.c")
set(exe_file "${out_dir}/bootstrap_${BOOTSTRAP_NAME}_e2e.exe")
message(STATUS "bootstrap C compile: ${c_file} -> ${exe_file}")
execute_process(
    COMMAND "${C_COMPILER}" ${MIKO_C_CC_FLAGS} "${c_file}" ${MIKO_C_LINK_FLAGS} -o "${exe_file}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE cc_result
    OUTPUT_VARIABLE cc_output
    ERROR_VARIABLE cc_error
)
if(NOT cc_result EQUAL 0)
    message(FATAL_ERROR "bootstrap C compile failed for ${c_file}: ${cc_error}\n${cc_output}")
endif()

message(STATUS "bootstrap C run: ${exe_file}")
execute_process(
    COMMAND "${exe_file}" ${PROGRAM_ARGS}
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "bootstrap C program failed for ${exe_file}: ${run_error}\n${run_output}")
endif()
if(NOT run_output MATCHES "${expected}")
    message(FATAL_ERROR "bootstrap C program output mismatch for ${exe_file}: expected /${expected}/, got:\n${run_output}")
endif()
message(STATUS "bootstrap C program output: ${run_output}")
