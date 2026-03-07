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

# ── Copy exe ──────────────────────────────────────────────────────────────────
Copy-Item $exePath $stageRoot

# ── Copy config files ─────────────────────────────────────────────────────────
$configFiles = @(
    "config\README.txt"
    "config\messages.ini"
    "config\sprites.ini"
    "config\midi-default.txt"
    "config\precompile.txt"
    "config\script-default.txt"
    "config\visualizer-keys.txt"
    "config\settings.ini"
)
foreach ($f in $configFiles) {
    if (Test-Path $f) {
        Copy-Item $f $stageRoot
    } else {
        # Fall back to Release/ directory
        $fallback = Join-Path "Release" (Split-Path $f -Leaf)
        if (Test-Path $fallback) { Copy-Item $fallback $stageRoot }
        else { Write-Warning "Config file not found: $f" }
    }
}

# ── Copy resources (presets + textures) ───────────────────────────────────────
$resSource = "Release\resources"
if (Test-Path $resSource) {
    $resDest = Join-Path $stageRoot "resources"
    Copy-Item $resSource $resDest -Recurse
} else {
    Write-Warning "Release\resources not found - zip will not include presets/textures"
}

# ── Create empty directories that the app expects ─────────────────────────────
@("capture", "cache") | ForEach-Object {
    New-Item -ItemType Directory -Path (Join-Path $stageRoot $_) -Force | Out-Null
}

# ── Remove any stale files from staging ───────────────────────────────────────
Get-ChildItem $stageRoot -Recurse -Include "*.pdb","debug.log","diag_*","*.obj","*.tlog" |
    Remove-Item -Force -ErrorAction SilentlyContinue

# ── Create zip ────────────────────────────────────────────────────────────────
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path $stageRoot -DestinationPath $zipPath -CompressionLevel Optimal

# ── Clean up staging ──────────────────────────────────────────────────────────
Remove-Item $staging -Recurse -Force -ErrorAction SilentlyContinue

$size = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
Write-Host ""
Write-Host "Created: $zipPath ($size MB)"
Write-Host "Done."
