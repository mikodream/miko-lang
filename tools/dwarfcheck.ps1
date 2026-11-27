# dwarfcheck.ps1 - checks that compiled programs carry DWARF line information.
#
# The type checker, the GC and DWARF were all added without a test that fails when
# they stop working, and DWARF is the easiest of the three to lose silently: the
# metadata travels in the emitted IR, so a mistake there shows up as correct IR for
# a program that runs fine and simply has no debug info. Nothing in difftest would
# notice, because difftest compares stdout.
#
# What this asserts, on a two-function program:
#   1. the IR carries a DICompileUnit and per-function DISubprogram entries
#   2. the linked executable has .debug_line and .debug_info sections
#   3. the line table names the source file and maps code back to real lines of it
#
# The third check is the one that matters. .debug_* sections alone prove nothing:
# the mingw CRT ships its own, so a binary with no miko metadata still has them.
#
# -IROnly stops after the first group. Use it on a toolchain that cannot carry debug
# info into the executable at all: the Windows gate links through the MSVC
# toolchain, and the executable it produces there has no debug sections of any kind,
# so asserting that DWARF arrived would report a fact about the linker rather than
# about the compiler. The release job links with llvm-mingw and checks everything.

param(
    [string]$RepoRoot = ".",
    [string]$Compiler = "",
    [string]$DwarfDump = "",
    [string]$Objdump = "",
    [switch]$IROnly
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path $RepoRoot).Path
if ($Compiler -eq "") { $Compiler = Join-Path $RepoRoot "dist/miko.exe" }
if ($DwarfDump -eq "") {
    foreach ($candidate in @("llvm-dwarfdump", "llvm-dwarfdump.exe")) {
        $found = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($found) { $DwarfDump = $found.Source; break }
    }
}
# No early exit when dwarfdump is missing: the IR assertions do not need it, and
# returning here would skip them on a machine that happens not to have the tool.
# The exe part checks for it where it is used.

$work = Join-Path $RepoRoot "out/dwarf-check"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Path $work -Force | Out-Null

$src = Join-Path $work "linecheck.miko"
@'
fn add(a: i64, b: i64): i64 {
    let c: i64 = a + b;
    return c;
}

fn main() {
    let x: i64 = 2;
    let y: i64 = 3;
    let z: i64 = add(x, y);
    print(z);
}
'@ | Set-Content -Path $src -Encoding ascii

$failures = [System.Collections.Generic.List[string]]::new()
$prefix = Join-Path $work "linecheck"
# With -IROnly the compile stops at the IR, so no linker and no C runtime are
# involved: the assertions there are about what the compiler emits, and linking
# would only add ways for the run to fail over something that is not being checked.
if ($IROnly) { $emit = & $Compiler --emit-ir $src $prefix 2>&1 } else { $emit = & $Compiler $src $prefix 2>&1 }
if ($LASTEXITCODE -ne 0) {
    Write-Host "dwarf-check: compile FAILED"
    Write-Host ($emit | Out-String).Trim()
    exit 1
}

$ll = "$prefix.ll"
# The output name carries no extension on Unix: the compiler appends .exe only on
# Windows. Assuming .exe made this report "no executable written" on all four
# Linux and macOS release jobs, where the binary was there under the plain name.
$exe = "$prefix"
if (Test-Path "$prefix.exe") { $exe = "$prefix.exe" }
if (-not (Test-Path $ll)) { $failures.Add("no IR written") }
if (-not $IROnly -and -not (Test-Path $exe)) { $failures.Add("no executable written") }

if ((Test-Path $ll)) {
    $ir = Get-Content $ll -Raw
    if ($ir -notmatch "!DICompileUnit") { $failures.Add("IR has no DICompileUnit") }
    if ($ir -notmatch "!DISubprogram") { $failures.Add("IR has no DISubprogram") }
    if ($ir -notmatch "!DILocation") { $failures.Add("IR has no DILocation") }
    if ($ir -match ";@miko") { $failures.Add("IR still holds source markers") }
    # A !dbg pointing at an undefined node makes clang reject the module outright,
    # which is exactly how a mistake here first showed up.
    $declared = @{}
    foreach ($m in [regex]::Matches($ir, "(?m)^!(\d+) = ")) { $declared[$m.Groups[1].Value] = $true }
    foreach ($m in [regex]::Matches($ir, "!dbg !(\d+)")) {
        if (-not $declared.ContainsKey($m.Groups[1].Value)) {
            $failures.Add("!dbg points at undefined metadata !$($m.Groups[1].Value)")
            break
        }
    }
}
if ($IROnly) {
    $script:dwarfSkipped = "exe assertions skipped on request (-IROnly): the compiler's output is the IR"
} elseif ($DwarfDump -eq "" -or -not (Test-Path $DwarfDump)) {
    $script:dwarfSkipped = "exe assertions skipped: no llvm-dwarfdump available to read the debug info"
} elseif ((Test-Path $exe)) {
    # One tool decides this. llvm-dwarfdump reads ELF, COFF and Mach-O alike and
    # names the file in each, so there is no section-naming or objdump-flag to get
    # wrong per platform: earlier versions of this script asked llvm-objdump -h and
    # got "none" on macOS for debug info that was present, twice, for two different
    # reasons (the __debug_ prefix, then the output format).
    $dump = & $DwarfDump --debug-line $exe 2>&1 | Out-String
    $info = & $DwarfDump --debug-info $exe 2>&1 | Out-String
    $named = ($dump -match "linecheck\.miko") -or ($info -match "linecheck\.miko")

    if (-not $named) {
        $failures.Add("the executable's debug info does not name linecheck.miko")
    } else {
        # The file name is the decisive part: a binary carrying only the runtime's
        # debug info has "linecheck.miko" nowhere in it, so this cannot pass for the
        # wrong reason.
        #
        # The line count is softer on purpose. -O2 folds the assignments away and
        # their locations with them, and how much it folds differs by target, so
        # asking for a particular line or for many of them would fail for reasons
        # that have nothing to do with the compiler. Two distinct lines means the
        # table maps code, not just a file name.
        $seen = @{}
        foreach ($m in [regex]::Matches($dump, "(?m)^\s*0x[0-9a-fA-F]+\s+(\d+)\s")) {
            $n = [int]$m.Groups[1].Value
            if ($n -gt 0) { $seen[$n] = $true }
        }
        if ($seen.Count -lt 2) {
            $failures.Add("line table maps code to $($seen.Count) source line(s); expected at least 2")
        }
    }
}
if ($failures.Count -gt 0) {
    Write-Host "dwarf-check: FAILED"
    foreach ($f in $failures) { Write-Host ("  - " + $f) }
    exit 1
}

if ($script:dwarfSkipped) {
    Write-Host "dwarf-check: IR carries compile unit, subprograms and locations"
    Write-Host ("dwarf-check: " + $script:dwarfSkipped)
} else {
    Write-Host "dwarf-check: compile unit, subprograms and line mappings all present in the executable"
}
exit 0
