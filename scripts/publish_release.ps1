[CmdletBinding()]
param(
    [string]$ReleaseDir,
    [string]$Repo = "chenjuncheng/listening-wind-fxsound-engine",
    [string]$Tag,
    [string]$Title,
    [string]$Notes,
    [switch]$Draft,
    [switch]$Prerelease,
    [switch]$NoPush,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

function Resolve-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    return $item.FullName
}

function Assert-Command {
    param([Parameter(Mandatory = $true)][string]$Name)

    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command not found: $Name"
    }
}

function Assert-File {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file missing: $Path"
    }
}

function Assert-Dir {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "Required directory missing: $Path"
    }
}

function Remove-GeneratedDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$AllowedRoot
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $fullRoot = [System.IO.Path]::GetFullPath($AllowedRoot)
    if (-not $fullPath.StartsWith($fullRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove path outside generated output root: $fullPath"
    }

    if (Test-Path -LiteralPath $fullPath) {
        Remove-Item -LiteralPath $fullPath -Recurse -Force
    }
}

Assert-Command git
Assert-Command gh

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $scriptDir ".."))
$outRoot = Join-Path $repoRoot "out"

if (-not $ReleaseDir) {
    $ReleaseDir = Join-Path $repoRoot "..\release"
}

$releaseRoot = Resolve-FullPath $ReleaseDir
Assert-Dir $releaseRoot

Push-Location $repoRoot
try {
    $commit = (& git rev-parse --short HEAD).Trim()
    $branch = (& git rev-parse --abbrev-ref HEAD).Trim()
    if (-not $Tag) {
        $date = Get-Date -Format "yyyy.MM.dd"
        $Tag = "v$date-$commit"
    }
    if (-not $Title) {
        $Title = "Listening Wind Release $Tag"
    }
    if (-not $Notes) {
        $Notes = "Windows release package with app, fxsound_engine, FAC presets, FxSound virtual audio driver, and helper scripts. Source commit: $commit."
    }

    Assert-File (Join-Path $releaseRoot "listening_wind_app.exe")
    Assert-File (Join-Path $releaseRoot "engine\fxsound_engine.exe")
    Assert-File (Join-Path $releaseRoot "engine\drivers\win10\x64\fxdevcon64.exe")
    Assert-File (Join-Path $releaseRoot "engine\drivers\win10\x64\fxvad.inf")
    Assert-File (Join-Path $releaseRoot "engine\drivers\win10\x64\fxvad.sys")
    Assert-File (Join-Path $releaseRoot "engine\drivers\win10\x64\fxvadntamd64.cat")
    Assert-File (Join-Path $releaseRoot "force_uninstall_fxsound_driver.bat")

    $facFiles = Get-ChildItem -LiteralPath (Join-Path $releaseRoot "fac") -Filter "*.fac" -File -ErrorAction Stop
    if ($facFiles.Count -lt 1) {
        throw "No FAC files found under release fac directory."
    }

    New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
    $stageRoot = Join-Path $outRoot "release-upload-$Tag"
    $stagePayload = Join-Path $stageRoot "listening_wind_release"
    $zipPath = Join-Path $outRoot "listening_wind_release_$Tag.zip"

    if ($DryRun) {
        Write-Host "[dry-run] Repo: $Repo"
        Write-Host "[dry-run] Branch: $branch"
        Write-Host "[dry-run] Commit: $commit"
        Write-Host "[dry-run] Tag: $Tag"
        Write-Host "[dry-run] ReleaseDir: $releaseRoot"
        Write-Host "[dry-run] Zip: $zipPath"
        Write-Host "[dry-run] FAC count: $($facFiles.Count)"
        exit 0
    }

    Remove-GeneratedDirectory -Path $stageRoot -AllowedRoot $outRoot
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }

    New-Item -ItemType Directory -Force -Path $stagePayload | Out-Null
    & robocopy $releaseRoot $stagePayload /E /XD crash_logs /XF *.log *.dmp | Out-Host
    if ($LASTEXITCODE -gt 7) {
        throw "robocopy failed with exit code $LASTEXITCODE"
    }

    Compress-Archive -Path $stagePayload -DestinationPath $zipPath -CompressionLevel Optimal
    Assert-File $zipPath

    if (-not $NoPush) {
        & git push origin $branch
    }

    $releaseExists = $false
    & gh release view $Tag --repo $Repo *> $null
    if ($LASTEXITCODE -eq 0) {
        $releaseExists = $true
    }

    if ($releaseExists) {
        & gh release upload $Tag $zipPath --repo $Repo --clobber
    } else {
        $args = @(
            "release", "create", $Tag, $zipPath,
            "--repo", $Repo,
            "--target", $branch,
            "--title", $Title,
            "--notes", $Notes
        )
        if ($Draft) { $args += "--draft" }
        if ($Prerelease) { $args += "--prerelease" }
        & gh @args
    }

    & gh release view $Tag --repo $Repo --json url,tagName,assets
} finally {
    Pop-Location
}
