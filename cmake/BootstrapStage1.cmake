if(NOT DEFINED MIKO_EXE)
    message(FATAL_ERROR "MIKO_EXE is required")
endif()
if(NOT DEFINED C_COMPILER)
    message(FATAL_ERROR "C_COMPILER is required")
endif()
if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()
if(NOT DEFINED BOOTSTRAP_OUT_DIR)
    message(FATAL_ERROR "BOOTSTRAP_OUT_DIR is required")
endif()

# On Windows, `execute_process` does not inherit the MSVC/SDK INCLUDE/LIB env
# vars that a VS dev prompt sets. Include the shared flag-detection module which
# sets MIKO_C_CC_FLAGS / MIKO_C_LINK_FLAGS, then alias them into C_CC_FLAGS /
# C_LINK_FLAGS used throughout this script.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/MikoCCFlags.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/MikoCCFlags.cmake")
endif()
if(WIN32)
    if(NOT DEFINED C_CC_FLAGS)
        set(C_CC_FLAGS "${MIKO_C_CC_FLAGS}")
    endif()
    if(NOT DEFINED C_LINK_FLAGS)
        set(C_LINK_FLAGS "${MIKO_C_LINK_FLAGS}")
    endif()
endif()

set(out_dir "${BOOTSTRAP_OUT_DIR}")
file(MAKE_DIRECTORY "${out_dir}")

if(NOT DEFINED STAGE1_SMOKE_SOURCE)
    set(STAGE1_SMOKE_SOURCE "examples/functions.miko")
endif()
if(NOT DEFINED STAGE1_SMOKE_EXPECTED)
    set(STAGE1_SMOKE_EXPECTED "30")
endif()
if(NOT DEFINED STAGE1_SELF_SOURCE)
    set(STAGE1_SELF_SOURCE "compiler/main.miko")
endif()
if(NOT IS_ABSOLUTE "${STAGE1_SMOKE_SOURCE}")
    set(STAGE1_SMOKE_SOURCE "${SOURCE_DIR}/${STAGE1_SMOKE_SOURCE}")
endif()
if(NOT IS_ABSOLUTE "${STAGE1_SELF_SOURCE}")
    set(STAGE1_SELF_SOURCE "${SOURCE_DIR}/${STAGE1_SELF_SOURCE}")
endif()
file(TO_CMAKE_PATH "${STAGE1_SMOKE_SOURCE}" STAGE1_SMOKE_SOURCE)
file(TO_CMAKE_PATH "${STAGE1_SELF_SOURCE}" STAGE1_SELF_SOURCE)
if(NOT EXISTS "${STAGE1_SMOKE_SOURCE}" AND EXISTS "${STAGE1_SMOKE_SOURCE}.miko")
    set(STAGE1_SMOKE_SOURCE "${STAGE1_SMOKE_SOURCE}.miko")
endif()
if(NOT EXISTS "${STAGE1_SELF_SOURCE}" AND EXISTS "${STAGE1_SELF_SOURCE}.miko")
    set(STAGE1_SELF_SOURCE "${STAGE1_SELF_SOURCE}.miko")
endif()

function(run_step step_name)
    set(stdout_log "${out_dir}/${step_name}.stdout.log")
    set(stderr_log "${out_dir}/${step_name}.stderr.log")
    message(STATUS "bootstrap stage1 step: ${step_name}: ${ARGN}")
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE step_result
        OUTPUT_FILE "${stdout_log}"
        ERROR_FILE "${stderr_log}"
    )
    if(NOT step_result EQUAL 0)
        message(FATAL_ERROR
            "bootstrap stage1 step failed: ${step_name} (exit ${step_result})\n"
            "stdout: ${stdout_log}\n"
            "stderr: ${stderr_log}")
    endif()
endfunction()

set(stage1_modules
    token
    ast
    symbols
    type
    lexer
    parser
    sema
    codegen_c
    main
)

foreach(module IN LISTS stage1_modules)
    run_step("stage0_generate_${module}"
        "${MIKO_EXE}" "${SOURCE_DIR}/compiler/main.miko" --run --
        "compiler/${module}.miko" "${out_dir}/stage1_${module}")
endforeach()

set(stage1_exe "${out_dir}/stage1_compiler.exe")

# The link line is derived from stage1_modules (main first so the entry point
# is obvious in diagnostics), so adding a compiler module cannot silently
# leave it out of the linked stage binaries.
function(module_link_sources generation out_var)
    set(sources "")
    foreach(module IN LISTS stage1_modules)
        list(APPEND sources "${out_dir}/${generation}_${module}.c")
    endforeach()
    set(${out_var} "${sources}" PARENT_SCOPE)
endfunction()

module_link_sources(stage1 stage1_sources)
run_step("stage1_link"
    "${C_COMPILER}" ${C_CC_FLAGS}
    ${stage1_sources}
    ${C_LINK_FLAGS} -o "${stage1_exe}")

set(stage1_sample_prefix "${out_dir}/stage1_sample")
run_step("stage1_compile_sample"
    "${stage1_exe}" "${STAGE1_SMOKE_SOURCE}" "${stage1_sample_prefix}")

set(stage2_sample_exe "${out_dir}/stage2_sample.exe")
run_step("stage2_sample_link"
    "${C_COMPILER}" ${C_CC_FLAGS} "${stage1_sample_prefix}.c" ${C_LINK_FLAGS} -o "${stage2_sample_exe}")

run_step("stage2_sample_run" "${stage2_sample_exe}")
file(READ "${out_dir}/stage2_sample_run.stdout.log" stage2_output)
if(NOT stage2_output MATCHES "${STAGE1_SMOKE_EXPECTED}")
    message(FATAL_ERROR
        "stage2 sample output mismatch: expected /${STAGE1_SMOKE_EXPECTED}/, got:\n${stage2_output}")
endif()

set(stage1_self_prefix "${out_dir}/stage1_self")
run_step("stage1_compile_self"
    "${stage1_exe}" "${STAGE1_SELF_SOURCE}" "${stage1_self_prefix}")

run_step("stage1_self_syntax"
    "${C_COMPILER}" ${C_CC_FLAGS} -fsyntax-only "${stage1_self_prefix}.c")

foreach(module IN LISTS stage1_modules)
    run_step("stage1_generate_stage2_${module}"
        "${stage1_exe}"
        "compiler/${module}.miko" "${out_dir}/stage2_${module}")
endforeach()

set(stage2_exe "${out_dir}/stage2_compiler.exe")
module_link_sources(stage2 stage2_sources)
run_step("stage2_link"
    "${C_COMPILER}" ${C_CC_FLAGS}
    ${stage2_sources}
    ${C_LINK_FLAGS} -o "${stage2_exe}")

set(stage2_sample_prefix "${out_dir}/stage2_sample")
run_step("stage2_compile_sample"
    "${stage2_exe}" "${STAGE1_SMOKE_SOURCE}" "${stage2_sample_prefix}")

set(stage2_final_exe "${out_dir}/stage2_sample.exe")
run_step("stage2_final_link"
    "${C_COMPILER}" ${C_CC_FLAGS} "${stage2_sample_prefix}.c" ${C_LINK_FLAGS} -o "${stage2_final_exe}")

run_step("stage2_final_run" "${stage2_final_exe}")
file(READ "${out_dir}/stage2_final_run.stdout.log" stage2_final_output)
if(NOT stage2_final_output MATCHES "${STAGE1_SMOKE_EXPECTED}")
    message(FATAL_ERROR
        "stage2-generated sample output mismatch: expected /${STAGE1_SMOKE_EXPECTED}/, got:\n${stage2_final_output}")
endif()

# Stronger completion criterion: the compiler must be a byte-fixed-point.
# g1 (interpreter-run, "immature") != g2 (first self-compiled), but g2 == g3 == ...
# because the string-rewriter is idempotent after the first self-compile.
# Verify by generating a third generation and asserting stage2 == stage3.
set(stage3_exe "${out_dir}/stage3_compiler.exe")
foreach(module IN LISTS stage1_modules)
    run_step("stage2_generate_stage3_${module}"
        "${stage2_exe}"
        "compiler/${module}.miko" "${out_dir}/stage3_${module}")
endforeach()

module_link_sources(stage3 stage3_sources)
run_step("stage3_link"
    "${C_COMPILER}" ${C_CC_FLAGS}
    ${stage3_sources}
    ${C_LINK_FLAGS} -o "${stage3_exe}")

# Byte-identity check: stage2 vs stage3 (the settled fixed point)
foreach(module IN LISTS stage1_modules)
    set(stage2_mod_c "${out_dir}/stage2_${module}.c")
    set(stage3_mod_c "${out_dir}/stage3_${module}.c")
    if(NOT EXISTS "${stage2_mod_c}")
        message(FATAL_ERROR "byte-identity check: missing ${stage2_mod_c}")
    endif()
    if(NOT EXISTS "${stage3_mod_c}")
        message(FATAL_ERROR "byte-identity check: missing ${stage3_mod_c}")
    endif()
    file(READ "${stage2_mod_c}" stage2_mod_text)
    file(READ "${stage3_mod_c}" stage3_mod_text)
    if(NOT stage2_mod_text STREQUAL stage3_mod_text)
        message(FATAL_ERROR
            "byte-identity check failed: stage2 vs stage3 C output for compiler/${module}.miko differs. "
            "The codegen is not yet a byte-fixed-point; see stage-chain/${module} logs.")
    endif()
    message(STATUS "byte-identity ok: compiler/${module}.miko (stage2==stage3)")
endforeach()

message(STATUS "bootstrap stage0->stage1->stage2 smoke passed: ${stage2_exe}")
message(STATUS "bootstrap byte-fixed-point verified: stage2 C output == stage3 C output (all modules)")
