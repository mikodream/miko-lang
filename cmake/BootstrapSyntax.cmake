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
if(NOT DEFINED BOOTSTRAP_SOURCE)
    message(FATAL_ERROR "BOOTSTRAP_SOURCE is required")
endif()
if(NOT DEFINED BOOTSTRAP_PREFIX)
    message(FATAL_ERROR "BOOTSTRAP_PREFIX is required")
endif()

if(DEFINED BOOTSTRAP_OUT_DIR AND NOT IS_ABSOLUTE "${BOOTSTRAP_PREFIX}")
    set(BOOTSTRAP_PREFIX "${BOOTSTRAP_OUT_DIR}/${BOOTSTRAP_PREFIX}")
endif()
get_filename_component(out_dir "${BOOTSTRAP_PREFIX}" DIRECTORY)
file(MAKE_DIRECTORY "${out_dir}")

message(STATUS "bootstrap syntax generate: ${BOOTSTRAP_SOURCE} -> ${BOOTSTRAP_PREFIX}.c")
execute_process(
    COMMAND "${MIKO_EXE}" "${SOURCE_DIR}/compiler/main.miko" --run -- "${BOOTSTRAP_SOURCE}" "${BOOTSTRAP_PREFIX}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE gen_result
    OUTPUT_VARIABLE gen_output
    ERROR_VARIABLE gen_error
)
if(NOT gen_result EQUAL 0)
    message(FATAL_ERROR "bootstrap C generation failed for ${BOOTSTRAP_SOURCE}: ${gen_error}\n${gen_output}")
endif()

message(STATUS "bootstrap syntax check: ${BOOTSTRAP_PREFIX}.c")
execute_process(
    COMMAND "${C_COMPILER}" ${MIKO_C_CC_FLAGS} -fsyntax-only "${BOOTSTRAP_PREFIX}.c"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE cc_result
    OUTPUT_VARIABLE cc_output
    ERROR_VARIABLE cc_error
)
if(NOT cc_result EQUAL 0)
    message(FATAL_ERROR "bootstrap C syntax check failed for ${BOOTSTRAP_PREFIX}.c: ${cc_error}\n${cc_output}")
endif()
