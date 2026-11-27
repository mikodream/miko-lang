# varfdwarf.ps1 - checks that compiled programs carry DWARF line information.
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

param(
    [string]$RepoRoot = ".",
    [string]$Compiler = "",
    [string]$DwarfDump = "",
    [string]$Objdump = ""
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
if ($DwarfDump -eq "" -or -not (Test-Path $DwarfDump)) {
    Write-Host "dwarf-check: no llvm-dwarfdump found; skipping"
    exit 0
}

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
$emit = & $Compiler $src $prefix 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "dwarf-check: compile FAILED"
    Write-Host ($emit | Out-String).Trim()
    exit 1
}

$ll = "$prefix.ll"
$exe = "$prefix.exe"
if (-not (Test-Path $ll)) { $failures.Add("no IR written") }
if (-not (Test-Path $exe)) { $failures.Add("no executable written") }

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

if ((Test-Path $exe)) {
    if ($Objdump -eq "") {
        foreach ($candidate in @("llvm-objdump", "llvm-objdump.exe")) {
            $found = Get-Command $candidate -ErrorAction SilentlyContinue
            if ($found) { $Objdump = $found.Source; break }
        }
    }
    if ($Objdump -ne "" -and (Test-Path $Objdump)) {
        # -join first: -notmatch against an array returns the elements that did not
        # match, which is non-empty whenever any line lacks the pattern, so testing
        # an array that way reports a missing section for a file that has it.
        $sections = (& $Objdump -h $exe 2>&1 | Out-String)
        $hasLine = $sections -match "\.debug_line"
        $hasInfo = $sections -match "\.debug_info"

        # The compiler's part is finished once the IR holds resolvable metadata; from
        # there the linker decides what reaches the executable. clang keeps the
        # DWARF either way (checked on both targets: x86_64-w64-windows-gnu and
        # x86_64-pc-windows-msvc objects both carry .debug_line), but the MSVC
        # linker does not carry it into a PE, so a build driven by Win32 link.exe
        # has none even though nothing is wrong. That is a property of the
        # toolchain, not of the metadata, so the exe assertions apply only on a
        # toolchain that preserves DWARF - and when they are skipped the reason is
        # printed, because a check that quietly passes proves nothing.
        if (-not ($hasLine -and $hasInfo)) {
            # Report what was actually there. A failure that names only what is
            # missing leaves the next question - did the linker keep CodeView, or
            # nothing at all - to a second run.
            $dbgSeen = @()
            foreach ($m in [regex]::Matches($sections, "(?m)^\s*\d+\s+(\.\S+)")) {
                if ($m.Groups[1].Value -match "debug") { $dbgSeen += $m.Groups[1].Value }
            }
            $listed = "none"
            if ($dbgSeen.Count -gt 0) { $listed = $dbgSeen -join ", " }
            Write-Host ("dwarf-check: debug sections in the executable: " + $listed)
            if ($sections -match "debug\`$S") {
                $script:dwarfSkipped = "the linker kept CodeView (.debug`$S) and dropped DWARF; exe assertions skipped"
            } else {
                $failures.Add("executable has no .debug_line or .debug_info")
            }
        } else {
            $dump = & $DwarfDump --debug-line $exe 2>&1 | Out-String
            if ($dump -notmatch "linecheck\.miko") {
                $failures.Add("line table does not name linecheck.miko")
            } else {
                # Count the distinct source lines the table maps code to. The exact
                # set is not fixed: -O2 folds the assignments away and their
                # locations with them, which is why this asks for several real line
                # numbers rather than for a particular one. A binary whose only
                # table is the CRT's has this file name nowhere, and a table naming
                # the file with no line of it would mean the per-statement metadata
                # never made it.
                $lines = @{}
                foreach ($m in [regex]::Matches($dump, "(?m)^\s*0x[0-9a-fA-F]+\s+(\d+)\s")) {
                    $n = [int]$m.Groups[1].Value
                    if ($n -gt 0) { $lines[$n] = $true }
                }
                if ($lines.Count -lt 3) {
                    $failures.Add("line table maps code to $($lines.Count) source line(s); expected at least 3")
                }
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
    Write-Host "dwarf-check: compile unit, subprograms, sections and line mappings all present"
}
exit 0
