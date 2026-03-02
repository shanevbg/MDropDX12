# MDropDX12 Development Guide

How to set up a development environment on a fresh Windows machine, clone the source, and build MDropDX12.

## Prerequisites

Install the following tools. All can be installed via `winget` from a PowerShell terminal, or downloaded manually.

### 1. Git for Windows

```powershell
winget install Git.Git
```

Or download from [git-scm.com](https://git-scm.com/download/win). After installing, restart your terminal so `git` is on your PATH.

### 2. Visual Studio 2022 Build Tools

MDropDX12 uses the MSVC v143 compiler and MSBuild. You need the **Build Tools** package (not the full Visual Studio IDE).

```powershell
winget install Microsoft.VisualStudio.2022.BuildTools
```

Or download the installer from [aka.ms/vs/17/release/vs_BuildTools.exe](https://aka.ms/vs/17/release/vs_BuildTools.exe).

When the Visual Studio Installer opens, select:

- **Workload**: "Desktop development with C++"
- **Individual Components** (verify these are checked):
  - MSVC v143 - VS 2022 C++ x64 build tools
  - Windows 11 SDK (10.0.26100.0)

### 3. VSCodium

```powershell
winget install VSCodium.VSCodium
```

Or download from [vscodium.com](https://vscodium.com/).

After installing, open VSCodium and install the **C/C++** extension:

1. Open the Extensions panel (Ctrl+Shift+X)
2. Search for `C/C++` by Microsoft
3. Install the `ms-vscode.cpptools` extension from Open VSX

## Clone the Repository

```powershell
git clone https://github.com/shanevbg/MDropDX12.git
cd MDropDX12
```

## First-Time Setup

Run the developer setup script to populate the `Release/` folder with default configuration files and resources. This folder is used as the working directory for Debug builds.

```powershell
Release\Developer-Setup.cmd
```

This copies `config/*.ini`, `config/script-default.txt`, and the `resources/` directory into `Release/`.

The Spout2 SDK (the only external dependency) is automatically fetched by the build script on the first build -- no manual setup needed.

## Building

### From Terminal

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 Release x64
```

Arguments: `[Debug|Release]` `[x64]` `[Clean]`

The build script will:

1. Locate MSBuild via `vswhere.exe`
2. Clone the Spout2 SDK into `external/Spout2/` if not already present
3. Invoke MSBuild with the specified configuration and platform

### From VSCodium

The project includes pre-configured build tasks in `.vscode/tasks.json`.

- **Ctrl+Shift+B** -- runs the default build task (Debug|x64)
- **Ctrl+Shift+P** > "Tasks: Run Task" -- shows all available tasks:
  - Build Visualizer (Debug)
  - Build Visualizer (Release)
  - Build RELEASE (builds + copies files to Release folder)
  - Clean

## Running and Debugging

### Debug (F5)

Press **F5** in VSCodium to build and launch the debugger. The launch configuration (`.vscode/launch.json`) builds the Debug configuration, then runs the exe with `Release/` as the working directory.

Make sure you've run `Developer-Setup.cmd` first so the `Release/` folder has the required config files and resources.

### Release

After a Release build, the executable is in the build output directory (see table below). To test a release build with the full runtime environment, run:

```powershell
config\release.cmd
```

This copies the built exe and default configs into the `Release/` folder. Then run `Release/MDropDX12.exe`.

## Project Structure

```
MDropDX12/
  build.ps1              -- Build script (locates MSBuild, fetches Spout2, builds)
  MDropDX12.sln          -- Visual Studio solution file
  CLAUDE.md              -- AI assistant project context and critical warnings
  src/
    mDropDX12/           -- Main visualizer engine (C++17, DX12, Win32 API)
      engine.vcxproj     -- MSBuild project file
      engine.cpp/h       -- Core engine, render loop, state management
      dxcontext.cpp/h    -- DX12 device, swap chain, descriptor heaps
      milkdropfs.cpp     -- Preset shader generation (warp/comp/blur)
      engine_settings_ui.cpp -- Settings window (8-tab dark theme UI)
      engine_input.cpp   -- Keyboard/mouse/hotkey handlers
      engine_presets.cpp  -- Preset loading, parsing, cross-fading
      state.cpp/h        -- Preset state and parameter storage
      overlay.cpp/h      -- GDI layered window for HUD text
      ...
    audio/               -- WASAPI loopback audio capture
    ns-eel2/             -- Expression evaluator (Cockos WDL ns-eel2, x64 JIT)
  external/              -- Auto-fetched dependencies (Spout2 SDK)
  resources/
    data/                -- Shader source files (blur, warp, comp, include.fx)
    presets/              -- Preset collections (.milk files)
    textures/            -- Texture files (not in git; download separately)
    sprites/             -- Sprite image files
  config/                -- Default config files and build helpers
  docs/                  -- Documentation
  install/               -- NSIS installer script
  .vscode/               -- VSCodium/VS Code IDE configuration
  Release/               -- Runtime working directory (populated by Developer-Setup.cmd)
```

## Build Output Paths

| Configuration | Output Directory             |
| ------------- | ---------------------------- |
| Debug\|x64    | `src/mDropDX12/Debug_x64/`   |
| Release\|x64  | `src/mDropDX12/Release_x64/` |

## Troubleshooting

### "MSBuild not found"

The build script uses `vswhere.exe` to locate MSBuild. Ensure:

- Visual Studio 2022 Build Tools is installed
- The "Desktop development with C++" workload is selected
- Restart your terminal after installing

### Spout2 clone fails

`build.ps1` runs `git clone https://github.com/leadedge/Spout2.git` into `external/Spout2/`. If this fails:

- Verify Git is installed and on your PATH (`git --version`)
- Check your internet connection
- Try cloning manually: `git clone --depth 1 https://github.com/leadedge/Spout2.git external/Spout2`

### IntelliSense errors in VSCodium

If IntelliSense shows false errors or missing includes:

- Open Command Palette (Ctrl+Shift+P) > "C/C++: Reset IntelliSense Database"
- Verify the C/C++ extension is installed and active
- Check that `.vscode/c_cpp_properties.json` paths match your SDK installation

### Windows SDK version mismatch

The project targets Windows SDK `10.0.26100.0`. If you have a different SDK version:

1. Open Visual Studio Installer
2. Select "Modify" on Build Tools 2022
3. Go to "Individual Components"
4. Install "Windows 11 SDK (10.0.26100.0)"

### Build succeeds but exe won't start

- Make sure you ran `Developer-Setup.cmd` (Debug builds need `Release/` populated)
- Check that `Release/resources/data/` contains shader files (include.fx, blur*.fx, etc.)
- Check `Release/debug.log` for error details
