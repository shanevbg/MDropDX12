# package.ps1 — Build a portable release zip for MDropDX12
# Usage: powershell -ExecutionPolicy Bypass -File package.ps1
#
# Prerequisites: Run build.ps1 Release x64 first.
# Output: MDropDX12-<version>-Portable.zip in the repo root.

param(
    [string]$OutputDir = "."
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ── Read version from version.h ──────────────────────────────────────────────
$versionHeader = Get-Content "src\mDropDX12\version.h" -Raw
if ($versionHeader -match '#define\s+MDROP_VERSION_MAJOR\s+(\d+)') { $major = $Matches[1] } else { Write-Error "Cannot read MDROP_VERSION_MAJOR"; exit 1 }
if ($versionHeader -match '#define\s+MDROP_VERSION_MINOR\s+(\d+)') { $minor = $Matches[1] } else { Write-Error "Cannot read MDROP_VERSION_MINOR"; exit 1 }
if ($versionHeader -match '#define\s+MDROP_VERSION_PATCH\s+(\d+)') { $patch = $Matches[1] } else { $patch = "0" }
if ($patch -eq "0") { $version = "$major.$minor" } else { $version = "$major.$minor.$patch" }
$zipName = "MDropDX12-$version-Portable.zip"
$zipPath = Join-Path $OutputDir $zipName

Write-Host "Packaging MDropDX12 v$version..."

# ── Verify build output exists ────────────────────────────────────────────────
$exePath = "src\mDropDX12\Release_x64\MDropDX12.exe"
if (-not (Test-Path $exePath)) {
    Write-Error "Release exe not found at $exePath. Run: powershell -ExecutionPolicy Bypass -File build.ps1 Release x64"
    exit 1
}

# ── Create staging directory ──────────────────────────────────────────────────
$staging = Join-Path $env:TEMP "MDropDX12-package-$([guid]::NewGuid().ToString('N').Substring(0,8))"
$stageRoot = Join-Path $staging "MDropDX12"
New-Item -ItemType Directory -Path $stageRoot -Force | Out-Null

Write-Host "Staging to: $staging"

# ── Copy exe and README ──────────────────────────────────────────────────────
# The exe is fully self-bootstrapping: it creates config files, resources/,
# cache/, and capture/ directories on first run. Only the exe is required.
Copy-Item $exePath $stageRoot
if (Test-Path "config\README.txt") { Copy-Item "config\README.txt" $stageRoot }

# ── Create zip ────────────────────────────────────────────────────────────────
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path $stageRoot -DestinationPath $zipPath -CompressionLevel Optimal

# ── Clean up staging ──────────────────────────────────────────────────────────
Remove-Item $staging -Recurse -Force -ErrorAction SilentlyContinue

$size = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
Write-Host ""
Write-Host "Created: $zipPath ($size MB)"
Write-Host "Done."
