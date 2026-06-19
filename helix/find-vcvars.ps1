# Locate vcvars64.bat without hardcoding a path. Works on dev machines (any VS 2022
# edition incl. Build Tools) and on GitHub Actions Windows runners. Dot-source this and
# call Find-VcVars. Override with $env:HELIX_VCVARS if you have an unusual layout.
function Find-VcVars {
    if ($env:HELIX_VCVARS -and (Test-Path $env:HELIX_VCVARS)) { return $env:HELIX_VCVARS }

    # vswhere ships at a fixed location with every VS 2015.2+ install and on GH runners.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $inst = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null | Select-Object -First 1
        if ($inst) {
            $v = Join-Path $inst 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path $v) { return $v }
        }
    }

    # Last resort: scan the common fixed install locations.
    foreach ($pf in ${env:ProgramFiles(x86)}, $env:ProgramFiles) {
        foreach ($ed in 'BuildTools', 'Community', 'Professional', 'Enterprise') {
            $v = Join-Path $pf "Microsoft Visual Studio\2022\$ed\VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $v) { return $v }
        }
    }
    throw "vcvars64.bat not found. Install VS 2022 C++ tools, or set `$env:HELIX_VCVARS to its path."
}
