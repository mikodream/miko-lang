#!/bin/bash
# Build + optionally test the stage0 miko compiler with the MSVC/SDK env
# that a VS dev prompt would provide (clang needs INCLUDE/LIB on Windows).
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${2:-$ROOT/build-bootstrap}"

MSVC_DIR=$(ls -d "/c/Program Files/Microsoft Visual Studio/"*/Community/VC/Tools/MSVC/*/ 2>/dev/null | head -1)
SDK_INC=$(ls -d "/c/Program Files (x86)/Windows Kits/10/Include/"10.0.* 2>/dev/null | head -1)
SDK_VER=$(basename "$SDK_INC")
SDK_LIB="/c/Program Files (x86)/Windows Kits/10/Lib/$SDK_VER"
export INCLUDE="$(cygpath -w "$MSVC_DIR/include");$(cygpath -w "$SDK_INC/ucrt");$(cygpath -w "$SDK_INC/um");$(cygpath -w "$SDK_INC/shared")"
export LIB="$(cygpath -w "$MSVC_DIR/lib/x64");$(cygpath -w "$SDK_LIB/ucrt/x64");$(cygpath -w "$SDK_LIB/um/x64")"

cd "$BUILD_DIR"
if [ "$1" = "test" ]; then
    cmake --build . "$@" >/dev/null 2>&1 || cmake --build .
    ctest --output-on-failure
else
    cmake --build . "$@"
fi
