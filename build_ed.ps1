# build_ed.ps1 -- firmware build for projects/english-dict.
#
# Host fixes carried over from build_qiaopi.ps1 (verified on this machine):
#   1) Git Bash leaks MSYSTEM into child processes; ESP-IDF's export aborts with
#      "MSys/Mingw is not supported", so scrub those vars before building.
#      MSYS itself must go too, or idf_tools.py export runs half-MSYS-aware and
#      never reports IDF_PYTHON_ENV_PATH.
#   2) The GSP "/D:/" ninja deps bug (which forces deleting .ninja_deps in GSP
#      projects such as poet_life/weiqi-quest) does NOT apply here: this project
#      is plain LVGL and has no GSP-generated files. The deps index is kept
#      unless it actually contains "/D:/" paths (counted below), so a normal
#      incremental build is minutes, not 25+.
#   3) .ninja_log is never touched: moving it alongside .ninja_deps degenerates
#      into a full recompile (known-issues #35).
#   4) Env pairing (known-issues #8): D:\esp-mo\esp-idf pairs with the default
#      %USERPROFILE%\.espressif. The other checkout D:\esp-tools\esp-idf pairs
#      with D:\esp-tools\espressif; mixing them fails and IDF_TARGET cannot save it.
# Keep this file ASCII-only: PowerShell 5.x decodes a BOM-less .ps1 with the
# system ANSI codepage (GBK here) and non-ASCII bytes can break parsing.
param(
    [switch]$Clean,      # remove the whole build dir (reconfigure too)
    [switch]$KeepDeps    # keep .ninja_deps even if it contains "/D:/" paths
)

$ErrorActionPreference = "Stop"

foreach ($v in @('MSYSTEM', 'MSYS', 'MSYSCON', 'MINGW_PREFIX', 'MINGW_CHOST', 'CHERE_INVOKING')) {
    if (Test-Path "Env:\$v") { Remove-Item "Env:\$v" }
}

$workspace = "D:\esp-mo\esp-mosaico-vibe-main"
$project = "projects/english-dict"
$build = "$workspace\projects\english-dict\build"
$deps = "$build\.ninja_deps"
$script = "$workspace\submodule\esp-mosaico-utils\mosaico-tools\skills\idf-low-noise-build\scripts\idf_low_noise_build.py"

if ($Clean -and (Test-Path $build)) {
    Write-Host "[build] clean: removing $build"
    Remove-Item $build -Recurse -Force
}

# known-issues #1: "/D:/" paths in the deps index break ninja incremental builds
# (FindFirstFileExA). Count first; only move when non-zero (move = full rebuild).
if ((Test-Path $deps) -and -not $KeepDeps) {
    $bad = (Select-String -Path $deps -Pattern '/D:/' -AllMatches |
            ForEach-Object { $_.Matches.Count } |
            Measure-Object -Sum).Sum
    if ($bad -gt 0) {
        Write-Host "[build] .ninja_deps holds $bad '/D:/' entries; moving aside (full rebuild this run)"
        Move-Item $deps "$deps.stale.$(Get-Date -Format yyyyMMddHHmmss)" -Force
    } else {
        Write-Host "[build] .ninja_deps clean (0 '/D:/' entries); keeping incremental index"
    }
}

# Verify the pinned checkout before building (AGENTS.md).
$head = (git -C "D:\esp-mo\esp-idf" rev-parse HEAD).Trim()
if ($head -ne "7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe") {
    throw "IDF checkout mismatch: $head (expected 7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe)"
}

Write-Host "[build] building $project ..."
Push-Location $workspace
try {
    $env:IDF_PATH = "D:\esp-mo\esp-idf"
    python $script --project $project build
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
} finally {
    Pop-Location
}
Write-Host "[build] OK"
