# Build (and optionally test) the C++ implementation of dc_determinism on Windows with MSVC.
#
# Usage: scripts\build.ps1 [-Config Release] [-BuildDir <dir>] [-Test] [-Clean] [-Generator Ninja] [-NoTests]
#
# Finds Visual Studio with vswhere, imports the x64 developer environment (vcvars64.bat), then runs
# cmake configure/build (and ctest with -Test) for src/dc_determinism. Output: build/dc_determinism/windows-amd64
# (ignored by Git); the binary is <BuildDir>\dc_determinism.exe. Release builds link the runtime
# statically (/MT). No network access. -NoTests configures without the unit tests (the dc_determinism binary is the
# same; scripts/prebuild.ps1 builds a copy of the sources at a fixed path this way).
param(
    [string]$Config = 'Release',
    [string]$BuildDir,
    [string]$Generator,
    [switch]$Test,
    [switch]$Clean,
    [switch]$NoTests
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $BuildDir) { $BuildDir = Join-Path $root 'build\dc_determinism\windows-amd64' }

function Import-VisualStudioEnvironment {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw "vswhere.exe not found: $vswhere" }
    $installation = (& $vswhere -latest -products '*' -requires 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' `
        -property installationPath | Select-Object -First 1)
    if (-not $installation) { throw 'no Visual Studio installation with the C++ x64 tools was found' }
    $vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path -LiteralPath $vcvars)) { throw "vcvars64.bat not found: $vcvars" }
    $dump = & cmd.exe /d /c "`"$vcvars`" >nul && set"
    if ($LASTEXITCODE -ne 0) { throw "vcvars64.bat failed (exit $LASTEXITCODE)" }
    foreach ($line in $dump) {
        $split = $line.IndexOf('=')
        if ($split -gt 0) {
            [Environment]::SetEnvironmentVariable($line.Substring(0, $split), $line.Substring($split + 1), 'Process')
        }
    }
    Write-Host "Visual Studio: $installation"
}

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed (exit $LASTEXITCODE)" }
}

Import-VisualStudioEnvironment

if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

if (-not $Generator) {
    # Without Ninja, CMake picks the newest installed Visual Studio generator.
    $Generator = if (Get-Command ninja -ErrorAction SilentlyContinue) { 'Ninja' } else { '' }
}
$configure = @('-S', (Join-Path $root 'src\dc_determinism'), '-B', $BuildDir, "-DCMAKE_BUILD_TYPE=$Config")
if ($Generator) { $configure += @('-G', $Generator) }
if ($Generator -eq 'Ninja') { $configure += @('-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl') }
if ($NoTests) { $configure += '-DDC_DETERMINISM_BUILD_TESTS=OFF' }
Invoke-Checked cmake $configure
Invoke-Checked cmake @('--build', $BuildDir, '--config', $Config)

if ($Test) {
    Invoke-Checked ctest @('--test-dir', $BuildDir, '--build-config', $Config, '--output-on-failure')
}

$binary = Get-ChildItem -LiteralPath $BuildDir -Recurse -Filter 'dc_determinism.exe' | Select-Object -First 1
if ($binary) { Write-Host "binary: $($binary.FullName)" }
