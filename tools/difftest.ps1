<#
difftest.ps1 - differential test harness for miko.

Every case in tests/diff/ is executed by every backend that can execute it,
and each backend's stdout is compared against the case's hand-written
.expected file. A backend that crashes, refuses to compile, or prints
something else is a failure.

The point of the harness is not "the output did not change since yesterday".
It is "all backends agree with the spec, today". Golden snapshots cannot
detect two implementations that disagree; this can.

Layout:
  tests/diff/<name>.miko        program
  tests/diff/<name>.expected    exact expected stdout
  tests/diff/<name>.reject      (optional) program must be REJECTED at compile time
  tests/diff/<name>.error       (optional) program must FAIL at run time
                                (a diagnosable non-zero exit, never a silent value)

Backends:
  interp  the C++ tree-walk interpreter        (<cpp> --run)
  cpp     the C++ LLVM backend                 (--emit-ir, then clang -c + link)
  self    the self-hosted compiler             (miko <src> <prefix>)

Exit code is the number of cases with at least one failing backend, so ctest
reports it directly.
#>

[CmdletBinding()]
param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$CppCompiler = "",
    [string]$SelfCompiler = "",
    [string]$Filter = "",
    [string]$WorkDir = "",
    [int]$TimeoutMs = 60000,
    [switch]$KeepArtifacts,
    [switch]$ShowDetail,
    [string]$Trace = ""
)

$ErrorActionPreference = "Stop"
$script:Trace = $Trace

function Resolve-Default($explicit, $candidates, $what) {
    if ($explicit -and (Test-Path $explicit)) { return (Resolve-Path $explicit).Path }
    foreach ($c in $candidates) {
        if (Test-Path $c) { return (Resolve-Path $c).Path }
    }
    throw "difftest: cannot find $what. Pass it explicitly."
}

# Executables carry a suffix on Windows and none elsewhere. The harness used to
# hard-code ".exe" in five places, which meant it could only ever run on Windows -
# a release for Linux or macOS had no way to prove the compiler it shipped worked.
$script:exeSuffix = if ($env:OS -eq "Windows_NT") { ".exe" } else { "" }

$cpp = Resolve-Default $CppCompiler @(
    (Join-Path $RepoRoot "build-vs-llvm/Release/miko.exe"),
    (Join-Path $RepoRoot "build-vs-llvm/Release/miko"),
    (Join-Path $RepoRoot "build/miko.exe"),
    (Join-Path $RepoRoot "build/miko")
) "the C++ compiler"

$self = Resolve-Default $SelfCompiler @(
    (Join-Path $RepoRoot "dist/miko.exe"),
    (Join-Path $RepoRoot "dist/miko")
) "the self-hosted compiler"

if (-not $WorkDir) { $WorkDir = Join-Path $RepoRoot "out\difftest" }
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

$clang = (Get-Command clang -ErrorAction SilentlyContinue).Source
$cc    = (Get-Command gcc   -ErrorAction SilentlyContinue).Source
if (-not $clang) { throw "difftest: clang not found on PATH (needed to compile emitted IR)" }
if (-not $cc)    { throw "difftest: gcc not found on PATH (needed to link emitted IR)" }

# The emitted IR calls the miko_* runtime entry points (printing, strings,
# clocks, diagnostics), so the runtime source has to be linked in. The C++
# backend used to inline all of that, and this harness silently relied on it.
$runtimeSrc = Join-Path $RepoRoot "runtime\miko_runtime.c"
if (-not (Test-Path $runtimeSrc)) { throw "difftest: runtime\miko_runtime.c not found" }

# ---------------------------------------------------------------- utilities

function Normalize([string]$text) {
    if ($null -eq $text -or $text -eq "") { return "" }
    $text = $text -replace "`r`n", "`n"
    $lines = $text -split "`n"
    # $lines[0..($n-2)] with n=1 is $lines[0..-1], which PowerShell reads as
    # "element 0 through the last element" - the same one-element array. The
    # loop then never shrinks and spins forever. Empty output hits this on the
    # first backend that fails, which is exactly when the harness is needed.
    while ($lines.Count -gt 0 -and $lines[-1].Trim() -eq "") {
        if ($lines.Count -eq 1) { return "" }
        $lines = $lines[0..($lines.Count - 2)]
    }
    return ($lines -join "`n")
}

function Invoke-Captured([string]$exe, [string[]]$argv, [string]$stdinFile = "") {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardInput = $true
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $script:RepoRoot
    foreach ($a in $argv) { [void]$psi.ArgumentList.Add($a) }

    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    [void]$p.Start()
    if ($script:Trace) { Add-Content -Path $script:Trace -Value ("    started " + (Split-Path -Leaf $exe) + " " + ($argv -join " ")) }
    if ($stdinFile -and (Test-Path $stdinFile)) {
        $p.StandardInput.Write((Get-Content $stdinFile -Raw))
    }
    $p.StandardInput.Close()

    # Read stdout/stderr on background tasks so a program that never terminates
    # (or floods output) cannot deadlock the pump, then bound the wait. A hang
    # is a failure mode in its own right and has to be reported, not inherited.
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()

    $exited = $p.WaitForExit($script:TimeoutMs)
    if ($script:Trace) { Add-Content -Path $script:Trace -Value ("    exited=" + $exited + " code=" + $p.ExitCode) }
    if (-not $exited) {
        try { $p.Kill($true) } catch { }
        return [pscustomobject]@{ ExitCode = -1; Stdout = ""; Stderr = ""; Crashed = $false; TimedOut = $true }
    }
    $p.WaitForExit()

    $stdoutText = $outTask.GetAwaiter().GetResult()
    $stderrText = $errTask.GetAwaiter().GetResult()
    if ($script:Trace) { Add-Content -Path $script:Trace -Value "    drained" }

    return [pscustomobject]@{
        ExitCode = $p.ExitCode
        Stdout   = $stdoutText
        Stderr   = $stderrText
        Crashed  = ($p.ExitCode -lt 0)
        TimedOut = $false
    }
}

function Fail-Reason($r) {
    if ($r.TimedOut) { return "TIMEOUT" }
    if ($r.Crashed) { return ("CRASH 0x{0:X8}" -f $r.ExitCode) }
    if ($r.ExitCode -ne 0) { return "exit=$($r.ExitCode)" }
    return ""
}

# ---------------------------------------------------------------- backends
# Each returns @{ Outcome = 'ok'|'reject'|'error'; Output = <stdout>; Detail = <text> }

function Run-Interp($src) {
    $r = Invoke-Captured $script:cpp @("--run", $src)
    if ($r.ExitCode -ne 0) { return @{ Outcome = "error"; Output = (Normalize $r.Stdout); Stderr = (Normalize $r.Stderr); Detail = (Fail-Reason $r) } }
    return @{ Outcome = "ok"; Output = (Normalize $r.Stdout); Stderr = (Normalize $r.Stderr); Detail = "" }
}

function Run-Cpp($name, $src) {
    $ll  = Join-Path $script:WorkDir "$name.cpp.ll"
    $obj = Join-Path $script:WorkDir ($name + ".cpp.o")
    $exe = Join-Path $script:WorkDir ($name + ".cpp" + $script:exeSuffix)
    Remove-Item $ll, $obj, $exe -ErrorAction SilentlyContinue

    $r = Invoke-Captured $script:cpp @($src, "--compile", "--emit-ir", "-o", $ll)
    if ($r.ExitCode -ne 0) { return @{ Outcome = "reject"; Output = ""; Stderr = ""; Detail = (Normalize $r.Stderr) } }

    $r = Invoke-Captured $script:clang @("-c", $ll, "-o", $obj, "-Wno-override-module")
    if ($r.ExitCode -ne 0) { return @{ Outcome = "reject"; Output = ""; Stderr = ""; Detail = ("IR does not compile: " + (Normalize $r.Stderr)) } }

    # -lm is not decoration: sqrt/floor/ceil/round lower to libm calls, glibc keeps
    # them in a separate library, and without it this link fails on Linux while
    # succeeding on Windows (ucrt has them in the default import libraries). That
    # asymmetry is how 17_floats came back as "C++ backend rejects" on the first
    # real Linux run. MinGW accepts the flag and ignores it.
    $r = Invoke-Captured $script:cc @($obj, $script:runtimeSrc, "-o", $exe, "-lm")
    if ($r.ExitCode -ne 0) { return @{ Outcome = "reject"; Output = ""; Stderr = ""; Detail = ("link failed: " + (Normalize $r.Stderr)) } }

    $run = Invoke-Captured $exe @()
    if ($run.ExitCode -ne 0) { return @{ Outcome = "error"; Output = (Normalize $run.Stdout); Stderr = (Normalize $run.Stderr); Detail = (Fail-Reason $run) } }
    return @{ Outcome = "ok"; Output = (Normalize $run.Stdout); Stderr = (Normalize $run.Stderr); Detail = "" }
}

function Run-Self($name, $src) {
    $prefix = Join-Path $script:WorkDir "$name.self"
    $binary = $prefix + $script:exeSuffix

    $r = Invoke-Captured $script:self @($src, $prefix)

    # The exit status is the compiler's own verdict, so trust it first. Inferring
    # rejection from "no binary was produced" was weaker: a compiler that reports a
    # diagnostic and still writes a binary would have been scored as accepting.
    if ($r.ExitCode -ne 0) {
        return @{ Outcome = "reject"; Output = (Normalize $r.Stdout); Stderr = (Normalize $r.Stderr); Detail = (Normalize ($r.Stdout + " " + $r.Stderr)) }
    }
    if (-not (Test-Path $binary)) {
        return @{ Outcome = "reject"; Output = ""; Stderr = ""; Detail = (Normalize ($r.Stdout + " " + $r.Stderr)) }
    }

    $run = Invoke-Captured $binary @()
    if ($run.ExitCode -ne 0) { return @{ Outcome = "error"; Output = (Normalize $run.Stdout); Stderr = (Normalize $run.Stderr); Detail = (Fail-Reason $run) } }
    return @{ Outcome = "ok"; Output = (Normalize $run.Stdout); Stderr = (Normalize $run.Stderr); Detail = "" }
}

# ---------------------------------------------------------------- main loop

$cases = @()
$cases += Get-ChildItem (Join-Path $RepoRoot "tests\diff") -Filter *.miko
# examples/*_error.miko are rejection tests by naming convention. They used to be
# run by hand only, which is how four of them came to be silently accepted by the
# self-hosted compiler: nothing in the gate ever asked.
$exampleDir = Join-Path $RepoRoot "examples"
if (Test-Path $exampleDir) {
    $cases += Get-ChildItem $exampleDir -Filter "*_error.miko"
}
$cases = $cases | Sort-Object Name
if ($Filter) { $cases = $cases | Where-Object { $_.BaseName -like $Filter } }
if (-not $cases) { throw "difftest: no cases in tests/diff or examples" }

$rows = @()
$failures = 0

foreach ($case in $cases) {
    $name = $case.BaseName
    $src  = $case.FullName
    $expPath = [IO.Path]::ChangeExtension($src, ".expected")
    $isReject = (Test-Path ([IO.Path]::ChangeExtension($src, ".reject"))) -or ($name -like "*_error")
    $mustFail = Test-Path ([IO.Path]::ChangeExtension($src, ".error"))

    $expected = ""
    if (Test-Path $expPath) { $expected = Normalize (Get-Content $expPath -Raw) }

    $results = [ordered]@{}
    if ($script:Trace) { Add-Content -Path $script:Trace -Value "$name interp" }
    $results["interp"] = Run-Interp $src
    if ($script:Trace) { Add-Content -Path $script:Trace -Value "$name cpp" }
    $results["cpp"]    = Run-Cpp    $name $src
    if ($script:Trace) { Add-Content -Path $script:Trace -Value "$name self" }
    $results["self"]   = Run-Self   $name $src
    if ($script:Trace) { Add-Content -Path $script:Trace -Value "$name done" }

    $caseFailed = $false
    $cells = @()
    foreach ($k in $results.Keys) {
        $res = $results[$k]
        if ($isReject) {
            # The interpreter runs parse and sema before executing, so a program
            # it refuses arrives as Outcome 'error' with an empty stdout and a
            # diagnostic on stderr - that is a rejection, not a failed run.
            $ok = ($res.Outcome -eq "reject") -or
                  (($res.Outcome -eq "error") -and ($res.Output -eq "") -and ($res.Stderr -ne ""))
            $cell = if ($ok) { "reject" } else { "ACCEPTED!" }
        } elseif ($mustFail) {
            # Correct behaviour is a diagnosable failure: a non-zero exit, the
            # expected stdout (usually empty), and a message on stderr. A
            # silent wrong value or a hard crash is not acceptable - a crash
            # gives the program no chance to say what went wrong, and printing
            # the diagnostic to stdout corrupts the program's own output.
            $diagnosed = ($res.Stderr -ne "")
            $ok = ($res.Outcome -eq "error") -and ($res.Output -eq $expected) -and $diagnosed
            if ($ok) { $cell = "fail" }
            elseif ($res.Outcome -eq "ok") { $cell = "SILENT:$($res.Output)" }
            elseif ($res.Outcome -ne "error") { $cell = "CRASH" }
            elseif ($res.Output -ne $expected) { $cell = "STDOUT:$($res.Output)" }
            else { $cell = "NOMSG" }
        } else {
            $ok = ($res.Outcome -eq "ok") -and ($res.Output -eq $expected)
            $cell = if ($ok) { "ok" } elseif ($res.Outcome -eq "reject") { "REJECT" } elseif ($res.Outcome -eq "error") { "ERR:$($res.Detail)" } else { "DIFF" }
        }
        if (-not $ok) { $caseFailed = $true }
        $cells += $cell
        if (-not $ok -and $ShowDetail) { Write-Host "    [$k] $($res.Detail)" -ForegroundColor DarkGray }
    }
    if ($caseFailed) { $failures++ }

    $rows += [pscustomobject]@{
        Case   = $name
        Interp = $cells[0]
        Cpp    = $cells[1]
        Self   = $cells[2]
        Status = if ($caseFailed) { "FAIL" } else { "pass" }
    }
}

$rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host

$pass = ($rows | Where-Object { $_.Status -eq "pass" }).Count
"difftest: $pass / $($rows.Count) cases agree with expected output across all backends"

if (-not $KeepArtifacts) {
    Get-ChildItem $WorkDir -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
}

exit $failures

