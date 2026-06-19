#requires -version 5
# Helix CI entry point: build + run the unit suite, then build all differential
# fuzzers and smoke-run a few seeds of each. Exits non-zero on the first failure so
# CI (see ../.github/workflows/ci.yml) goes red. Run locally with: ./ci.ps1
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$vc = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vc)) {
    $vc = "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"  # GH runner
}

# 1. Unit suite (also compiles every src file; the verifier runs in-codegen in debug builds).
Write-Host "==> unit tests" -ForegroundColor Cyan
& "$root\build.ps1"
if ($LASTEXITCODE -ne 0) { Write-Host "UNIT TESTS FAILED" -ForegroundColor Red; exit 1 }

# 2. Build the fuzzers + repro/bench tools.
$srcs = (Get-ChildItem "$root\src\*.cpp" | ForEach-Object { '"' + $_.FullName + '"' }) -join ' '
$cxx = '/nologo /EHsc /std:c++20 /O2'
$inc = "/I `"$root\include`" /I `"$root\tools`""
foreach ($f in 'fuzz_cf','fuzz_imp','fuzz_mem','fuzz_opt','fuzz_parse') {
    New-Item -ItemType Directory -Force "$root\build\$f" | Out-Null
    $cmd = "cl $cxx $inc `"$root\tools\$f.cpp`" $srcs /Fe:`"$root\build\$f.exe`" /Fo`"$root\build\$f\\`""
    cmd /c "call `"$vc`" >nul 2>&1 && $cmd" | Out-Null
    if (-not (Test-Path "$root\build\$f.exe")) { Write-Host "BUILD FAILED: $f" -ForegroundColor Red; exit 1 }
}

# 3. Smoke-fuzz each engine on a couple of seeds (small counts; the nightly job runs more).
function Smoke($exe, $seeds, $n) {
    foreach ($s in $seeds) {
        $out = & "$root\build\$exe.exe" $s $n 2>&1
        # final "mismatches : N" summary line (anchored, last occurrence)
        $mm = ($out | Select-String '^mismatches\s*:\s*(\d+)').Matches.Groups[1].Value | Select-Object -Last 1
        # real failure markers are upper-case and specific (case-SENSITIVE so the word
        # "mismatches" in the summary doesn't trip it).
        $bad = $out | Select-String -CaseSensitive -Pattern 'NON-TERMINATION|MISMATCH FOUND|OPT MISMATCH'
        Write-Host ("  {0,-9} seed {1,-6} -> mismatches={2}" -f $exe, $s, $mm)
        if ($mm -ne '0' -or $bad) { Write-Host "FUZZ FAILURE in $exe seed $s" -ForegroundColor Red; $out | Out-Host; exit 1 }
    }
}
Write-Host "==> smoke fuzz" -ForegroundColor Cyan
Smoke 'fuzz_imp' @(1, 7) 3000
Smoke 'fuzz_mem' @(1, 7) 8000
Smoke 'fuzz_cf'  @(1)    1200   # 8-param programs are allocation-heavy; keep the smoke small
Smoke 'fuzz_opt' @(1)    250

# Parser robustness: must process malformed input without crashing (exit 0 + "NO CRASH").
$pout = & "$root\build\fuzz_parse.exe" 1 40000 2>&1
if ($LASTEXITCODE -ne 0 -or -not ($pout | Select-String 'NO CRASH')) {
    Write-Host "PARSER FUZZ FAILURE" -ForegroundColor Red; $pout | Out-Host; exit 1
}
Write-Host "  fuzz_parse -> no crash"

Write-Host "ALL CI CHECKS PASSED" -ForegroundColor Green
