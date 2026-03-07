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
Write-Host "  This script installs Git, VS 2022 Build Tools, and VSCodium,"
Write-Host "  clones the MDropDX12 repo, and builds it. Build Tools install can"
Write-Host "  take 15-30 minutes depending on your internet connection."
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
    Write-Host "  This is the longest step - typically 15-30 minutes."
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
Write-Step "Locating MSBuild"
Refresh-Path
$msbuild = $null

# Try vswhere first
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    $vswhere = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
}
if (Test-Path $vswhere) {
    Write-Host "  vswhere: $vswhere"
    $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild `
               -find "MSBuild\**\Bin\MSBuild.exe" 2>$null | Select-Object -First 1
    if (-not $msbuild) {
        # Try without -requires filter (some installs don't register components correctly)
        Write-Host "  vswhere -requires filter found nothing, trying without filter..."
        $msbuild = & $vswhere -latest -find "MSBuild\**\Bin\MSBuild.exe" 2>$null | Select-Object -First 1
    }
}

# Fallback: search known VS installation paths directly
if (-not $msbuild -or -not (Test-Path $msbuild)) {
    Write-Host "  vswhere failed, searching known paths..."
    $searchPaths = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
    )
    foreach ($p in $searchPaths) {
        if (Test-Path $p) {
            $msbuild = $p
            Write-Host "  Found at known path: $msbuild"
            break
        }
    }
}

if ($msbuild -and (Test-Path $msbuild)) {
    $msbuildDir = Split-Path $msbuild -Parent
    if ($env:Path -notlike "*$msbuildDir*") {
        $env:Path += ";$msbuildDir"
        # Also update Machine PATH so child processes (build.ps1) can find MSBuild
        $machinePath = [System.Environment]::GetEnvironmentVariable("Path", "Machine")
        if ($machinePath -notlike "*$msbuildDir*") {
            [System.Environment]::SetEnvironmentVariable("Path", "$machinePath;$msbuildDir", "Machine")
        }
    }
    Write-Host "  MSBuild: $msbuild"
} else {
    Write-Host "  Searched paths:" -ForegroundColor Yellow
    if (Test-Path $vswhere) {
        $allInstalls = & $vswhere -all -format json 2>$null
        Write-Host "  vswhere -all: $allInstalls"
    }
    Write-Error "MSBuild not found. VS Build Tools may not have installed correctly."
}

# ── 3. Install VSCodium ───────────────────────────────────────────────────────
Write-Step "Checking VSCodium"
$hasVSCodium = Get-Command codium -ErrorAction SilentlyContinue
if (-not $hasVSCodium) {
    Write-Host "  Finding latest VSCodium release..."
    $apiUrl = "https://api.github.com/repos/VSCodium/vscodium/releases/latest"
    $release = Invoke-RestMethod -Uri $apiUrl -UseBasicParsing
    $asset = $release.assets | Where-Object { $_.name -match "VSCodiumSetup-x64-.*\.exe$" } | Select-Object -First 1
    if (-not $asset) {
        Write-Host "  WARNING: Could not find VSCodium installer. Skipping." -ForegroundColor Yellow
    } else {
        Write-Host "  Downloading $($asset.name)..."
        $vscodiumInstaller = "$env:TEMP\$($asset.name)"
        Download $asset.browser_download_url $vscodiumInstaller

        $proc = Start-Process -FilePath $vscodiumInstaller -ArgumentList "/VERYSILENT", "/NORESTART", "/MERGETASKS=!runcode,addcontextmenufiles,addcontextmenufolders,addtopath" -PassThru
        Wait-Install $proc "Installing VSCodium"
        Remove-Item $vscodiumInstaller -ErrorAction SilentlyContinue
        Refresh-Path

        if (-not (Get-Command codium -ErrorAction SilentlyContinue)) {
            $codiumPath = "${env:LocalAppData}\Programs\VSCodium\bin"
            if (Test-Path $codiumPath) { $env:Path += ";$codiumPath" }
        }
        if (Get-Command codium -ErrorAction SilentlyContinue) {
            Write-Host "  Installing C/C++ extension..."
            codium --install-extension ms-vscode.cpptools 2>$null
            Write-Host "  VSCodium installed."
        } else {
            Write-Host "  WARNING: VSCodium installed but not on PATH." -ForegroundColor Yellow
        }
    }
} else {
    Write-Host "  VSCodium already available."
}

# ── 4. Clone repo ────────────────────────────────────────────────────────────
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

# ── 5. Build ─────────────────────────────────────────────────────────────────
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
