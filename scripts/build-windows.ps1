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
Import-Module (Join-Path $PSScriptRoot 'ReleasePrivacy.psm1') -Force

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

function Move-Whole([string]$Source, [string]$Destination) {
    # One rename on the volume, which moves everything or nothing. Move-Item
    # is not used for this: for a folder, Windows PowerShell falls back to
    # copying and deleting, and a file held open then splits the folder
    # between the two names.
    if (Test-Path -LiteralPath $Source -PathType Container) {
        [System.IO.Directory]::Move($Source, $Destination)
    }
    else {
        [System.IO.File]::Move($Source, $Destination)
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

# vcpkg.json pins every port to builtin-baseline. A shallow clone holds only
# the commit it cloned, and vcpkg refuses a baseline it cannot read, so the
# baseline commit is fetched when it is missing; a fresh clone checks it out.
$VcpkgBaseline = (Get-Content -Raw -LiteralPath (Join-Path $ProjectRoot "vcpkg.json") |
    ConvertFrom-Json).'builtin-baseline'
if ($VcpkgBaseline -notmatch '^[0-9a-f]{40}$') {
    throw "vcpkg.json has no builtin-baseline commit."
}
$FreshVcpkg = -not (Test-Path (Join-Path $VcpkgRoot ".git"))
if ($FreshVcpkg) {
    git clone --depth 1 https://github.com/microsoft/vcpkg $VcpkgRoot
    Assert-NativeSuccess "vcpkg clone"
}
git -C $VcpkgRoot rev-parse --quiet --verify "$VcpkgBaseline^{commit}" | Out-Null
if ($LASTEXITCODE -ne 0) {
    git -C $VcpkgRoot fetch --depth 1 origin $VcpkgBaseline
    Assert-NativeSuccess "vcpkg baseline fetch"
}
if ($FreshVcpkg) {
    git -C $VcpkgRoot -c advice.detachedHead=false checkout $VcpkgBaseline
    Assert-NativeSuccess "vcpkg baseline checkout"
}

$VcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
if (-not (Test-Path $VcpkgExe)) {
    & (Join-Path $VcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics
    Assert-NativeSuccess "vcpkg bootstrap"
}

$Toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
cmake -S $ProjectRoot -B $BuildRoot -G "Visual Studio 17 2022" -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
    -DVCPKG_TARGET_TRIPLET=x64-windows
Assert-NativeSuccess "CMake configure"

Assert-QuadDeckTargetsIdle @((Join-Path $BuildRoot "Release\QuadDeck.exe"))
cmake --build $BuildRoot --config Release --parallel
Assert-NativeSuccess "CMake build"
New-Item -ItemType Directory -Force -Path $DistRoot | Out-Null
Assert-ChildPath $DistRoot $AppDist

# A run that stopped part-way can leave a staging folder, or the backup of
# the player it replaced (its PDB and log hold private paths), beside
# dist\QuadDeck. A backup with no player beside it is the player: it is put
# back, not removed.
$Leftovers = @(Get-ChildItem -LiteralPath $DistRoot -Force |
    Where-Object { $_.Name -like ".previous-*" -or $_.Name -like ".staging-*" })
if (-not (Test-Path -LiteralPath $AppDist)) {
    $Stranded = @($Leftovers | Where-Object { $_.PSIsContainer -and $_.Name -like ".previous-*" } |
        Sort-Object CreationTimeUtc -Descending)
    if ($Stranded.Count -gt 0) {
        Assert-ChildPath $DistRoot $Stranded[0].FullName
        Move-Whole $Stranded[0].FullName $AppDist
        Write-Host "Put the previous player back from $($Stranded[0].Name)."
        $Leftovers = @($Leftovers | Where-Object { $_.FullName -ne $Stranded[0].FullName })
    }
}
foreach ($Leftover in $Leftovers) {
    Assert-ChildPath $DistRoot $Leftover.FullName
    try {
        Remove-Item -LiteralPath $Leftover.FullName -Recurse -Force
    }
    catch {
        Write-Warning "Could not remove $($Leftover.FullName) ($($_.Exception.Message)); the next build tries again."
    }
}

$ZipPath = Join-Path $DistRoot "QuadDeck-win-x64.zip"
$StagingRoot = Join-Path $DistRoot (".staging-" + [guid]::NewGuid().ToString("N"))
$StagingApp = Join-Path $StagingRoot "QuadDeck"
$StagingZip = Join-Path $StagingRoot "QuadDeck-win-x64.zip"
Assert-ChildPath $DistRoot $StagingRoot
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

    # Only the staging copies are rewritten. Unknown private paths fail closed.
    Protect-QuadDeckRelease -StagingDirectory $StagingApp
    Compress-Archive -Path $StagingApp -DestinationPath $StagingZip -CompressionLevel Optimal
    Assert-QuadDeckReleasePrivacy -Archive $StagingZip

    # Beside the executable, not in the archive: the PDB lets a hang report in
    # QuadDeck.log name functions and lines, and it records this machine's
    # build paths.
    $Symbols = Join-Path $ReleaseRoot "QuadDeck.pdb"
    if (-not (Test-Path -LiteralPath $Symbols)) {
        throw "The Release build produced no QuadDeck.pdb."
    }
    Copy-Item -LiteralPath $Symbols $StagingApp -Force

    Assert-QuadDeckTargetsIdle @((Join-Path $AppDist "QuadDeck.exe"))
    # The player being replaced is renamed aside first. A file someone holds
    # open (a debugger on the PDB, a shell inside the folder) then stops the
    # build with the old player and its log untouched, where deleting it
    # file by file left half a player and lost the log.
    $Backup = $null
    if (Test-Path -LiteralPath $AppDist) {
        $Backup = Join-Path $DistRoot (".previous-" + [guid]::NewGuid().ToString("N"))
        Assert-ChildPath $DistRoot $Backup
        try {
            Move-Whole $AppDist $Backup
        }
        catch {
            throw ("Could not move $AppDist aside: $($_.Exception.GetBaseException().Message) " +
                "Close whatever has a file in it open and build again; the previous player and its log are untouched.")
        }
    }
    try {
        Move-Whole $StagingApp $AppDist
    }
    catch {
        $Reason = $_.Exception.GetBaseException().Message
        if ($Backup) {
            try {
                Move-Whole $Backup $AppDist
            }
            catch {
                throw ("Could not move the new build into $AppDist ($Reason), nor the previous player back: " +
                    "it is in $Backup, and the next build puts it back.")
            }
        }
        throw "Could not move the new build into $AppDist ($Reason); the previous player is as it was."
    }
    if ($Backup) {
        # The log of the build being replaced may hold the only record of a
        # problem not yet looked at; it moves into the new folder, still
        # outside the archive. QuadDeck.1.log is its older half.
        $KeepBackup = $false
        foreach ($LogName in @("QuadDeck.log", "QuadDeck.1.log")) {
            $PreviousLog = Join-Path $Backup $LogName
            if (Test-Path -LiteralPath $PreviousLog) {
                try {
                    Move-Whole $PreviousLog (Join-Path $AppDist $LogName)
                }
                catch {
                    $KeepBackup = $true
                    Write-Warning "The previous $LogName stays in $Backup; copy it out before the next build removes that folder."
                }
            }
        }
        if (-not $KeepBackup) {
            try {
                Remove-Item -LiteralPath $Backup -Recurse -Force
            }
            catch {
                Write-Warning "Could not remove all of $Backup ($($_.Exception.Message)); the next build tries again."
            }
        }
    }

    # The archive the same way: a held one stays whole.
    $ZipBackup = $null
    if (Test-Path -LiteralPath $ZipPath) {
        $ZipBackup = Join-Path $DistRoot (".previous-" + [guid]::NewGuid().ToString("N") + ".zip")
        Assert-ChildPath $DistRoot $ZipBackup
        try {
            Move-Whole $ZipPath $ZipBackup
        }
        catch {
            throw ("$AppDist is updated, but $ZipPath could not be replaced: " +
                "$($_.Exception.GetBaseException().Message) Close whatever has it open and build again.")
        }
    }
    try {
        Move-Whole $StagingZip $ZipPath
    }
    catch {
        if ($ZipBackup) { Move-Whole $ZipBackup $ZipPath }
        throw
    }
    if ($ZipBackup) {
        try {
            Remove-Item -LiteralPath $ZipBackup -Force
        }
        catch {
            Write-Warning "Could not remove $ZipBackup; the next build tries again."
        }
    }
}
finally {
    if (Test-Path -LiteralPath $StagingRoot) {
        Remove-Item -LiteralPath $StagingRoot -Recurse -Force
    }
}

Write-Host "Build complete: $AppDist\QuadDeck.exe"
Write-Host "Package: $ZipPath"
