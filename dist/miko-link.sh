#!/bin/sh
# Link a miko .ll into a native executable.
#
# Usage: miko-link.sh <input.ll> <output>
#
# The counterpart to miko-link.bat. Everything platform specific lives here, so
# the compiler itself only has to name a script: miko.toolchain holds one line
# and the compiler runs it with the IR path and the output path.
#
# Why the flags:
#   -Wno-override-module  the emitted IR declares its own target triple, and
#                         clang warns when that disagrees with the default.
#   -O2                   the compiler hands over IR that is already lowered;
#                         without optimisation every alloca and temporary
#                         survives into the binary (measured 3.5x slower on a
#                         50M-iteration loop).
#   -lm                   sqrt/floor/ceil/round come from the C math library,
#                         which clang does not link by default on glibc systems.
set -e

if [ "$#" -lt 2 ]; then
    echo "usage: miko-link.sh <input.ll> <output>" >&2
    exit 2
fi

ir=$1
out=$2

: "${MIKO_OPT:=-O2}"

# runtime/miko_runtime.c holds the miko_* entry points the IR calls. Resolve it
# next to the repository root first, then beside this script, so the compiler
# works from the repo root and from an installed layout.
runtime="runtime/miko_runtime.c"
if [ ! -f "$runtime" ]; then
    here=$(dirname "$0")
    for candidate in "$here/../runtime/miko_runtime.c" "$here/runtime/miko_runtime.c"; do
        if [ -f "$candidate" ]; then
            runtime=$candidate
            break
        fi
    done
fi

: "${CC:=clang}"

"$CC" "$MIKO_OPT" -w -Wno-override-module "$ir" "$runtime" -o "$out" -lm
