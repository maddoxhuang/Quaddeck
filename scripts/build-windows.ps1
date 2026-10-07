[CmdletBinding()]
param(
    [string]$VcpkgRoot = "$env:USERPROFILE\vcpkg",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$BuildRoot = Join-Path $ProjectRoot "build\windows"
$DistRoot = Join-Path $ProjectRoot "dist"
$AppDist = Join-Path $DistRoot "QuadDeck"

function Assert-NativeSuccess([string]$Step) {
    if ($LASTEXITCODE -ne 0) {
        throw "$Step failed with exit code $LASTEXITCODE. Build stopped."
    }
}

function Assert-ChildPath([string]$Parent, [string]$Child) {
    $ParentFull = [System.IO.Path]::GetFullPath($Parent).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    $ChildFull = [System.IO.Path]::GetFullPath($Child)
    $Prefix = $ParentFull + [System.IO.Path]::DirectorySeparatorChar
    if (-not $ChildFull.StartsWith(
            $Prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside ${ParentFull}: $ChildFull"
    }
}

function Assert-QuadDeckTargetsIdle([string[]]$Targets) {
    # Fail closed if Windows cannot identify a running copy. Never stop a
    # user's player or replace files beneath its loaded executable.
    $ResolvedTargets = @($Targets | ForEach-Object { [System.IO.Path]::GetFullPath($_) })
    $RunningCopies = @(Get-CimInstance Win32_Process -Filter "Name = 'QuadDeck.exe'" -ErrorAction Stop)
    foreach ($Copy in $RunningCopies) {
        if ([string]::IsNullOrWhiteSpace($Copy.ExecutablePath)) {
            throw "Cannot identify a running QuadDeck executable. Build/package replacement stopped."
        }
        $Executable = [System.IO.Path]::GetFullPath($Copy.ExecutablePath)
        foreach ($Target in $ResolvedTargets) {
            if ([string]::Equals($Executable, $Target, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "QuadDeck is running from a build/package target. Close it normally before rebuilding that target."
            }
        }
    }
}

$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $VsWhere)) {
    throw "Visual Studio Installer was not found. Install Visual Studio Build Tools 2022 with the Desktop development with C++ workload."
}
$VsInstall = & $VsWhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
Assert-NativeSuccess "Visual Studio detection"
if (-not $VsInstall) {
    throw "MSVC v143 x64/x86 tools are missing. Open Visual Studio Installer, modify Build Tools 2022, and select 'Desktop development with C++', a Windows 11 SDK, and C++ CMake tools."
}

if ($Clean) {
    Assert-ChildPath $ProjectRoot $BuildRoot
    Assert-QuadDeckTargetsIdle @((Join-Path $BuildRoot "Release\QuadDeck.exe"), (Join-Path $BuildRoot "Debug\QuadDeck.exe"))
    if (Test-Path -LiteralPath $BuildRoot) { Remove-Item -LiteralPath $BuildRoot -Recurse -Force }
}

if (-not (Test-Path (Join-Path $VcpkgRoot ".git"))) {
    git clone --depth 1 https://github.com/microsoft/vcpkg $VcpkgRoot
    Assert-NativeSuccess "vcpkg clone"
}

$VcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
if (-not (Test-Path $VcpkgExe)) {
    & (Join-Path $VcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics
    Assert-NativeSuccess "vcpkg bootstrap"
}

$Toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
cmake -S $ProjectRoot -B $BuildRoot -G "Visual Studio 17 2022" -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
    -DVCPKG_TARGET_TRIPLET=x64-windows `
    -DQUADDECK_BUILD_TESTS=ON
Assert-NativeSuccess "CMake configure"

Assert-QuadDeckTargetsIdle @((Join-Path $BuildRoot "Release\QuadDeck.exe"))
cmake --build $BuildRoot --config Release --parallel
Assert-NativeSuccess "CMake build"
ctest --test-dir $BuildRoot -C Release --output-on-failure
Assert-NativeSuccess "CTest"

New-Item -ItemType Directory -Force -Path $DistRoot | Out-Null
$ZipPath = Join-Path $DistRoot "QuadDeck-win-x64.zip"
$StagingRoot = Join-Path $DistRoot (".staging-" + [guid]::NewGuid().ToString("N"))
$StagingApp = Join-Path $StagingRoot "QuadDeck"
$StagingZip = Join-Path $StagingRoot "QuadDeck-win-x64.zip"
Assert-ChildPath $DistRoot $StagingRoot
Assert-ChildPath $DistRoot $AppDist
Assert-ChildPath $DistRoot $ZipPath

try {
    New-Item -ItemType Directory -Force -Path $StagingApp | Out-Null
    $ReleaseRoot = Join-Path $BuildRoot "Release"
    Copy-Item (Join-Path $ReleaseRoot "QuadDeck.exe") $StagingApp -Force
    Copy-Item (Join-Path $ProjectRoot "README.md") $StagingApp -Force
    Copy-Item (Join-Path $ProjectRoot "CHANGELOG.md") $StagingApp -Force
    Copy-Item (Join-Path $ProjectRoot "LICENSE") $StagingApp -Force
    if (Test-Path (Join-Path $ProjectRoot "shaders")) {
        Copy-Item (Join-Path $ProjectRoot "shaders") $StagingApp -Recurse -Force
    }

    # The DLLs below are LGPL, ISC, MIT and FreeType-licensed; their notices
    # travel with them, one file per vcpkg port.
    $Licenses = Join-Path $StagingApp "licenses"
    New-Item -ItemType Directory -Force -Path $Licenses | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $BuildRoot "vcpkg_installed\x64-windows\share") -Directory |
        ForEach-Object {
            $Copyright = Join-Path $_.FullName "copyright"
            if (Test-Path -LiteralPath $Copyright -PathType Leaf) {
                Copy-Item -LiteralPath $Copyright -Destination (Join-Path $Licenses ($_.Name + ".txt")) -Force
            }
        }

    # vcpkg's app-local deployment has already copied exactly the DLLs that
    # QuadDeck needs beside the Release executable. Copying the entire vcpkg
    # bin directory also shipped unrelated libraries. An allow-list staging
    # directory prevents a previous run's QuadDeck.log (which may contain
    # private media paths) from entering a later archive.
    Get-ChildItem -LiteralPath $ReleaseRoot -Filter "*.dll" -File |
        Copy-Item -Destination $StagingApp -Force
    if (Get-ChildItem -LiteralPath $StagingApp -Filter "*.log" -File -Recurse) {
        throw "Diagnostic logs must never be included in a release package."
    }

    Compress-Archive -Path $StagingApp -DestinationPath $StagingZip -CompressionLevel Optimal

    # Beside the executable, not in the archive: the PDB lets a hang report in
    # QuadDeck.log name functions and lines, and it records this machine's
    # build paths.
    $Symbols = Join-Path $ReleaseRoot "QuadDeck.pdb"
    if (-not (Test-Path -LiteralPath $Symbols)) {
        throw "The Release build produced no QuadDeck.pdb."
    }
    Copy-Item -LiteralPath $Symbols $StagingApp -Force

    Assert-QuadDeckTargetsIdle @((Join-Path $AppDist "QuadDeck.exe"))
    if (Test-Path -LiteralPath $AppDist) {
        # The log of the build being replaced may hold the only record of a
        # problem not yet looked at; it stays with the player, still outside
        # the archive.
        $PreviousLog = Join-Path $AppDist "QuadDeck.log"
        if (Test-Path -LiteralPath $PreviousLog) {
            Copy-Item -LiteralPath $PreviousLog $StagingApp -Force
        }
        Remove-Item -LiteralPath $AppDist -Recurse -Force
    }
    Move-Item -LiteralPath $StagingApp -Destination $AppDist
    if (Test-Path -LiteralPath $ZipPath) {
        Remove-Item -LiteralPath $ZipPath -Force
    }
    Move-Item -LiteralPath $StagingZip -Destination $ZipPath
}
finally {
    if (Test-Path -LiteralPath $StagingRoot) {
        Remove-Item -LiteralPath $StagingRoot -Recurse -Force
    }
}

Write-Host "Build complete: $AppDist\QuadDeck.exe"
Write-Host "Package: $ZipPath"
