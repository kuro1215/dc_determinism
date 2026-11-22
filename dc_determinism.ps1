#Requires -Version 7.0
# Thin wrapper: runs the prebuilt Linux binary prebuilt/dc_determinism/linux-<arch>/dc_determinism inside a Linux
# Docker container from a Windows (or any PowerShell 7) host, so the Linux worker and loaders are
# used. <arch> is the architecture of the selected image (amd64 -> linux-amd64, arm64 -> linux-arm64;
# any other image architecture exits 2) and the container runs with `--platform linux/<arch>`: on an
# x86-64 PC `DC_DETERMINISM_IMAGE` / `--image` may name an arm64 image to run the linux-arm64 binary under
# emulation. Every command, the pybind ones (`compare pybind`, `capture simulator --engine pybind-*`) included,
# runs the C++ dc_determinism in that image; the pybind commands start the Python worker src/worker/pybind/pybind_worker.py
# there and need an amd64 image. PowerShell on macOS is not supported: use dc_determinism.sh there.
#
# Usage: .\dc_determinism.ps1 [--image NAME] <command> [options]
#   - Same commands and options as `dc_determinism --help`.
#   - Image: --image NAME, else $env:DC_DETERMINISM_IMAGE, else dc_determinism-cpu:local (pybind work included: compare pybind,
#     capture simulator --engine pybind-*; there is no other image and no fallback:
#     a missing dc_determinism-cpu:local is an error that names `image build`). Every image of another architecture
#     is refused unless --image or $env:DC_DETERMINISM_IMAGE names it explicitly (on an arm64 host, and on an x86-64
#     host for a non-explicit arm64 image).
#   - `image build [--tag NAME] [--platform linux/amd64|linux/arm64]` builds the Docker image of this repository from
#     the Dockerfile on stdin, so no build context is sent to the daemon (-> dc_determinism-cpu:local, or dc_determinism-cpu:local-arm64
#     for --platform linux/arm64). It needs no image, so --image / $env:DC_DETERMINISM_IMAGE do not apply. The Docker build
#     downloads packages (apt); nothing else does.
#   - `prebuild` is the exception: it builds for the host it runs on and drives the Docker CLI, which a container cannot
#     do, so it runs the prebuilt binary of the host directly (Windows: prebuilt\dc_determinism\windows-amd64\dc_determinism.exe;
#     PowerShell on Linux: prebuilt/dc_determinism/linux-<arch>/dc_determinism for the architecture of the host; macOS:
#     use dc_determinism.sh) and no path is translated. A `--image NAME` given before `prebuild` is forwarded as
#     `prebuild --image NAME` ($env:DC_DETERMINISM_IMAGE is not forwarded; giving --image on both sides exits 2). See
#     `dc_determinism prebuild --help` and the README.
#   - The repository is mounted at /w/dc_determinism and, when the current directory is
#     outside it, the current directory at /data. Path arguments are translated; paths outside
#     both are rejected.
$ErrorActionPreference = 'Stop'
if ($IsMacOS) {
    [Console]::Error.WriteLine('dc_determinism.ps1: PowerShell on macOS is not supported; use dc_determinism.sh')
    exit 2
}
$Repo = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\', '/')
$Caller = [IO.Path]::GetFullPath((Get-Location).ProviderPath).TrimEnd('\', '/')
$RepoInContainer = '/w/dc_determinism'
$CallerInContainer = '/data'
$PathOptions = @(
    '--logs-dir', '--prepared', '--output', '--shots', '--expect', '--trajectory-shots',
    '--official-py27', '--official-py36', '--record-py27', '--record-py36'
)

function Test-InDirectory([string]$Path, [string]$Directory) {
    $sep = [IO.Path]::DirectorySeparatorChar
    return $Path.Equals($Directory, [StringComparison]::OrdinalIgnoreCase) -or
        $Path.StartsWith($Directory + $sep, [StringComparison]::OrdinalIgnoreCase)
}

function Convert-PathArgument([string]$Value) {
    $full = if ([IO.Path]::IsPathRooted($Value)) { [IO.Path]::GetFullPath($Value) }
            else { [IO.Path]::GetFullPath((Join-Path $Caller $Value)) }
    $full = $full.TrimEnd('\', '/')
    foreach ($pair in @(@($Repo, $RepoInContainer), @($Caller, $CallerInContainer))) {
        if (Test-InDirectory $full $pair[0]) {
            $relative = $full.Substring($pair[0].Length).TrimStart('\', '/').Replace('\', '/')
            if ($relative) { return "$($pair[1])/$relative" }
            return $pair[1]
        }
    }
    throw "dc_determinism.ps1: path is outside the repository and the current directory: $Value"
}

function Test-ImagePresent([string]$Name) {
    docker image inspect $Name *> $null
    return $LASTEXITCODE -eq 0
}

$Forward = [Collections.Generic.List[string]]::new()
foreach ($argument in $args) { $Forward.Add([string]$argument) }
$Image = $env:DC_DETERMINISM_IMAGE
$Explicit = [bool]$Image
$CliImage = $null
if ($Forward.Count -ge 2 -and $Forward[0] -eq '--image') {
    $Image = $Forward[1]
    $CliImage = $Forward[1]
    $Explicit = $true
    $Forward.RemoveRange(0, 2)
}
if ($Forward.Count -eq 0) {
    [Console]::Error.WriteLine('usage: dc_determinism.ps1 [--image NAME] <command> [options]  (see dc_determinism --help)')
    exit 2
}
if ($Forward[0] -eq 'image') {
    # `image build [--tag NAME] [--platform linux/amd64|linux/arm64]`: builds the Docker image of this repository from
    # the Dockerfile on stdin (no build context). Handled here, before the image selection: no image is needed, and
    # --image / $env:DC_DETERMINISM_IMAGE do not apply.
    $ImageUsage = 'usage: dc_determinism.ps1 image build [--tag NAME] [--platform linux/amd64|linux/arm64]'
    if ($null -ne $CliImage) {
        [Console]::Error.WriteLine('dc_determinism.ps1: --image does not apply to `image build` (use --tag NAME)')
        exit 2
    }
    if ($Forward.Count -lt 2 -or $Forward[1] -ne 'build') {
        [Console]::Error.WriteLine($ImageUsage)
        exit 2
    }
    $Tag = $null
    $BuildPlatform = $null
    for ($i = 2; $i -lt $Forward.Count; $i++) {
        $name, $inline = if ($Forward[$i] -match '^(--[^=]+)=(.*)$') { $Matches[1], $Matches[2] } else { $Forward[$i], $null }
        if ($name -notin @('--tag', '--platform')) {
            [Console]::Error.WriteLine("dc_determinism.ps1: unknown argument: $($Forward[$i])`n$ImageUsage")
            exit 2
        }
        if ($null -ne $inline) {
            $value = $inline
        } elseif ($i + 1 -lt $Forward.Count) {
            $i++
            $value = $Forward[$i]
        } else {
            [Console]::Error.WriteLine("dc_determinism.ps1: $name needs a value")
            exit 2
        }
        switch ($name) {
            '--tag' { $Tag = $value }
            '--platform' { $BuildPlatform = $value }
        }
    }
    if ($Tag -eq '' -or $BuildPlatform -eq '') {
        [Console]::Error.WriteLine('dc_determinism.ps1: an option of `image build` has an empty value')
        exit 2
    }
    if ($null -ne $BuildPlatform -and $BuildPlatform -notin @('linux/amd64', 'linux/arm64')) {
        [Console]::Error.WriteLine("dc_determinism.ps1: --platform must be linux/amd64 or linux/arm64, not $BuildPlatform")
        exit 2
    }
    $DockerfilePath = Join-Path $Repo 'Dockerfile'
    if (-not (Test-Path -LiteralPath $DockerfilePath)) {
        [Console]::Error.WriteLine("dc_determinism.ps1: Dockerfile not found in $Repo")
        exit 2
    }
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
        [Console]::Error.WriteLine('dc_determinism.ps1: docker was not found on PATH')
        exit 2
    }
    $PlatformArguments = if ($null -ne $BuildPlatform) { @('--platform', $BuildPlatform) } else { @() }
    # dc_determinism-cpu:local-arm64 is the default tag of an image built for linux/arm64.
    if (-not $Tag) { $Tag = if ($BuildPlatform -eq 'linux/arm64') { 'dc_determinism-cpu:local-arm64' } else { 'dc_determinism-cpu:local' } }
    # The Dockerfile goes in on stdin (LF line endings, no build context is sent to the daemon).
    (Get-Content -LiteralPath $DockerfilePath -Raw) | & docker build -t $Tag @PlatformArguments -
    if ($LASTEXITCODE -ne 0) {
        [Console]::Error.WriteLine("dc_determinism.ps1: docker build failed (exit $LASTEXITCODE)")
        exit $LASTEXITCODE
    }
    $Id = ([string](@(& docker image inspect --format '{{.Id}}' $Tag)[0])).Trim()
    Write-Host "image:    $Tag"
    Write-Host "image id: $Id"
    exit 0
}
if ($Forward[0] -eq 'prebuild') {
    if ($IsWindows) {
        $HostBinary = 'prebuilt\dc_determinism\windows-amd64\dc_determinism.exe'
        $BuildHint = 'scripts\prebuild.ps1'
    } else {
        $PrebuildArchitecture = switch ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()) {
            'X64' { 'amd64' }
            'Arm64' { 'arm64' }
            default { $null }
        }
        if (-not $PrebuildArchitecture) {
            [Console]::Error.WriteLine('dc_determinism.ps1: prebuild supports x86-64 and aarch64 hosts only')
            exit 2
        }
        $HostBinary = "prebuilt/dc_determinism/linux-$PrebuildArchitecture/dc_determinism"
        $BuildHint = 'scripts/prebuild.sh'
    }
    $HostBinaryPath = Join-Path $Repo $HostBinary
    if (-not (Test-Path -LiteralPath $HostBinaryPath)) {
        [Console]::Error.WriteLine("dc_determinism.ps1: $HostBinary not found (build it with $BuildHint)")
        exit 2
    }
    if ($null -ne $CliImage) {
        if (@($Forward | Where-Object { $_ -eq '--image' -or $_ -like '--image=*' }).Count -gt 0) {
            [Console]::Error.WriteLine('dc_determinism.ps1: --image was given both before prebuild and after it; give it once')
            exit 2
        }
        $Forward.InsertRange(1, [string[]]@('--image', $CliImage))
    }
    $PriorRepo = $env:DC_DETERMINISM_REPO
    try {
        if (-not $env:DC_DETERMINISM_REPO) { $env:DC_DETERMINISM_REPO = $Repo }
        & $HostBinaryPath @Forward
        exit $LASTEXITCODE
    } finally {
        $env:DC_DETERMINISM_REPO = $PriorRepo
    }
}
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    [Console]::Error.WriteLine('dc_determinism.ps1: docker was not found on PATH')
    exit 2
}

$NeedsPybind = $Forward.Count -ge 2 -and $Forward[0] -eq 'compare' -and $Forward[1] -eq 'pybind'
if ($Forward.Count -ge 2 -and $Forward[0] -eq 'capture' -and $Forward[1] -eq 'simulator') {
    for ($i = 0; $i -lt $Forward.Count; $i++) {
        if ($Forward[$i] -like '--engine=pybind-*' -or
            ($Forward[$i] -eq '--engine' -and $i + 1 -lt $Forward.Count -and $Forward[$i + 1] -like 'pybind-*')) {
            $NeedsPybind = $true
        }
    }
}
# The architecture of this host (the OS, not the process: an x64 pwsh on Windows on Arm reports Arm64).
$HostArchitecture = switch ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()) {
    'X64' { 'amd64' }
    'Arm64' { 'arm64' }
    default { 'unknown' }
}
if (-not $Image) {
    $Image = 'dc_determinism-cpu:local'
}
if (-not (Test-ImagePresent $Image)) {
    [Console]::Error.WriteLine("dc_determinism.ps1: Docker image '$Image' not found; build it with .\dc_determinism.ps1 image build (dc_determinism-cpu:local)")
    exit 2
}

# The image decides the binary: its architecture is the Docker platform and the prebuilt directory.
$ImageArchitecture = ([string](@(& docker image inspect --format '{{.Architecture}}' $Image)[0])).Trim()
$Platform = switch ($ImageArchitecture) {
    'amd64' { 'linux-amd64' }
    'arm64' { 'linux-arm64' }
    default {
        [Console]::Error.WriteLine("dc_determinism.ps1: Docker image '$Image' has the architecture '$ImageArchitecture'; only amd64 and arm64 images are supported")
        exit 2
    }
}
if ($NeedsPybind -and $ImageArchitecture -ne 'amd64') {
    [Console]::Error.WriteLine("dc_determinism.ps1: the pybind commands need an amd64 image (the official pybind extensions are x86-64 only), but '$Image' is $ImageArchitecture")
    exit 2
}
if (-not $Explicit -and $HostArchitecture -in @('amd64', 'arm64') -and $HostArchitecture -ne $ImageArchitecture) {
    [Console]::Error.WriteLine("dc_determinism.ps1: Docker image '$Image' is $ImageArchitecture but this host is $HostArchitecture; pass --image $Image (or set DC_DETERMINISM_IMAGE) to run it anyway (under emulation)")
    exit 2
}

# Translate path arguments: values of known path options, and the two positional files of
# `compare simulator|player A B`.
$Converted = [Collections.Generic.List[string]]::new()
$Positional = ($Forward.Count -ge 2 -and $Forward[0] -eq 'compare' -and $Forward[1] -in @('simulator', 'player'))
try {
    for ($i = 0; $i -lt $Forward.Count; $i++) {
        $value = $Forward[$i]
        $name, $inline = if ($value -match '^(--[^=]+)=(.*)$') { $Matches[1], $Matches[2] } else { $value, $null }
        if ($name -in $PathOptions) {
            if ($null -ne $inline) { $Converted.Add("$name=$(Convert-PathArgument $inline)"); continue }
            $Converted.Add($value)
            if ($i + 1 -lt $Forward.Count) { $i++; $Converted.Add((Convert-PathArgument $Forward[$i])) }
            continue
        }
        if ($Positional -and $i -ge 2 -and -not $value.StartsWith('-') -and
            ($i -eq 0 -or $Forward[$i - 1] -notin @('--output', '--workers'))) {
            $Converted.Add((Convert-PathArgument $value)); continue
        }
        $Converted.Add($value)
    }
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 2
}

$CppBinary = "prebuilt/dc_determinism/$Platform/dc_determinism"
if (-not (Test-Path -LiteralPath (Join-Path $Repo $CppBinary.Replace('/', '\')))) {
    [Console]::Error.WriteLine("dc_determinism.ps1: $CppBinary not found (build it with scripts\prebuild.ps1)")
    exit 2
}

$DockerArgs = @('run', '--rm', '-i', '--platform', "linux/$ImageArchitecture", '-e', 'PYTHONDONTWRITEBYTECODE=1',
                '-v', "${Repo}:$RepoInContainer")
$LogsPath = Join-Path $Repo 'logs'
if (Test-Path -LiteralPath $LogsPath) {
    $logsItem = Get-Item -LiteralPath $LogsPath -Force
    if ($logsItem.LinkType) {
        # Docker cannot follow a junction or symbolic link inside a bind mount: mount the target.
        # (Item.Target works on every PowerShell 7 release; a relative target is resolved
        # against the link's directory, since docker -v needs an absolute host path.)
        $logsTarget = [string](@($logsItem.Target)[0])
        if (-not [IO.Path]::IsPathRooted($logsTarget)) {
            $logsTarget = [IO.Path]::GetFullPath((Join-Path $Repo $logsTarget))
        }
        $DockerArgs += @('-v', "${logsTarget}:$RepoInContainer/logs:ro")
    }
}
if (Test-InDirectory $Caller $Repo) {
    $relative = $Caller.Substring($Repo.Length).TrimStart('\', '/').Replace('\', '/')
    $Workdir = if ($relative) { "$RepoInContainer/$relative" } else { $RepoInContainer }
} else {
    $DockerArgs += @('-v', "${Caller}:$CallerInContainer")
    $Workdir = $CallerInContainer
}
$DockerArgs += @('-w', $Workdir, '-e', "DC_DETERMINISM_REPO=$RepoInContainer", '--entrypoint', "$RepoInContainer/$CppBinary",
                 $Image) + $Converted
& docker @DockerArgs
exit $LASTEXITCODE
