# release.ps1 - Build and package MDropDX12 portable release
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File release.ps1              # Full build + package
#   powershell -ExecutionPolicy Bypass -File release.ps1 -SkipBuild   # Package existing build
#   powershell -ExecutionPolicy Bypass -File release.ps1 -DryRun      # Show what would be packaged
#   powershell -ExecutionPolicy Bypass -File release.ps1 -GitHubRelease  # Build + package + create GH release
#
# Output: MDropDX12-v{VERSION}-Portable.zip in repo root
# Contents: MDropDX12.exe, README.md, LICENSE, THIRD-PARTY-LICENSES.txt

param(
    [switch]$SkipBuild,
    [switch]$GitHubRelease,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# -- 1. Read version from version.h ------------------------------------------
$versionFile = Join-Path $PSScriptRoot "src\mDropDX12\version.h"
if (-not (Test-Path $versionFile)) {
    Write-Error "version.h not found at $versionFile"
    exit 1
}

$versionContent = Get-Content $versionFile -Raw
if ($versionContent -match '#define\s+MDROP_VERSION_STR\s+"([^"]+)"') {
    $version = $Matches[1]
} else {
    Write-Error "Could not parse MDROP_VERSION_STR from version.h"
    exit 1
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "   MDropDX12 v$version Release Builder" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""

# -- 2. Build (unless -SkipBuild) --------------------------------------------
$exeSrc = Join-Path $PSScriptRoot "src\mDropDX12\Release_x64\MDropDX12.exe"

if (-not $SkipBuild) {
    Write-Host "[1/6] Building Release x64..." -ForegroundColor Yellow
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build.ps1") Release x64
    if ($LASTEXITCODE -ne 0) {
        Write-Host ""
        Write-Host "BUILD FAILED - aborting release." -ForegroundColor Red
        exit 1
    }
    Write-Host "[1/6] Build succeeded." -ForegroundColor Green
} else {
    Write-Host "[1/6] Skipping build (-SkipBuild)." -ForegroundColor DarkGray
}

# -- 3. Verify exe exists and is fresh ---------------------------------------
Write-Host "[2/6] Verifying build output..." -ForegroundColor Yellow

if (-not (Test-Path $exeSrc)) {
    Write-Host ""
    Write-Host "ERROR: Build output not found: $exeSrc" -ForegroundColor Red
    Write-Host "Run without -SkipBuild or build manually first." -ForegroundColor Red
    exit 1
}

$exeInfo = Get-Item $exeSrc
$exeAge = (Get-Date) - $exeInfo.LastWriteTime
if ($exeAge.TotalMinutes -gt 5 -and -not $SkipBuild) {
    Write-Host "  WARNING: Exe is $([math]::Round($exeAge.TotalMinutes, 0)) minutes old - build may not have updated it." -ForegroundColor Yellow
}

$exeSizeMB = [math]::Round($exeInfo.Length / 1MB, 2)
Write-Host "  MDropDX12.exe: $exeSizeMB MB (built $($exeInfo.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')))" -ForegroundColor DarkGray
Write-Host "[2/6] Exe verified." -ForegroundColor Green

# -- 4. Verify required files exist ------------------------------------------
Write-Host "[3/6] Checking release files..." -ForegroundColor Yellow

$releaseFiles = @(
    @{ Src = $exeSrc;                                                    Dst = "MDropDX12.exe" },
    @{ Src = (Join-Path $PSScriptRoot "README.md");                      Dst = "README.md" },
    @{ Src = (Join-Path $PSScriptRoot "LICENSE");                        Dst = "LICENSE" },
    @{ Src = (Join-Path $PSScriptRoot "THIRD-PARTY-LICENSES.txt");       Dst = "THIRD-PARTY-LICENSES.txt" }
)

$missing = @()
foreach ($f in $releaseFiles) {
    if (-not (Test-Path $f.Src)) {
        $missing += $f.Src
    }
}

if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host "ERROR: Missing required files:" -ForegroundColor Red
    foreach ($m in $missing) {
        Write-Host "  - $m" -ForegroundColor Red
    }
    exit 1
}

Write-Host "[3/6] All 4 files present." -ForegroundColor Green

# -- 5. Stage and create zip -------------------------------------------------
$zipName = "MDropDX12-v$version-Portable.zip"
$zipPath = Join-Path $PSScriptRoot $zipName

if ($DryRun) {
    Write-Host ""
    Write-Host "[DRY RUN] Would create: $zipName" -ForegroundColor Magenta
    Write-Host ""
    Write-Host "Contents:" -ForegroundColor White
    foreach ($f in $releaseFiles) {
        $size = [math]::Round((Get-Item $f.Src).Length / 1KB, 1)
        Write-Host "  $($f.Dst)  ($($size) KB)" -ForegroundColor DarkGray
    }
    Write-Host ""
    Write-Host "No files were created." -ForegroundColor Magenta
    exit 0
}

Write-Host "[4/6] Staging release files..." -ForegroundColor Yellow

$stagingDir = Join-Path $env:TEMP "MDropDX12-release-$([guid]::NewGuid().ToString('N').Substring(0,8))"
New-Item -ItemType Directory -Path $stagingDir -Force | Out-Null

foreach ($f in $releaseFiles) {
    Copy-Item $f.Src (Join-Path $stagingDir $f.Dst)
}

# Create zip
if (Test-Path $zipPath) {
    Remove-Item $zipPath -Force
}

Compress-Archive -Path (Join-Path $stagingDir "*") -DestinationPath $zipPath -CompressionLevel Optimal
Remove-Item $stagingDir -Recurse -Force

Write-Host "[4/6] Zip created." -ForegroundColor Green

# -- 6. Validate zip --------------------------------------------------------
Write-Host "[5/6] Validating zip..." -ForegroundColor Yellow

$verifyDir = Join-Path $env:TEMP "MDropDX12-verify-$([guid]::NewGuid().ToString('N').Substring(0,8))"
Expand-Archive -Path $zipPath -DestinationPath $verifyDir -Force

$verifyExe = Join-Path $verifyDir "MDropDX12.exe"
if (-not (Test-Path $verifyExe)) {
    Write-Host ""
    Write-Host "VALIDATION FAILED: MDropDX12.exe not found in zip!" -ForegroundColor Red
    Remove-Item $verifyDir -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $zipPath -Force
    exit 1
}

$verifyExeSize = (Get-Item $verifyExe).Length
$srcExeSize = (Get-Item $exeSrc).Length
if ($verifyExeSize -ne $srcExeSize) {
    Write-Host ""
    Write-Host "VALIDATION FAILED: Exe size mismatch! Zip: $verifyExeSize, Source: $srcExeSize" -ForegroundColor Red
    Remove-Item $verifyDir -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $zipPath -Force
    exit 1
}

$fileCount = (Get-ChildItem $verifyDir -File).Count
Remove-Item $verifyDir -Recurse -Force

Write-Host "[5/6] Zip validated ($fileCount files, exe size matches)." -ForegroundColor Green

# -- 7. Update Release/ working directory ------------------------------------
$releaseExe = Join-Path $PSScriptRoot "Release\MDropDX12.exe"
if (Test-Path (Split-Path $releaseExe)) {
    Copy-Item $exeSrc $releaseExe -Force
    Write-Host "  Updated Release\MDropDX12.exe" -ForegroundColor DarkGray
}

# -- 8. Report --------------------------------------------------------------
$zipSize = (Get-Item $zipPath).Length
$zipSizeMB = [math]::Round($zipSize / 1MB, 2)

Write-Host ""
Write-Host "============================================" -ForegroundColor Green
Write-Host "   Release package created successfully" -ForegroundColor Green
Write-Host "============================================" -ForegroundColor Green
Write-Host ""
Write-Host "  File: $zipName" -ForegroundColor White
Write-Host "  Size: $zipSizeMB MB" -ForegroundColor White
Write-Host ""
Write-Host "  Contents:" -ForegroundColor White
foreach ($f in $releaseFiles) {
    $size = [math]::Round((Get-Item $f.Src).Length / 1KB, 1)
    Write-Host "    $($f.Dst)  ($($size) KB)" -ForegroundColor DarkGray
}
Write-Host ""

# -- 9. GitHub Release (optional) -------------------------------------------
if ($GitHubRelease) {
    Write-Host "[6/6] Creating GitHub release..." -ForegroundColor Yellow

    # Check gh CLI is available
    $ghCmd = Get-Command gh -ErrorAction SilentlyContinue
    if (-not $ghCmd) {
        Write-Host "WARNING: gh CLI not found. Install from https://cli.github.com/" -ForegroundColor Yellow
        Write-Host "Skipping GitHub release creation." -ForegroundColor Yellow
        exit 0
    }

    # Extract latest changelog section from docs/Changes.md
    $changesFile = Join-Path $PSScriptRoot "docs\Changes.md"
    $releaseBody = ""
    if (Test-Path $changesFile) {
        $lines = Get-Content $changesFile
        $inSection = $false
        $bodyLines = @()
        foreach ($line in $lines) {
            if ($line -match "^## v$([regex]::Escape($version))") {
                $inSection = $true
                continue
            }
            if ($inSection -and $line -match "^## v") {
                break
            }
            if ($inSection) {
                $bodyLines += $line
            }
        }
        $releaseBody = ($bodyLines -join "`n").Trim()
    }

    if ([string]::IsNullOrWhiteSpace($releaseBody)) {
        $releaseBody = "MDropDX12 v$version portable release."
    }

    # Write body to temp file to avoid shell escaping issues
    $notesFile = Join-Path $env:TEMP "MDropDX12-release-notes.md"
    Set-Content -Path $notesFile -Value $releaseBody -Encoding UTF8

    $tag = "v$version"
    & gh release create $tag $zipPath --title "MDropDX12 v$version" --notes-file $notesFile
    Remove-Item $notesFile -Force -ErrorAction SilentlyContinue
    if ($LASTEXITCODE -eq 0) {
        Write-Host "[6/6] GitHub release created: $tag" -ForegroundColor Green
    } else {
        Write-Host "WARNING: GitHub release creation failed (exit code $LASTEXITCODE)." -ForegroundColor Yellow
    }
} else {
    Write-Host "[6/6] Skipping GitHub release (use -GitHubRelease to create one)." -ForegroundColor DarkGray
}

Write-Host ""
