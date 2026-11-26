if(NOT DEFINED MIKO_EXE)
    message(FATAL_ERROR "MIKO_EXE is required")
endif()
if(NOT DEFINED C_COMPILER)
    message(FATAL_ERROR "C_COMPILER is required")
endif()
if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

# On Windows, the C compiler invoked via execute_process does not inherit the
# MSVC/SDK env vars (INCLUDE/LIB) that a VS dev prompt would set. Detect the
# toolchain and inject include + lib flags so `clang` can find <stdio.h>.
if(WIN32 AND NOT DEFINED MIKO_C_CC_FLAGS)
    set(MIKO_C_CC_FLAGS "")
    set(MIKO_C_LINK_FLAGS "")
    file(GLOB _msvc_dirs "C:/Program Files/Microsoft Visual Studio/*/Community/VC/Tools/MSVC/*")
    list(SORT _msvc_dirs)
    set(_msvc_toolchain "")
    foreach(_v IN LISTS _msvc_dirs)
        if(EXISTS "${_v}/include")
            set(_msvc_toolchain "${_v}")
            break()
        endif()
    endforeach()
    file(GLOB _sdk_dirs "C:/Program Files (x86)/Windows Kits/10/Include/10.0.*")
    list(SORT _sdk_dirs)
    set(_sdk_include "")
    foreach(_s IN LISTS _sdk_dirs)
        if(EXISTS "${_s}/ucrt")
            set(_sdk_include "${_s}")
            break()
        endif()
    endforeach()
    if(_msvc_toolchain AND _sdk_include)
        get_filename_component(_sdk_ver "${_sdk_include}" NAME)
        set(_sdk_lib "C:/Program Files (x86)/Windows Kits/10/Lib/${_sdk_ver}")
        list(APPEND MIKO_C_CC_FLAGS
            "-I${_msvc_toolchain}/include"
            "-I${_sdk_include}/ucrt"
            "-I${_sdk_include}/um"
            "-I${_sdk_include}/shared")
        list(APPEND MIKO_C_LINK_FLAGS
            "-L${_msvc_toolchain}/lib/x64"
            "-L${_sdk_lib}/ucrt/x64"
            "-L${_sdk_lib}/um/x64"
            "-lucrt" "-lmsvcrt" "-lkernel32" "-lntdll" "-luser32")

        # clang's default Windows target is the MSVC ABI, whose linker wants
        # /LIBPATH: rather than the GNU -L/-l pair above. Any step that links
        # with clang (the LLVM bootstrap chain) uses this list instead.
        set(MIKO_CLANG_LINK_FLAGS
            "-Xlinker" "/LIBPATH:${_msvc_toolchain}/lib/x64"
            "-Xlinker" "/LIBPATH:${_sdk_lib}/ucrt/x64"
            "-Xlinker" "/LIBPATH:${_sdk_lib}/um/x64")
    endif()
endif()

# The LLVM backend needs no include paths for the IR itself (it declares the
# runtime functions it calls), but the runtime implementation is C, so those
# paths travel with MIKO_C_CC_FLAGS. Keep a dedicated name so callers do not
# have to know that detail.
if(NOT DEFINED MIKO_CLANG_CC_FLAGS)
    set(MIKO_CLANG_CC_FLAGS "${MIKO_C_CC_FLAGS}")
endif()
if(NOT DEFINED MIKO_CLANG_LINK_FLAGS)
    set(MIKO_CLANG_LINK_FLAGS "")
endif()
