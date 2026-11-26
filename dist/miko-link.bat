@echo off
rem miko link step: clang over the generated .ll plus the C runtime.
rem
rem Optimization level defaults to -O2; set MIKO_OPT to override it (e.g. -O0 when
rem debugging codegen). With no level clang stays at -O0, which measured ~3.9x
rem slower than -O2 on a 50M-iteration integer loop.
rem
rem Usage: miko-link.bat <input.ll> <output.exe>
rem
rem Everything platform specific is supposed to live in this file, and for a while
rem it did - too specifically. It named one machine's MSVC toolset version and one
rem Windows SDK version as -I and /LIBPATH flags, so the script worked on the
rem machine that wrote it and on no other. A released compiler built on top of that
rem could not link anything for anybody.
rem
rem What replaces it: clang already understands the INCLUDE and LIB variables that
rem a Visual Studio environment sets, so if they are present this just calls clang.
rem If they are not - the normal case for someone running a released binary from a
rem plain shell - it finds Visual Studio with vswhere and borrows its environment,
rem which is the same thing "x64 Native Tools Command Prompt" does.
setlocal EnableDelayedExpansion

if "%MIKO_OPT%"=="" set "MIKO_OPT=-O2"

rem Prefer a clang on PATH, since that is the one the user chose; fall back to a
rem default install location so a bare double-click still works.
set "MIKO_CC=clang"
where clang >nul 2>nul
if errorlevel 1 (
    if exist "%ProgramFiles%\LLVM\bin\clang.exe" (
        set "MIKO_CC=%ProgramFiles%\LLVM\bin\clang.exe"
    ) else (
        echo miko: clang not found on PATH and not at "%ProgramFiles%\LLVM\bin\clang.exe" >&2
        echo miko: install LLVM, or set MIKO_CC to the clang you want. >&2
        exit /b 1
    )
)

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

rem With INCLUDE and LIB already set, clang drives the MSVC toolchain directly.
if defined INCLUDE if defined LIB goto :link

rem Otherwise load a Visual Studio environment. vswhere ships with every VS 2017+
rem install at this fixed path, which is why it can be trusted without searching.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo miko: cannot find vswhere.exe - is Visual Studio installed? >&2
    echo miko: alternatively run this from an x64 Native Tools Command Prompt. >&2
    exit /b 1
)

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo miko: Visual Studio is installed but has no C++ toolset >&2
    echo miko: install the "Desktop development with C++" workload, or run this from an x64 Native Tools Command Prompt. >&2
    exit /b 1
)

set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo miko: cannot find "%VCVARS%" >&2
    exit /b 1
)

rem vcvars prints a banner and occasionally warnings; none of it belongs in the
rem compiler's output, so it is swallowed. Its environment changes do survive,
rem because they are applied by calling it in this same cmd instance.
call "%VCVARS%" >nul 2>nul

:link
"%MIKO_CC%" %MIKO_OPT% -w "%~1" "%MIKO_RUNTIME%" -o "%~2"
exit /b %ERRORLEVEL%
