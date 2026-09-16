# release.ps1 - Build and package MDropDX12 portable release
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File release.ps1              # Full build + package
#   powershell -ExecutionPolicy Bypass -File release.ps1 -SkipBuild   # Package existing build
#   powershell -ExecutionPolicy Bypass -File release.ps1 -DryRun      # Show what would be packaged
#   powershell -ExecutionPolicy Bypass -File release.ps1 -GitHubRelease  # Build + package + create GH release
#
# Output: MDropDX12-v{VERSION}-Portable.zip in repo root
# Contents: MDropDX12.exe, README.md, LICENSE, THIRD-PARTY-LICENSES.txt,
#           tools/*.py (the shader converters), docs/GLSL_importing.md,
#           docs/Changes.md.  NO PRESETS -- see the note at $bundledPresets.

param(
    [switch]$SkipBuild,
    [switch]$GitHubRelease,
    [switch]$DryRun,
    [switch]$WithPresets
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# -- 1. Read version from version.h ------------------------------------------
$versionFile = Join-Path $PSScriptRoot "src\mDropDX12\version.h"
if (-not (Test-Path $versionFile)) {
    Write-Error "version.h not found at $versionFile"
    exit 1
}

# Read the three NUMBERS, not MDROP_VERSION_STR. That macro is now computed
# from them by stringification (issue 19), so there is no literal "3.0.0" in
# version.h for a regex to find -- matching on it would fail every release.
$versionContent = Get-Content $versionFile -Raw
$maj = if ($versionContent -match '#define\s+MDROP_VERSION_MAJOR\s+(\d+)') { $Matches[1] } else { $null }
$min = if ($versionContent -match '#define\s+MDROP_VERSION_MINOR\s+(\d+)') { $Matches[1] } else { $null }
$pat = if ($versionContent -match '#define\s+MDROP_VERSION_PATCH\s+(\d+)') { $Matches[1] } else { $null }
if ($null -ne $maj -and $null -ne $min -and $null -ne $pat) {
    $version = "$maj.$min.$pat"
} else {
    Write-Error "Could not parse MDROP_VERSION_MAJOR/MINOR/PATCH from version.h"
    exit 1
}

# -- 1b. README's version line must agree with version.h ---------------------
#
# README.md says "**Current version: X.Y.Z**" and nothing kept it honest, so it
# sat at 2.8.0 through 2.9, 2.11 and 3.0 -- the first thing anyone reads about
# this project, three releases stale, on the front page of the public repo.
#
# Checked HERE rather than trusted, because this script already knows the real
# version and a release is the moment the claim starts being wrong. Fatal, not
# a warning: a warning scrolls past in a build log.
$readmeFile = Join-Path $PSScriptRoot "README.md"
if (Test-Path $readmeFile) {
    $readmeText = Get-Content $readmeFile -Raw
    if ($readmeText -match '\*\*Current version:\s*([0-9]+\.[0-9]+\.[0-9]+)\*\*') {
        $readmeVersion = $Matches[1]
        if ($readmeVersion -ne $version) {
            Write-Host ""
            Write-Host "ERROR: README.md says version $readmeVersion, version.h says $version." -ForegroundColor Red
            Write-Host "       Update the 'Current version:' line in README.md and re-run." -ForegroundColor Red
            exit 1
        }
    } else {
        Write-Host "WARNING: no '**Current version: X.Y.Z**' line found in README.md" -ForegroundColor Yellow
    }
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
    @{ Src = (Join-Path $PSScriptRoot "THIRD-PARTY-LICENSES.txt");       Dst = "THIRD-PARTY-LICENSES.txt" },
    # The shader converters, because the release notes tell people to run them.
    # Both are Python standard library only, so there is nothing to install and
    # nothing to go stale; they are 82 KB together. Shipping the notes without
    # the scripts would send every reader to the repository for two files.
    @{ Src = (Join-Path $PSScriptRoot "tools\glslsandbox_to_milk3.py");   Dst = "tools\glslsandbox_to_milk3.py" },
    @{ Src = (Join-Path $PSScriptRoot "tools\milk3_shadered.py");         Dst = "tools\milk3_shadered.py" },
    # The two guides those scripts belong to. A portable zip that explains
    # nothing sends the user back to GitHub for the thing they just downloaded.
    @{ Src = (Join-Path $PSScriptRoot "docs\GLSL_importing.md");          Dst = "docs\GLSL_importing.md" },
    @{ Src = (Join-Path $PSScriptRoot "docs\Changes.md");                 Dst = "docs\Changes.md" }
)

# -- 4b. The demo bundle -----------------------------------------------------
#
# bundle/ ships as resources/presets and resources/textures, because that is
# where the engine looks: m_szMilkdrop2Path is <exe>\resources\, and the preset
# and texture searches hang off it.
#
# It is a SEPARATE directory in the repository on purpose. resources/presets is
# a runtime working directory -- the app writes imports into it -- and it is
# gitignored for that reason, after twenty-five .milk3 files once ended up
# tracked there, twelve of them conversions of other people's published
# shaders. bundle/ is curated, tracked, and nothing writes to it; see
# bundle/CREDITS.md for the four conditions every preset in it meets.
#
# Not gated behind -WithPresets. That switch packages whatever happens to be
# sitting in the working directory, which is the thing that went wrong; this is
# a reviewed set and ships by default.
$bundleDir = Join-Path $PSScriptRoot "bundle"
foreach ($sub in @("presets", "textures")) {
    $srcDir = Join-Path $bundleDir $sub
    if (-not (Test-Path $srcDir)) { continue }
    foreach ($f in (Get-ChildItem -Path $srcDir -File)) {
        $releaseFiles += @{ Src = $f.FullName; Dst = "resources\$sub\$($f.Name)" }
    }
}
if (Test-Path (Join-Path $bundleDir "CREDITS.md")) {
    $releaseFiles += @{ Src = (Join-Path $bundleDir "CREDITS.md");
                        Dst  = "resources\CREDITS.md" }
}

# Demo presets. OFF BY DEFAULT -- pass -WithPresets to include them.
#
# Since the preset files were untracked (they are .gitignore'd now), a clean
# checkout has none of these and -WithPresets bundles nothing. The switch is
# kept because it still does the right thing on a working tree that has some:
# it packages whatever the person running the release deliberately put there.
#
# This used to bundle everything matching *.milk* in resources/presets, which is
# also where working imports accumulate: by v2.11 that was 23 files, twelve of
# them untracked, including glslsandbox and Shadertoy conversions whose
# licences are not ours to redistribute. Shipping the engine alone is what
# every release up to v2.9.0 actually did.
$presetSrcDir = Join-Path $PSScriptRoot "resources\presets"
$bundledPresets = @()
if ($WithPresets -and (Test-Path $presetSrcDir)) {
    $bundledPresets = @(Get-ChildItem -Path $presetSrcDir -File -Filter "*.milk*" -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notmatch '^(test|Quicksave)' })
}

# -LiteralPath, not -Path. PowerShell treats [ and ] in a -Path as a wildcard
# character class, so "Flexi - color drops [another jelly attempt].milk" is a
# pattern that matches nothing and the file reports as missing. Every entry
# here is a literal filename, never a pattern, and MilkDrop preset names use
# brackets constantly -- four of the first demo bundle's ninety-three.
$missing = @()
foreach ($f in $releaseFiles) {
    if (-not (Test-Path -LiteralPath $f.Src)) {
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

Write-Host "[3/6] Core files present ($($releaseFiles.Count)); bundled presets: $($bundledPresets.Count)." -ForegroundColor Green
foreach ($p in $bundledPresets) {
    Write-Host "  + resources/presets/$($p.Name)" -ForegroundColor DarkGray
}

# -- 5. Stage and create zip -------------------------------------------------
$zipName = "MDropDX12-v$version-Portable.zip"

# Into releases\, not the repository root.
#
# Every release dropped its zip at the top level and left it there: twelve of
# them, 22 MB, sitting alongside README.md and the solution file. releases\
# already existed and held exactly one (v2.1.0), which is how the habit of
# leaving them at the root went unnoticed. *.zip is gitignored either way, so
# this changes nothing about what is committed -- only where a build puts its
# output, which is the thing that kept re-cluttering the root.
$releasesDir = Join-Path $PSScriptRoot "releases"
if (-not (Test-Path $releasesDir)) {
    New-Item -ItemType Directory -Path $releasesDir -Force | Out-Null
}
$zipPath = Join-Path $releasesDir $zipName

if ($DryRun) {
    Write-Host ""
    Write-Host "[DRY RUN] Would create: $zipName" -ForegroundColor Magenta
    Write-Host ""
    Write-Host "Contents:" -ForegroundColor White
    foreach ($f in $releaseFiles) {
        $size = [math]::Round((Get-Item -LiteralPath $f.Src).Length / 1KB, 1)
        Write-Host "  $($f.Dst)  ($($size) KB)" -ForegroundColor DarkGray
    }
    foreach ($p in $bundledPresets) {
        $size = [math]::Round($p.Length / 1KB, 1)
        Write-Host "  resources/presets/$($p.Name)  ($($size) KB)" -ForegroundColor DarkGray
    }
    Write-Host ""
    Write-Host "No files were created." -ForegroundColor Magenta
    exit 0
}

Write-Host "[4/6] Staging release files..." -ForegroundColor Yellow

$stagingDir = Join-Path $env:TEMP "MDropDX12-release-$([guid]::NewGuid().ToString('N').Substring(0,8))"
New-Item -ItemType Directory -Path $stagingDir -Force | Out-Null

foreach ($f in $releaseFiles) {
    $dst = Join-Path $stagingDir $f.Dst
    # Dst carries a subdirectory now (tools\, docs\), and Copy-Item does not
    # create one -- it fails. Every entry was at the root until this release.
    $dstDir = Split-Path -Parent $dst
    if ($dstDir -and -not (Test-Path $dstDir)) {
        New-Item -ItemType Directory -Path $dstDir -Force | Out-Null
    }
    Copy-Item -LiteralPath $f.Src -Destination $dst
}

if ($bundledPresets.Count -gt 0) {
    $stagePresets = Join-Path $stagingDir "resources\presets"
    New-Item -ItemType Directory -Path $stagePresets -Force | Out-Null
    foreach ($p in $bundledPresets) {
        Copy-Item $p.FullName (Join-Path $stagePresets $p.Name)
    }
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

# -Recurse: without it this counts only the files at the ROOT of the zip, so it
# reported 4 when the package held 8 (docs\ and tools\ were already there) and
# would report 4 for the 127 the demo bundle brings. A validation line that
# under-reports by two orders of magnitude is worse than none -- it reads as
# confirmation that the extra content is missing.
$fileCount = (Get-ChildItem $verifyDir -File -Recurse).Count
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
Write-Host "  File: releases\$zipName" -ForegroundColor White
Write-Host "  Size: $zipSizeMB MB" -ForegroundColor White
Write-Host ""
Write-Host "  Contents:" -ForegroundColor White
foreach ($f in $releaseFiles) {
    # -LiteralPath, as everywhere else that touches these paths: a preset name
    # containing [ ] is a wildcard to -Path and resolves to nothing.
    $size = [math]::Round((Get-Item -LiteralPath $f.Src).Length / 1KB, 1)
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
        # -Encoding UTF8, because this script runs under WINDOWS PowerShell
        # (powershell.exe 5.1), where Get-Content defaults to the ANSI codepage.
        # docs/Changes.md is UTF-8 and full of em dashes, so the default read
        # turned every one into mojibake and published it as the release notes.
        $lines = Get-Content $changesFile -Encoding UTF8
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
    # NOT Set-Content -Encoding UTF8: on PowerShell 5.1 that writes a BYTE ORDER
    # MARK, and GitHub renders it as a stray character at the very top of the
    # release notes. WriteAllText with UTF8Encoding($false) omits it.
    [System.IO.File]::WriteAllText($notesFile, $releaseBody, (New-Object System.Text.UTF8Encoding($false)))

    $tag = "v$version"
    # --repo is REQUIRED. Without it gh resolves to the current directory's
    # remote, which is `origin` -- the PRIVATE dev repo. Releases belong on the
    # public repo, built from the commit the CI sync already filtered and pushed.
    & gh release create $tag $zipPath --repo shanevbg/MDropDX12 --title "MDropDX12 v$version" --notes-file $notesFile
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
