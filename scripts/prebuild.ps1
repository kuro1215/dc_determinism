#Requires -Version 7.0
# Rebuild the prebuilt binaries of the C++ dc_determinism tool on a Windows host and record them in prebuilt/manifest.json, or
# (-Check) rebuild them and report whether the installed ones are what the sources produce.
#   prebuilt/dc_determinism/windows-amd64/dc_determinism.exe   (MSVC, /MT static runtime, /Brepro; scripts/build.ps1)
#   prebuilt/dc_determinism/linux-amd64/dc_determinism         (g++, static, stripped, in the dc_determinism-cpu:local image;
#                                             scripts/prebuild.sh --in-image)
#   prebuilt/manifest.json                   (new/updated `dc_determinism` section only)
# The linux-arm64 binary is built on an arm64 host: an aarch64 Linux host or an Apple-silicon Mac runs
# scripts/prebuild.sh (it builds linux-<its architecture> in the image of that architecture).
#
# Usage: scripts\prebuild.ps1 [-Image dc_determinism-cpu:local] [-Test] [-SkipWindows] [-SkipLinux] [-Check]
#   -Test         also run CTest of the windows-amd64 and linux-amd64 builds (before the binaries are copied).
#   -SkipWindows / -SkipLinux   rebuild only one platform; the other entries of the manifest are kept (when src/dc_determinism
#               and its dependencies are unchanged; a changed source drops the entries of the platforms that were not rebuilt,
#               with their files, and they have to be rebuilt on their own hosts).
#               The Docker image check of the Linux platform runs first, before any binary is built or replaced.
#   -Check        installs nothing and writes no manifest: rebuilds the binaries and prints one line per binary and per
#                 manifest member, "identical" or "differs" (for a differing binary also whether its code sections are
#                 identical). The toolchain recorded in the manifest (MSVC toolset, link.exe, Windows SDK, cmake, ninja;
#                 the Docker image id) is compared first: "toolchain differs: ..." or "toolchain not recorded: ..." ends the
#                 run with exit code 2 (the binaries are built before they are compared; the report is the tool's).
#                 Exit code: 0 everything identical, 1 something differs with the same toolchain, 2 error or toolchain.
# Run it LAST, after any change to src/dc_determinism, so that the binaries come from the same source tree.
#
# Reproducible builds: the Windows binary is built from a copy of the sources (src/dc_determinism, the dependencies it uses and
# build.ps1) at ONE fixed absolute path (<system drive>\dc_determinism-pb\d; removed before and after, a second run at
# the same time is refused), with /Brepro for the compiler and the linker, so that it does not depend on where the
# repository is checked out. The Linux binary is built at /work inside the image. `-Test` runs CTest in a separate build
# below build/dc_determinism/pb.
#
# The `dc_determinism` section is written by the freshly built binary (`dc_determinism prebuild --tool-fragment`, one fragment per
# platform: src/dc_determinism, the dependencies it is built from (checked against src/vendor/manifest.json), the toolchain,
# and for each binary its path, sha256, compiler identity, compile/link flags and, for Linux, the Docker image and image
# id). Every other section of the manifest (schema, source, binaries, players) is left untouched. `git add` the result with the Linux binary marked executable:
#   git update-index --add --chmod=+x prebuilt/dc_determinism/linux-amd64/dc_determinism
param(
    [string]$Image = 'dc_determinism-cpu:local',
    [switch]$Test,
    [switch]$SkipWindows,
    [switch]$SkipLinux,
    [switch]$Check
)

$ErrorActionPreference = 'Stop'
# -Check ends with exit code 2 on any error (0: identical, 1: differs).
trap {
    if ($Check) {
        [Console]::Error.WriteLine("dc_determinism check failed: $($_.Exception.Message)")
        exit 2
    }
    throw $_
}
if ($SkipWindows -and $SkipLinux) { throw 'nothing to build: -SkipWindows and -SkipLinux exclude every platform' }
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$prebuildDir = Join-Path $root 'build\dc_determinism\pb'
$winRel = 'prebuilt/dc_determinism/windows-amd64/dc_determinism.exe'
# The fixed build path of the Windows binary (DC_DETERMINISM_PREBUILD_TOOL_SCRATCH: experiments only; another path may give other bytes).
$scratchRoot = $env:DC_DETERMINISM_PREBUILD_TOOL_SCRATCH
if (-not $scratchRoot) { $scratchRoot = Join-Path ([IO.Path]::GetPathRoot([Environment]::SystemDirectory)) 'dc_determinism-pb\d' }
$scratchMarker = '.dc_determinism-scratch'
$scratchInputs = @('src\dc_determinism', 'src\vendor\nlohmann-json', 'src\vendor\miniz', 'scripts\build.ps1')

function Get-FileSha256Hex([string]$path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
}

# Same scheme as dc_determinism::tree_sha256_lf: sorted POSIX relative paths,
# each contributing "<path>\0<sha256(LF-normalized content)>".
function Get-TreeSha256Lf([string]$directory) {
    $base = (Resolve-Path -LiteralPath $directory).Path.TrimEnd('\')
    $entries = @(Get-ChildItem -LiteralPath $base -Recurse -File -Force | ForEach-Object {
        [pscustomobject]@{ Rel = $_.FullName.Substring($base.Length + 1).Replace('\', '/'); Full = $_.FullName }
    })
    $rels = [string[]]@($entries | ForEach-Object { $_.Rel })
    $fulls = [string[]]@($entries | ForEach-Object { $_.Full })
    [Array]::Sort($rels, $fulls, [System.StringComparer]::Ordinal)
    $latin1 = [System.Text.Encoding]::GetEncoding('iso-8859-1')
    $outer = [System.Security.Cryptography.IncrementalHash]::CreateHash([System.Security.Cryptography.HashAlgorithmName]::SHA256)
    $inner = [System.Security.Cryptography.SHA256]::Create()
    try {
        for ($i = 0; $i -lt $rels.Length; ++$i) {
            $bytes = [System.IO.File]::ReadAllBytes($fulls[$i])
            $content = $latin1.GetBytes(($latin1.GetString($bytes) -replace "`r`n", "`n"))
            $outer.AppendData([System.Text.Encoding]::UTF8.GetBytes($rels[$i]))
            $outer.AppendData([byte[]]@(0))
            $outer.AppendData($inner.ComputeHash($content))
        }
        return ([BitConverter]::ToString($outer.GetHashAndReset()) -replace '-', '').ToLowerInvariant()
    } finally { $outer.Dispose(); $inner.Dispose() }
}

function Invoke-Native {
    param([string]$What, [scriptblock]$Block)
    $output = & $Block
    if ($LASTEXITCODE -ne 0) { throw "$What failed with exit code $LASTEXITCODE" }
    return $output
}

function Write-Utf8NoBom([string]$path, [string]$text) {
    [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($false)))
}

# Flags of a ninja build without paths: the compile flags of main.cpp and the link flags.
function Get-NinjaFlags([string]$buildDir, [string]$compileMarker, [string]$linkMarker, [string]$linkerPattern) {
    $commands = @(Invoke-Native 'ninja -t commands' { & ninja -C $buildDir -t commands dc_determinism })
    $compileLine = $commands | Where-Object { $_ -like "*$compileMarker*" } | Select-Object -First 1
    $linkLine = $commands | Where-Object { $_ -like "*$linkMarker*" } | Select-Object -Last 1
    if (-not $compileLine -or -not $linkLine) { throw "cannot find the dc_determinism compile/link command in $buildDir" }
    $compile = @()
    foreach ($token in ($compileLine -split '\s+' | Select-Object -Skip 1)) {
        if ($token -match '^(/|-)(nologo|TP|D[A-Za-z_]|std:|MT|MTd|W[0-4]|utf-8|permissive-|EHsc|O[0-9a-z]|external:W|Zc:|Zi|FS|Brepro)' -and
            $token -notmatch '^(/|-)(Fo|Fd|showIncludes|external:I|I)') {
            $compile += $token
        }
    }
    $link = @()
    foreach ($token in ($linkLine -split '\s+')) {
        if ($token -match $linkerPattern) { $link += $token }
    }
    return [pscustomobject]@{ Compile = $compile | Select-Object -Unique; Link = $link | Select-Object -Unique }
}

# ---------------------------------------------------------------- fixed scratch path
$script:scratchLock = $null

# Takes the lock of the scratch path (a second run at the same time is refused), empties the path and copies the
# build inputs below it at their repository-relative paths.
function Enter-Scratch {
    if ([IO.Path]::GetPathRoot($scratchRoot).TrimEnd('\', '/') -eq $scratchRoot.TrimEnd('\', '/')) {
        throw "the scratch path must not be a drive root: $scratchRoot"
    }
    $lockPath = "$scratchRoot.lock"
    # The lock file sits next to the directory, in a parent a user may create (not in the root of the system drive).
    New-Item -ItemType Directory -Force (Split-Path $scratchRoot) | Out-Null
    try {
        $script:scratchLock = [System.IO.File]::Open($lockPath, [System.IO.FileMode]::OpenOrCreate,
            [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    } catch {
        throw "another run is using $scratchRoot (cannot lock $lockPath): $($_.Exception.Message)"
    }
    # Only a directory this script made (it holds the marker) or an empty one is emptied; anything else is somebody else's data.
    if (Test-Path -LiteralPath $scratchRoot) {
        $marker = Join-Path $scratchRoot $scratchMarker
        $isEmpty = -not (Get-ChildItem -LiteralPath $scratchRoot -Force | Select-Object -First 1)
        if (-not $isEmpty -and -not (Test-Path -LiteralPath $marker)) {
            throw "refusing to use $scratchRoot as the scratch directory: it is not empty and was not created by this script (no $scratchMarker file); nothing was deleted"
        }
        Remove-Item -LiteralPath $scratchRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Force $scratchRoot | Out-Null
    New-Item -ItemType File -Force (Join-Path $scratchRoot $scratchMarker) | Out-Null
    foreach ($relative in $scratchInputs) {
        $destination = Join-Path $scratchRoot $relative
        New-Item -ItemType Directory -Force (Split-Path $destination) | Out-Null
        Copy-Item -LiteralPath (Join-Path $root $relative) -Destination $destination -Recurse -Force
    }
}

function Exit-Scratch {
    if (Test-Path -LiteralPath (Join-Path $scratchRoot $scratchMarker)) {
        try { Remove-Item -LiteralPath $scratchRoot -Recurse -Force } catch { Write-Warning "cannot remove ${scratchRoot}: $($_.Exception.Message)" }
    }
    if ($script:scratchLock) {
        $script:scratchLock.Dispose()
        $script:scratchLock = $null
        Remove-Item -LiteralPath "$scratchRoot.lock" -Force -ErrorAction SilentlyContinue
    }
}

# The MSVC toolchain versions (build-local.ps1 `facts`): what can change the bytes of a Windows build.
function Get-WindowsFacts {
    $script = Join-Path $PSScriptRoot 'build-local.ps1'
    $line = @(& powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $script -Action probe -Kind loader -Compiler msvc) |
        Where-Object { $_.Trim() } | Select-Object -Last 1
    $probe = $line | ConvertFrom-Json
    if (-not $probe.ok) { throw "the MSVC toolchain probe failed: $($probe.error)" }
    $facts = [ordered]@{}
    foreach ($property in $probe.facts.PSObject.Properties) { $facts[$property.Name] = $property.Value }
    return $facts
}

# ---------------------------------------------------------------- Docker preflight (the Linux platform)
# Checked before anything is built or written, so that a missing image cannot leave a half-updated prebuilt/ tree. The image must
# be an amd64 one (nothing is emulated). Returns the image id.
function Test-LinuxImage([string]$dockerImage) {
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { throw 'docker was not found on PATH' }
    & docker image inspect $dockerImage *> $null
    if ($LASTEXITCODE -ne 0) { throw "Docker image '$dockerImage' not found; build it with .\dc_determinism.ps1 image build --tag $dockerImage" }
    $imageArchitecture = (@(Invoke-Native 'docker image inspect' { & docker image inspect --format '{{.Architecture}}' $dockerImage })[0]).Trim()
    if ($imageArchitecture -ne 'amd64') {
        throw "Docker image '$dockerImage' is $imageArchitecture, but the linux-amd64 build needs an amd64 image (nothing is emulated); build one with .\dc_determinism.ps1 image build"
    }
    return (@(Invoke-Native 'docker image inspect' { & docker image inspect --format '{{.Id}}' $dockerImage })[0]).Trim()
}

New-Item -ItemType Directory -Force $prebuildDir | Out-Null
$fragments = @()
$toolBinary = $null   # the binary that merges the fragments: the freshly built Windows one

$imageId = $null
if (-not $SkipLinux) { $imageId = Test-LinuxImage $Image }

# ---------------------------------------------------------------- Windows (MSVC /MT, /Brepro, fixed path)
if (-not $SkipWindows) {
    $windowsFacts = Get-WindowsFacts
    $sourceTree = Get-TreeSha256Lf (Join-Path $root 'src\dc_determinism')
    if ($Test -and -not $Check) {
        # CTest runs in the checkout (it needs the unit tests and the repository); the shipped binary comes from the scratch build.
        $testArgs = @{ BuildDir = (Join-Path $prebuildDir 'win-test'); Clean = $true; Generator = 'Ninja'; Test = $true }
        & (Join-Path $PSScriptRoot 'build.ps1') @testArgs
    }
    try {
        Enter-Scratch
        if ((Get-TreeSha256Lf (Join-Path $scratchRoot 'src\dc_determinism')) -ne $sourceTree) { throw 'the copy of src/dc_determinism differs from the checkout' }
        $winBuild = Join-Path $scratchRoot 'build'
        $buildArgs = @{ BuildDir = $winBuild; Clean = $true; Generator = 'Ninja'; NoTests = $true }
        & (Join-Path $scratchRoot 'scripts\build.ps1') @buildArgs
        $built = Join-Path $winBuild 'dc_determinism.exe'
        if (-not (Test-Path -LiteralPath $built)) { throw "build did not produce $built" }
        # The banner goes to stderr; capture it through cmd.exe.
        $bannerFile = Join-Path $prebuildDir 'cl-version.txt'
        & cmd.exe /d /c "cl.exe > `"$bannerFile`" 2>&1"
        $banner = ([IO.File]::ReadAllLines($bannerFile) | Where-Object { $_.Trim() } | Select-Object -First 1).Trim()
        if ($banner -notlike 'Microsoft (R) C/C++ Optimizing Compiler*') { throw "unexpected cl.exe banner: $banner" }
        $flags = Get-NinjaFlags $winBuild 'main.cpp.obj' 'dc_determinism.exe' '^/(nologo|machine:|INCREMENTAL:|subsystem:|version:|Brepro)'
        # Kept outside the scratch path, which is removed below: the tool installs it (or compares it) from there.
        $windowsDir = Join-Path $prebuildDir 'windows-amd64'
        New-Item -ItemType Directory -Force $windowsDir | Out-Null
        $toolBinary = Join-Path $windowsDir 'dc_determinism.exe'
        Copy-Item -LiteralPath $built -Destination $toolBinary -Force
        $null = Invoke-Native 'windows dc_determinism --version' { & $toolBinary --version }
        $sha = Get-FileSha256Hex $toolBinary
        $fragment = [ordered]@{
            platform = 'windows-amd64'
            path = $winRel
            sha256 = $sha
            compiler = $banner
            flags = @($flags.Compile)
            link_flags = @($flags.Link)
            toolchain = $windowsFacts
            built = 'build/dc_determinism/pb/windows-amd64/dc_determinism.exe'
        }
        $fragmentPath = Join-Path $windowsDir 'dc_determinism-windows-amd64.json'
        Write-Utf8NoBom $fragmentPath (($fragment | ConvertTo-Json -Depth 20) + "`n")
        $fragments += $fragmentPath
        Write-Host "windows-amd64: $sha"
    } finally {
        Exit-Scratch
    }
}

# ---------------------------------------------------------------- Linux (g++ static, Docker, amd64 image)
if (-not $SkipLinux) {
    $linuxScript = @('scripts/prebuild.sh', '--in-image', '--image', $Image, '--image-id', $imageId)
    if ($Test -and -not $Check) { $linuxScript += '--test' }
    $dockerArgs = @('run', '--rm', '--platform', 'linux/amd64', '-u', '1000:1000', '-e', 'HOME=/tmp', '-v', "${root}:/work", '-w', '/work')
    # The tests read the corpus; Docker cannot follow a junction or symlink inside a bind mount.
    $logsItem = Get-Item -LiteralPath (Join-Path $root 'logs') -Force -ErrorAction SilentlyContinue
    if ($logsItem -and $logsItem.LinkType) {
        $logsTarget = [string](@($logsItem.Target)[0])
        if (-not [IO.Path]::IsPathRooted($logsTarget)) { $logsTarget = [IO.Path]::GetFullPath((Join-Path $root $logsTarget)) }
        $dockerArgs += @('-v', "${logsTarget}:/work/logs:ro")
    }
    & docker @dockerArgs $Image bash @linuxScript
    if ($LASTEXITCODE -ne 0) { throw "linux-amd64 prebuild.sh failed with exit code $LASTEXITCODE" }
    $linuxFragment = Join-Path $prebuildDir 'linux-amd64\dc_determinism-linux-amd64.json'
    $fragmentObject = Get-Content -LiteralPath $linuxFragment -Raw | ConvertFrom-Json
    $stripped = Join-Path $prebuildDir 'linux-amd64\dc_determinism.stripped'
    if ((Get-FileSha256Hex $stripped) -ne $fragmentObject.sha256) { throw 'the rebuilt linux-amd64 binary differs from the build fragment' }
    $fragments += $linuxFragment
    Write-Host "linux-amd64:   $($fragmentObject.sha256)"
}

# ---------------------------------------------------------------- the manifest: written (or checked) by the tool
if (-not $toolBinary) {
    # -SkipWindows: the binary of the installed Windows build runs the merge (the Linux binary cannot run here).
    $toolBinary = Join-Path $root ($winRel -replace '/', '\')
    if (-not (Test-Path -LiteralPath $toolBinary)) { throw "$winRel is not installed: it runs the merge of the fragments; build it first (no -SkipWindows)" }
}
$toolArguments = @('prebuild')
foreach ($path in $fragments) { $toolArguments += @('--tool-fragment', $path) }
if ($Check) { $toolArguments += '--check' }
$priorRepo = $env:DC_DETERMINISM_REPO
try {
    $env:DC_DETERMINISM_REPO = $root
    & $toolBinary @toolArguments
    $code = $LASTEXITCODE
} finally {
    $env:DC_DETERMINISM_REPO = $priorRepo
}
if ($code -ne 0) { exit $code }
if (-not $Check) {
    Write-Host 'Mark the Linux binary executable in Git: git update-index --add --chmod=+x prebuilt/dc_determinism/linux-amd64/dc_determinism'
}
exit 0
