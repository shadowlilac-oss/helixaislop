#requires -version 5
# Build + run the Helix test suite (and optionally the CLI) with MSVC.
param([switch]$NoRun, [switch]$Cli)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
. "$root\find-vcvars.ps1"
$vc = Find-VcVars

$srcFiles = (Get-ChildItem "$root\src\*.cpp" | ForEach-Object { '"' + $_.FullName + '"' }) -join ' '
New-Item -ItemType Directory -Force "$root\build" | Out-Null

$cxx = '/nologo /EHsc /std:c++20 /O2 /W4 /wd4100 /wd4127 /wd4244 /wd4267'
$inc = "/I `"$root\include`""

# --- test runner ---
$testFiles = (Get-ChildItem "$root\tests\*.cpp" | ForEach-Object { '"' + $_.FullName + '"' }) -join ' '
$testOut = "$root\build\helix_tests.exe"
$clTests = "cl $cxx $inc $srcFiles $testFiles /Fe:`"$testOut`" /Fo`"$root\build\obj\\`""
New-Item -ItemType Directory -Force "$root\build\obj" | Out-Null

Write-Host "==> building helix_tests.exe" -ForegroundColor Cyan
cmd /c "call `"$vc`" >nul 2>&1 && $clTests" 2>&1 | Where-Object { $_ -notmatch '^\s*\w+\.cpp\s*$' -and $_ -notmatch 'Microsoft \(R\)|Copyright' } | ForEach-Object { Write-Host $_ }
if (-not (Test-Path $testOut)) { Write-Host "BUILD FAILED" -ForegroundColor Red; exit 1 }

if ($Cli) {
    $cliOut = "$root\build\helixc.exe"
    $clCli = "cl $cxx $inc $srcFiles `"$root\tools\helixc.cpp`" /Fe:`"$cliOut`" /Fo`"$root\build\obj\\`""
    Write-Host "==> building helixc.exe" -ForegroundColor Cyan
    cmd /c "call `"$vc`" >nul 2>&1 && $clCli" 2>&1 | Where-Object { $_ -notmatch '^\s*\w+\.cpp\s*$' -and $_ -notmatch 'Microsoft \(R\)|Copyright' } | ForEach-Object { Write-Host $_ }
}

if (-not $NoRun) {
    Write-Host "==> running tests" -ForegroundColor Cyan
    & $testOut
    exit $LASTEXITCODE
}
