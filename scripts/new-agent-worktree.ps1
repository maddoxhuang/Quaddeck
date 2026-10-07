[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("codex", "claude")]
    [string]$Agent,

    [Parameter(Mandatory = $true)]
    [ValidatePattern("^[a-z0-9][a-z0-9_-]*$")]
    [string]$Task,

    [string]$Base = "master",
    [string]$DestinationRoot
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
if (-not $DestinationRoot) {
    $DestinationRoot = Split-Path -Parent $ProjectRoot
}

$Branch = "$Agent/$Task"
$ProjectName = Split-Path -Leaf $ProjectRoot
$Destination = Join-Path $DestinationRoot "$ProjectName-$Agent-$Task"

if (Test-Path -LiteralPath $Destination) {
    throw "Destination already exists: $Destination"
}

git -C $ProjectRoot rev-parse --verify "$Base^{commit}" | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "Base commit or branch does not exist: $Base"
}

git -C $ProjectRoot show-ref --verify --quiet "refs/heads/$Branch"
if ($LASTEXITCODE -eq 0) {
    throw "Branch already exists: $Branch"
}
if ($LASTEXITCODE -ne 1) {
    throw "Could not inspect existing branches"
}

if (-not $PSCmdlet.ShouldProcess(
        "$Destination (branch $Branch from $Base)", "Create Git worktree")) {
    return
}

git -C $ProjectRoot worktree add -b $Branch $Destination $Base
if ($LASTEXITCODE -ne 0) {
    throw "Could not create worktree for $Branch"
}

Write-Host "Created worktree: $Destination"
Write-Host "Created branch:   $Branch"
Write-Host "Open that directory in $Agent and keep master as the integration branch."
