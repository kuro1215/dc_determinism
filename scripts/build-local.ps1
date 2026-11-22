# Toolchain part of the local build recipe library (src/dc_determinism/localbuild) on Windows: the compiler discovery
# and tool invocation that `dc_determinism prebuild` uses on a Windows host. No `dc_determinism` command builds locally
# apart from `prebuild`. The C++ tool owns everything else: vendored-source identity, cache decisions, receipts,
# and the exact command lines (they are part of the cache identity).
#
# Two actions; both are driven by `dc_determinism`, not meant for interactive use:
#
#   build-local.ps1 -Action probe -Kind loader|player -Compiler msvc|gcc|clang|wasm
#       Locates the toolchain (vswhere + vcvars64.bat for msvc;
#       MSYS2/MinGW g++ or clang++ on PATH or in C:/msys64/{ucrt64,clang64,mingw64}/bin; em++ and
#       node for wasm; for -Kind loader also cmake >= 3.26 and the generator: Visual Studio for
#       msvc, Ninja for MinGW). Prints ONE line of JSON on stdout (ASCII only):
#         {"ok":true,"id":..,"cxx":..,"cc":..,"version":..,"target":..,"bin_dir":..,
#          "generator":..|null,"generator_args":[..],"cmake":..,"cmake_version":..,
#          "node":..,"node_version":..,"facts":{..}}  (cmake* only for loader, node* only for wasm)
#       `facts` (msvc, gcc, clang) are the toolchain versions `dc_determinism prebuild` records in its manifests and compares in
#       `dc_determinism prebuild --check`: the compiler line and the ninja version (loader: also cmake); msvc: the toolset
#       directory, the link.exe banner line and the Windows SDK/UCRT versions; MinGW: `ld --version`, the MSYS2
#       package versions (`pacman -Q`) and the hashes of the runtime DLLs that the build copies next to the loader.
#         {"ok":false,"error":"<message of the DcDeterminismError>"}
#       The exit code is 0 in both cases.
#
#   build-local.ps1 -Action run -Kind loader|player -Compiler C [-Cwd DIR] -CommandFile FILE
#       Runs one command in the toolchain environment (vcvars64.bat environment for msvc, the compiler
#       directory (and Ninja) first on PATH for MinGW). FILE is UTF-8, one argument per line, the
#       program first. The command inherits stdout and stderr; the exit code of the script is the exit
#       code of the command. A failure of the script itself is reported on stderr as
#         DC-DETERMINISM-BUILD-ERROR: <message>            (exit code 125)
#         DC-DETERMINISM-BUILD-START-FAILED: <message>     (the command could not be started; exit code 125)
#
# Nothing here touches the network or the repository.
param(
    [Parameter(Mandatory = $true)][ValidateSet('probe', 'run')][string]$Action,
    [Parameter(Mandatory = $true)][ValidateSet('loader', 'player')][string]$Kind,
    [Parameter(Mandatory = $true)][string]$Compiler,
    [string]$Cwd = '',
    [string]$CommandFile = ''
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$ProbeTimeout = 60
$CmakeMinimum = @(3, 26)
$Msys2BinDirs = @('C:\msys64\ucrt64\bin', 'C:\msys64\clang64\bin', 'C:\msys64\mingw64\bin')
$Compilers = @('msvc', 'gcc', 'clang')

try {
    $ansi = [System.Text.Encoding]::GetEncoding([System.Globalization.CultureInfo]::CurrentCulture.TextInfo.ANSICodePage)
} catch {
    $ansi = [System.Text.Encoding]::UTF8
}

# ---------------------------------------------------------------------------------------------------
# JSON (ASCII only, so that no console code page can damage it)
# ---------------------------------------------------------------------------------------------------

function ConvertTo-JsonString([string]$Text) {
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    foreach ($char in $Text.ToCharArray()) {
        $code = [int]$char
        if ($char -eq '"') { [void]$builder.Append('\"') }
        elseif ($char -eq '\') { [void]$builder.Append('\\') }
        elseif ($code -eq 10) { [void]$builder.Append('\n') }
        elseif ($code -eq 13) { [void]$builder.Append('\r') }
        elseif ($code -eq 9) { [void]$builder.Append('\t') }
        elseif ($code -lt 32 -or $code -gt 126) { [void]$builder.Append(('\u{0:x4}' -f $code)) }
        else { [void]$builder.Append($char) }
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function ConvertTo-JsonValue($Value) {
    if ($null -eq $Value) { return 'null' }
    if ($Value -is [bool]) { if ($Value) { return 'true' } else { return 'false' } }
    if ($Value -is [string]) { return (ConvertTo-JsonString $Value) }
    if ($Value -is [System.Collections.IDictionary]) {
        $items = @()
        foreach ($key in $Value.Keys) {
            $items += ((ConvertTo-JsonString ([string]$key)) + ':' + (ConvertTo-JsonValue $Value[$key]))
        }
        return '{' + ($items -join ',') + '}'
    }
    if ($Value -is [System.Collections.IEnumerable]) {
        $items = @()
        foreach ($item in $Value) { $items += (ConvertTo-JsonValue $item) }
        return '[' + ($items -join ',') + ']'
    }
    return (ConvertTo-JsonString ([string]$Value))
}

function Write-Result($Value) {
    [Console]::Out.WriteLine((ConvertTo-JsonValue $Value))
    [Console]::Out.Flush()
}

# ---------------------------------------------------------------------------------------------------
# Processes
# ---------------------------------------------------------------------------------------------------

function ConvertTo-CommandLineArgument([string]$Argument) {
    if ($Argument.Length -gt 0 -and $Argument -notmatch '[\s"]') { return $Argument }
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    $backslashes = 0
    foreach ($char in $Argument.ToCharArray()) {
        if ($char -eq '\') {
            $backslashes++
        } elseif ($char -eq '"') {
            [void]$builder.Append('\', $backslashes * 2 + 1)
            [void]$builder.Append('"')
            $backslashes = 0
        } else {
            if ($backslashes -gt 0) { [void]$builder.Append('\', $backslashes) }
            [void]$builder.Append($char)
            $backslashes = 0
        }
    }
    if ($backslashes -gt 0) { [void]$builder.Append('\', $backslashes * 2) }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Join-CommandLine([string[]]$Arguments) {
    $parts = @()
    foreach ($argument in $Arguments) { $parts += (ConvertTo-CommandLineArgument $argument) }
    return ($parts -join ' ')
}

# subprocess.run(capture_output=True, text=True, errors="replace", timeout=...): @{ Started; Error;
# TimedOut; ExitCode; Out; Err }
function Invoke-Captured([string]$File, [string]$Arguments, [int]$TimeoutSeconds) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $File
    $info.Arguments = $Arguments
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = $ansi
    $info.StandardErrorEncoding = $ansi
    $result = @{ Started = $false; Error = ''; TimedOut = $false; ExitCode = -1; Out = ''; Err = '' }
    try {
        $process = [System.Diagnostics.Process]::Start($info)
    } catch {
        $result.Error = $_.Exception.Message
        return $result
    }
    $result.Started = $true
    $outTask = $process.StandardOutput.ReadToEndAsync()
    $errTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $result.TimedOut = $true
        try { $process.Kill() } catch { }
        try { $process.WaitForExit() } catch { }
    } else {
        $process.WaitForExit()
        $result.ExitCode = $process.ExitCode
    }
    $result.Out = $outTask.Result
    $result.Err = $errTask.Result
    $process.Dispose()
    return $result
}

function Get-FirstLine([string]$Text) {
    foreach ($line in ($Text -split "\r\n|\r|\n")) {
        if ($line.Trim().Length -gt 0) { return $line.Trim() }
    }
    return ''
}

# run_command: the stdout of a tool; a failure is a DcDeterminismError with the tail of the output.
function Invoke-Tool([string]$What, [string]$File, [string[]]$Arguments, [int]$TimeoutSeconds) {
    $result = Invoke-Captured $File (Join-CommandLine $Arguments) $TimeoutSeconds
    if (-not $result.Started) { throw "cannot run ${What}: $($result.Error)" }
    if ($result.TimedOut) { throw "$What timed out after $TimeoutSeconds seconds" }
    if ($result.ExitCode -ne 0) {
        $lines = @(($result.Out + $result.Err).Trim() -split "\r\n|\r|\n" | Where-Object { $true })
        if ($lines.Count -gt 40) { $lines = $lines[($lines.Count - 40)..($lines.Count - 1)] }
        throw ("$What failed (exit code $($result.ExitCode)):`n" + ($lines -join "`n"))
    }
    return $result.Out
}

# _probe: the output of a tool that must succeed, otherwise ''.
function Invoke-Probe([string]$File, [string[]]$Arguments) {
    $result = Invoke-Captured $File (Join-CommandLine $Arguments) $ProbeTimeout
    if (-not $result.Started -or $result.TimedOut -or $result.ExitCode -ne 0) { return '' }
    return $result.Out + $result.Err
}

# ---------------------------------------------------------------------------------------------------
# Tool discovery (shutil.which semantics on Windows: PATHEXT spelling is kept)
# ---------------------------------------------------------------------------------------------------

function Normalize-Path([string]$Path) {
    $text = $Path -replace '/', '\'
    while ($text.Length -gt 3 -and $text.EndsWith('\')) { $text = $text.Substring(0, $text.Length - 1) }
    return $text
}

function Find-Tool([string]$Name, [string[]]$ExtraDirs = @(), [string]$SearchPath = $null) {
    if ($null -eq $SearchPath -or $SearchPath -eq '') { $SearchPath = $env:PATH }
    $pathext = $env:PATHEXT
    if (-not $pathext) { $pathext = '.COM;.EXE;.BAT;.CMD' }
    $extensions = @($pathext -split ';' | Where-Object { $_ })
    $hasExtension = $false
    foreach ($extension in $extensions) {
        if ($Name.ToLowerInvariant().EndsWith($extension.ToLowerInvariant())) { $hasExtension = $true }
    }
    if ($hasExtension) { $files = @($Name) } else { $files = @($extensions | ForEach-Object { $Name + $_ }) }
    $seen = @{}
    foreach ($directory in ($SearchPath -split ';')) {
        if (-not $directory) { continue }
        $key = $directory.ToLowerInvariant()
        if ($seen.ContainsKey($key)) { continue }
        $seen[$key] = $true
        foreach ($file in $files) {
            $candidate = if ($directory.EndsWith('\') -or $directory.EndsWith('/')) { $directory + $file } else { "$directory\$file" }
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { return (Normalize-Path $candidate) }
        }
    }
    foreach ($directory in $ExtraDirs) {
        $candidate = Join-Path $directory ($Name + '.exe')
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return (Normalize-Path $candidate) }
    }
    return $null
}

function Get-ParentDirectory([string]$Path) {
    $index = $Path.LastIndexOf('\')
    if ($index -le 0) { return '' }
    return $Path.Substring(0, $index)
}

function Get-MissingError([string]$Tool, [string]$Hint) {
    return "required tool not found: $Tool. $Hint"
}

# ---------------------------------------------------------------------------------------------------
# Toolchains
# ---------------------------------------------------------------------------------------------------

function Get-Vswhere {
    $programFiles = ${env:ProgramFiles(x86)}
    if (-not $programFiles) { throw 'MSVC is only available on Windows (ProgramFiles(x86) is not set)' }
    $vswhere = "$programFiles\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw (Get-MissingError 'vswhere.exe' "Install Visual Studio with the C++ x64 build tools (expected $vswhere).")
    }
    return $vswhere
}

function Get-VswhereProperty([string]$Name) {
    $output = Invoke-Tool 'vswhere' (Get-Vswhere) @('-latest', '-products', '*', '-requires',
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', $Name) $ProbeTimeout
    return $output.Trim()
}

# The output of `cmd /c "vcvars64.bat" && set` (msvc_environment).
function Get-MsvcEnvironmentText {
    $installation = Get-VswhereProperty 'installationPath'
    $vcvars = ''
    if ($installation) { $vcvars = Normalize-Path "$installation\VC\Auxiliary\Build\vcvars64.bat" }
    if (-not $vcvars -or -not (Test-Path -LiteralPath $vcvars -PathType Leaf)) {
        throw (Get-MissingError 'Visual Studio C++ x64 build tools (vcvars64.bat)' "Install the 'Desktop development with C++' workload of Visual Studio.")
    }
    # cmd.exe /d /s /c ""<vcvars>" >nul && set"
    $commandLine = '/d /s /c ""' + $vcvars + '" >nul && set"'
    $result = Invoke-Captured 'cmd.exe' $commandLine ($ProbeTimeout * 2)
    if (-not $result.Started) { throw "cannot run vcvars64.bat: $($result.Error)" }
    if ($result.TimedOut) { throw "vcvars64.bat timed out after $($ProbeTimeout * 2) seconds" }
    if ($result.ExitCode -ne 0) {
        $lines = @(($result.Out + $result.Err).Trim() -split "\r\n|\r|\n" | Where-Object { $true })
        if ($lines.Count -gt 40) { $lines = $lines[($lines.Count - 40)..($lines.Count - 1)] }
        throw ("vcvars64.bat failed (exit code $($result.ExitCode)):`n" + ($lines -join "`n"))
    }
    return $result.Out
}

# Imports the environment of vcvars64.bat into this process; PATH = vcvars PATH + original PATH.
function Set-MsvcEnvironment {
    $text = Get-MsvcEnvironmentText
    $original = $env:PATH
    foreach ($line in ($text -split "\r\n|\r|\n")) {
        $index = $line.IndexOf('=')
        if ($index -gt 0) {
            [Environment]::SetEnvironmentVariable($line.Substring(0, $index).ToUpperInvariant(), $line.Substring($index + 1), 'Process')
        }
    }
    $env:PATH = $env:PATH + ';' + $original
}

function Get-MsvcGenerator([string]$InstallationVersion) {
    $major = [int]($InstallationVersion.Split('.')[0])
    if ($major -eq 18) { return 'Visual Studio 18 2026' }
    if ($major -eq 17) { return 'Visual Studio 17 2022' }
    throw "unsupported Visual Studio version: $InstallationVersion"
}

function Get-MsvcToolchain {
    Set-MsvcEnvironment
    $cl = Find-Tool 'cl' @() $env:PATH
    if (-not $cl) { throw (Get-MissingError 'cl.exe' 'It is not available in the Visual Studio x64 environment (vcvars64.bat).') }
    $banner = Invoke-Captured $cl '' $ProbeTimeout
    if (-not $banner.Started) { throw "cannot run cl.exe: $($banner.Error)" }
    if ($banner.TimedOut) { throw "cannot run cl.exe: timed out after $ProbeTimeout seconds" }
    $version = Get-FirstLine ($banner.Err + $banner.Out)
    $generator = Get-MsvcGenerator (Get-VswhereProperty 'installationVersion')
    return @{ id = 'msvc'; cxx = $cl; cc = $cl; version = $version; target = 'x86_64-pc-windows-msvc';
             bin_dir = (Get-ParentDirectory $cl); generator = $generator; generator_args = @('-A', 'x64');
             facts = (Get-MsvcFacts $cl $version) }
}

# (.NET instead of Get-FileHash: the cmdlet is not found when the script runs without a console, as dc_determinism starts it.)
function Get-FileSha256([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash([System.IO.File]::ReadAllBytes($Path))) -replace '-', '').ToLowerInvariant()
    } finally { $sha.Dispose() }
}

# `ninja --version` of the Ninja the build would use ('' without one).
function Get-NinjaVersion([string[]]$ExtraDirs = @()) {
    $ninja = Find-Tool 'ninja' $ExtraDirs $env:PATH
    if (-not $ninja) { return '' }
    return Get-FirstLine (Invoke-Probe $ninja @('--version'))
}

# The toolchain versions of an MSVC build (see the header): everything that can change the bytes.
function Get-MsvcFacts([string]$Cl, [string]$Banner) {
    $binDir = Get-ParentDirectory $Cl
    $toolset = ''
    $match = [regex]::Match($Cl, '(?i)[\\/]MSVC[\\/]([^\\/]+)[\\/]')
    if ($match.Success) { $toolset = $match.Groups[1].Value }
    $linkVersion = ''
    $link = Normalize-Path "$binDir\link.exe"
    if (Test-Path -LiteralPath $link -PathType Leaf) {
        # link.exe without input prints its banner and fails: the exit code does not matter here.
        $linkRun = Invoke-Captured $link '' $ProbeTimeout
        if ($linkRun.Started -and -not $linkRun.TimedOut) { $linkVersion = Get-FirstLine ($linkRun.Out + $linkRun.Err) }
    }
    $sdk = ([string]$env:WindowsSDKVersion).Trim('\', '/')
    $ucrt = ([string]$env:UCRTVersion).Trim('\', '/')
    return [ordered]@{ cl = $Banner; toolset_dir = $toolset; link = $linkVersion; windows_sdk = $sdk; ucrt = $ucrt;
                       ninja = (Get-NinjaVersion) }
}

# The toolchain versions of a MinGW build (see the header). The MSYS2 packages are those that provide the compiler,
# the linker, the C runtime, winpthreads and the GCC/LLVM runtime libraries of the environment `$BinDir` belongs to.
function Get-MingwFacts([string]$Banner, [string]$BinDir) {
    $facts = [ordered]@{ compiler = $Banner }
    $ld = Normalize-Path "$BinDir\ld.exe"
    $facts['ld'] = if (Test-Path -LiteralPath $ld -PathType Leaf) { Get-FirstLine (Invoke-Probe $ld @('--version')) } else { '' }
    $environment = Split-Path $BinDir -Parent
    $msys = Split-Path $environment -Parent
    $prefix = switch ((Split-Path $environment -Leaf).ToLowerInvariant()) {
        'ucrt64' { 'mingw-w64-ucrt-x86_64-' }
        'clang64' { 'mingw-w64-clang-x86_64-' }
        'mingw64' { 'mingw-w64-x86_64-' }
        default { '' }
    }
    $pacman = Normalize-Path "$msys\usr\bin\pacman.exe"
    $packages = [ordered]@{}
    if ($prefix -and (Test-Path -LiteralPath $pacman -PathType Leaf)) {
        $names = 'gcc|gcc-libs|libgcc|libstdc\+\+|binutils|crt|crt-git|headers|headers-git|winpthreads|winpthreads-git|libwinpthread|libwinpthread-git|clang|clang-libs|lld|libc\+\+|libunwind|compiler-rt'
        $pattern = '^' + [regex]::Escape($prefix) + '(' + $names + ') (\S+)$'
        $lines = @((Invoke-Probe $pacman @('-Q')) -split "\r\n|\r|\n")
        foreach ($line in ($lines | Sort-Object)) {
            $m = [regex]::Match($line.Trim(), $pattern)
            if ($m.Success) { $packages[$prefix + $m.Groups[1].Value] = $m.Groups[2].Value }
        }
    }
    $facts['msys2_packages'] = $packages
    $dlls = [ordered]@{}
    foreach ($name in @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll', 'libc++.dll', 'libunwind.dll')) {
        $path = Normalize-Path "$BinDir\$name"
        if (Test-Path -LiteralPath $path -PathType Leaf) { $dlls[$name] = Get-FileSha256 $path }
    }
    $facts['runtime_dlls'] = $dlls
    $facts['ninja'] = Get-NinjaVersion @($BinDir)
    return $facts
}

function Get-UnixLikeToolchain([string]$Id) {
    if ($Id -eq 'gcc') { $cxxName = 'g++'; $ccName = 'gcc'; $package = 'gcc' } else { $cxxName = 'clang++'; $ccName = 'clang'; $package = 'clang' }
    $hint = "Install MSYS2 and its UCRT64 toolchain (https://www.msys2.org; pacman -S mingw-w64-ucrt-x86_64-$package); expected C:/msys64/ucrt64/bin or the tool on PATH."
    $candidates = @()
    $onPath = Find-Tool $cxxName
    if ($onPath) { $candidates += $onPath }
    foreach ($directory in $Msys2BinDirs) {
        $candidate = Normalize-Path "$directory\$cxxName.exe"
        if ((Test-Path -LiteralPath $candidate -PathType Leaf) -and -not ($candidates | Where-Object { $_ -ieq $candidate })) {
            $candidates += $candidate
        }
    }
    if ($candidates.Count -eq 0) { throw (Get-MissingError $cxxName $hint) }
    $reasons = @()
    foreach ($cxx in $candidates) {
        $target = (Invoke-Probe $cxx @('-dumpmachine')).Trim()
        $version = Get-FirstLine (Invoke-Probe $cxx @('--version'))
        $lowered = $target.ToLowerInvariant()
        $acceptable = ($lowered.Contains('windows') -or $lowered.Contains('mingw')) -and ($lowered.Contains('x86_64') -or $lowered.Contains('amd64'))
        if (-not ($target -and $version -and $acceptable)) {
            if ($target) { $shown = $target } else { $shown = 'unknown' }
            $reasons += "${cxx}: target $shown is not Windows x64"
            continue
        }
        $parent = Get-ParentDirectory $cxx
        $cc = Find-Tool $ccName @($parent) ($parent + ';' + $env:PATH)
        if (-not $cc) { throw (Get-MissingError $ccName "It is expected next to $cxx. $hint") }
        $env:PATH = $parent + ';' + $env:PATH
        return @{ id = $Id; cxx = $cxx; cc = $cc; version = $version; target = $target; bin_dir = $parent;
                 generator = $null; generator_args = @(); facts = (Get-MingwFacts $version $parent) }
    }
    throw ("no usable $Id compiler for this host: " + ($reasons -join '; '))
}

function Get-Toolchain([string]$Id) {
    if ($Compilers -notcontains $Id) {
        throw "unknown compiler for a local build: $Id (choose from $($Compilers -join ', '))"
    }
    if ($Id -eq 'msvc') { return (Get-MsvcToolchain) }
    return (Get-UnixLikeToolchain $Id)
}

function Get-WasmToolchain {
    $em = Find-Tool 'em++'
    if (-not $em) {
        throw (Get-MissingError 'em++' 'Install the Emscripten SDK (https://emscripten.org) and activate it so that em++ is on PATH (Debian/Ubuntu: apt install emscripten).')
    }
    $node = Find-Tool 'node'
    if (-not $node) {
        throw (Get-MissingError 'node' 'Install Node.js and put it on PATH (the wasm player runs with `node worker.js`).')
    }
    $version = Get-FirstLine (Invoke-Tool 'em++ --version' $em @('--version') $ProbeTimeout)
    $nodeVersion = Get-FirstLine (Invoke-Tool 'node --version' $node @('--version') $ProbeTimeout)
    return @{ id = 'wasm'; cxx = $em; cc = $em; version = $version; target = 'wasm32-emscripten';
             bin_dir = (Get-ParentDirectory $em); generator = $null; generator_args = @();
             node = $node; node_version = $nodeVersion }
}

# find_cmake: cmake on the toolchain PATH (plus the default install location), version >= 3.26.
function Get-Cmake {
    $cmake = Find-Tool 'cmake' @('C:\Program Files\CMake\bin') $env:PATH
    if (-not $cmake) {
        throw (Get-MissingError 'cmake' "Install CMake $($CmakeMinimum[0]).$($CmakeMinimum[1]) or newer and put it on PATH (Windows: winget install Kitware.CMake; Linux: apt install cmake).")
    }
    $version = Get-FirstLine (Invoke-Tool 'cmake --version' $cmake @('--version') $ProbeTimeout)
    $match = [regex]::Match($version, '(\d+)\.(\d+)')
    $tooOld = $true
    if ($match.Success) {
        $major = [int]$match.Groups[1].Value
        $minor = [int]$match.Groups[2].Value
        $tooOld = ($major -lt $CmakeMinimum[0]) -or ($major -eq $CmakeMinimum[0] -and $minor -lt $CmakeMinimum[1])
    }
    if ($tooOld) { throw "cmake $($CmakeMinimum[0]).$($CmakeMinimum[1]) or newer is required, found: $version" }
    return @{ path = $cmake; version = $version }
}

# select_generator: Visual Studio for msvc, Ninja for MinGW (its directory goes first on PATH).
function Select-Generator($Toolchain) {
    if ($Toolchain.generator) { return @{ name = $Toolchain.generator; args = $Toolchain.generator_args } }
    $ninja = Find-Tool 'ninja' @($Toolchain.bin_dir) $env:PATH
    if (-not $ninja) {
        throw (Get-MissingError 'ninja' 'Install Ninja and put it on PATH (winget install Ninja-build.Ninja, or pacman -S mingw-w64-ucrt-x86_64-ninja).')
    }
    $env:PATH = (Get-ParentDirectory $ninja) + ';' + $env:PATH
    return @{ name = 'Ninja'; args = @() }
}

# Sets up the environment of the toolchain for `Kind`; returns the toolchain description.
function Initialize-Toolchain {
    if ($Compiler -eq 'wasm') {
        if ($Kind -ne 'player') { throw "unknown compiler for a local build: wasm (choose from $($Compilers -join ', '))" }
        return @{ toolchain = (Get-WasmToolchain) }
    }
    $toolchain = Get-Toolchain $Compiler
    if ($Kind -eq 'loader') {
        $cmake = Get-Cmake
        $generator = Select-Generator $toolchain
        return @{ toolchain = $toolchain; cmake = $cmake; generator = $generator }
    }
    return @{ toolchain = $toolchain }
}

# ---------------------------------------------------------------------------------------------------
# Actions
# ---------------------------------------------------------------------------------------------------

function Invoke-ProbeAction {
    try {
        $setup = Initialize-Toolchain
        $toolchain = $setup.toolchain
        $result = [ordered]@{
            ok = $true; id = $toolchain.id; cxx = $toolchain.cxx; cc = $toolchain.cc; version = $toolchain.version;
            target = $toolchain.target; bin_dir = $toolchain.bin_dir
        }
        if ($Kind -eq 'loader') {
            $result['generator'] = $setup.generator.name
            $result['generator_args'] = @($setup.generator.args)
            $result['cmake'] = $setup.cmake.path
            $result['cmake_version'] = $setup.cmake.version
        } else {
            $result['generator'] = $null
            $result['generator_args'] = @()
        }
        if ($toolchain.ContainsKey('facts')) {
            $facts = [ordered]@{}
            foreach ($key in $toolchain.facts.Keys) { $facts[$key] = $toolchain.facts[$key] }
            if ($Kind -eq 'loader') { $facts['cmake'] = $setup.cmake.version }
            $result['facts'] = $facts
        }
        if ($toolchain.ContainsKey('node')) {
            $result['node'] = $toolchain.node
            $result['node_version'] = $toolchain.node_version
        }
        Write-Result $result
    } catch {
        Write-Result ([ordered]@{ ok = $false; error = $_.Exception.Message })
    }
    exit 0
}

function Invoke-RunAction {
    try {
        if (-not $CommandFile) { throw 'the run action needs -CommandFile' }
        $lines = [System.IO.File]::ReadAllLines($CommandFile, [System.Text.Encoding]::UTF8)
        if ($lines.Count -lt 1 -or -not $lines[0]) { throw "the command file is empty: $CommandFile" }
        [void](Initialize-Toolchain)
    } catch {
        [Console]::Error.WriteLine('DC-DETERMINISM-BUILD-ERROR: ' + $_.Exception.Message)
        exit 125
    }
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $lines[0]
    $info.Arguments = Join-CommandLine @($lines | Select-Object -Skip 1)
    $info.UseShellExecute = $false
    if ($Cwd) { $info.WorkingDirectory = $Cwd }
    try {
        $process = [System.Diagnostics.Process]::Start($info)
    } catch {
        [Console]::Error.WriteLine('DC-DETERMINISM-BUILD-START-FAILED: ' + $_.Exception.Message)
        exit 125
    }
    $process.WaitForExit()
    exit $process.ExitCode
}

if ($Action -eq 'probe') { Invoke-ProbeAction } else { Invoke-RunAction }
