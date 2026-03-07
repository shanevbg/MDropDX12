# setup-dev.ps1 - Set up MDropDX12 dev environment on a fresh Windows machine
# Usage: powershell -ExecutionPolicy Bypass -File setup-dev.ps1
#
# Installs Git, VS 2022 Build Tools (MSVC v143, MSBuild, Windows 11 SDK),
# VSCodium with C/C++ extension, clones the repo, and runs a Release x64 build.
#
# Tested on: Hyper-V VMs, fresh Windows installs.
# Note: Windows Sandbox is not suitable - VS Build Tools hangs during SDK install.

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
function Find-MSBuild {
    # Try vswhere first
    $vw = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vw)) { $vw = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe" }
    if (Test-Path $vw) {
        $found = & $vw -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" 2>$null | Select-Object -First 1
        if ($found -and (Test-Path $found)) { return $found }
        $found = & $vw -latest -find "MSBuild\**\Bin\MSBuild.exe" 2>$null | Select-Object -First 1
        if ($found -and (Test-Path $found)) { return $found }
    }
    # Fallback: known paths
    $paths = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
    )
    foreach ($p in $paths) { if (Test-Path $p) { return $p } }
    return $null
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

# ── 1. Install winget ────────────────────────────────────────────────────────
Write-Step "Checking winget"
$hasWinget = Get-Command winget -ErrorAction SilentlyContinue
if (-not $hasWinget) {
    Write-Host "  Installing winget (App Installer)..."
    try {
        Add-AppxPackage -RegisterByFamilyName Microsoft.DesktopAppInstaller_8wekyb3d8bbwe -ErrorAction Stop
        Refresh-Path
        $hasWinget = Get-Command winget -ErrorAction SilentlyContinue
    } catch {
        Write-Host "  RegisterByFamilyName failed: $_"
    }
    if (-not $hasWinget) {
        Write-Host "  Downloading winget msixbundle..."
        # Install VCLibs dependency first
        try {
            Add-AppxPackage -Path "https://aka.ms/Microsoft.VCLibs.x64.14.00.Desktop.appx" -ErrorAction Stop
        } catch { Write-Host "  VCLibs: $_" }
        $wingetPath = "$env:TEMP\winget.msixbundle"
        Download "https://aka.ms/getwinget" $wingetPath
        try {
            Add-AppxPackage -Path $wingetPath -ErrorAction Stop
        } catch { Write-Host "  winget install failed: $_" }
        Remove-Item $wingetPath -ErrorAction SilentlyContinue
        Refresh-Path
    }
    if (Get-Command winget -ErrorAction SilentlyContinue) {
        Write-Host "  winget installed."
    } else {
        Write-Host "  WARNING: Could not install winget. VSCodium will be skipped." -ForegroundColor Yellow
    }
} else {
    Write-Host "  winget already available."
}

# ── 2. Install Git ───────────────────────────────────────────────────────────
Write-Step "Checking Git"
$hasGit = Get-Command git -ErrorAction SilentlyContinue
if (-not $hasGit) {
    $hasWinget = Get-Command winget -ErrorAction SilentlyContinue
    if ($hasWinget) {
        Write-Host "  Installing Git via winget..."
        winget install Git.Git --accept-source-agreements --accept-package-agreements --silent
    } else {
        Write-Host "  Finding latest Git for Windows release..."
        $apiUrl = "https://api.github.com/repos/git-for-windows/git/releases/latest"
        $release = Invoke-RestMethod -Uri $apiUrl -UseBasicParsing
        $asset = $release.assets | Where-Object { $_.name -match "^Git-.*-64-bit\.exe$" } | Select-Object -First 1
        if (-not $asset) { Write-Error "Could not find Git 64-bit installer." }
        Write-Host "  Downloading $($asset.name)..."
        $gitInstaller = "$env:TEMP\$($asset.name)"
        Download $asset.browser_download_url $gitInstaller
        $proc = Start-Process -FilePath $gitInstaller -ArgumentList "/VERYSILENT", "/NORESTART", "/NOCANCEL", "/SP-", "/CLOSEAPPLICATIONS", "/RESTARTAPPLICATIONS", "/COMPONENTS=icons,ext\reg\shellhere,assoc,assoc_sh" -PassThru
        Wait-Install $proc "Installing Git"
        Remove-Item $gitInstaller -ErrorAction SilentlyContinue
    }
    Refresh-Path
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
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

# ── 3. Install VS 2022 Build Tools ──────────────────────────────────────────
Write-Step "Checking Visual Studio Build Tools"
$msbuild = Find-MSBuild
if ($msbuild) {
    Write-Host "  Build Tools already installed. MSBuild: $msbuild"
} else {
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

    $msbuild = Find-MSBuild
    if (-not $msbuild) {
        Write-Error "MSBuild not found after VS Build Tools install."
    }
}
Write-Host "  MSBuild: $msbuild"

# ── 4. Install VSCodium ──────────────────────────────────────────────────────
Write-Step "Checking VSCodium"
$hasVSCodium = Get-Command codium -ErrorAction SilentlyContinue
if (-not $hasVSCodium) {
    $hasWinget = Get-Command winget -ErrorAction SilentlyContinue
    if ($hasWinget) {
        Write-Host "  Installing VSCodium via winget..."
        winget install VSCodium.VSCodium --accept-source-agreements --accept-package-agreements --silent
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
    } else {
        Write-Host "  WARNING: winget not available, skipping VSCodium." -ForegroundColor Yellow
    }
} else {
    Write-Host "  VSCodium already available."
}

# ── 5. Clone repo ────────────────────────────────────────────────────────────
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

# ── 6. Fetch Spout2 SDK ──────────────────────────────────────────────────────
$spoutDir = Join-Path $cloneDir "external\Spout2"
if (-not (Test-Path (Join-Path $spoutDir "SPOUTSDK"))) {
    Write-Step "Fetching Spout2 SDK"
    git clone --depth 1 https://github.com/leadedge/Spout2.git $spoutDir
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to clone Spout2 SDK."
    }
    Write-Host "  Spout2 SDK fetched."
}

# ── 7. Build ─────────────────────────────────────────────────────────────────
Write-Step "Building MDropDX12 (Release x64)"
$project = Join-Path $cloneDir "src\mDropDX12\engine.vcxproj"
Write-Host "  MSBuild: $msbuild"
Write-Host "  Project: $project"
& $msbuild $project /t:Build /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /m /nologo /clp:Summary
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
