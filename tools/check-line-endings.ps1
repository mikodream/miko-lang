[CmdletBinding()]
param([string]$RepoRoot = (Split-Path -Parent $PSScriptRoot))

$ErrorActionPreference = 'Stop'
Push-Location $RepoRoot
try {
    $files = @(git ls-files -- '*.miko')
    if ($LASTEXITCODE -ne 0) { throw 'git ls-files failed' }

    $bad = @()
    foreach ($relative in $files) {
        $path = Join-Path $RepoRoot $relative
        $bytes = [System.IO.File]::ReadAllBytes($path)
        for ($i = 0; $i -lt $bytes.Length - 1; $i++) {
            if ($bytes[$i] -eq 13 -and $bytes[$i + 1] -eq 10) {
                $bad += $relative
                break
            }
        }
    }

    if ($bad.Count -gt 0) {
        Write-Host "line-endings: $($bad.Count) tracked .miko file(s) contain CRLF"
        $bad | ForEach-Object { Write-Host "  $_" }
        exit 1
    }

    Write-Host "line-endings: $($files.Count) tracked .miko file(s) use LF"
} finally {
    Pop-Location
}
