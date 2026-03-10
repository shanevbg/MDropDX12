# release.ps1 — Build and package MDropDX12 portable release
# Usage: powershell -ExecutionPolicy Bypass -File release.ps1
#
# Produces: MDropDX12-v{VERSION}-Portable.zip containing:
#   MDropDX12.exe
#   README.md
#   LICENSE

param(
    [string]$Config = "Release",
    [string]$Platform = "x64",
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Read version from version.h
$versionFile = Join-Path $PSScriptRoot "src\mDropDX12\version.h"
$versionContent = Get-Content $versionFile -Raw
if ($versionContent -match '#define\s+MDROP_VERSION_STR\s+"([^"]+)"') {
    $version = $Matches[1]
} else {
    Write-Error "Could not read version from version.h"
    exit 1
}

Write-Host "=== MDropDX12 v$version Release ===" -ForegroundColor Cyan

# Build
if (-not $SkipBuild) {
    Write-Host "Building $Config $Platform..." -ForegroundColor Yellow
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build.ps1") $Config $Platform
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed"
        exit 1
    }
}

# Verify exe exists
$exeSrc = Join-Path $PSScriptRoot "src\mDropDX12\${Config}_${Platform}\MDropDX12.exe"
if (-not (Test-Path $exeSrc)) {
    Write-Error "Build output not found: $exeSrc"
    exit 1
}

# Create staging directory
$stagingDir = Join-Path $PSScriptRoot "_release_staging"
if (Test-Path $stagingDir) {
    Remove-Item $stagingDir -Recurse -Force
}
New-Item -ItemType Directory -Path $stagingDir | Out-Null

# Copy release files
Write-Host "Staging release files..." -ForegroundColor Yellow
Copy-Item $exeSrc (Join-Path $stagingDir "MDropDX12.exe")
Copy-Item (Join-Path $PSScriptRoot "README.md") (Join-Path $stagingDir "README.md")
Copy-Item (Join-Path $PSScriptRoot "THIRD-PARTY-LICENSES.txt") (Join-Path $stagingDir "THIRD-PARTY-LICENSES.txt")
Copy-Item (Join-Path $PSScriptRoot "LICENSE") (Join-Path $stagingDir "LICENSE")

# Also update the Release/ working directory exe
$releaseExe = Join-Path $PSScriptRoot "Release\MDropDX12.exe"
if (Test-Path (Split-Path $releaseExe)) {
    Copy-Item $exeSrc $releaseExe
    Write-Host "Updated Release/MDropDX12.exe" -ForegroundColor DarkGray
}

# Create zip
$zipName = "MDropDX12-v$version-Portable.zip"
$zipPath = Join-Path $PSScriptRoot $zipName
if (Test-Path $zipPath) {
    Remove-Item $zipPath -Force
}

Write-Host "Creating $zipName..." -ForegroundColor Yellow
Compress-Archive -Path (Join-Path $stagingDir "*") -DestinationPath $zipPath

# Clean up staging
Remove-Item $stagingDir -Recurse -Force

# Report
$zipSize = (Get-Item $zipPath).Length
$zipSizeMB = [math]::Round($zipSize / 1MB, 2)
Write-Host ""
Write-Host "=== Release package created ===" -ForegroundColor Green
Write-Host "  File: $zipName"
Write-Host "  Size: $zipSizeMB MB"
Write-Host "  Contents:"
Write-Host "    MDropDX12.exe"
Write-Host "    README.md"
Write-Host "    THIRD-PARTY-LICENSES.txt"
Write-Host "    LICENSE"
