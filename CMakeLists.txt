cmake_minimum_required(VERSION 3.20)
project(miko LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MIKO_ENABLE_LLVM "Enable LLVM integration" OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(MIKO_ENABLE_LLVM AND NOT LLVM_DIR)
    # A developer checkout that keeps LLVM unpacked in a known place can point at
    # it here, but only if it actually exists: a hard-coded path silently wins
    # over find_package() and then fails on every machine that is not this one
    # (which is what happened to CI on Linux and macOS).
    if(EXISTS "C:/Users/Administrator/env/llvm/lib/cmake/llvm")
        set(LLVM_DIR "C:/Users/Administrator/env/llvm/lib/cmake/llvm")
    endif()
endif()

if(WIN32 AND MIKO_ENABLE_LLVM AND NOT MSVC)
    message(FATAL_ERROR
        "MIKO_ENABLE_LLVM=ON on Windows requires an MSVC-compatible generator for the LLVM package at LLVM_DIR. "
        "Use: cmake -S . -B build-vs -G \"Visual Studio 18 2026\" -A x64 -DMIKO_ENABLE_LLVM=ON")
endif()

set(MIKO_SOURCES
    src/main.cpp
    src/ast/Ast.cpp
    src/codegen/NativeCompiler.cpp
    src/lexer/Lexer.cpp
    src/lexer/Token.cpp
    src/parser/Parser.cpp
    src/runtime/Interpreter.cpp
    src/runtime/Value.cpp
    src/sema/Sema.cpp
    src/sema/Type.cpp
)

add_executable(miko ${MIKO_SOURCES})

if(MINGW)
    target_link_options(miko PRIVATE -static-libgcc -static-libstdc++)
endif()

target_include_directories(miko PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(miko PRIVATE
    # Kept in step with the version the self-hosted compiler prints in
    # compiler/main_ll.miko. The two drifted once (0.1.0 here, 0.2.0 there), which
    # made "which compiler built this binary" unanswerable.
    MIKO_VERSION="0.2.0"
)

target_compile_options(miko PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

if(MIKO_ENABLE_LLVM)
    # find_package(LLVM) only searches the standard CMake prefixes, and LLVM rarely
    # lands in one: a user's unpacked tarball, choco's tools directory, and
    # Homebrew's keg all sit somewhere else. Rather than make every caller pass
    # LLVM_DIR - which is what the CI workflows had to do, and what made the build
    # fail the first time it ran anywhere but the machine it was written on - look
    # in the places LLVM actually installs to before giving up.
    if(NOT LLVM_DIR)
        find_program(MIKO_LLVM_CONFIG NAMES llvm-config llvm-config.exe)
        if(MIKO_LLVM_CONFIG)
            execute_process(
                COMMAND ${MIKO_LLVM_CONFIG} --cmakedir
                OUTPUT_VARIABLE MIKO_LLVM_CMAKEDIR
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
            if(MIKO_LLVM_CMAKEDIR AND EXISTS "${MIKO_LLVM_CMAKEDIR}/LLVMConfig.cmake")
                set(LLVM_DIR "${MIKO_LLVM_CMAKEDIR}")
            endif()
        endif()
    endif()

    if(NOT LLVM_DIR)
        foreach(_candidate
            "$ENV{LLVM_ROOT}/lib/cmake/llvm"
            "$ENV{LLVM_INSTALL_DIR}/lib/cmake/llvm"
            "C:/Program Files/LLVM/lib/cmake/llvm"
            "C:/ProgramData/chocolatey/lib/llvm/tools/LLVM/lib/cmake/llvm"
            "$ENV{ProgramFiles}/LLVM/lib/cmake/llvm"
            "/usr/lib/llvm-22/lib/cmake/llvm"
            "/usr/lib/llvm-21/lib/cmake/llvm"
            "/usr/lib/llvm-20/lib/cmake/llvm"
            "/usr/lib/llvm-19/lib/cmake/llvm"
            "/usr/lib/llvm-18/lib/cmake/llvm"
            "/usr/local/opt/llvm/lib/cmake/llvm"
            "/opt/homebrew/opt/llvm/lib/cmake/llvm"
        )
            if(EXISTS "${_candidate}/LLVMConfig.cmake")
                set(LLVM_DIR "${_candidate}")
                break()
            endif()
        endforeach()
    endif()

    # LLVM's exported targets name optional dependencies without defining them:
    # LLVMSupport lists ZLIB::ZLIB and zstd::libzstd_static, LLVMDebugInfoPDB lists
    # DIASDK::Diaguids. A distribution's LLVM usually defines these through its own
    # config. The official release archive does not, and LLVMExports.cmake then
    # refuses to load, one missing name at a time.
    #
    # An empty interface target gets configure through but is not enough to link:
    # LLVMSupport's Compression.cpp really does call crc32/compress2 and the zstd
    # entry points, and the linker pulls that object in. So when zlib and zstd can
    # be found, link them for real; when they cannot, the stubs still let configure
    # succeed and the link error names exactly what is missing.
    if(NOT TARGET ZLIB::ZLIB)
        find_package(ZLIB QUIET)
    endif()
    if(NOT TARGET ZLIB::ZLIB AND ZLIB_FOUND)
        add_library(ZLIB::ZLIB UNKNOWN IMPORTED)
        set_target_properties(ZLIB::ZLIB PROPERTIES
            IMPORTED_LOCATION "${ZLIB_LIBRARIES}"
            INTERFACE_INCLUDE_DIRECTORIES "${ZLIB_INCLUDE_DIRS}")
    endif()

    # zstd has no FindZstd module, so accept explicit locations and fall back to a
    # search of the usual prefixes.
    if(NOT MIKO_ZSTD_LIBRARY)
        find_library(MIKO_ZSTD_LIBRARY NAMES zstd_static zstd libzstd_static)
    endif()
    if(NOT MIKO_ZSTD_INCLUDE_DIR)
        find_path(MIKO_ZSTD_INCLUDE_DIR NAMES zstd.h)
    endif()
    if(MIKO_ZSTD_LIBRARY AND NOT TARGET zstd::libzstd_static)
        add_library(zstd::libzstd_static UNKNOWN IMPORTED)
        set_target_properties(zstd::libzstd_static PROPERTIES
            IMPORTED_LOCATION "${MIKO_ZSTD_LIBRARY}")
        if(MIKO_ZSTD_INCLUDE_DIR)
            set_target_properties(zstd::libzstd_static PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${MIKO_ZSTD_INCLUDE_DIR}")
        endif()
    endif()

    foreach(_stub
        ZLIB::ZLIB
        zstd::libzstd_static
        zstd::libzstd_shared
        DIASDK::Diaguids
        Terminfo::terminfo
        LibXml2::LibXml2
        CURL::libcurl
    )
        if(NOT TARGET ${_stub})
            add_library(${_stub} INTERFACE IMPORTED)
        endif()
    endforeach()

    find_package(LLVM REQUIRED CONFIG)

    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
    message(STATUS "LLVM_DIR: ${LLVM_DIR}")
    message(STATUS "LLVM include dirs: ${LLVM_INCLUDE_DIRS}")

    target_include_directories(miko PRIVATE ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(miko PRIVATE ${LLVM_DEFINITIONS} MIKO_ENABLE_LLVM=1)

    llvm_map_components_to_libnames(LLVM_LIBS
        core
        support
        native
        target
        passes
    )

    target_link_libraries(miko PRIVATE ${LLVM_LIBS})
else()
    target_compile_definitions(miko PRIVATE MIKO_ENABLE_LLVM=0)
endif()

enable_testing()

# Bootstrap e2e tests drive the C compiler with clang/GNU-style flags
# (see cmake/BootstrapStage1.cmake and cmake/BootstrapC.cmake). On Windows a
# Visual Studio generator would otherwise hand them cl.exe, which neither
# understands those flags nor works without a VS dev prompt environment.
# Prefer clang when it is available - the miko --compile path already
# requires it.
set(MIKO_TEST_C_COMPILER "${CMAKE_C_COMPILER}")
if(WIN32)
    find_program(MIKO_CLANG_C_COMPILER clang)
    if(MIKO_CLANG_C_COMPILER)
        set(MIKO_TEST_C_COMPILER "${MIKO_CLANG_C_COMPILER}")
    endif()
endif()

set(MIKO_BOOTSTRAP_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/bootstrap")

add_test(NAME miko_version
    COMMAND miko --version
)

add_test(NAME miko_hello_tokens
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/hello.miko --debug-tokens
)

add_test(NAME miko_hello_ast
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/hello.miko --debug-ast
)

add_test(NAME miko_hello_types
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/hello.miko --debug-types
)

add_test(NAME miko_control_ast
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/control.miko --debug-ast
)

add_test(NAME miko_control_types
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/control.miko --debug-types
)

add_test(NAME miko_functions_ast
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/functions.miko --debug-ast
)

add_test(NAME miko_functions_types
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/functions.miko --debug-types
)

add_test(NAME miko_loops_unary_char_types
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/loops_unary_char.miko --debug-types
)

add_test(NAME miko_const_types
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/const.miko --debug-types
)

add_test(NAME miko_arrays_types
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/arrays.miko --debug-types
)

add_test(NAME miko_array_assign_types
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/array_assign.miko --debug-types
)

add_test(NAME miko_strings_runtime_types
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/strings_runtime.miko --debug-types
)

add_test(NAME miko_modules_types
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/modules.miko --debug-types
)
set_tests_properties(miko_modules_types PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(NAME miko_package_import_types
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/package_import.miko --debug-types
)
set_tests_properties(miko_package_import_types PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(NAME miko_module_struct_types
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/module_struct_types.miko --debug-types
)
set_tests_properties(miko_module_struct_types PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(NAME miko_structs_types
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/structs.miko --debug-types
)

add_test(NAME miko_const_assign_error
    COMMAND ${CMAKE_COMMAND}
        -DCOMMAND_PATH=$<TARGET_FILE:miko>
        "-DCOMMAND_ARGS=${CMAKE_CURRENT_SOURCE_DIR}/examples/const_assign_error.miko;--debug-types"
        "-DEXPECTED_ERROR=cannot assign to const variable 'answer'"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectFailure.cmake
)

add_test(NAME miko_array_index_error
    COMMAND ${CMAKE_COMMAND}
        -DCOMMAND_PATH=$<TARGET_FILE:miko>
        "-DCOMMAND_ARGS=${CMAKE_CURRENT_SOURCE_DIR}/examples/array_index_error.miko;--debug-types"
        "-DEXPECTED_ERROR=array index must be an integer"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectFailure.cmake
)

add_test(NAME miko_array_const_assign_error
    COMMAND ${CMAKE_COMMAND}
        -DCOMMAND_PATH=$<TARGET_FILE:miko>
        "-DCOMMAND_ARGS=${CMAKE_CURRENT_SOURCE_DIR}/examples/array_const_assign_error.miko;--debug-types"
        "-DEXPECTED_ERROR=cannot assign to const array variable 'nums'"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectFailure.cmake
)

add_test(NAME miko_array_length_error
    COMMAND ${CMAKE_COMMAND}
        -DCOMMAND_PATH=$<TARGET_FILE:miko>
        "-DCOMMAND_ARGS=${CMAKE_CURRENT_SOURCE_DIR}/examples/array_length_error.miko;--debug-types"
        "-DEXPECTED_ERROR=cannot initialize variable 'nums' of type '[i32; 4]' with array of type '[i32; 3]'"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectFailure.cmake
)

add_test(NAME miko_module_private_error
    COMMAND ${CMAKE_COMMAND}
        -DCOMMAND_PATH=$<TARGET_FILE:miko>
        "-DCOMMAND_ARGS=${CMAKE_CURRENT_SOURCE_DIR}/examples/module_private_error.miko;--debug-types"
        "-DEXPECTED_ERROR=module alias 'math' does not export function 'pick_low'"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectFailure.cmake
)
set_tests_properties(miko_module_private_error PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(NAME miko_module_alias_error
    COMMAND ${CMAKE_COMMAND}
        -DCOMMAND_PATH=$<TARGET_FILE:miko>
        "-DCOMMAND_ARGS=${CMAKE_CURRENT_SOURCE_DIR}/examples/module_alias_error.miko;--debug-types"
        "-DEXPECTED_ERROR=duplicate import alias 'core'"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectFailure.cmake
)
set_tests_properties(miko_module_alias_error PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(NAME miko_struct_field_error
    COMMAND ${CMAKE_COMMAND}
        -DCOMMAND_PATH=$<TARGET_FILE:miko>
        "-DCOMMAND_ARGS=${CMAKE_CURRENT_SOURCE_DIR}/examples/struct_field_error.miko;--debug-types"
        "-DEXPECTED_ERROR=struct 'Point' has no field 'z'"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectFailure.cmake
)

add_test(NAME miko_struct_missing_field_error
    COMMAND ${CMAKE_COMMAND}
        -DCOMMAND_PATH=$<TARGET_FILE:miko>
        "-DCOMMAND_ARGS=${CMAKE_CURRENT_SOURCE_DIR}/examples/struct_missing_field_error.miko;--debug-types"
        "-DEXPECTED_ERROR=missing field 'y' for struct 'Point'"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectFailure.cmake
)

add_test(NAME miko_hello_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/hello.miko --run
)
set_tests_properties(miko_hello_run PROPERTIES
    PASS_REGULAR_EXPRESSION "30"
)

add_test(NAME miko_control_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/control.miko --run
)
set_tests_properties(miko_control_run PROPERTIES
    PASS_REGULAR_EXPRESSION "0\n1\n2\nthree\n4"
)

add_test(NAME miko_functions_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/functions.miko --run
)
set_tests_properties(miko_functions_run PROPERTIES
    PASS_REGULAR_EXPRESSION "30"
)

add_test(NAME miko_loops_unary_char_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/loops_unary_char.miko --run
)
set_tests_properties(miko_loops_unary_char_run PROPERTIES
    PASS_REGULAR_EXPRESSION "8\ntrue\n-8\nA"
)

add_test(NAME miko_const_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/const.miko --run
)
set_tests_properties(miko_const_run PROPERTIES
    PASS_REGULAR_EXPRESSION "42"
)

add_test(NAME miko_arrays_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/arrays.miko --run
)
set_tests_properties(miko_arrays_run PROPERTIES
    PASS_REGULAR_EXPRESSION "10\n30\n60"
)

add_test(NAME miko_array_assign_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/array_assign.miko --run
)
set_tests_properties(miko_array_assign_run PROPERTIES
    PASS_REGULAR_EXPRESSION "10\n99\n40"
)

add_test(NAME miko_strings_runtime_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/strings_runtime.miko --run
)
set_tests_properties(miko_strings_runtime_run PROPERTIES
    PASS_REGULAR_EXPRESSION "hello world\n11\ntrue\ntrue\n4"
)

add_test(NAME miko_string_functions_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/string_functions.miko --run
)
set_tests_properties(miko_string_functions_run PROPERTIES
    PASS_REGULAR_EXPRESSION "hello miko"
)

add_test(NAME miko_args_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/args.miko --run -- left right
)
set_tests_properties(miko_args_run PROPERTIES
    PASS_REGULAR_EXPRESSION "2\nleft:right"
)

add_test(NAME miko_modules_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/modules.miko --run
)
set_tests_properties(miko_modules_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PASS_REGULAR_EXPRESSION "10\n3\n7\n9\n4\nhello miko"
)

add_test(NAME miko_package_import_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/package_import.miko --run
)
set_tests_properties(miko_package_import_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PASS_REGULAR_EXPRESSION "15"
)

add_test(NAME miko_module_struct_types_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/module_struct_types.miko --run
)
set_tests_properties(miko_module_struct_types_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PASS_REGULAR_EXPRESSION "30"
)

add_test(NAME miko_structs_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/structs.miko --run
)
set_tests_properties(miko_structs_run PROPERTIES
    PASS_REGULAR_EXPRESSION "10\n20\n30"
)

add_test(NAME miko_enums_types
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/enums.miko --debug-types
)

add_test(NAME miko_module_enums_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/module_enums.miko --run
)
set_tests_properties(miko_module_enums_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PASS_REGULAR_EXPRESSION "true\nbanana\napple"
)

add_test(NAME miko_enums_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/enums.miko --run
)
set_tests_properties(miko_enums_run PROPERTIES
    PASS_REGULAR_EXPRESSION "true\nfalse\ngreen\nblue\ntrue\nblue"
)

add_test(NAME miko_vec_basics_types
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/vec_basics.miko --debug-types
)

add_test(NAME miko_vec_basics_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/vec_basics.miko --run
)
set_tests_properties(miko_vec_basics_run PROPERTIES
    PASS_REGULAR_EXPRESSION "0\n3\n60\n99\n139\n2\n99\n0\n1\nroute\n5"
)

add_test(NAME miko_strings_builtins_types
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/strings_builtins.miko --debug-types
)

add_test(NAME miko_strings_builtins_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/strings_builtins.miko --run
)
set_tests_properties(miko_strings_builtins_run PROPERTIES
    PASS_REGULAR_EXPRESSION "1234\n-42\n2.5\ntrue\nfalse\nHi\n8193\n7\n4\n-1\ntrue\nfalse\n4"
)

add_test(NAME miko_std_string_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/std_string.miko --run
)
set_tests_properties(miko_std_string_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PASS_REGULAR_EXPRESSION "hello miko\nhello\nhello\nMIKO\nmiko\n3\nb\na-b-c\nmiKO miKO\ntrue\ntrue\ntrue\n4\nababab\nokim\n007\n7..\ntrue\nfalse\nx!y"
)

add_test(NAME miko_std_math_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/std_math.miko --run
)
set_tests_properties(miko_std_math_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PASS_REGULAR_EXPRESSION "10\n7\n9\n10\n6\n12\n1024\ntrue\n2.5\n2.5\n1.5\n4\n2\n3\n3\n7"
)

add_test(NAME miko_std_collections_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/std_collections.miko --run
)
set_tests_properties(miko_std_collections_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PASS_REGULAR_EXPRESSION "0\n2\n100\ntrue\nfalse\n95\ntrue\nfalse\n1\nmissing\n4\n-1\n2\n2\nhits"
)

add_test(NAME miko_sys_io_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/sys_io.miko --run
)
set_tests_properties(miko_sys_io_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PASS_REGULAR_EXPRESSION "file-ok\ntrue\nfalse\nthis goes to stderr\ndone"
)

add_test(NAME miko_clock_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/clock.miko --run
)
set_tests_properties(miko_clock_run PROPERTIES
    PASS_REGULAR_EXPRESSION "true\n6765\ntrue\ncompile-like workload took [0-9]+ ms"
)

add_test(NAME miko_enum_unknown_member_error
    COMMAND ${CMAKE_COMMAND}
        -DCOMMAND_PATH=$<TARGET_FILE:miko>
        "-DCOMMAND_ARGS=${CMAKE_CURRENT_SOURCE_DIR}/examples/enum_unknown_member_error.miko;--debug-types"
        "-DEXPECTED_ERROR=enum 'Color' has no member 'Purple'"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectFailure.cmake
)

add_test(NAME miko_vec_type_error
    COMMAND ${CMAKE_COMMAND}
        -DCOMMAND_PATH=$<TARGET_FILE:miko>
        "-DCOMMAND_ARGS=${CMAKE_CURRENT_SOURCE_DIR}/examples/vec_type_error.miko;--debug-types"
        "-DEXPECTED_ERROR=cannot push 'string' into vec of type 'vec<i32>'"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectFailure.cmake
)

add_test(NAME miko_vec_index_error
    COMMAND ${CMAKE_COMMAND}
        -DCOMMAND_PATH=$<TARGET_FILE:miko>
        "-DCOMMAND_ARGS=${CMAKE_CURRENT_SOURCE_DIR}/examples/vec_index_error.miko;--run"
        "-DEXPECTED_ERROR=vec index out of bounds"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectFailure.cmake
)

add_test(NAME miko_bootstrap_lexer_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/compiler/main.miko --run
)
set_tests_properties(miko_bootstrap_lexer_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PASS_REGULAR_EXPRESSION "KwFn\\|2\\|fn"
)

add_test(NAME miko_bootstrap_parser_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/compiler/main.miko --run
)
set_tests_properties(miko_bootstrap_parser_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PASS_REGULAR_EXPRESSION "Program\n  Function main"
)

add_test(NAME miko_bootstrap_sema_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/compiler/main.miko --run
)
set_tests_properties(miko_bootstrap_sema_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PASS_REGULAR_EXPRESSION "Resolved binding a: i64"
)

add_test(NAME miko_bootstrap_codegen_c_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/compiler/main.miko --run
)
set_tests_properties(miko_bootstrap_codegen_c_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PASS_REGULAR_EXPRESSION "#include <stdio.h>"
)

add_test(NAME miko_bootstrap_functions_codegen_c_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/compiler/main.miko --run
)
set_tests_properties(miko_bootstrap_functions_codegen_c_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PASS_REGULAR_EXPRESSION "long long add.*long long a, long long b"
)


add_test(NAME miko_bootstrap_functions_c_e2e
    COMMAND ${CMAKE_COMMAND}
        -DMIKO_EXE=$<TARGET_FILE:miko>
        -DC_COMPILER=${MIKO_TEST_C_COMPILER}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBOOTSTRAP_OUT_DIR=${MIKO_BOOTSTRAP_OUT_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BootstrapC.cmake
)

add_test(NAME miko_bootstrap_control_codegen_c_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/compiler/main.miko --run
)
set_tests_properties(miko_bootstrap_control_codegen_c_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PASS_REGULAR_EXPRESSION "while .*i < 5"
)

add_test(NAME miko_bootstrap_control_c_e2e
    COMMAND ${CMAKE_COMMAND}
        -DMIKO_EXE=$<TARGET_FILE:miko>
        -DC_COMPILER=${MIKO_TEST_C_COMPILER}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBOOTSTRAP_OUT_DIR=${MIKO_BOOTSTRAP_OUT_DIR}
        -DBOOTSTRAP_NAME=control
        -DEXPECTED_OUTPUT=three
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BootstrapC.cmake
)

add_test(NAME miko_bootstrap_structs_codegen_c_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/compiler/main.miko --run
)
set_tests_properties(miko_bootstrap_structs_codegen_c_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PASS_REGULAR_EXPRESSION "typedef struct"
)

add_test(NAME miko_bootstrap_structs_c_e2e
    COMMAND ${CMAKE_COMMAND}
        -DMIKO_EXE=$<TARGET_FILE:miko>
        -DC_COMPILER=${MIKO_TEST_C_COMPILER}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBOOTSTRAP_OUT_DIR=${MIKO_BOOTSTRAP_OUT_DIR}
        -DBOOTSTRAP_NAME=structs
        -DEXPECTED_OUTPUT=30
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BootstrapC.cmake
)

add_test(NAME miko_bootstrap_arrays_codegen_c_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/compiler/main.miko --run
)
set_tests_properties(miko_bootstrap_arrays_codegen_c_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PASS_REGULAR_EXPRESSION "long long nums\\[\\]"
)

add_test(NAME miko_bootstrap_arrays_c_e2e
    COMMAND ${CMAKE_COMMAND}
        -DMIKO_EXE=$<TARGET_FILE:miko>
        -DC_COMPILER=${MIKO_TEST_C_COMPILER}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBOOTSTRAP_OUT_DIR=${MIKO_BOOTSTRAP_OUT_DIR}
        -DBOOTSTRAP_NAME=arrays
        -DEXPECTED_OUTPUT=60
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BootstrapC.cmake
)

add_test(NAME miko_bootstrap_array_assign_c_e2e
    COMMAND ${CMAKE_COMMAND}
        -DMIKO_EXE=$<TARGET_FILE:miko>
        -DC_COMPILER=${MIKO_TEST_C_COMPILER}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBOOTSTRAP_OUT_DIR=${MIKO_BOOTSTRAP_OUT_DIR}
        -DBOOTSTRAP_NAME=array_assign
        -DEXPECTED_OUTPUT=99
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BootstrapC.cmake
)

add_test(NAME miko_bootstrap_strings_codegen_c_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/compiler/main.miko --run
)
set_tests_properties(miko_bootstrap_strings_codegen_c_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PASS_REGULAR_EXPRESSION "miko_str_concat"
)

add_test(NAME miko_bootstrap_strings_c_e2e
    COMMAND ${CMAKE_COMMAND}
        -DMIKO_EXE=$<TARGET_FILE:miko>
        -DC_COMPILER=${MIKO_TEST_C_COMPILER}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBOOTSTRAP_OUT_DIR=${MIKO_BOOTSTRAP_OUT_DIR}
        -DBOOTSTRAP_NAME=strings_runtime
        -DEXPECTED_OUTPUT=hello.world
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BootstrapC.cmake
)

add_test(NAME miko_bootstrap_string_functions_c_e2e
    COMMAND ${CMAKE_COMMAND}
        -DMIKO_EXE=$<TARGET_FILE:miko>
        -DC_COMPILER=${MIKO_TEST_C_COMPILER}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBOOTSTRAP_OUT_DIR=${MIKO_BOOTSTRAP_OUT_DIR}
        -DBOOTSTRAP_NAME=string_functions
        -DEXPECTED_OUTPUT=hello.miko
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BootstrapC.cmake
)

add_test(NAME miko_bootstrap_args_c_e2e
    COMMAND ${CMAKE_COMMAND}
        -DMIKO_EXE=$<TARGET_FILE:miko>
        -DC_COMPILER=${MIKO_TEST_C_COMPILER}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBOOTSTRAP_OUT_DIR=${MIKO_BOOTSTRAP_OUT_DIR}
        -DBOOTSTRAP_NAME=args
        "-DPROGRAM_ARGS=left;right"
        -DEXPECTED_OUTPUT=left:right
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BootstrapC.cmake
)

# enum compiled through the self-hosted miko->C pipeline (stage1).
add_test(NAME miko_bootstrap_enum_stage1_c_e2e
    COMMAND ${CMAKE_COMMAND}
        -DMIKO_EXE=$<TARGET_FILE:miko>
        -DC_COMPILER=${MIKO_TEST_C_COMPILER}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBOOTSTRAP_OUT_DIR=${MIKO_BOOTSTRAP_OUT_DIR}
        -DBOOTSTRAP_NAME=enum_stage1
        -DEXPECTED_OUTPUT=true.green.blue.true.a :: b
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BootstrapC.cmake
)

add_test(NAME miko_bootstrap_compiler_main_codegen_c_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/compiler/main.miko --run -- compiler/main.miko ${MIKO_BOOTSTRAP_OUT_DIR}/bootstrap_compiler_main
)
set_tests_properties(miko_bootstrap_compiler_main_codegen_c_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PASS_REGULAR_EXPRESSION "miko_read_file"
)

add_test(NAME miko_bootstrap_compiler_main_args_codegen_c_run
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/compiler/main.miko --run -- compiler/main.miko ${MIKO_BOOTSTRAP_OUT_DIR}/bootstrap_compiler_main
)
set_tests_properties(miko_bootstrap_compiler_main_args_codegen_c_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PASS_REGULAR_EXPRESSION "\\(argc - 1\\) >= 2"
)

add_test(NAME miko_bootstrap_compiler_main_c_syntax
    COMMAND ${CMAKE_COMMAND}
        -DMIKO_EXE=$<TARGET_FILE:miko>
        -DC_COMPILER=${MIKO_TEST_C_COMPILER}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBOOTSTRAP_OUT_DIR=${MIKO_BOOTSTRAP_OUT_DIR}
        -DBOOTSTRAP_SOURCE=compiler/main.miko
        -DBOOTSTRAP_PREFIX=stage1_main
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BootstrapSyntax.cmake
)

add_test(NAME miko_bootstrap_lexer_c_syntax
    COMMAND ${CMAKE_COMMAND}
        -DMIKO_EXE=$<TARGET_FILE:miko>
        -DC_COMPILER=${MIKO_TEST_C_COMPILER}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBOOTSTRAP_OUT_DIR=${MIKO_BOOTSTRAP_OUT_DIR}
        -DBOOTSTRAP_SOURCE=compiler/lexer.miko
        -DBOOTSTRAP_PREFIX=stage1_lexer
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BootstrapSyntax.cmake
)

add_test(NAME miko_bootstrap_parser_c_syntax
    COMMAND ${CMAKE_COMMAND}
        -DMIKO_EXE=$<TARGET_FILE:miko>
        -DC_COMPILER=${MIKO_TEST_C_COMPILER}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBOOTSTRAP_OUT_DIR=${MIKO_BOOTSTRAP_OUT_DIR}
        -DBOOTSTRAP_SOURCE=compiler/parser.miko
        -DBOOTSTRAP_PREFIX=stage1_parser
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BootstrapSyntax.cmake
)

add_test(NAME miko_bootstrap_sema_c_syntax
    COMMAND ${CMAKE_COMMAND}
        -DMIKO_EXE=$<TARGET_FILE:miko>
        -DC_COMPILER=${MIKO_TEST_C_COMPILER}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBOOTSTRAP_OUT_DIR=${MIKO_BOOTSTRAP_OUT_DIR}
        -DBOOTSTRAP_SOURCE=compiler/sema.miko
        -DBOOTSTRAP_PREFIX=stage1_sema
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BootstrapSyntax.cmake
)

add_test(NAME miko_bootstrap_type_c_syntax
    COMMAND ${CMAKE_COMMAND}
        -DMIKO_EXE=$<TARGET_FILE:miko>
        -DC_COMPILER=${MIKO_TEST_C_COMPILER}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBOOTSTRAP_OUT_DIR=${MIKO_BOOTSTRAP_OUT_DIR}
        -DBOOTSTRAP_SOURCE=compiler/type.miko
        -DBOOTSTRAP_PREFIX=stage1_type
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BootstrapSyntax.cmake
)

add_test(NAME miko_bootstrap_codegen_c_c_syntax
    COMMAND ${CMAKE_COMMAND}
        -DMIKO_EXE=$<TARGET_FILE:miko>
        -DC_COMPILER=${MIKO_TEST_C_COMPILER}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBOOTSTRAP_OUT_DIR=${MIKO_BOOTSTRAP_OUT_DIR}
        -DBOOTSTRAP_SOURCE=compiler/codegen_c.miko
        -DBOOTSTRAP_PREFIX=stage1_codegen_c
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BootstrapSyntax.cmake
)

add_test(NAME miko_bootstrap_stage0_stage1_stage2_smoke
    COMMAND ${CMAKE_COMMAND}
        -DMIKO_EXE=$<TARGET_FILE:miko>
        -DC_COMPILER=${MIKO_TEST_C_COMPILER}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBOOTSTRAP_OUT_DIR=${MIKO_BOOTSTRAP_OUT_DIR}/stage-chain
        "-DSTAGE1_SMOKE_SOURCE:STRING=examples/functions.miko"
        "-DSTAGE1_SMOKE_EXPECTED:STRING=30"
        "-DSTAGE1_SELF_SOURCE:STRING=compiler/main.miko"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BootstrapStage1.cmake
)
# This smoke test now also enforces the strongest bootstrap criterion: the
# generated C of stage1 and stage2 must be byte-identical across every
# compiler module (a true byte-fixed-point / self-hosting invariant).

set_tests_properties(
    miko_bootstrap_lexer_c_syntax
    miko_bootstrap_parser_c_syntax
    miko_bootstrap_sema_c_syntax
    miko_bootstrap_type_c_syntax
    PROPERTIES TIMEOUT 90
)

set_tests_properties(miko_bootstrap_codegen_c_c_syntax PROPERTIES TIMEOUT 240)
set_tests_properties(miko_bootstrap_stage0_stage1_stage2_smoke PROPERTIES TIMEOUT 420)

# LLVM self-hosting chain. The compiler is written in miko twice: codegen_c
# emits C, codegen_ll emits textual LLVM IR. The LLVM chain is verified
# separately because it compiles the whole program as a single IR module (so
# cross-module calls resolve by signature) rather than one C file per module.
#
# The assertion that makes this self-hosting rather than "it runs": the IR
# emitted by the LLVM-built compiler (stage2) is byte-identical to the IR it
# emits one generation later (stage3).
if(MIKO_ENABLE_LLVM)
    add_test(NAME miko_bootstrap_llvm_self_hosting
        COMMAND ${CMAKE_COMMAND}
            -DMIKO_EXE=$<TARGET_FILE:miko>
            -DCLANG_EXE=${MIKO_TEST_C_COMPILER}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DBOOTSTRAP_OUT_DIR=${MIKO_BOOTSTRAP_OUT_DIR}/llvm-chain
            "-DLLVM_SAMPLE_SOURCE:STRING=examples/structs.miko"
            "-DLLVM_SAMPLE_EXPECTED:STRING=10"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BootstrapLLVM.cmake
    )
    set_tests_properties(miko_bootstrap_llvm_self_hosting PROPERTIES TIMEOUT 900)

    # A miko program written the way a user would write one - not a special
    # build hook - compiles the whole compiler, and the result is used to
    # compile another program and checked against the interpreter.
    add_test(NAME miko_llvm_driver_builds_compiler
        COMMAND ${CMAKE_COMMAND}
            -DMIKO_EXE=$<TARGET_FILE:miko>
            -DCLANG_EXE=${MIKO_TEST_C_COMPILER}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DBOOTSTRAP_OUT_DIR=${MIKO_BOOTSTRAP_OUT_DIR}/driver-build
            "-DDRIVER_PROBE_SOURCE:STRING=examples/arrays_ir.miko"
            "-DDRIVER_PROBE_EXPECTED:STRING=60"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/DriverBuildsCompiler.cmake
    )
    set_tests_properties(miko_llvm_driver_builds_compiler PROPERTIES TIMEOUT 900)

    # Builds a miko.exe that is itself written in miko, and proves it is a fixed
    # point: the IR it emits recompiles to byte-identical IR. Only the very first
    # .ll comes from the C++ stage0; everything after that uses the binary this
    # test produced, so the C++ compiler is not needed to compile miko.
    add_test(NAME miko_llvm_selfhosted_miko_exe
        COMMAND ${CMAKE_COMMAND}
            -DMIKO_EXE=$<TARGET_FILE:miko>
            -DCLANG_EXE=${MIKO_TEST_C_COMPILER}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DOUTPUT_DIR=${MIKO_BOOTSTRAP_OUT_DIR}/miko-exe
            "-DSMOKE_SOURCE:STRING=examples/arrays_ir.miko"
            "-DSMOKE_EXPECTED:STRING=60"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildMikoExe.cmake
    )
    set_tests_properties(miko_llvm_selfhosted_miko_exe PROPERTIES TIMEOUT 900)

    # Differential test suite: every case in tests/diff/ is run by the
    # interpreter, the C++ backend and the self-hosted compiler, and each one
    # must match the case's hand-written .expected file. Unlike the golden-file
    # tests above this catches two implementations that disagree with each
    # other, which is how the `for`-loop, short-circuit, i64-literal and escape
    # defects were found.
    add_test(NAME miko_difftest
        COMMAND pwsh -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/tools/difftest.ps1
            -RepoRoot ${CMAKE_CURRENT_SOURCE_DIR}
            -CppCompiler $<TARGET_FILE:miko>
            -SelfCompiler ${CMAKE_CURRENT_SOURCE_DIR}/dist/miko.exe
            -WorkDir ${MIKO_BOOTSTRAP_OUT_DIR}/difftest
            -TimeoutMs 60000
    )
    set_tests_properties(miko_difftest PROPERTIES TIMEOUT 1800)

    # Behavioural bootstrap check. `miko_llvm_selfhosted_miko_exe` above only
    # compares IR text between two generations, which stays green even when the
    # compiler that IR produces crashes on every input. This one compiles the
    # compiler with itself and then requires each generation to compile the
    # whole tests/diff suite correctly.
    add_test(NAME miko_bootstrap_behaviour
        COMMAND pwsh -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/tools/bootstrap.ps1
            -RepoRoot ${CMAKE_CURRENT_SOURCE_DIR}
            -StartCompiler ${CMAKE_CURRENT_SOURCE_DIR}/dist/miko.exe
            -Generations 2
            -WorkDir ${MIKO_BOOTSTRAP_OUT_DIR}/bootstrap-behaviour
    )
    set_tests_properties(miko_bootstrap_behaviour PROPERTIES TIMEOUT 3600)
endif()
