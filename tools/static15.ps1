# 15 typical static errors, one per file. Each must be rejected by the self-hosted
# frontend with a message that carries file:line:col and a caret.

param(
    [string]$RepoRoot = ".",
    [string]$Compiler = ""
)

$cases = [ordered]@{
    '01_undefined_variable'   = @('fn main() {', '    print(missing);', '}')
    '02_wrong_arg_count'      = @('fn f(a: i64): i64 {', '    return a;', '}', 'fn main() {', '    print(f(1, 2));', '}')
    '03_wrong_arg_type'       = @('fn f(a: i64): i64 {', '    return a;', '}', 'fn main() {', '    print(f("s"));', '}')
    '04_assign_type_mismatch' = @('fn main() {', '    let x: i64 = 1;', '    x = "s";', '    print(x);', '}')
    '05_return_type_mismatch' = @('fn f(): i64 {', '    return "s";', '}', 'fn main() {', '    print(f());', '}')
    '06_bad_condition'        = @('fn main() {', '    let s: string = "a";', '    if s {', '        print(1);', '    }', '}')
    '07_undefined_function'   = @('fn main() {', '    print(nope(1));', '}')
    '08_undefined_struct'     = @('fn main() {', '    let p: Missing = 0;', '    print(p);', '}')
    '09_missing_field'        = @('struct P {', '    x: i64,', '}', 'fn main() {', '    let p: P = P { y: 1 };', '    print(p.x);', '}')
    '10_field_type_mismatch'  = @('struct P {', '    x: i64,', '}', 'fn main() {', '    let p: P = P { x: "s" };', '    print(p.x);', '}')
    # Arrays are declared without a type annotation (`let a = [1, 2, 3];`), which is
    # why the first version of this case - `let a: i64[] = [1, 2]` - made the C++
    # frontend report a parse error instead of the semantic one. The point of the
    # case is a string index, so it has to use syntax both frontends parse.
    '11_index_not_integer'    = @('fn main() {', '    let a = [1, 2];', '    let i: string = "0";', '    print(a[i]);', '}')
    '12_const_assign'         = @('fn main() {', '    let n: i64 = 5;', '    const k: i64 = 3;', '    k = n;', '    print(k);', '}')
    '13_undefined_enum'       = @('fn main() {', '    let e: NoEnum = NoEnum::A;', '    print(1);', '}')
    '14_undefined_enum_case'  = @('enum E {', '    A,', '    B,', '}', 'fn main() {', '    let e: E = E::C;', '    print(1);', '}')
    '15_not_callable'         = @('fn main() {', '    let n: i64 = 1;', '    print(n(2));', '}')
}

$root = $RepoRoot
if (-not $root) { $root = "." }
$work = Join-Path $root "out/static15"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Path $work -Force | Out-Null

$compiler = $Compiler
if (-not $compiler) {
    $compiler = Join-Path $root "dist/miko.exe"
    if (-not (Test-Path $compiler)) { $compiler = Join-Path $root "dist/miko" }
}

$ok = 0
$bad = @()
foreach ($name in $cases.Keys) {
    $src = Join-Path $work "$name.miko"
    Set-Content -Path $src -Value $cases[$name] -Encoding ascii
    $out = & $compiler $src (Join-Path $work $name) 2>&1 | Out-String
    $hasError = $out -match "error:"
    # file:line:col - a colon, digits, colon, digits at the end of the --> line
    $hasPos = $out -match " --> .*:\d+:\d+"
    $hasCaret = $out -match "(?m)^\s*\d+\s+\|.*\r?\n\s*\|?\s*\^"
    $status = "MISSING"
    if ($hasError -and $hasPos -and $hasCaret) { $status = "ok"; $ok++ }
    elseif ($hasError -and $hasPos) { $status = "no-caret" }
    elseif ($hasError) { $status = "no-position" }
    if ($status -ne "ok") { $bad += "$name ($status)" }
    $first = ($out -split "`r?`n" | Where-Object { $_ -match 'error:|-->' } | Select-Object -First 2) -join " / "
    Write-Host ("  {0,-24} {1,-12} {2}" -f $name, $status, $first)
}

Write-Host ""
Write-Host ("static15: $ok / $($cases.Count) rejected with file:line:col and a caret")
if ($bad.Count -gt 0) {
    Write-Host ("  not conforming: " + ($bad -join ", "))
    exit 1
}
exit 0
