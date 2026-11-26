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

# The miko LLVM backend compiles the whole program as a single IR module, so
# there is exactly one artifact per generation: whole_program.ll. The driver
# reads every module listed here, concatenates their ASTs, and emits one .ll.
#
# Building the compiler as one module is what makes cross-module calls
# resolvable by signature: the generator sees all definitions at once, so no
# external declarations are guessed and no return type is assumed. Definitions
# are disambiguated by module-name mangling because modules legitimately reuse
# private helper names (`find_char` exists in several of them).
set(miko_ll_modules
    token
    ast
    symbols
    type
    lexer
    parser
    sema
    codegen_ll
    main_ll
)

set(out_dir "${BOOTSTRAP_OUT_DIR}")
file(MAKE_DIRECTORY "${out_dir}")

# clang invoked through execute_process does not inherit the MSVC/SDK env vars a
# VS dev prompt would set. The shared detection module supplies the include
# paths the runtime needs plus the /LIBPATH: flags clang's MSVC-targeted linker
# expects.
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
if(NOT EXISTS "${runtime_c}")
    message(FATAL_ERROR "missing runtime: ${runtime_c}")
endif()

function(run_step step_name)
    set(stdout_log "${out_dir}/${step_name}.stdout.log")
    set(stderr_log "${out_dir}/${step_name}.stderr.log")
    message(STATUS "llvm-bootstrap step: ${step_name}: ${ARGN}")
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE step_result
        OUTPUT_FILE "${stdout_log}"
        ERROR_FILE "${stderr_log}"
    )
    if(NOT step_result EQUAL 0)
        message(FATAL_ERROR
            "llvm-bootstrap step failed: ${step_name} (exit ${step_result})\n"
            "stdout: ${stdout_log}\n"
            "stderr: ${stderr_log}")
    endif()
endfunction()

# stage0: the C++ miko binary interprets compiler/main_ll.miko, which lexes and
# parses every module and emits one .ll for the whole compiler.
run_step("stage0_emit_ir" "${MIKO_EXE}" "compiler/main_ll.miko" --run)
set(stage1_ll "${SOURCE_DIR}/out/whole_program.ll")
if(NOT EXISTS "${stage1_ll}")
    message(FATAL_ERROR "stage0 produced no IR at ${stage1_ll}")
endif()
file(COPY "${stage1_ll}" DESTINATION "${out_dir}")
file(RENAME "${out_dir}/whole_program.ll" "${out_dir}/stage1_ll.ll")

# stage1_ll: clang assembles the IR and links it against the runtime into a
# native executable. No C headers are involved - the IR declares the runtime
# entry points it calls - which is why this needs no SDK include paths.
set(stage1_exe "${out_dir}/stage1_ll.exe")
run_step("stage1_link" "${CLANG_EXE}" ${CLANG_CC_FLAGS} "${out_dir}/stage1_ll.ll" "${runtime_c}" ${CLANG_LINK_FLAGS} -o "${stage1_exe}" -O1)

# The IR-compiled compiler must also be a working miko compiler for ordinary
# programs, not just for itself.
if(NOT DEFINED LLVM_SAMPLE_SOURCE)
    set(LLVM_SAMPLE_SOURCE "examples/structs.miko")
endif()
if(NOT DEFINED LLVM_SAMPLE_EXPECTED)
    set(LLVM_SAMPLE_EXPECTED "10")
endif()
if(NOT IS_ABSOLUTE "${LLVM_SAMPLE_SOURCE}")
    set(LLVM_SAMPLE_SOURCE "${SOURCE_DIR}/${LLVM_SAMPLE_SOURCE}")
endif()
file(TO_CMAKE_PATH "${LLVM_SAMPLE_SOURCE}" LLVM_SAMPLE_SOURCE)

set(sample_prefix "${out_dir}/stage1_sample")
run_step("stage1_compile_sample" "${stage1_exe}" "${LLVM_SAMPLE_SOURCE}" "${sample_prefix}")

set(sample_exe "${out_dir}/stage1_sample.exe")
run_step("stage1_sample_link" "${CLANG_EXE}" ${CLANG_CC_FLAGS} "${sample_prefix}.ll" "${runtime_c}" ${CLANG_LINK_FLAGS} -o "${sample_exe}" -O1)
run_step("stage1_sample_run" "${sample_exe}")
file(READ "${out_dir}/stage1_sample_run.stdout.log" sample_output)
if(NOT sample_output MATCHES "${LLVM_SAMPLE_EXPECTED}")
    message(FATAL_ERROR
        "IR-compiled compiler produced wrong output for ${LLVM_SAMPLE_SOURCE}: "
        "expected /${LLVM_SAMPLE_EXPECTED}/, got:\n${sample_output}")
endif()

# stage2: the IR-compiled compiler compiles the compiler again, from source.
run_step("stage1_compile_self" "${stage1_exe}")
set(stage2_ll "${SOURCE_DIR}/out/whole_program.ll")
file(COPY "${stage2_ll}" DESTINATION "${out_dir}")
file(RENAME "${out_dir}/whole_program.ll" "${out_dir}/stage2_ll.ll")

# stage3: and once more, to observe the fixed point rather than a single step.
set(stage2_exe "${out_dir}/stage2_ll.exe")
run_step("stage2_link" "${CLANG_EXE}" ${CLANG_CC_FLAGS} "${out_dir}/stage2_ll.ll" "${runtime_c}" ${CLANG_LINK_FLAGS} -o "${stage2_exe}" -O1)
run_step("stage2_compile_self" "${stage2_exe}")
set(stage3_ll "${SOURCE_DIR}/out/whole_program.ll")
file(COPY "${stage3_ll}" DESTINATION "${out_dir}")
file(RENAME "${out_dir}/whole_program.ll" "${out_dir}/stage3_ll.ll")

# The self-hosting criterion for the LLVM backend: the IR emitted by a compiler
# that was itself built by LLVM is byte-identical to the IR emitted one
# generation later. This is a stronger statement than "it runs" - it says the
# backend is a fixed point of its own output, so no information is lost or
# drifting through the LLVM round trip.
#
# All three generations are compared, not just the last two. In practice the
# very first IR already reproduces itself, which is the strongest form of the
# property: a single pass through LLVM lands on the fixed point, so there is no
# generational drift to converge away at all. Comparing all three reports that
# directly instead of asserting it implicitly.
file(READ "${out_dir}/stage1_ll.ll" stage1_ir)
file(READ "${out_dir}/stage2_ll.ll" stage2_ir)
file(READ "${out_dir}/stage3_ll.ll" stage3_ir)
string(LENGTH "${stage1_ir}" stage1_len)
string(LENGTH "${stage2_ir}" stage2_len)
string(LENGTH "${stage3_ir}" stage3_len)
if(NOT stage2_ir STREQUAL stage3_ir)
    message(FATAL_ERROR
        "LLVM byte-fixed-point failed: stage2 IR (${stage2_len} bytes) != stage3 IR (${stage3_len} bytes). "
        "See ${out_dir}/stage1_compile_self.stderr.log and stage2_compile_self.stderr.log.")
endif()
if(NOT stage1_ir STREQUAL stage2_ir)
    message(FATAL_ERROR
        "LLVM IR drifted across one generation: stage1 IR (${stage1_len} bytes) != stage2 IR (${stage2_len} bytes). "
        "stage2 == stage3 still holds, so the chain is self-consistent, but the first LLVM-built "
        "compiler does not reproduce the IR that produced it.")
endif()
message(STATUS "llvm byte-fixed-point verified: stage1 IR == stage2 IR == stage3 IR (${stage3_len} bytes)")

message(STATUS "llvm bootstrap stage0->stage1->stage2 passed: ${stage2_exe}")
