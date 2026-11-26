if(NOT DEFINED MIKO_EXE)
    message(FATAL_ERROR "MIKO_EXE is required")
endif()
if(NOT DEFINED CLANG_EXE)
    message(FATAL_ERROR "CLANG_EXE is required")
endif()
if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "OUTPUT_DIR is required")
endif()

# Produces a miko.exe that is itself written in miko.
#
# Bootstrapping is unavoidable: something has to turn the first .ll into a
# binary. That first step uses the C++ stage0 (MIKO_EXE). Everything after it
# uses the binary this script just produced - so once this succeeds, the C++
# compiler is no longer needed to compile miko.
#
#   stage0 (C++) + compiler/main_ll.miko  -> miko_1.ll  --clang-->  miko.exe
#   miko.exe                              -> miko_2.ll
#   miko.exe (rebuilt from miko_2.ll)     -> miko_3.ll
#   require miko_2.ll == miko_3.ll
#
# The last step is what makes it self-hosting rather than merely self-compiled:
# a compiler built by the miko-written compiler reproduces the exact same IR.

set(out_dir "${OUTPUT_DIR}")
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

# Linking needs the C runtime search paths, and those are machine-specific. The
# compiler reads them from a config file at run time rather than hardcoding them,
# so the same miko.exe source works elsewhere by regenerating the config.
#
# Composing a command line by concatenation is fragile: `system()` goes through a
# shell, so every path containing a space has to be quoted, and cmd.exe has its
# own rules about a command that starts with a quote. So the build writes the
# quoting once, into a link script, and the compiler only ever passes two plain
# path arguments to it.
#
#   miko.toolchain   line 1: the link script to invoke
#   miko-link.bat    clang + flags, taking <ir.ll> <out.exe>
foreach(flag ${CLANG_CC_FLAGS} ${CLANG_LINK_FLAGS})
    string(APPEND tc_quoted " \"${flag}\"")
endforeach()
file(TO_NATIVE_PATH "${CLANG_EXE}" clang_native)
set(runtime_native "runtime\\miko_runtime.c")

if(WIN32)
    set(link_script "${out_dir}/miko-link.bat")
    # -w silences warnings from compiling the runtime. They are not actionable by
    # whoever is compiling their own program, and leaking them makes a successful
    # compile look like it went wrong.
    file(WRITE "${link_script}"
        "@echo off\r\n"
        "\"${clang_native}\" -w \"%~1\" ${runtime_native}${tc_quoted} -o \"%~2\"\r\n"
        "exit /b %ERRORLEVEL%\r\n")
else()
    set(link_script "${out_dir}/miko-link.sh")
    file(WRITE "${link_script}"
        "#!/bin/sh\n"
        "exec \"${clang_native}\" -w \"$1\" runtime/miko_runtime.c${tc_quoted} -o \"$2\"\n")
    execute_process(COMMAND chmod +x "${link_script}")
endif()

file(TO_NATIVE_PATH "${link_script}" link_script_native)
file(WRITE "${out_dir}/miko.toolchain" "${link_script_native}\n")
message(STATUS "toolchain config: ${out_dir}/miko.toolchain -> ${link_script_native}")

set(runtime_c "${SOURCE_DIR}/runtime/miko_runtime.c")
if(NOT EXISTS "${runtime_c}")
    message(FATAL_ERROR "missing runtime: ${runtime_c}")
endif()

function(run_step step_name)
    set(stdout_log "${out_dir}/${step_name}.stdout.log")
    set(stderr_log "${out_dir}/${step_name}.stderr.log")
    message(STATUS "miko-exe step: ${step_name}: ${ARGN}")
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE step_result
        OUTPUT_FILE "${stdout_log}"
        ERROR_FILE "${stderr_log}"
    )
    if(NOT step_result EQUAL 0)
        message(FATAL_ERROR
            "miko-exe step failed: ${step_name} (exit ${step_result})\n"
            "stdout: ${stdout_log}\n"
            "stderr: ${stderr_log}")
    endif()
endfunction()

# Generation 1: the C++ stage0 interprets the miko-written driver.
run_step("gen1_emit" "${MIKO_EXE}" "compiler/main_ll.miko" --run -- --self "${out_dir}/gen1")
if(NOT EXISTS "${out_dir}/gen1.ll")
    message(FATAL_ERROR "stage0 produced no IR at ${out_dir}/gen1.ll")
endif()
run_step("gen1_link" "${CLANG_EXE}" ${CLANG_CC_FLAGS}
    "${out_dir}/gen1.ll" "${runtime_c}" ${CLANG_LINK_FLAGS} -o "${out_dir}/miko.exe" -O1)

# Generation 2: the miko-written miko.exe compiles the compiler itself.
run_step("gen2_emit" "${out_dir}/miko.exe" --self "${out_dir}/gen2")
if(NOT EXISTS "${out_dir}/gen2.ll")
    message(FATAL_ERROR "miko.exe produced no IR at ${out_dir}/gen2.ll")
endif()

# Generation 3: rebuild miko.exe from generation 2 and compile once more. The
# IR from two independently built miko compilers must agree.
run_step("gen2_link" "${CLANG_EXE}" ${CLANG_CC_FLAGS}
    "${out_dir}/gen2.ll" "${runtime_c}" ${CLANG_LINK_FLAGS} -o "${out_dir}/miko_gen2.exe" -O1)
run_step("gen3_emit" "${out_dir}/miko_gen2.exe" --self "${out_dir}/gen3")

file(READ "${out_dir}/gen2.ll" gen2_ir)
file(READ "${out_dir}/gen3.ll" gen3_ir)
if(NOT gen2_ir STREQUAL gen3_ir)
    string(LENGTH "${gen2_ir}" gen2_len)
    string(LENGTH "${gen3_ir}" gen3_len)
    message(FATAL_ERROR
        "miko.exe is not a fixed point: gen2 IR (${gen2_len} bytes) != gen3 IR (${gen3_len} bytes).")
endif()
string(LENGTH "${gen3_ir}" ir_len)

# The binary has to behave like a compiler, not just exist.
run_step("version_check" "${out_dir}/miko.exe" --version)
file(READ "${out_dir}/version_check.stdout.log" version_out)
if(NOT version_out MATCHES "miko-implemented")
    message(FATAL_ERROR "miko.exe --version did not report a miko implementation:\n${version_out}")
endif()

if(NOT DEFINED SMOKE_SOURCE)
    set(SMOKE_SOURCE "examples/arrays_ir.miko")
endif()
if(NOT DEFINED SMOKE_EXPECTED)
    set(SMOKE_EXPECTED "60")
endif()
set(smoke_src "${SOURCE_DIR}/${SMOKE_SOURCE}")
file(TO_CMAKE_PATH "${smoke_src}" smoke_src)

run_step("compile_smoke" "${out_dir}/miko.exe" "${smoke_src}" "${out_dir}/smoke")

# No clang step here on purpose: compiling has to produce the executable. If the
# compiler only wrote IR, this test fails rather than papering over it.
if(NOT EXISTS "${out_dir}/smoke.exe")
    message(FATAL_ERROR
        "miko.exe compiled ${SMOKE_SOURCE} but produced no ${out_dir}/smoke.exe - "
        "compiling a program must yield an executable, not just IR.")
endif()
run_step("smoke_run" "${out_dir}/smoke.exe")
file(READ "${out_dir}/smoke_run.stdout.log" smoke_out)
if(NOT smoke_out MATCHES "${SMOKE_EXPECTED}")
    message(FATAL_ERROR
        "miko.exe miscompiled ${SMOKE_SOURCE}: expected /${SMOKE_EXPECTED}/, got:\n${smoke_out}")
endif()

# And it must agree with the interpreter on the same program.
run_step("interpreter_reference" "${MIKO_EXE}" "${smoke_src}" --run)
file(READ "${out_dir}/interpreter_reference.stdout.log" reference_out)
if(NOT smoke_out STREQUAL reference_out)
    message(FATAL_ERROR
        "miko.exe disagrees with the interpreter on ${SMOKE_SOURCE}.\n"
        "interpreter:\n${reference_out}\nmiko.exe:\n${smoke_out}")
endif()

message(STATUS "miko.exe (written in miko) built and verified: ${out_dir}/miko.exe")
message(STATUS "self-hosting fixed point: gen2 IR == gen3 IR (${ir_len} bytes)")
