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

    # Every call inside a `define` that carries !dbg must carry one too, or LLVM
    # discards the compile unit instead of the call. The symptom is not local: the
    # executable keeps .debug_* from the C runtime, gets an empty line table for the
    # language being compiled, and names no subprogram of it. That is what the
    # Windows package shipped, and every check here passed because none of them
    # looked at this and no Windows job ran them anyway.
    #
    # The assertion is on the IR rather than on the artifact because it does not need
    # a linker, so it holds equally on a machine that cannot link for the target.
    $inDbgFn = $false
    $lineNo = 0
    foreach ($line in ($ir -split "`r?`n")) {
        $lineNo++
        $t = $line.Trim()
        if ($t -match '^define\b') {
            $inDbgFn = $t -match '!dbg !\d+'
            continue
        }
        if ($inDbgFn -and $t -eq '}') { $inDbgFn = $false; continue }
        if (-not $inDbgFn) { continue }
        if ($t -match '(^|\s)call\s' -and $t -notmatch '!dbg !\d+') {
            $failures.Add("IR line ${lineNo}: call without !dbg inside a function that has one - $t")
            if ($failures.Count -ge 4) { break }
        }
    }
}
# Where debug info has to survive depends on what the platform's linker does with
# it, and that was measured rather than assumed:
#
#   ELF (Linux)                 .debug_* sections survive into the executable
#   COFF linked by llvm-mingw   survive
#   COFF linked by the MSVC one do not, and neither does CodeView
#   Mach-O (macOS)              __debug_* reaches the object, ld64 drops it from the
#                               linked image - the object carries "linecheck.miko",
#                               the executable does not, with or without -g
#
# So the assertion follows the artifact that can hold it: the IR always, the object
# on Mach-O, the executable everywhere else. Checking something the platform throws
# away would fail for a reason that says nothing about the compiler.

function Test-DebugNames {
    param([string]$Path)
    $info = & $DwarfDump --debug-info $Path 2>&1 | Out-String
    $line = & $DwarfDump --debug-line $Path 2>&1 | Out-String
    return @{ Named = (($info -match "linecheck\.miko") -or ($line -match "linecheck\.miko")); Info = $info; Line = $line }
}

if ($IROnly) {
    $script:dwarfSkipped = "artifact assertions skipped on request (-IROnly): the compiler's output is the IR"
} elseif ($DwarfDump -eq "" -or -not (Test-Path $DwarfDump)) {
    $script:dwarfSkipped = "artifact assertions skipped: no llvm-dwarfdump available to read the debug info"
} else {
    $target = $exe
    $what = "the executable"
    if ($IsMacOS) {
        # Compile the same IR to an object with the same driver the link step uses.
        $obj = "$prefix.o"
        $cc = $env:MIKO_CC
        if ([string]::IsNullOrEmpty($cc)) { $cc = "clang" }
        & $cc -c -O2 -g -w -Wno-override-module "$ll" -o $obj 2>&1 | Out-Null
        if (Test-Path $obj) { $target = $obj; $what = "the object file (ld64 drops DWARF from the linked image)" }
    }

    if (-not (Test-Path $target)) {
        $failures.Add("nothing to read debug info from: $target is missing")
    } else {
        $r = Test-DebugNames -Path $target
        if (-not $r.Named) {
            # Say what the debug info does contain. "does not name linecheck.miko" is
            # true of a binary with no debug info at all and of one whose compile unit
            # is present under another name, and those need different fixes.
            $cunames = @()
            foreach ($m in [regex]::Matches($r.Info, '(?m)^\s*DW_AT_name\s+\("([^"]*)"\)')) {
                if ($cunames -notcontains $m.Groups[1].Value) { $cunames += $m.Groups[1].Value }
            }
            if ($cunames.Count -gt 0) {
                Write-Host ("dwarf-check: names in the debug info: " + (($cunames | Select-Object -First 6) -join ", "))
            } else {
                Write-Host "dwarf-check: the debug info holds no names at all"
            }
            Write-Host ("dwarf-check: dwarfdump reported " + $r.Info.Length + " bytes of --debug-info for " + (Split-Path $target -Leaf))
            $failures.Add("the debug info in $what does not name linecheck.miko")
        } else {
            # The file name is the decisive part: an artifact carrying only the
            # runtime's debug info has "linecheck.miko" nowhere in it.
            #
            # The line count is softer on purpose. -O2 folds assignments away and
            # their locations with them, and how much it folds differs by target, so
            # asking for a particular line or for many of them would fail for reasons
            # that have nothing to do with the compiler. Two distinct lines means the
            # table maps code, not just a file name.
            $seen = @{}
            foreach ($m in [regex]::Matches($r.Line, "(?m)^\s*0x[0-9a-fA-F]+\s+(\d+)\s")) {
                $n = [int]$m.Groups[1].Value
                if ($n -gt 0) { $seen[$n] = $true }
            }
            if ($seen.Count -lt 2) {
                $failures.Add("line table maps code to $($seen.Count) source line(s); expected at least 2")
            } else {
                $script:dwarfWhere = $what
            }
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
    Write-Host ("dwarf-check: compile unit, subprograms and line mappings present in " + $script:dwarfWhere)
}
exit 0
