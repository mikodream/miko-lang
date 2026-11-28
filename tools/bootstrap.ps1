# bootstrap.ps1 - proves the self-hosted compiler actually works.
#
# The existing ctest `miko_llvm_selfhosted_miko_exe` compares the IR text of two
# generations and stops there. That check stayed green while the compiler it
# produced crashed on every input and emitted IR with the opcode missing
# (`%t4 =  i64 %t3, 1`): an IR fixed point says nothing about whether the
# binary that emits it can compile anything.
#
# This script closes that gap. For each generation it:
#   1. compiles the compiler with the previous generation,
#   2. links the IR through dist/miko-link.bat (the same path miko uses),
#   3. runs the resulting compiler on every tests/diff case and compares the
#      program output against the case's .expected file.
#
# A generation only counts as good when it compiles programs correctly.

[CmdletBinding()]
param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$StartCompiler,
    [int]$Generations = 2,
    [string]$WorkDir,
    [int]$TimeoutMs = 600000
)

$ErrorActionPreference = 'Stop'
$inv = [System.Globalization.CultureInfo]::InvariantCulture

# The linker is a .bat on Windows and a .sh everywhere else, and an executable has
# no suffix on Unix. Without this the script only ran on the one Windows gate, so
# the fixed point was only ever proven there; running it on the release platforms is
# what shows the property holds per target rather than on a single machine.
$onWindows = $true
if ($env:OS -ne 'Windows_NT') { $onWindows = $false }
$exeSuffix = ''
if ($onWindows) { $exeSuffix = '.exe' }
$linkScript = Join-Path $RepoRoot 'dist/miko-link.bat'
if (-not $onWindows) { $linkScript = Join-Path $RepoRoot 'dist/miko-link.sh' }

# Runs the linker on an IR file, returning the artifact path it should have written.
function Invoke-Link {
    param([string]$Ir, [string]$Prefix, [string]$WorkingDir)
    $out = "$Prefix$exeSuffix"
    if ($onWindows) {
        return Invoke-Tool -Exe 'cmd.exe' -Arguments @('/c', $linkScript, $Ir, $out) -WorkingDir $WorkingDir
    }
    return Invoke-Tool -Exe 'bash' -Arguments @($linkScript, $Ir, $out) -WorkingDir $WorkingDir
}

if (-not $StartCompiler) {
    $StartCompiler = Join-Path $RepoRoot "dist/miko$exeSuffix"
}
if (-not $WorkDir) {
    $WorkDir = Join-Path $RepoRoot 'out/bootstrap-check'
}
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

if (-not (Test-Path $linkScript)) {
    Write-Host "bootstrap: missing $linkScript"
    exit 2
}
if (-not (Test-Path $StartCompiler)) {
    Write-Host "bootstrap: missing start compiler $StartCompiler"
    exit 2
}

function Invoke-Tool {
    param([string]$Exe, [string[]]$Arguments, [string]$WorkingDir)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    foreach ($a in $Arguments) { [void]$psi.ArgumentList.Add($a) }
    $psi.WorkingDirectory = $WorkingDir
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $p = [System.Diagnostics.Process]::Start($psi)
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($TimeoutMs)) {
        try { $p.Kill() } catch { }
        return [pscustomobject]@{ ExitCode = -1; Stdout = ''; Stderr = 'timeout' }
    }
    $p.WaitForExit()
    return [pscustomobject]@{ ExitCode = $p.ExitCode; Stdout = $outTask.Result; Stderr = $errTask.Result }
}

function Get-Normalized([string]$text) {
    if ($null -eq $text) { return '' }
    $t = $text -replace "`r`n", "`n"
    return $t.TrimEnd("`n")
}

# Collect cases: every .miko with a .expected is a program whose output must match.
$cases = @()
foreach ($f in (Get-ChildItem (Join-Path $RepoRoot 'tests/diff') -Filter *.miko | Sort-Object Name)) {
    $expected = [System.IO.Path]::ChangeExtension($f.FullName, '.expected')
    if (Test-Path $expected) {
        $isError = Test-Path ([System.IO.Path]::ChangeExtension($f.FullName, '.error'))
        $cases += [pscustomobject]@{
            Name = $f.BaseName
            Source = $f.FullName
            Expected = $expected
            IsError = $isError
        }
    }
}

$hashes = @()
$failures = @()
$compiler = $StartCompiler

for ($g = 1; $g -le $Generations; $g++) {
    $prefix = Join-Path $WorkDir "gen$g"
    $self = Invoke-Tool -Exe $compiler -Arguments @('--self', $prefix) -WorkingDir $RepoRoot
    if ($self.ExitCode -ne 0 -or -not (Test-Path "$prefix.ll")) {
        $failures += "gen${g}: emitting own IR failed (exit $($self.ExitCode))"
        Write-Host "bootstrap: gen$g emit FAILED (exit $($self.ExitCode))"
        if ($self.Stderr) { Write-Host ("  " + $self.Stderr.Trim()) }
        break
    }

    $hash = (Get-FileHash "$prefix.ll" -Algorithm SHA256).Hash
    $hashes += [pscustomobject]@{ Gen = $g; Hash = $hash }

    $exe = "$prefix$exeSuffix"
    $link = Invoke-Link -Ir "$prefix.ll" -Prefix $prefix -WorkingDir $RepoRoot
    if ($link.ExitCode -ne 0 -or -not (Test-Path $exe)) {
        $failures += "gen${g}: linking own IR failed (exit $($link.ExitCode))"
        Write-Host "bootstrap: gen$g link FAILED (exit $($link.ExitCode))"
        break
    }

    # Behavioural check: this generation must compile the cases correctly.
    $passed = 0
    $bad = @()
    foreach ($case in $cases) {
        $stem = Join-Path $WorkDir ("gen{0}_{1}" -f $g, $case.Name)
        $res = Invoke-Tool -Exe $exe -Arguments @($case.Source, $stem) -WorkingDir $RepoRoot
        if ($res.ExitCode -ne 0 -or -not (Test-Path "$stem$exeSuffix")) {
            $bad += "$($case.Name): compile exit $($res.ExitCode)"
            continue
        }
        $run = Invoke-Tool -Exe "$stem$exeSuffix" -Arguments @() -WorkingDir $RepoRoot
        $want = Get-Normalized (Get-Content $case.Expected -Raw)
        $got = Get-Normalized $run.Stdout
        if ($case.IsError) {
            # A trap case: the program must die with a diagnostic on stderr and
            # print nothing. Requiring exit 0 here is what made the first run of
            # this script report 12/14 while every backend was behaving.
            if ($run.ExitCode -ne 0 -and $got -eq $want -and $run.Stderr -ne '') {
                $passed++
            } else {
                $bad += "$($case.Name): expected failure, got exit $($run.ExitCode) out [$got]"
            }
            continue
        }
        if ($run.ExitCode -eq 0 -and $got -eq $want) {
            $passed++
        } else {
            $bad += "$($case.Name): want [$want] got [$got]"
        }
    }

    Write-Host ("bootstrap: gen{0}  {1} bytes  {2}/{3} cases  {4}" -f `
        $g, (Get-Item "$prefix.ll").Length, $passed, $cases.Count, $hash.Substring(0, 16))
    foreach ($b in $bad) { Write-Host "  $b" }
    if ($bad.Count -gt 0) { $failures += "gen${g}: $($bad.Count) case(s) wrong" }
    $compiler = $exe
}

$distinct = ($hashes | ForEach-Object { $_.Hash } | Sort-Object -Unique).Count
if ($hashes.Count -gt 1) {
    if ($distinct -eq 1) {
        Write-Host "bootstrap: IR fixed point holds across $($hashes.Count) generations"
    } else {
        Write-Host "bootstrap: IR differs between generations"
        $failures += 'IR is not a fixed point'
    }
}

if ($failures.Count -gt 0) {
    Write-Host ("bootstrap: FAILED ({0})" -f ($failures -join '; '))
    exit 1
}

Write-Host "bootstrap: every generation compiled every case correctly"
exit 0
