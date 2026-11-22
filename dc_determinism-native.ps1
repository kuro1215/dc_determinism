#Requires -Version 7.0
# Entry point on a Windows host: runs the prebuilt C++ binary prebuilt/dc_determinism/windows-amd64/dc_determinism.exe
# (MSVC, static runtime; no Python needed).
# Usage: .\dc_determinism-native.ps1 <command> [options]   (same commands and options as `dc_determinism --help`)
# The pybind commands (compare pybind, capture simulator --engine pybind-*) run on linux-amd64 only (they need
# python3 and the x86-64 extensions; this binary refuses them): run them with .\dc_determinism.ps1, which uses the
# dc_determinism-cpu:local Docker image.
$ErrorActionPreference = 'Stop'
$Repo = [IO.Path]::GetFullPath($PSScriptRoot)
$Binary = Join-Path $Repo 'prebuilt\dc_determinism\windows-amd64\dc_determinism.exe'
if (-not (Test-Path -LiteralPath $Binary)) {
    [Console]::Error.WriteLine('dc_determinism-native.ps1: prebuilt\dc_determinism\windows-amd64\dc_determinism.exe not found (build it with scripts\prebuild.ps1)')
    exit 2
}
$PriorRepo = $env:DC_DETERMINISM_REPO
try {
    if (-not $env:DC_DETERMINISM_REPO) { $env:DC_DETERMINISM_REPO = $Repo }
    & $Binary @args
    exit $LASTEXITCODE
} finally {
    $env:DC_DETERMINISM_REPO = $PriorRepo
}
