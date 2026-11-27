@echo off
rem miko link step: clang over the generated .ll plus the C runtime.
rem
rem Optimization level defaults to -O2; set MIKO_OPT to override it (e.g. -O0 when
rem debugging codegen). With no level clang stays at -O0, which measured ~3.9x
rem slower than -O2 on a 50M-iteration integer loop.
rem
rem Usage: miko-link.bat <input.ll> <output.exe>
rem
rem This deliberately does not involve Visual Studio. An earlier version found
rem vswhere, called vcvars64.bat and let clang drive the MSVC toolchain, which tied
rem the released compiler to an install nobody else necessarily has and made the
rem Windows build impossible to reproduce away from Visual Studio. clang carries its
rem own linker and, with llvm-mingw, its own C runtime and headers, so the whole
rem Windows toolchain can be one directory that does not care what else is installed.
rem
rem What is needed: any clang that can link a Windows executable. LLVM's own
rem installer works when the Windows SDK is present; llvm-mingw works on its own.
rem
rem Nothing here sets an architecture. The compiler emits IR with no target triple,
rem and this script links it for the machine it runs on, so the same script and the
rem same IR serve x64 and arm64.
setlocal EnableDelayedExpansion

if "%MIKO_OPT%"=="" set "MIKO_OPT=-O2"

rem MIKO_CC set by the user wins, then a clang on PATH, then the usual places
rem either toolchain installs itself. MIKO_CC being explicit matters: a user with
rem two clangs needs a way to say which one without editing PATH.
if defined MIKO_CC if exist "%MIKO_CC%" goto :have_cc

where clang.exe >nul 2>nul
if not errorlevel 1 (
    for /f "usebackq delims=" %%i in (`where clang.exe`) do (
        if not defined MIKO_CC set "MIKO_CC=%%i"
    )
)
if defined MIKO_CC goto :have_cc

for %%d in (
    "%ProgramFiles%\LLVM\bin\clang.exe"
    "%ProgramFiles%\llvm-mingw\bin\clang.exe"
    "C:\llvm-mingw\bin\clang.exe"
    "%USERPROFILE%\llvm-mingw\bin\clang.exe"
) do (
    if not defined MIKO_CC if exist %%d set "MIKO_CC=%%~d"
)
if not defined MIKO_CC (
    echo miko: no clang found. >&2
    echo miko: install llvm-mingw or LLVM, put clang on PATH, or set MIKO_CC to it. >&2
    exit /b 1
)

:have_cc

rem The runtime source sits beside this script in a release package and at the
rem repository root in a checkout. Resolve it relative to the script first, then
rem the working directory, so both layouts link.
set "MIKO_RUNTIME="
if exist "%~dp0runtime\miko_runtime.c" set "MIKO_RUNTIME=%~dp0runtime\miko_runtime.c"
if not defined MIKO_RUNTIME if exist "runtime\miko_runtime.c" set "MIKO_RUNTIME=runtime\miko_runtime.c"
if not defined MIKO_RUNTIME (
    echo miko: cannot find runtime\miko_runtime.c beside the link script or in the working directory >&2
    exit /b 1
)

rem -g is what asks clang to keep debug information. The IR carries DWARF metadata
rem on its own, and some clang builds turn that into .debug_* sections regardless,
rem but Apple's clang does not, which is why the macOS release jobs produced
rem executables with no debug info while Linux and Windows did not. Passing it
rem everywhere makes the three platforms agree, at about 10% binary size (261632 to
rem 289280 bytes on the sample program).
"%MIKO_CC%" %MIKO_OPT% -g -w "%~1" "%MIKO_RUNTIME%" -o "%~2"
exit /b %ERRORLEVEL%
