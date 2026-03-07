# setup-dev.ps1 - Set up MDropDX12 dev environment on a fresh Windows machine
# Usage: powershell -ExecutionPolicy Bypass -File setup-dev.ps1
#
# Installs Git, VS 2022 Build Tools (MSVC v143, MSBuild, Windows 11 SDK),
# clones the repo, and runs a Release x64 build.
#
# Tested on: Hyper-V VMs, fresh Windows installs.
# Note: Windows Sandbox may hang during SDK install due to I/O constraints.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"  # speeds up Invoke-WebRequest
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$repoUrl = "https://github.com/shanevbg/MDropDX12.git"
$defaultDir = "C:\Code"

function Write-Step($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }
function Refresh-Path {
    $env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
                [System.Environment]::GetEnvironmentVariable("Path", "User")
}
function Download($url, $outFile) {
    Invoke-WebRequest -Uri $url -OutFile $outFile -UseBasicParsing
}
function Format-Elapsed($ts) {
    if ($ts.TotalHours -ge 1) { return $ts.ToString("h\:mm\:ss") }
    return $ts.ToString("mm\:ss")
}
function Wait-Install($proc, $label) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while (-not $proc.HasExited) {
        Write-Host "`r  $label... $(Format-Elapsed $sw.Elapsed)" -NoNewline
        Start-Sleep -Seconds 2
    }
    $proc.WaitForExit()
    Write-Host "`r  $label... done ($(Format-Elapsed $sw.Elapsed), exit code $($proc.ExitCode))    "
}

# ── 0. Prompt for clone directory ─────────────────────────────────────────────
Write-Step "MDropDX12 Development Environment Setup"
Write-Host "  This script installs Git and VS 2022 Build Tools, clones the"
Write-Host "  MDropDX12 repo, and builds it. The VS Build Tools install can"
Write-Host "  take 1-2+ hours depending on your internet connection."
Write-Host ""
Write-Host "  Base directory for source code [default: $defaultDir]: " -NoNewline
$userDir = Read-Host
$baseDir = if ($userDir.Trim()) { $userDir.Trim() } else { $defaultDir }
$cloneDir = Join-Path $baseDir "MDropDX12"
Write-Host "  Repo will be at: $cloneDir"

# ── 1. Install Git ───────────────────────────────────────────────────────────
# Download the installer directly - winget has missing dependencies in Sandbox.
Write-Step "Checking Git"
$hasGit = Get-Command git -ErrorAction SilentlyContinue
if (-not $hasGit) {
    # Query GitHub API for the latest Git for Windows release
    Write-Host "  Finding latest Git for Windows release..."
    $apiUrl = "https://api.github.com/repos/git-for-windows/git/releases/latest"
    $release = Invoke-RestMethod -Uri $apiUrl -UseBasicParsing
    $asset = $release.assets | Where-Object { $_.name -match "^Git-.*-64-bit\.exe$" } | Select-Object -First 1
    if (-not $asset) {
        Write-Error "Could not find Git 64-bit installer in latest release."
    }

    Write-Host "  Downloading $($asset.name)..."
    $gitInstaller = "$env:TEMP\$($asset.name)"
    Download $asset.browser_download_url $gitInstaller

    $proc = Start-Process -FilePath $gitInstaller -ArgumentList "/VERYSILENT", "/NORESTART", "/NOCANCEL", "/SP-", "/CLOSEAPPLICATIONS", "/RESTARTAPPLICATIONS", "/COMPONENTS=icons,ext\reg\shellhere,assoc,assoc_sh" -PassThru
    Wait-Install $proc "Installing Git"
    Remove-Item $gitInstaller -ErrorAction SilentlyContinue
    Refresh-Path

    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        # Git installs to a known path - add it manually if PATH refresh missed it
        $gitPath = "C:\Program Files\Git\cmd"
        if (Test-Path $gitPath) { $env:Path += ";$gitPath" }
    }
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        Write-Error "Git installation failed. Install manually from https://git-scm.com"
    }
    Write-Host "  Git installed: $(git --version)"
} else {
    Write-Host "  Git already available: $(git --version)"
}

# ── 2. Install VS 2022 Build Tools ──────────────────────────────────────────
Write-Step "Checking Visual Studio Build Tools"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    $vswhere = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
}

$needsInstall = $true
if (Test-Path $vswhere) {
    $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild `
               -find "MSBuild\**\Bin\MSBuild.exe" 2>$null | Select-Object -First 1
    if ($msbuild -and (Test-Path $msbuild)) {
        Write-Host "  Build Tools already installed. MSBuild: $msbuild"
        $needsInstall = $false
    }
}

if ($needsInstall) {
    Write-Host "  Downloading VS 2022 Build Tools installer..."
    $vsInstaller = "$env:TEMP\vs_BuildTools.exe"
    Download "https://aka.ms/vs/17/release/vs_BuildTools.exe" $vsInstaller

    Write-Host "  Components: MSVC v143, MSBuild, Windows 11 SDK (10.0.26100.0)"
    Write-Host "  This is the longest step - typically 1-2+ hours."
    $vsArgs = @(
        "--passive", "--wait", "--norestart", "--nocache",
        "--add", "Microsoft.VisualStudio.Workload.VCTools",
        "--add", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
        "--add", "Microsoft.Component.MSBuild",
        "--add", "Microsoft.VisualStudio.Component.Windows11SDK.26100",
        "--includeRecommended"
    )
    $proc = Start-Process -FilePath $vsInstaller -ArgumentList $vsArgs -PassThru
    Wait-Install $proc "Installing VS Build Tools"
    Remove-Item $vsInstaller -ErrorAction SilentlyContinue
    Refresh-Path

    if ($proc.ExitCode -ne 0 -and $proc.ExitCode -ne 3010) {
        Write-Error "VS Build Tools installation failed with exit code $($proc.ExitCode)."
    }
    if ($proc.ExitCode -eq 3010) {
        Write-Host "  Exit code 3010 = reboot recommended (safe to continue)."
    }
}

# Ensure vswhere/MSBuild are on PATH (VS installer may not update current session)
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    $vswhere = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
}
if (Test-Path $vswhere) {
    $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild `
               -find "MSBuild\**\Bin\MSBuild.exe" 2>$null | Select-Object -First 1
    if ($msbuild -and (Test-Path $msbuild)) {
        $msbuildDir = Split-Path $msbuild -Parent
        if ($env:Path -notlike "*$msbuildDir*") {
            $env:Path += ";$msbuildDir"
            Write-Host "  Added MSBuild to PATH: $msbuildDir"
        }
    } else {
        Write-Error "VS Build Tools installed but MSBuild not found via vswhere."
    }
} else {
    Write-Error "vswhere.exe not found after VS Build Tools install."
}

# ── 3. Clone repo ────────────────────────────────────────────────────────────
Write-Step "Checking repository"
$inRepo = $false
try {
    $repoRoot = git rev-parse --show-toplevel 2>$null
    if ($LASTEXITCODE -eq 0 -and $repoRoot) {
        $inRepo = $true
        $cloneDir = $repoRoot
        Write-Host "  Already in repo: $cloneDir"
    }
} catch {}

if (-not $inRepo) {
    if (Test-Path "$cloneDir\.git") {
        Write-Host "  Repo already cloned at $cloneDir"
    } else {
        Write-Host "  Cloning $repoUrl into $cloneDir..."
        git clone $repoUrl $cloneDir
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Failed to clone repository."
        }
        Write-Host "  Cloned."
    }
}

# ── 4. Build ─────────────────────────────────────────────────────────────────
Write-Step "Building MDropDX12 (Release x64)"
Set-Location $cloneDir
& powershell -ExecutionPolicy Bypass -File build.ps1 Release x64
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed with exit code $LASTEXITCODE."
}

# ── Done ─────────────────────────────────────────────────────────────────────
$exe = Join-Path $cloneDir "src\mDropDX12\Release_x64\MDropDX12.exe"
if (Test-Path $exe) {
    Write-Step "SUCCESS"
    Write-Host "  Executable: $exe"
    Write-Host "  Run it:     & '$exe'"
} else {
    Write-Error "Build completed but exe not found at $exe"
}
