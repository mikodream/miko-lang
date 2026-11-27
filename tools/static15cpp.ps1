# Runs the same 15 programs through the C++ compiler and reports, per case,
# whether it rejects and whether the message carries a source location.
# The C++ compiler takes `miko <input.miko> [options]` - passing a second bare
# argument makes it complain "unexpected argument", which a naive match on "error"
# reads as a rejection, so it uses --emit-ir and one path only.

$root = $args[0]
if (-not $root) { $root = "." }
$cpp = $args[1]
if (-not $cpp) { $cpp = "C:\llvmtest\build22\Release\miko.exe" }

$files = Get-ChildItem (Join-Path $root "out/static15/*.miko") | Sort-Object Name
$rej = 0
$pos = 0
foreach ($f in $files) {
    $out = & $cpp $f.FullName --compile -o (Join-Path $f.DirectoryName "cpp.exe") 2>&1 | Out-String
    $isReject = $out -match "(?m)^error: "
    $hasPos = $out -match "\.miko:\d+:\d+"
    if ($isReject) { $rej++ }
    if ($hasPos) { $pos++ }
    $first = ($out -split "`r?`n" | Where-Object { $_ -match 'error:' } | Select-Object -First 1)
    Write-Host ("  {0,-24} reject={1,-6} pos={2,-6} {3}" -f $f.BaseName, $isReject, $hasPos, $first)
}
Write-Host ""
Write-Host ("cpp-backend: $rej / $($files.Count) rejected, $pos / $($files.Count) with file:line:col")
