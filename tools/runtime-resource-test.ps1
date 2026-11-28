[CmdletBinding()]
param([string]$RepoRoot = (Split-Path -Parent $PSScriptRoot))

$ErrorActionPreference = 'Stop'
$runtime = Join-Path $RepoRoot 'runtime/miko_runtime.c'
$test = Join-Path $RepoRoot 'tests/runtime_resource.c'
$outDir = Join-Path $RepoRoot 'out/runtime-resource'
$exeSuffix = if ($env:OS -eq 'Windows_NT') { '.exe' } else { '' }
$exe = Join-Path $outDir "runtime-resource$exeSuffix"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$cc = $env:MIKO_CC
if (-not $cc) {
    $found = Get-Command clang -ErrorAction SilentlyContinue
    if ($found) { $cc = $found.Source }
}
if (-not $cc) {
    $found = Get-Command cc -ErrorAction SilentlyContinue
    if ($found) { $cc = $found.Source }
}
if (-not $cc) { throw 'runtime-resource: no C compiler found' }

& $cc '-std=c11' '-O2' '-I' (Join-Path $RepoRoot 'runtime') $test $runtime '-o' $exe
if ($LASTEXITCODE -ne 0) { throw "runtime-resource: compile failed with exit code $LASTEXITCODE" }
& $exe
if ($LASTEXITCODE -ne 0) { throw "runtime-resource: test failed with exit code $LASTEXITCODE" }
