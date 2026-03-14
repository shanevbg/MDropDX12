# MDropDX12 Development Guide

How to set up a development environment on a fresh Windows machine, clone the source, and build MDropDX12.

## Automated Setup

For a fresh Windows machine or Hyper-V VM, two scripts are available in `install/`:

| Script | Installs | Use case |
| ------ | -------- | -------- |
| `setup-dev.ps1` | Git, VS Build Tools, VSCodium, clone + build | Full dev environment |
| `install-noide.ps1` | Git, VS Build Tools, clone + build | Build-only (no editor) |

```powershell
# Full setup with VSCodium
powershell -ExecutionPolicy Bypass -File install/setup-dev.ps1

# Build-only (no IDE)
powershell -ExecutionPolicy Bypass -File install/install-noide.ps1
```

Both scripts install Git, VS 2022 Build Tools (MSVC v143, MSBuild, Windows 11 SDK), clone the repo, and run a Release x64 build. The VS Build Tools install typically takes 15-30 minutes.

> **Note:** Windows Sandbox is not suitable for development — the VS Build Tools installer hangs during the Windows SDK install due to Sandbox I/O constraints. Use a Hyper-V VM or a fresh Windows install instead.

## Manual Setup (Prerequisites)

Install the following tools via `winget` from a PowerShell terminal. On fresh installs where `winget` is not available, install it first:

```powershell
# Install winget (App Installer) if not present
Add-AppxPackage -RegisterByFamilyName Microsoft.DesktopAppInstaller_8wekyb3d8bbwe
```

If that doesn't work, download the latest `.msixbundle` from [github.com/microsoft/winget-cli/releases](https://github.com/microsoft/winget-cli/releases/latest) and install it manually:

```powershell
# Download and install winget + dependencies
$ProgressPreference = 'SilentlyContinue'
Invoke-WebRequest -Uri https://aka.ms/getwinget -OutFile winget.msixbundle
Add-AppxPackage -Path winget.msixbundle
Remove-Item winget.msixbundle
```

After installing, close and reopen your PowerShell terminal so `winget` is on your PATH.

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

After installing, open VSCodium and install a C/C++ extension:

1. Open the Extensions panel (Ctrl+Shift+X)
2. Search for `clangd` and install `llvm-vs-code-extensions.vscode-clangd`

> **Note:** The Microsoft `ms-vscode.cpptools` extension is not available on Open VSX (VSCodium's default registry). The `clangd` extension provides equivalent IntelliSense and navigation.

## Clone the Repository

```powershell
git clone https://github.com/shanevbg/MDropDX12.git
cd MDropDX12
```

## First-Time Setup

No manual setup commands are needed. The application self-bootstraps on first run:

1. **Build** the project (see below)
2. **Run** the exe — it detects the empty environment and creates `resources/presets/`, `resources/textures/`, etc.
3. A **Welcome window** appears with:
   - **Browse for Resources Folder...** — point it at an existing folder containing presets (`.milk` files) and textures
   - **Open Shader Import...** — import Shadertoy shaders directly
   - **Open Settings...** — configure paths, audio, display options

The `settings.ini` file is created automatically with defaults when the app first writes settings. No template INI files need to be copied.

> **Legacy**: A `Developer-Setup.cmd` script exists in `Release/` for backward compatibility. It copies `config/*.ini` and `resources/` into the working directory, but this is no longer required.

The Spout2 SDK (the only external dependency) is automatically fetched by the build script on the first build — no manual setup needed.

## Building

### From Terminal

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 Release x64
```

Arguments: `[Debug|Release]` `[x64]` `[Clean]`

Pass `Clean` as the third argument to do a full clean + rebuild (deletes all .obj files first).

The build script will:

1. Locate MSBuild via `vswhere.exe`
2. Clone the Spout2 SDK into `external/Spout2/` if not already present
3. Kill any running `MDropDX12.exe` instance (the linker overwrites the exe)
4. Invoke MSBuild with the specified configuration and platform

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

The exe self-bootstraps on first run — no manual setup of the `Release/` folder is needed.

### Release

After a Release build, the executable is in the build output directory (see table below). To create a portable release zip:

```powershell
powershell -ExecutionPolicy Bypass -File release.ps1
```

This builds Release x64, stages the exe + docs into a zip (`MDropDX12-v{VERSION}-Portable.zip`), validates the zip contents, and updates `Release/MDropDX12.exe`. The version is read automatically from `version.h`.

Options:
- `-SkipBuild` — package from an existing build without rebuilding
- `-DryRun` — show what would be packaged without creating the zip
- `-GitHubRelease` — also create a GitHub release with the zip attached (requires `gh` CLI)

To test a release build manually, run `Release/MDropDX12.exe` after the release script updates it.

## Project Structure

```
MDropDX12/
  build.ps1              -- Build script (locates MSBuild, fetches Spout2, builds)
  release.ps1            -- Release script (build + package + validate + optional GitHub release)
  MDropDX12.sln          -- Visual Studio solution file
  CLAUDE.md              -- AI assistant project context and critical warnings
  src/
    mDropDX12/           -- Main visualizer engine (C++17, DX12, Win32 API)
      engine.vcxproj     -- MSBuild project file
      engine.cpp/h       -- Core engine, render loop, state management
      dxcontext.cpp/h    -- DX12 device, swap chain, descriptor heaps
      milkdropfs.cpp     -- Preset shader generation (warp/comp/blur)
      engine_settings_ui.cpp -- Settings window (5-tab UI: General, Tools, System, Files, About)
      engine_input.cpp   -- Keyboard/mouse/hotkey handlers
      engine_presets.cpp  -- Preset loading, parsing, cross-fading
      state.cpp/h        -- Preset state and parameter storage
      textmgr.cpp/h      -- DX12 font atlas text rendering (GDI atlas + sprite quads)
      ...
    ns-eel2/             -- Expression evaluator (Cockos WDL ns-eel2, x64 JIT)
  external/              -- Auto-fetched dependencies (Spout2 SDK)
  resources/
    presets/              -- Preset collections (.milk files)
    textures/            -- Texture files referenced by presets
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

- The exe self-bootstraps — it creates required directories on first run
- All shaders are embedded in the exe — no external `resources/data/` directory is needed
- For Debug builds, the working directory is `Release/` (set in `.vscode/launch.json`)
- Check `debug.log` in the working directory for error details

## Debug Logging

MDropDX12 has a leveled logging system controlled via `settings.ini`:

| INI Key     | Values | Default | Description                                    |
| ----------- | ------ | ------- | ---------------------------------------------- |
| `LogLevel`  | 0–4    | 3       | 0=Off, 1=Error, 2=Warn, 3=Info, 4=Verbose     |
| `LogOutput` | 1–3    | 3       | 1=File only, 2=OutputDebugString only, 3=Both  |

These can also be changed at runtime via **Settings -> About** tab (log level radio buttons and output checkboxes).

Log output goes to `debug.log` in the base directory (rotated to `debug.prev.log` on startup).

### Level-gated macros

Code uses `DLOG_ERROR`, `DLOG_WARN`, `DLOG_INFO`, `DLOG_VERBOSE` macros (defined in `utility.h`) which skip all formatting work when the current log level would suppress the message. Prefer these over raw `DebugLogA`/`DebugLogAFmt` calls.

```cpp
DLOG_VERBOSE("DX12: PSO created for %s, slots=%d", name, count);
```

### Diagnostic files (Verbose only)

When `LogLevel=4` (Verbose), diagnostic dump files are written to the base directory:

| File | Contents |
| ---- | -------- |
| `diag_comp_shader.txt` | Assembled comp/Image shader HLSL sent to compiler |
| `diag_warp_shader.txt` | Assembled warp shader HLSL sent to compiler |
| `diag_converter_image.txt` | GLSL->HLSL converter output for Image pass |
| `diag_converter_bufferA.txt` | GLSL->HLSL converter output for Buffer A pass |
| `diag_cacheparams.txt` | Shader constant/sampler binding diagnostics |
| `diag_bindings.txt` | Resource binding slot assignments |

Error files (`diag_*_shader_error.txt`) are always written on compile failure regardless of log level — these are needed by the Import UI error display.

When debugging Shadertoy imports, the project name from the loaded `.json` file is included in diagnostic headers (`project=filename.json`) and the preset description (`m_szDesc`) is updated to match, so diag files clearly indicate which import is being debugged rather than showing the last regular preset name.

## Shadertoy Import Workflow

The Shader Import window (opened from Settings) converts Shadertoy GLSL to HLSL for live preview:

1. **Load Import** — loads a `.json` project file containing GLSL source for each pass
2. **Convert** — runs the GLSL->HLSL converter pipeline
3. **Apply** — compiles HLSL and activates the Shadertoy render path
4. **Save** — exports as `.milk3` (Shadertoy preset) or `.milk` (legacy)
5. **Save Import** — saves the project back to `.json` for later editing

Import projects (`.json`) store raw GLSL, channel mappings, and notes. The `.milk3` preset format stores converted HLSL and is what the visualizer loads at runtime.

See `docs/GLSL_importing.md` for details on the GLSL->HLSL conversion pipeline.

## Coding Patterns

### File Organization Convention

Engine source files follow a split pattern:

- **`engine_<feature>.cpp`** — Engine:: business logic (persistence, dispatch, lifecycle)
- **`engine_<feature>_ui.cpp`** — ToolWindow subclass + `Open<Feature>Window` / `Close<Feature>Window` bridge methods only

Examples: `engine_midi.cpp` (MIDI persistence, device lifecycle, knob/button dispatch) vs `engine_midi_ui.cpp` (MidiWindow ToolWindow subclass and open/close bridges).

### Where to Put New Code

| Domain | File |
| ------ | ---- |
| DX12 rendering, shader passes | `milkdropfs.cpp` |
| Preset loading/parsing/blending | `engine_presets.cpp` |
| Shader compilation, text assembly | `engine_shaders.cpp` |
| GLSL->HLSL conversion | `engine_shader_import_ui.cpp` |
| Settings persistence, theme, user defaults, folder picker | `engine_config.cpp` |
| Sprite lifecycle, INI I/O | `engine_sprites.cpp` |
| MIDI persistence, device lifecycle, dispatch | `engine_midi.cpp` |
| Display outputs, Spout sender | `engine_displays.cpp` |
| Keyboard/mouse input, hotkey dispatch | `engine_input.cpp` |
| Texture loading, fallback logic | `engine_textures.cpp` |
| Message/supertext system | `engine_messages.cpp` |
| Text animation profiles | `engine_textanim.cpp` / `engine_textanim_ui.cpp` |
| UI window for feature X | `engine_<x>_ui.cpp` |

### Engine:: Method Distribution

All methods are declared in `engine.h` but defined across 40+ `.cpp` files. When looking for a method's implementation, check the file matching the method's domain (above table), not just `engine.cpp`.

### Shared Helpers

Standalone helpers used across multiple `engine_*.cpp` files live in `engine_helpers.h` as `inline` functions or `extern` declarations:

- `FormatSpriteSection` / `FormatSpriteSectionA` — sprite INI section name formatting
- `MakeRelativeSpritePath` — convert absolute texture paths to relative
- `StripNamedGroups` — regex named capture group stripping
- `ReadFileToString`, `StripComments`, `ConvertLLCto1310` — text processing
- `g_settingsDesc[]`, `SettingDesc`, `SettingType` — settings screen types

## Common Pitfalls

### BS_OWNERDRAW Controls

All ToolWindow controls use `BS_OWNERDRAW`. This means:

- `IsDlgButtonChecked()` / `CheckDlgButton()` / `BM_GETCHECK` **silently return 0** — they don't work with owner-draw buttons
- Use `ToolWindow::IsChecked(id)` / `SetChecked(id, bool)` instead
- **Checkboxes** are auto-toggled by the base class `WndProc` before `DoCommand` is called
- **Radio groups** must be toggled manually in `DoCommand` (the base class doesn't know group membership)

See `docs/tool_window.md` for the full ToolWindow reference.

### HWND_NOTOPMOST Spelling

`HWND_NOTOPMOST` has **one T** — never spell it `HWND_NOTTOPMOST`. The compiler won't catch this because it's a `#define` value.

### Wide Strings for File Paths

All file paths use `wchar_t` / `std::wstring`. Never use `char*` for paths — Windows APIs return wide strings and non-ASCII characters in filenames will be silently corrupted.

### Descriptor Heap Ordering

Font atlas SRV slots must be allocated **before** `m_srvSlotBaseline` is set. `ResetDynamicDescriptors()` rewinds to baseline on resize — anything allocated after baseline gets reclaimed.

- Font atlases are **permanent** (allocated before baseline)
- Render targets (VS[0], VS[1], blur) are **dynamic** (allocated after baseline)
- `AllocateDX9Stuff()` builds font atlases first, advances baseline, then creates render targets
- `ResetBufferAndFonts()` must rebuild font atlases after `CleanUpFonts()` (they're destroyed)

### Thread Safety

- Use `std::atomic` for cross-thread flags (e.g., `m_bScreenshotRequested`, `m_bMirrorStylesDirty`)
- **Render thread** owns DX12 resources and all HUD text rendering (CTextManager)
- **Message pump thread** owns HWNDs
- **IPC thread** runs a hidden 1x1 window for `WM_COPYDATA` from Milkwave Remote
- Use the `RenderCommand` queue (`EnqueueRenderCmd`) for cross-thread communication
- ToolWindows run on **their own threads** — don't access DX12 from ToolWindow code

### Sampler Architecture

4 shared `SamplerState` objects (s0–s3) cover all preset sampling needs. Textures use `Texture2D` t-registers (~128 limit). `#define tex2D` macro routes through `_samp_lw`. Special-mode samplers use text substitution in `LoadShaderFromMemory`.

- `s0` = LINEAR + WRAP
- `s1` = LINEAR + CLAMP
- `s2` = POINT + CLAMP
- `s3` = POINT + WRAP
- Blur uses `_samp_lc` (CLAMP) via text substitution

### Shadertoy Mode Flags

Use **`m_bLoadingShadertoyMode`** (not `m_bShadertoyMode`) in `LoadShaderFromMemory`. The latter isn't set until after shader compilation — too late for shader text generation. `m_bLoadingShadertoyMode` is set in `LoadPreset` before the async thread starts.

### Named Pipe IPC

MDropDX12 uses Named Pipe IPC (`\\.\pipe\Milkwave_<PID>`) for communication with Milkwave Remote, the MCP server, and other clients. The pipe server (`pipe_server.cpp`) runs on a dedicated thread with duplex message-mode, multi-instance support (each client gets its own handler thread).

**Message format**: Raw wide strings, newline-delimited. Two dispatch paths:

1. **Signals** — prefixed with `SIGNAL|`, dispatched via `DispatchSignal()` which posts a `WM_APP + offset` message to the render window. Signal table is in `pipe_server.cpp`:

   | Signal | Description |
   | ------ | ----------- |
   | `SIGNAL\|NEXT_PRESET` | Next preset |
   | `SIGNAL\|PREV_PRESET` | Previous preset |
   | `SIGNAL\|CAPTURE` | Screenshot |
   | `SIGNAL\|WATERMARK` | Toggle watermark mode |
   | `SIGNAL\|FULLSCREEN` | Toggle fullscreen |
   | `SIGNAL\|BORDERLESS_FS` | Toggle borderless fullscreen |
   | `SIGNAL\|STRETCH` | Toggle stretch |
   | `SIGNAL\|MIRROR` | Toggle mirror |
   | `SIGNAL\|MIRROR_WM` | Toggle mirror watermark |
   | `SIGNAL\|SPRITE_MODE` | Toggle sprite mode |
   | `SIGNAL\|MESSAGE_MODE` | Toggle message mode |
   | `SIGNAL\|COVER_CHANGED` | Notify cover art changed |
   | `SIGNAL\|SHOW_COVER` | Show cover art |
   | `SIGNAL\|SETVIDEODEVICE=N` | Set video device index |
   | `SIGNAL\|ENABLEVIDEOMIX=0\|1` | Enable/disable video mix |
   | `SIGNAL\|ENABLESPOUTMIX=0\|1` | Enable/disable Spout mix |
   | `SIGNAL\|SET_INPUTMIX_OPACITY=N` | Set input mix opacity |
   | `SIGNAL\|SET_INPUTMIX_ONTOP=0\|1` | Set input mix on-top |
   | `SIGNAL\|SET_INPUTMIX_LUMAKEY=threshold\|softness` | Set luma key params |

   To add a new signal: add an entry to `s_signalTable[]` in `pipe_server.cpp`, define a `WM_MW_*` message in `engine_helpers.h`, handle it in `App.cpp`'s message pump.

2. **Commands** — everything else is forwarded to `LaunchMessage()` in `engine_messages.cpp`. Commands are matched via `wcsncmp` and include:

   | Command | Description |
   | ------- | ----------- |
   | `PRESET=path` | Load preset by path |
   | `OPACITY=0.0-1.0` | Set window opacity |
   | `STATE` | Query current state (returns JSON-like response) |
   | `CAPTURE` | Screenshot to file |
   | `SHUTDOWN` | Clean shutdown |
   | `SET_LOGLEVEL=0-4` | Set log level |
   | `GET_LOGLEVEL` | Query log level |
   | `CLEAR_LOGS` | Delete all log files |
   | `SET_DIR=path` | Change preset directory |
   | `LOAD_LIST=path` | Load a preset list |
   | `ENUM_LISTS` | List available preset lists |
   | `TRACK\|title\|artist\|album` | Set track info |
   | `MSG\|text` | Display overlay message |
   | `COL_HUE=0.0-1.0` | Set color hue |
   | `COL_SATURATION=0.0-1.0` | Set saturation |
   | `COL_BRIGHTNESS=-1.0-1.0` | Set brightness |
   | `FFT_ATTACK=value` | Set FFT attack |
   | `FFT_DECAY=value` | Set FFT decay |
   | `SHADER_IMPORT=path` | Import shader from file |
   | `GET_RENDER_DIAG` | Get render diagnostics |
   | `GET_AUDIO_DIAG` | Get audio diagnostics |
   | `GET_EEL_STATE` | Get expression evaluator state |
   | `MOVE_TO_DISPLAY=N` | Move to display index |
   | `SET_WINDOW=x,y,w,h` | Set window position/size |

   To add a new command: add an `else if (wcsncmp(sMessage, L"MY_CMD", 6) == 0)` block in `LaunchMessage()`. Send responses via `g_pipeServer.Send(response)`.

### TCP Server

The TCP server (`tcp_server.cpp`) provides network-based remote control for LAN clients. It handles only TCP-specific concerns (framing, authentication, connection management) and forwards all commands through the same IPC dispatch path as the Named Pipe.

- **Port**: 9270 (default, configurable via INI)
- **Framing**: 4-byte little-endian length prefix + UTF-8 payload
- **Authentication**: Clients must send `AUTH|<pin>|<deviceId>|<deviceName>` first. Authorized devices are stored in INI `[AuthorizedDevices]` section.
- **Responses**: `AUTH_OK`, `AUTH_FAIL|<reason>`, `PONG` (for `PING`), or command-specific responses routed to the requesting client via `g_respondingTcpClient`
- **Limits**: 16 concurrent clients, 60-second inactivity timeout, 64KB receive buffer
- **mDNS**: Service advertised as `_milkwave._tcp` for automatic discovery

**Command routing** (App.cpp `onMessage` handler): TCP does NOT process commands itself — it forwards them to the existing IPC path:

- `SIGNAL|*` messages → `g_pipeServer.DispatchSignal()` (same as Named Pipe)
- All other messages → `PostMessage(WM_MW_IPC_MESSAGE)` → `LaunchMessage()` (same as Named Pipe)

After authentication, clients send the exact same commands and signals as Named Pipe clients. TCP is a transport layer, not a command handler.

### mDNS and UDP Beacon

LAN discovery uses two mechanisms in parallel (`mdns_advertiser.cpp`):

1. **mDNS (DNS-SD)** — registers as `MDropDX12-<hostname>._milkwave._tcp.local` via Windows DNS-SD APIs (`DnsServiceRegister`). Loaded dynamically from `dnsapi.dll` so the binary runs on older Windows builds where the APIs aren't available. TXT record carries `version=1` and `pid=<process_id>`.

2. **UDP broadcast beacon** — reliable fallback that always works regardless of mDNS support. Sends `MDROP_BEACON|<name>|<tcpPort>|<pid>` to `255.255.255.255` on port 9271 every 3 seconds. Clients listen on UDP 9271 to discover instances, then connect to the advertised TCP port.

Both start in `App.cpp` after the TCP server is running:

```cpp
g_mdns.Register("MDropDX12-" + hostname, g_tcpServer.GetPort(), GetCurrentProcessId());
```

Both are stopped on shutdown via `g_mdns.Unregister()` which joins the beacon thread and deregisters from DNS-SD.

**Key files**: `mdns_advertiser.h` (class declaration), `mdns_advertiser.cpp` (implementation), `App.cpp` (startup/shutdown calls).

**Ports**:

| Port | Protocol | Purpose |
| ---- | -------- | ------- |
| 9270 | TCP | Remote control (commands, auth) |
| 9271 | UDP | Broadcast beacon (discovery only) |

## Adding ToolWindows

ToolWindows are standalone windows that run on their own threads with independent always-on-top, position persistence, and dark theme support. All controls use `BS_OWNERDRAW` (see `docs/tool_window.md`).

### Creating a New ToolWindow

1. **Declare** the class in `tool_window.h`:

   ```cpp
   class MyFeatureWindow : public ToolWindow {
   public:
       MyFeatureWindow(Engine* pEngine);
   protected:
       TOOLWINDOW_META(L"My Feature", L"MyFeatureWndClass", L"MyFeature",
                       IDC_MY_PIN, IDC_MY_FONT_PLUS, IDC_MY_FONT_MINUS, 400, 300)
       void DoBuildControls() override;
       LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
       // Optional overrides:
       // LRESULT DoHScroll(HWND hCtrl, int code, int pos) override;
       // LRESULT DoNotify(NMHDR* pNMHDR) override;
       // void DoDestroy() override;
   };
   ```

   `TOOLWINDOW_META` generates boilerplate: window title, class name, INI section, control IDs for pin/font buttons, default width/height.

2. **Implement** in `engine_myfeature_ui.cpp`:

   ```cpp
   #include "tool_window.h"
   #include "engine.h"
   #include "engine_helpers.h"

   MyFeatureWindow::MyFeatureWindow(Engine* pEngine) : ToolWindow(pEngine, 400, 300) {}

   void MyFeatureWindow::DoBuildControls() {
       BuildBaseControls();  // creates pin, font+/- buttons
       int y = 40;
       CreateLabel(m_hWnd, L"My Label:", 10, y, 80, 20);
       CreateCheck(m_hWnd, IDC_MY_CHECK, L"Enable", 100, y, 100, 20);
       TrackControl(GetDlgItem(m_hWnd, IDC_MY_CHECK));  // register for dark theme
   }

   LRESULT MyFeatureWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
       switch (id) {
       case IDC_MY_CHECK:
           m_pEngine->m_bMyFeature = IsChecked(IDC_MY_CHECK);
           return 0;
       }
       return -1;  // not handled
   }
   ```

3. **Register** in `engine.h`:

   ```cpp
   std::unique_ptr<MyFeatureWindow> m_myFeatureWindow;
   void OpenMyFeatureWindow()  { OpenToolWindow(m_myFeatureWindow); }
   void CloseMyFeatureWindow() { CloseToolWindow(m_myFeatureWindow); }
   ```

4. **Initialize** in `engine.cpp` constructor:

   ```cpp
   m_myFeatureWindow = std::make_unique<MyFeatureWindow>(this);
   ```

5. **Add to vcxproj**: Include `engine_myfeature_ui.cpp` in the project's ClCompile list.

Key rules:

- Use `IsChecked(id)` / `SetChecked(id, bool)` — never `IsDlgButtonChecked()`
- Checkboxes auto-toggle before `DoCommand`; radio groups must be toggled manually
- Don't access DX12 resources from ToolWindow code (wrong thread)
- See `engine_colors_ui.cpp` for a simple example, `engine_midi_ui.cpp` for a complex one

## Expanding Menus and Reordering Items

### ButtonBoard Context Menu

The ButtonBoard's right-click "Assign Action..." submenu is built by `BuildActionSubMenu()` in `engine_board_ui.cpp`. It auto-populates from the hotkey table, grouped by category (`HKCAT_NAVIGATION`, `HKCAT_WINDOW`, `HKCAT_VISUAL`, etc.).

**Adding IPC signal actions** (actions that aren't hotkeys):

In `BuildActionSubMenu()`, append items to the relevant category submenu after the hotkey loop, using IDs starting at 2000:

```cpp
if (cat == HKCAT_WINDOW) {
    AppendMenuW(hCatMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hCatMenu, MF_STRING, 2000, L"Watermark");
    itemCount++;
}
```

Then handle the ID in `ShowSlotContextMenu()`:

```cpp
if (cmd == 2000) {
    s.action  = ButtonAction::ScriptCommand;
    s.payload = L"SIGNAL|WATERMARK";
    s.label   = L"Watermark";
    m_pPanel->Invalidate();
    SaveBoard();
}
```

**Reordering hotkey-based items**: Items appear in the order hotkeys are defined in `InitHotkeyDefaults()` (`engine_hotkeys.cpp`). Move the `HK_DEF` call to change the item's position within its category.

**Reordering categories**: The loop iterates `HKCAT_NAVIGATION` through `HKCAT_MISC`. To change the category order in the menu, modify the loop range or iterate a custom order array.

### Hotkey Categories

Categories are defined in `hotkeys.h`:

| Enum | Menu Name | Description |
| ---- | --------- | ----------- |
| `HKCAT_NAVIGATION` | Navigation | Preset navigation, browser, save |
| `HKCAT_VISUAL` | Visual | Opacity, wave mode, zoom (auto-grouped by prefix) |
| `HKCAT_MEDIA` | Media | Track info, song display |
| `HKCAT_WINDOW` | Window | Fullscreen, always-on-top, FPS, borderless |
| `HKCAT_TOOLS` | Tools | Open tool windows |
| `HKCAT_SHADER` | Shader | Inject effects, quality |
| `HKCAT_MISC` | Misc | Miscellaneous actions |
| `HKCAT_SCRIPT` | Script | User-defined script commands |
| `HKCAT_LAUNCH` | Launch | Launch external applications |

The Visual category is special — `BuildActionSubMenu()` auto-groups items by their first word (e.g., all "Opacity ..." actions become an "Opacity" submenu).

### Adding Actions Without Hotkeys

Not all actions need a hotkey binding. For actions triggered only from UI (ButtonBoard, context menus, IPC), use `ButtonAction::ScriptCommand` with a pipe command payload (e.g., `SIGNAL|WATERMARK` or `OPACITY=0.5`). The button's `ExecuteSlot()` dispatches it via `PostIPCMessage()` which routes through `LaunchMessage()`.

## DX12 Rendering Pipeline

### Render Target Ping-Pong

Two render targets (VS[0] and VS[1]) ping-pong each frame:

1. **Warp pass**: Reads VS[0] -> applies warp mesh distortion -> writes to VS[1]
2. **Shape/wave injection**: Custom shapes and waves drawn directly into VS[1]
3. **Comp pass**: Reads VS[1] -> applies comp mesh + comp shader -> writes to backbuffer

### Binding Layout

- `BINDING_BLOCK_SIZE = 32` descriptors per texture set
- `PASSES_PER_FRAME = 4` (warp + bufferA + bufferB + comp)
- Static samplers: s0=LINEAR+WRAP, s1=LINEAR+CLAMP, s2=POINT+CLAMP, s3=POINT+WRAP

### Key Differences from DX9

- **No projection matrix**: DX12 vertex shaders output directly to clip space
- **No half-texel offset**: DX12 pixel centers are at integer+0.5 (DX9 at integers)
- **No Y-flip compensation**: DX9 used `OrthoLH(2,-2)` which negated Y; DX12 passthrough VS doesn't flip
- **Post-processing via shader**: `RenderInjectEffect()` handles brighten/darken/solarize/invert via pixel shader (not blend states)
