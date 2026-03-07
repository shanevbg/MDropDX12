# ToolWindow — Standalone Window Base Class

ToolWindow is the base class for all standalone tool windows in MDropDX12 (Settings, Hotkeys, MIDI, Presets, etc.). Each window runs on its own thread with an independent message pump, supports dark theme painting, always-on-top pin, cross-window font sync, and INI-persisted position/size.

**Source files**: `src/mDropDX12/tool_window.h`, `src/mDropDX12/tool_window.cpp`

## Architecture

```
┌─────────────────────────────────────────┐
│ Render Thread (main)                    │
│   - DX12 rendering                     │
│   - Owns render HWND                   │
└─────────────────────────────────────────┘
        ↕ PostMessage / atomic flags
┌──────────────┐ ┌──────────────┐ ┌──────────────┐
│ Settings     │ │ Hotkeys      │ │ MIDI         │  ... (one thread per window)
│ Thread       │ │ Thread       │ │ Thread       │
│ - HWND       │ │ - HWND       │ │ - HWND       │
│ - GetMessage │ │ - GetMessage │ │ - GetMessage │
└──────────────┘ └──────────────┘ └──────────────┘
```

Each ToolWindow:
- Creates its own `HWND` on a dedicated `std::thread`
- Runs a `GetMessage` loop on that thread
- Paints with owner-draw controls for dark theme support
- Persists window position/size and active tab to `settings.ini`

## Creating a New Subclass — Step by Step

### 1. Define Control IDs

In `engine_helpers.h`, add unique IDs for your window's controls:

```cpp
// MyWindow (9400-9420)
#define IDC_MW_MYWIN_PIN          9400
#define IDC_MW_MYWIN_FONT_PLUS    9401
#define IDC_MW_MYWIN_FONT_MINUS   9402
#define IDC_MW_MYWIN_MY_CHECK     9403
#define IDC_MW_MYWIN_MY_RADIO_A   9404
#define IDC_MW_MYWIN_MY_RADIO_B   9405
#define IDC_MW_MYWIN_MY_BUTTON    9406
```

Every window needs at minimum: `_PIN`, `_FONT_PLUS`, `_FONT_MINUS`. These are handled by the base class for the always-on-top pin button and font size +/- buttons.

**Why per-window IDs?** Win32 `GetDlgItem(hwnd, id)` requires unique IDs within a window. Multiple tool windows can be open simultaneously, each with their own HWND. The base class dispatches pin/font buttons via virtual `GetPinControlID()` etc. to match the correct ID. This is the standard Win32 pattern.

### 2. Declare the Class

In `tool_window.h`:

```cpp
class MyWindow : public ToolWindow {
public:
  MyWindow(Engine* pEngine);

protected:
  // Required overrides
  const wchar_t* GetWindowTitle() const override { return L"My Window"; }
  const wchar_t* GetWindowClass() const override { return L"MDropDX12MyWnd"; }
  const wchar_t* GetINISection() const override  { return L"MyWindow"; }
  int GetPinControlID() const override       { return IDC_MW_MYWIN_PIN; }
  int GetFontPlusControlID() const override  { return IDC_MW_MYWIN_FONT_PLUS; }
  int GetFontMinusControlID() const override { return IDC_MW_MYWIN_FONT_MINUS; }

  // Optional: minimum window size (defaults: 400x350)
  int GetMinWidth() const override  { return 400; }
  int GetMinHeight() const override { return 350; }

  // Build all child controls
  void DoBuildControls() override;

  // Handle button clicks (optional — only if you have interactive controls)
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
};
```

### 3. Add to Engine

In `engine.h`:

```cpp
std::unique_ptr<MyWindow> m_myWindow;
void OpenMyWindow();
void CloseMyWindow();
```

### 4. Implement

In a new `engine_my_ui.cpp`:

```cpp
#include "engine.h"
#include "tool_window.h"

using namespace mdrop;

void Engine::OpenMyWindow() {
    if (!m_myWindow)
        m_myWindow = std::make_unique<MyWindow>(this);
    m_myWindow->Open();
}

void Engine::CloseMyWindow() {
    if (m_myWindow)
        m_myWindow->Close();
}

MyWindow::MyWindow(Engine* pEngine) : ToolWindow(pEngine, 400, 500) {}

void MyWindow::DoBuildControls() {
    auto L = BuildBaseControls();       // Creates header (pin, font +/-)
    HFONT hFont = GetFont();
    HFONT hFontBold = GetFontBold();
    HWND hw = m_hWnd;
    int x = L.x, y = L.y, rw = L.rw, lineH = L.lineH, gap = L.gap;

    // Add your controls:
    TrackControl(CreateLabel(hw, L"My Label:", x, y, rw, lineH, hFontBold));
    y += lineH + 4;

    TrackControl(CreateCheck(hw, L"Enable feature",
        IDC_MW_MYWIN_MY_CHECK, x + 8, y, rw - 16, lineH, hFont, true));
    y += lineH + 2;

    TrackControl(CreateBtn(hw, L"Do Something",
        IDC_MW_MYWIN_MY_BUTTON, x, y, 120, lineH + 8, hFont));
}

LRESULT MyWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
    if (code != BN_CLICKED) return -1;

    switch (id) {
    case IDC_MW_MYWIN_MY_CHECK:
        // Checkbox was auto-toggled by base class — just read the new state
        if (IsChecked(IDC_MW_MYWIN_MY_CHECK))
            m_pEngine->EnableFeature();
        else
            m_pEngine->DisableFeature();
        return 0;

    case IDC_MW_MYWIN_MY_BUTTON:
        m_pEngine->DoSomething();
        return 0;
    }
    return -1;
}
```

### 5. Register for Cleanup

In `engine.cpp`, add to `CleanUpMyNonDx9Stuff()`:
```cpp
CloseMyWindow();
```

In `tool_window.cpp`, add to `BroadcastFontSync()`:
```cpp
if (m_myWindow && m_myWindow->IsOpen() && m_myWindow->GetHWND() != hSender)
    PostMessage(m_myWindow->GetHWND(), WM_MW_REBUILD_FONTS, 0, 0);
```

Add the .cpp file to `engine.vcxproj`.

## Control Creation Helpers

All helpers are global functions in `engine.cpp` (namespace `mdrop`). All create `BS_OWNERDRAW` buttons for dark theme support.

| Helper | Signature |
|--------|-----------|
| `CreateLabel` | `(HWND parent, const wchar_t* text, int x, int y, int w, int h, HFONT, bool visible=true)` — No ID (static text) |
| `CreateCheck` | `(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT, bool checked, bool visible=true)` |
| `CreateRadio` | `(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT, bool checked, bool firstInGroup, bool visible=true)` |
| `CreateBtn` | `(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT, bool visible=true)` |
| `CreateSlider` | `(HWND parent, int id, int x, int y, int w, int h, int min, int max, int pos, bool visible=true)` |
| `CreateEdit` | `(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT, DWORD extraStyle=0, bool visible=true)` |

All returned HWNDs should be passed to `TrackControl()` (or `TrackPageControl()` for tabbed windows).

## Checkbox and Radio State

### CRITICAL: Never use `IsDlgButtonChecked()` / `CheckDlgButton()`

All controls are `BS_OWNERDRAW`. Win32's standard button state functions **silently fail** — they always return 0 for checkboxes and have no effect when setting state. This has caused real bugs.

### Use `IsChecked()` / `SetChecked()` instead

```cpp
// Read state (replaces IsDlgButtonChecked):
bool checked = IsChecked(IDC_MY_CHECKBOX);

// Write state (replaces CheckDlgButton):
SetChecked(IDC_MY_CHECKBOX, true);
```

These are protected methods on `ToolWindow`. They read/write the `"Checked"` window property that the owner-draw system uses for rendering.

### Checkboxes: Auto-toggled by Base Class

When a user clicks a checkbox, the base class WndProc **automatically toggles** its state before calling `DoCommand()`. Your `DoCommand()` receives the post-toggle state.

```cpp
case IDC_MY_CHECKBOX:
    // State already toggled — just read it
    bool isNowChecked = IsChecked(IDC_MY_CHECKBOX);
    m_pEngine->SetOption(isNowChecked);
    return 0;
```

You do **not** need to toggle checkboxes yourself.

### Radio Groups: Subclass Must Handle

Radio buttons are NOT auto-toggled because the base class doesn't know which radios belong to the same group. Toggle them in `DoCommand()`:

```cpp
LRESULT MyWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
    if (code != BN_CLICKED) return -1;

    // Toggle radio group
    HWND hCtrl = (HWND)lParam;
    bool bIsRadio = (bool)(intptr_t)GetPropW(hCtrl, L"IsRadio");
    if (bIsRadio) {
        static const int groupIDs[] = { IDC_MY_RADIO_A, IDC_MY_RADIO_B, IDC_MY_RADIO_C };
        for (int rid : groupIDs) {
            if (rid == id) {
                for (int gid : groupIDs) {
                    HWND hR = GetDlgItem(hWnd, gid);
                    if (hR) {
                        SetPropW(hR, L"Checked", (HANDLE)(intptr_t)(gid == id ? 1 : 0));
                        InvalidateRect(hR, NULL, TRUE);
                    }
                }
                break;
            }
        }
    }

    // Handle the selection
    switch (id) {
    case IDC_MY_RADIO_A: /* ... */ return 0;
    case IDC_MY_RADIO_B: /* ... */ return 0;
    }
    return -1;
}
```

## BuildBaseControls and BaseLayout

`BuildBaseControls()` creates the standard header (font +/- buttons top-left, pin button top-right) and returns layout metrics:

```cpp
struct BaseLayout {
    int y;        // Y position below header — start your controls here
    int lineH;    // Line height for current font (text height + padding, min 20px)
    int gap;      // Standard gap between groups (6px)
    int x;        // Left margin (16px)
    int rw;       // Row width (clientW - 2*margin)
    int clientW;  // Full client area width
};
```

Typical usage:
```cpp
void MyWindow::DoBuildControls() {
    auto L = BuildBaseControls();
    int y = L.y + L.gap;  // start below header with a small gap
    // ... create controls at (L.x, y) with width L.rw ...
}
```

## Tab Control Pattern

For multi-page windows:

```cpp
void MyWindow::DoBuildControls() {
    auto L = BuildBaseControls();
    HWND hw = m_hWnd;
    int lineH = L.lineH;

    // Create tabs
    static const wchar_t* tabNames[] = { L"General", L"Advanced" };
    RECT rcTab = BuildTabControl(IDC_MY_TAB, tabNames, 2, L.x, L.y, L.rw, L.clientW - L.y - 8);
    int tabTop = rcTab.top + 8;
    int y;

    // Page 0: General
    y = tabTop;
    PAGE_CTRL(0, CreateLabel(hw, L"Option:", L.x + 8, y, 100, lineH, GetFont()));
    y += lineH + 4;
    // ... more controls ...

    // Page 1: Advanced
    y = tabTop;
    PAGE_CTRL(1, CreateCheck(hw, L"Enable debug", IDC_MY_DEBUG, L.x + 8, y, 200, lineH, GetFont(), false));
    y += lineH + 4;

    SelectInitialTab();  // Restore persisted tab — MUST be last
}
```

Where `PAGE_CTRL` is a macro: `TrackPageControl(page, control)`. The base class handles tab switching, showing/hiding page controls, and persisting the active tab to INI.

## Virtual Override Reference

| Method | Required? | Default | Purpose |
|--------|-----------|---------|---------|
| `GetWindowTitle()` | Yes | — | Window title bar text |
| `GetWindowClass()` | Yes | — | Unique WNDCLASS name (e.g., `L"MDropDX12MyWnd"`) |
| `GetINISection()` | Yes | — | INI section for position/size persistence |
| `GetPinControlID()` | Yes | — | Control ID for always-on-top pin button |
| `GetFontPlusControlID()` | Yes | — | Control ID for font size + button |
| `GetFontMinusControlID()` | Yes | — | Control ID for font size - button |
| `GetMinWidth()` | No | 400 | Minimum resize width |
| `GetMinHeight()` | No | 350 | Minimum resize height |
| `DoBuildControls()` | Yes | — | Create all child controls |
| `DoCommand()` | No | returns -1 | Handle WM_COMMAND (button clicks) |
| `DoHScroll()` | No | returns -1 | Handle slider/trackbar changes |
| `DoNotify()` | No | returns -1 | Handle WM_NOTIFY (ListView, etc.) |
| `DoMessage()` | No | returns -1 | Catch-all for other messages |
| `DoDestroy()` | No | no-op | Cleanup before window destruction |
| `OnResize()` | No | RebuildFonts() | Handle WM_SIZE (custom layout) |
| `OnAlreadyOpen()` | No | SetForegroundWindow | Called when Open() is called while already open |
| `GetCommonControlFlags()` | No | 0 | ICC flags for InitCommonControlsEx |
| `AcceptsDragDrop()` | No | false | Enable OLE drag-and-drop |
| `ForwardAllKeys()` | No | false | Forward all keystrokes to render window |

## Existing Subclasses

| Class | File | Min Size | Notes |
|-------|------|----------|-------|
| SettingsWindow | engine_settings_ui.cpp | 500x450 | 9 tabs, custom OnResize |
| HotkeysWindow | engine_hotkeys_ui.cpp | 560x480 | ListView + modal dialogs |
| MidiWindow | engine_midi_ui.cpp | 580x550 | ListView, spin controls |
| PresetsWindow | engine_presets_ui.cpp | 420x400 | Directory browser |
| DisplaysWindow | engine_displays_ui.cpp | 400x350 | Spout/video input |
| SongInfoWindow | engine_songinfo_ui.cpp | 380x400 | Track metadata |
| ButtonBoardWindow | engine_board_ui.cpp | 300x250 | 4x4 grid, ForwardAllKeys |
| SpritesWindow | engine_sprites_ui.cpp | 500x700 | ListView + modal |
| MessagesWindow | engine_messages_ui.cpp | 420x480 | Message queue |
| VideoEffectsWindow | engine_video_effects_ui.cpp | 380x550 | 3 tabs |
| VFXProfileWindow | engine_vfx_profiles_ui.cpp | 280x350 | JSON profile picker |
| ShaderEditorWindow | engine_shader_import_ui.cpp | 500x400 | Code editor |
| ShaderImportWindow | engine_shader_import_ui.cpp | 350x450 | GLSL converter |
| WorkspaceLayoutWindow | engine_workspace_layout_ui.cpp | 380x700 | Window tiler |
| WelcomeWindow | engine_shader_import_ui.cpp | 300x400 | First-run prompt |

## FAQ

### Why are all controls BS_OWNERDRAW?

For dark theme support. Standard Win32 buttons/checkboxes cannot be custom-painted in dark mode without owner-draw. The base WndProc handles `WM_DRAWITEM` and calls `DrawOwnerCheckbox()`, `DrawOwnerRadio()`, `DrawOwnerButton()` in `engine.cpp` to render controls with the current theme colors.

### Why does each window need its own PIN / FONT_PLUS / FONT_MINUS IDs?

Win32 `GetDlgItem(hwnd, id)` identifies controls by integer ID within a single window. The base class WndProc compares `id == GetPinControlID()` to handle the pin button. If two windows shared the same ID, there would be no ambiguity (different HWNDs), but the virtual method pattern allows the base class to dispatch correctly without knowing the concrete subclass. Each window's IDs are defined in `engine_helpers.h` in a reserved range.

### Why doesn't the base class auto-toggle radio groups?

Radio groups vary per subclass — some windows have 1 group, some have 3, some have none. The base class has no way to know which radio buttons belong together. Auto-toggling checkboxes is safe because every checkbox toggles independently. Radio groups require domain knowledge (which IDs form a group), so the subclass handles it.

### How does font sync work?

When the user clicks font +/- in any window, that window calls `Engine::BroadcastFontSync(hSender)`, which posts `WM_MW_REBUILD_FONTS` to all other open tool windows. Each window then calls `RebuildFonts()`, which destroys all children and calls `DoBuildControls()` again with the new font size. The font size is stored in `Engine::m_nSettingsFontSize`.

### How do I read checkbox state in ApplyLayout / Save / etc.?

```cpp
bool isEnabled = IsChecked(IDC_MY_CHECKBOX);   // correct
bool isEnabled = IsDlgButtonChecked(...);       // WRONG — always returns 0
```

### Can I use `SetChecked()` to programmatically change checkbox state?

Yes. `SetChecked(id, true/false)` sets the `"Checked"` property and invalidates the control for redraw. Use this in `LoadPrefs()`, `ResetDefaults()`, etc.
