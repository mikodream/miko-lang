if(NOT DEFINED MIKO_EXE)
    message(FATAL_ERROR "MIKO_EXE is required")
endif()
if(NOT DEFINED CLANG_EXE)
    message(FATAL_ERROR "CLANG_EXE is required")
endif()
if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()
if(NOT DEFINED BOOTSTRAP_OUT_DIR)
    message(FATAL_ERROR "BOOTSTRAP_OUT_DIR is required")
endif()

# Guards the claim that a miko program - written the way a user would write one,
# not a special build hook - can compile the whole miko compiler.
#
# compiler/build_compiler.miko imports the compiler's own modules and emits one
# .ll for the entire compiler. This test runs that program, links the result,
# and then uses the resulting executable to compile an ordinary miko file. That
# last step matters: a .ll that merely assembles proves nothing about whether the
# compiler it encodes actually works.

set(out_dir "${BOOTSTRAP_OUT_DIR}")
file(MAKE_DIRECTORY "${out_dir}")

if(NOT DEFINED C_COMPILER)
    set(C_COMPILER "${CLANG_EXE}")
endif()
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/MikoCCFlags.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/MikoCCFlags.cmake")
endif()
if(WIN32)
    if(NOT DEFINED CLANG_CC_FLAGS)
        set(CLANG_CC_FLAGS "${MIKO_CLANG_CC_FLAGS}")
    endif()
    if(NOT DEFINED CLANG_LINK_FLAGS)
        set(CLANG_LINK_FLAGS "${MIKO_CLANG_LINK_FLAGS}")
    endif()
else()
    set(CLANG_CC_FLAGS "")
    set(CLANG_LINK_FLAGS "")
endif()

set(runtime_c "${SOURCE_DIR}/runtime/miko_runtime.c")

function(run_step step_name)
    set(stdout_log "${out_dir}/${step_name}.stdout.log")
    set(stderr_log "${out_dir}/${step_name}.stderr.log")
    message(STATUS "driver-build step: ${step_name}: ${ARGN}")
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE step_result
        OUTPUT_FILE "${stdout_log}"
        ERROR_FILE "${stderr_log}"
    )
    if(NOT step_result EQUAL 0)
        message(FATAL_ERROR
            "driver-build step failed: ${step_name} (exit ${step_result})\n"
            "stdout: ${stdout_log}\n"
            "stderr: ${stderr_log}")
    endif()
endfunction()

# 1. The ordinary miko program compiles the compiler. It writes to a fixed
#    prefix under out/, so move the artifact into this test's directory.
run_step("driver_run" "${MIKO_EXE}" "compiler/build_compiler.miko" --run)
set(driver_ir "${SOURCE_DIR}/out/compiler_from_miko.ll")
if(NOT EXISTS "${driver_ir}")
    message(FATAL_ERROR "build_compiler.miko produced no IR at ${driver_ir}")
endif()
file(COPY "${driver_ir}" DESTINATION "${out_dir}")
file(RENAME "${out_dir}/compiler_from_miko.ll" "${out_dir}/driver_compiler.ll")

# 2. Link it against the runtime and run it as a compiler.
set(driver_exe "${out_dir}/driver_compiler.exe")
run_step("driver_link" "${CLANG_EXE}" ${CLANG_CC_FLAGS}
    "${out_dir}/driver_compiler.ll" "${runtime_c}" ${CLANG_LINK_FLAGS} -o "${driver_exe}" -O1)

# 3. Compile a program that exercises the constructs the IR backend had to gain
#    for this to be possible: array literals, runtime indexing, element writes,
#    string arrays.
if(NOT DEFINED DRIVER_PROBE_SOURCE)
    set(DRIVER_PROBE_SOURCE "examples/arrays_ir.miko")
endif()
if(NOT DEFINED DRIVER_PROBE_EXPECTED)
    set(DRIVER_PROBE_EXPECTED "60")
endif()
set(probe_src "${SOURCE_DIR}/${DRIVER_PROBE_SOURCE}")
file(TO_CMAKE_PATH "${probe_src}" probe_src)
run_step("driver_compile_probe" "${driver_exe}" "${probe_src}" "${out_dir}/driver_probe")
run_step("driver_probe_link" "${CLANG_EXE}" ${CLANG_CC_FLAGS}
    "${out_dir}/driver_probe.ll" "${runtime_c}" ${CLANG_LINK_FLAGS} -o "${out_dir}/driver_probe.exe" -O1)
run_step("driver_probe_run" "${out_dir}/driver_probe.exe")

file(READ "${out_dir}/driver_probe_run.stdout.log" probe_out)
if(NOT probe_out MATCHES "${DRIVER_PROBE_EXPECTED}")
    message(FATAL_ERROR
        "compiler built by a miko program produced wrong output for ${DRIVER_PROBE_SOURCE}: "
        "expected /${DRIVER_PROBE_EXPECTED}/, got:\n${probe_out}")
endif()

# The compiler must agree with the interpreter on the same program. This is the
# check that catches a backend which runs but computes something else.
run_step("interpreter_reference" "${MIKO_EXE}" "${probe_src}" --run)
file(READ "${out_dir}/interpreter_reference.stdout.log" reference_out)
if(NOT probe_out STREQUAL reference_out)
    message(FATAL_ERROR
        "compiler built by a miko program disagrees with the interpreter on ${DRIVER_PROBE_SOURCE}.\n"
        "interpreter:\n${reference_out}\ncompiled:\n${probe_out}")
endif()

message(STATUS "miko-written driver compiled the miko compiler, and it works: ${driver_exe}")
