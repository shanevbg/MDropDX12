# ToolWindow — Standalone Window Base Class

ToolWindow is the base class for all standalone tool windows in MDropDX12 (Settings, Hotkeys, MIDI, Presets, etc.). Each window runs on its own thread with an independent message pump, supports dark theme painting, always-on-top pin, cross-window font sync, and persisted position/size.

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
- Persists its geometry through the `WindowStore` (below)

### Where geometry is persisted

**Not `settings.ini`.** Window size, position, always-on-top state, active tab
and was-open-last-session live in **`resources/windows.json`**, handled by the
`WindowStore` singleton in `tool_window.cpp`. Seven keys per window:

```text
WndW  WndH  PosX  PosY  OnTop  ActiveTab  WasOpen
```

None of that is a setting anybody edits by hand; it is where the windows happen
to be sitting, and it was a seventh of `settings.ini` before it moved.
`InitWindowStore` runs once at startup and `MigrateSection` lifts a window's
keys out of the INI the first time that window asks for them, so an upgrade
carries existing positions across without a migration list that can miss a
window.

`GetINISection()` keeps its name for compatibility, but what it returns is now
the window's **record name in that store**. Changing it orphans the record and
the window comes back at its default size and position.

Ordinary settings a window reads or writes still go through `Config()` — see
`docs/settings.md`. Only geometry moved.

## Creating a New Subclass — Step by Step

Nine touch points. Steps 2 and 6 are the ones most often missed, and neither
fails loudly.

### 1. Define Control IDs

In `engine_helpers.h`, add unique IDs for your window's controls:

```cpp
// Beat Sync window controls (9144-9159)
#define IDC_MW_BEAT_PIN          9144
#define IDC_MW_BEAT_FONT_PLUS    9145
#define IDC_MW_BEAT_FONT_MINUS   9146
#define IDC_MW_BEAT_ENABLE       9147  // Checkbox: enable beat sync
#define IDC_MW_BEAT_SENS         9148  // Slider: sensitivity 0..200
#define IDC_MW_BEAT_SENS_LABEL   9149  // Static: live slider readout
#define IDC_MW_BEAT_RESET        9150  // Button: restore defaults
```

Every window needs at minimum `_PIN`, `_FONT_PLUS`, `_FONT_MINUS`. The base
class draws those three buttons and dispatches them through
`GetPinControlID()` and friends. The **resize-to-fit** button beside the pin
uses one shared `IDC_MW_TOOLWIN_FIT` for every window and needs nothing from
you.

**Pick a block that is genuinely free.** Some apparently-empty ranges are
reserved for controls generated at runtime — the Workspace Layout per-window
checkboxes own `9350`–`9399` without a `#define` for any of them. Read the
range comments before claiming a block.

**Why per-window IDs?** Win32 `GetDlgItem(hwnd, id)` requires unique IDs within
a window. Multiple tool windows can be open simultaneously, each with their own
HWND. The virtual accessors let the base class dispatch pin and font buttons
without knowing the concrete subclass.

### 2. Define the action

This is what makes the window reachable — from the Settings Tools tab, from a
hotkey, and over IPC. Nothing in the later steps substitutes for it.

In `hotkeys.h`, add an entry to `HotkeyAction`, before the `HK_COUNT`
terminator:

```cpp
HK_OPEN_BEATSYNC,             // (unbound) — open Beat Sync window
```

`HK_COUNT` is derived and `NUM_HOTKEYS` follows from it, so array sizes and
loops need no edit.

In `engine_hotkeys.cpp`, add the table row among the other `HKCAT_TOOLS`
entries:

```cpp
HK_DEF(i++, HK_OPEN_BEATSYNC, 0, 0, HKSCOPE_LOCAL, HKCAT_TOOLS,
       L"Open Beat Sync", L"OpenBeatSync");
```

**The modifier and key stay `0`.** New actions ship unbound; users bind keys
themselves in the Hotkeys window (Ctrl+F7). See the hotkey rule in
`CLAUDE.md` — hardcoding a default binding overrides a choice the user already
made.

`HKCAT_TOOLS` is what puts the window on the Settings window's Tools tab, which
populates by filtering `m_hotkeys[]` on that category. The last argument is the
INI key, and it doubles as the **IPC name**: once this row exists,
`ACTION=OpenBeatSync` over the named pipe opens the window.

Then the dispatch case, in `DispatchHotkeyAction`:

```cpp
case HK_OPEN_BEATSYNC:
    OpenBeatSyncWindow();
    return true;
```

### 3. Declare the Class

In `tool_window.h`, using the `TOOLWINDOW_META` macro — it expands to the eight
required overrides:

```cpp
// ── Concrete subclass: Beat Sync window ──

class BeatSyncWindow : public ToolWindow {
public:
  BeatSyncWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"Beat Sync", L"MDropDX12BeatSyncWnd", L"BeatSyncWnd",
                  IDC_MW_BEAT_PIN, IDC_MW_BEAT_FONT_PLUS, IDC_MW_BEAT_FONT_MINUS,
                  380, 240)

  DWORD GetCommonControlFlags() const override;

  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoHScroll(HWND hWnd, int id, int pos) override;
};
```

The macro's arguments are title, window class, store record name, the three
control IDs, and the minimum width and height. The window class string must be
unique across the process; the record name must never change once shipped.

Anything beyond those eight is an ordinary `override` alongside the macro.

### 4. Add to Engine

In `engine.h`, one member and two one-line accessors:

```cpp
std::unique_ptr<BeatSyncWindow>       m_beatSyncWindow;

void OpenBeatSyncWindow()  { OpenToolWindow(m_beatSyncWindow); }
void CloseBeatSyncWindow() { CloseToolWindow(m_beatSyncWindow); }
```

`OpenToolWindow` / `CloseToolWindow` are templates on `Engine`; they construct
the window lazily on first open. Do not hand-roll the null check.

### 5. Implement

In a new `engine_beatsync_ui.cpp`:

```cpp
#include "tool_window.h"
#include "engine.h"
#include "engine_helpers.h"
#include "config_store.h"
#include "utility.h"
#include <commctrl.h>

namespace mdrop {

BeatSyncWindow::BeatSyncWindow(Engine* pEngine)
  : ToolWindow(pEngine, 420, 260) {}

DWORD BeatSyncWindow::GetCommonControlFlags() const { return ICC_BAR_CLASSES; }

void BeatSyncWindow::DoBuildControls() {
  HWND hw = m_hWnd;
  if (!hw) return;

  auto L = BuildBaseControls();       // header: font +/-, resize-to-fit, pin
  int y = L.y, lineH = L.lineH, gap = L.gap, x = L.x, rw = L.rw;
  HFONT hFont = GetFont();
  Engine* p = m_pEngine;

  TrackControl(CreateCheck(hw, L"Enable beat sync", IDC_MW_BEAT_ENABLE,
                           x, y, rw, lineH, hFont, p->m_bBeatSync));
  y += lineH + gap;

  TrackControl(CreateBtn(hw, L"Reset", IDC_MW_BEAT_RESET, x, y,
                         MulDiv(80, lineH, 26), lineH, hFont));
}

LRESULT BeatSyncWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
  if (code != BN_CLICKED) return -1;

  switch (id) {
  case IDC_MW_BEAT_ENABLE:
    // Already toggled by the base class — just read the new state
    m_pEngine->m_bBeatSync = IsChecked(IDC_MW_BEAT_ENABLE);
    Config().SetInt(L"Settings", L"bBeatSync", m_pEngine->m_bBeatSync);
    return 0;

  case IDC_MW_BEAT_RESET:
    m_pEngine->ResetBeatSync();
    return 0;
  }
  return -1;
}

} // namespace mdrop
```

Return `-1` from any handler for a message you did not consume; return `0` only
for the cases you did.

**Size every dimension with `MulDiv(n, lineH, 26)`.** 26 is the line height at
the default font size, so the idiom means "n pixels at the default font, scaled
to the current one". Raw pixel widths clip the moment the user presses the
font + button.

**You are not on the render thread.** Writing a plain member the render thread
reads is fine. Anything that touches DirectX resources, recompiles a shader or
loads a preset goes through `EnqueueRenderCmd` or a `PostMessage` to the render
window — see `ToolWindow::RequestLoadPreset` for the pattern.

### 6. Register in the tool-window map

In `engine_workspace_layout_ui.cpp`, two cases. `ToolWindowForAction` is the
one place that maps an action to its window object:

```cpp
case HK_OPEN_BEATSYNC:  if (open) e->OpenBeatSyncWindow();  return e->m_beatSyncWindow.get();
```

and `IsToolWindowAction` answers the same question without touching anything:

```cpp
case HK_OPEN_BEATSYNC:
```

The `open` flag exists so callers can *locate* a window without opening it,
which is how the list is enumerated without two dozen windows appearing.

Skip these and the window works in every other respect but is silently absent
from Workspace Layout, and from `Engine::ToolWindowNames()`. A hand-written
list was missing twelve existing windows before this was derived from the
hotkey table.

### 7. Add the .cpp to `engine.vcxproj`

```xml
<ClCompile Include="engine_beatsync_ui.cpp" />
```

New translation units are listed explicitly; there is no glob.

### What you do *not* register

The `ToolWindow` constructor adds itself to `Engine::m_toolWindows` and the
destructor removes it, and everything that needs the full list walks that
vector:

| What | Where |
|---|---|
| Close-all on shutdown | `engine.cpp`, iterates `m_toolWindows` |
| Font-size sync broadcast | `Engine::BroadcastFontSync`, iterates `m_toolWindows` |
| Dark theme | applied by the base class in `CreateOnThread` and on `WM_SETTINGCHANGE` |
| The Settings window's Tools tab | filtered from `m_hotkeys[]` on `HKCAT_TOOLS` (step 2) |
| `ACTION=<IniKey>` over IPC | from the `HK_DEF` row's INI key (step 2) |
| Hotkey binding + persistence | from the same row's `szIniKey` |

So there is no menu to edit and no list to append to. The last three rows come
from step 2 rather than for nothing — they are free *given the action exists*,
which is why step 2 is not optional.

## Resizing: anchors, and what a rebuild costs

`WM_SIZE` reaches `ToolWindow::OnResize()`, whose default is:

```cpp
if (HasAnchors()) ApplyAnchors();
else RebuildFonts();
```

**Rebuild** is the older path: destroy every control and call
`DoBuildControls` again. It is simple, and it is still the right thing for a
font-size change, where every control has to be remade at the new size.

**Anchoring** is what a window should do for a resize. Register each control
with the edges it should keep its distance from, and `OnResize` moves them in a
single `DeferWindowPos` batch instead of tearing the window down:

```cpp
TrackAnchored(CreateSlider(hw, IDC_..., x + lw + 4, y, sliderW, lineH, ...),
              kAnchorStretchWide);   // left+top+right: stretches
TrackAnchored(hValueLabel, kAnchorTopRight);   // top+right: slides
```

The rule is the one every toolkit uses:

- anchored to **one** edge — the control keeps its distance from that edge, so
  it **moves**;
- anchored to **both** edges of an axis — it keeps its distance from both, so it
  **stretches**.

Available combinations: `kAnchorTopLeft` (the default — stays put),
`kAnchorTopRight`, `kAnchorBottomLeft`, `kAnchorBottomRight`,
`kAnchorStretchWide`, `kAnchorStretchBottom`, `kAnchorFill`.

A control that is never anchored stays exactly where it was built, which is what
every control did before anchors existed. So a window adopts anchoring one
control at a time, and a window that adopts none behaves as it always has.

`engine_colors_ui.cpp` and `engine_visual_ui.cpp` are the worked examples.

### Why it matters beyond flicker

A rebuild runs once per mouse-move during a drag, and it used to take the user's
work with it. That is fixed generically now — `RebuildFonts` captures what the
controls hold and restores it afterwards — but the fix has rules worth knowing:

- **Edit text is only restored when the user typed it** (`EM_GETMODIFY`), or
  when the rebuilt control came back empty. An edit that `DoBuildControls`
  refreshes from live state is left alone, because putting the old text over a
  deliberate refresh would be its own bug.
- **Restoring happens in two steps** with `OnRebuilt()` between them: list
  selections first, then the window reconciles its own members with them, then
  the typing last so nothing overwrites it. Override `OnRebuilt()` if your
  window keeps the selected row in a member that `DoBuildControls` resets.
- **`IsFirstBuild()`** guards anything that loads a document from disk. The
  Sprites window used to re-read `sprites.ini` on every rebuild and discard
  every unsaved edit.
- **Tooltips must go through `AttachTip()`.** They are `WS_POPUP` windows
  *owned* by the tool window rather than children, so the teardown walks past
  them and a hand-rolled one leaks on every rebuild. `AttachTip` creates the
  tooltip, copies the text into a store that outlives the call, and registers it
  via `TrackTooltip()` for cleanup. `CreateLabelTip()` is the label-plus-tooltip
  shorthand.

## Control Creation Helpers

Declared in `engine_helpers.h`, defined in `engine.cpp` (namespace `mdrop`). All create `BS_OWNERDRAW` buttons for dark theme support.

| Helper | Signature |
|--------|-----------|
| `CreateLabel` | `(HWND parent, const wchar_t* text, int x, int y, int w, int h, HFONT, bool visible=true)` — No ID (static text) |
| `CreateCheck` | `(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT, bool checked, bool visible=true)` |
| `CreateRadio` | `(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT, bool checked, bool firstInGroup=false, bool visible=true, int radioGroup=0)` |
| `CreateBtn` | `(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT, bool visible=true)` |
| `CreateSlider` | `(HWND parent, int id, int x, int y, int w, int h, int rangeMin, int rangeMax, int pos, bool visible=true)` |
| `CreateEdit` | `(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT, DWORD extraStyle=0, bool visible=true)` |

All returned HWNDs should be passed to `TrackControl()`, `TrackAnchored()`, or `TrackPageControl()` for tabbed windows.

`CreateThemedListView()` is a member of `ToolWindow` (report-mode ListView with the standard styles) and does **not** call `TrackControl` for you.

**A label you intend to update needs an ID**, and `CreateLabel` does not take
one — so a live readout beside a slider is created with a raw `CreateWindowExW`
on `L"STATIC"`, given the font with `WM_SETFONT`, and then tracked. Every window
with a slider readout does this; it is the expected shape, not a workaround.

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

### Radio Groups: Auto-toggled via `radioGroup` Parameter

Pass a non-zero `radioGroup` ID to `CreateRadio()` to enable auto-toggle. All radios with the same `radioGroup` value form a group — when one is clicked, the base class unchecks all others in the group.

Use the first control ID in the group as the group identifier (natural, unique, self-documenting):

```cpp
void MyWindow::DoBuildControls() {
    // ...
    // Three radios in one group — use IDC_MY_RADIO_A as the group ID
    TrackControl(CreateRadio(hw, L"Option A", IDC_MY_RADIO_A, x, y, w, lineH, hFont,
        true, true, true, IDC_MY_RADIO_A));
    y += lineH + 2;
    TrackControl(CreateRadio(hw, L"Option B", IDC_MY_RADIO_B, x, y, w, lineH, hFont,
        false, false, true, IDC_MY_RADIO_A));
    y += lineH + 2;
    TrackControl(CreateRadio(hw, L"Option C", IDC_MY_RADIO_C, x, y, w, lineH, hFont,
        false, false, true, IDC_MY_RADIO_A));
}
```

In `DoCommand()`, just read the state — no toggle code needed:

```cpp
case IDC_MY_RADIO_A:
case IDC_MY_RADIO_B:
case IDC_MY_RADIO_C:
    // Radio group already toggled by base class — just act on the selection
    if (IsChecked(IDC_MY_RADIO_A)) m_pEngine->SetMode(MODE_A);
    else if (IsChecked(IDC_MY_RADIO_B)) m_pEngine->SetMode(MODE_B);
    else m_pEngine->SetMode(MODE_C);
    return 0;
```

**Note**: `radioGroup=0` (the default) means no auto-toggle. This is used for radios in non-ToolWindow dialogs (e.g., standalone modal `WndProc`).

## BuildBaseControls and BaseLayout

`BuildBaseControls()` creates the standard header — font +/- buttons top-left,
resize-to-fit and pin top-right — and returns layout metrics:

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

The pin is re-anchored after a resize by `LayoutBaseControls()`, called from
`WM_SIZE` before `OnResize`, so subclasses never have to move it.

`FitToContents()` (the resize-to-fit button) grows the window so every control
fits at the current font size, and restores the window's designed size scaled to
that font. `FitClippedContentsOnOpen()` runs once on open and grows **only**
what is actually hanging outside the client edge, leaving a window the user
deliberately made small alone.

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

`PAGE_CTRL` is a convenience macro each tabbed `.cpp` defines for itself —
`#define PAGE_CTRL(page, expr) TrackPageControl(page, (expr))`. Some files also
define a `PAGE_CTRL_ANCHORED` variant. The base class handles tab switching,
showing/hiding page controls, persisting the active tab, and calls
`OnPageShown(page)` after a page becomes visible.

## Virtual Override Reference

| Method | Required? | Default | Purpose |
|--------|-----------|---------|---------|
| `GetWindowTitle()` | Yes — via macro | — | Window title bar text |
| `GetWindowClass()` | Yes — via macro | — | Unique WNDCLASS name (e.g., `L"MDropDX12MyWnd"`) |
| `GetINISection()` | Yes — via macro | — | Record name in `resources/windows.json` |
| `GetPinControlID()` | Yes — via macro | — | Control ID for always-on-top pin button |
| `GetFontPlusControlID()` | Yes — via macro | — | Control ID for font size + button |
| `GetFontMinusControlID()` | Yes — via macro | — | Control ID for font size - button |
| `GetMinWidth()` | No — via macro | 400 | Minimum resize width |
| `GetMinHeight()` | No — via macro | 350 | Minimum resize height |
| `DoBuildControls()` | Yes | — | Create all child controls |
| `DoCommand()` | No | returns -1 | Handle WM_COMMAND (button clicks) |
| `DoHScroll()` | No | returns -1 | Handle slider/trackbar changes |
| `DoNotify()` | No | returns -1 | Handle WM_NOTIFY (ListView, etc.) |
| `DoContextMenu()` | No | returns -1 | Handle WM_CONTEXTMENU |
| `DoMessage()` | No | returns -1 | Catch-all for other messages |
| `DoDestroy()` | No | no-op | Cleanup before window destruction |
| `OnResize()` | No | anchors if any, else `RebuildFonts()` | Handle WM_SIZE |
| `OnRebuilt()` | No | no-op | Reconcile members after a rebuild restores list selections |
| `OnPageShown(int)` | No | no-op | Called after a tab page becomes visible |
| `OnAlreadyOpen()` | No | posts `WM_MW_BRING_TO_TOP` | Called when `Open()` is called while already open |
| `RebuildFonts()` | No | destroys + rebuilds children | Font-size change |
| `GetCommonControlFlags()` | No | `ICC_BAR_CLASSES \| ICC_UPDOWN_CLASS \| ICC_TAB_CLASSES` | ICC flags for InitCommonControlsEx |
| `GetExtraWindowStyle()` | No | 0 | Extra `WS_*` bits (e.g. `WS_MAXIMIZEBOX`) |
| `UsesToolWindowFrame()` | No | true | `WS_EX_TOOLWINDOW` frame; false for a normal taskbar window |
| `AcceptsDragDrop()` | No | false | Enable OLE drag-and-drop |
| `ForwardAllKeys()` | No | false | Forward all keystrokes to render window |
| `PlacesItselfInTestingMode()` | No | false | Keep a saved position under testing mode (see below) |

### Testing mode and placement

Under testing mode a tool window opens on the **render window's monitor** and
`SaveWindowPosition` refuses to write, so a test run keeps its windows with the
thing being measured and gives the layout back untouched. A window that
overrides `PlacesItselfInTestingMode()` keeps its saved position instead **if it
was open when the app was last used** (`WasOpen`), and otherwise opens in the
lower-right corner of the render monitor.

Note the asymmetry: the saved position is read and never written, so honouring
it cannot turn a test's placement into a permanent one.

`Open()` also refuses outright in child mode — a `-child` instance is a render
surface driven over IPC, not a UI.

## Existing Subclasses

26 windows, all declared in `tool_window.h` with `TOOLWINDOW_META`.

| Class | File | Record | Min Size |
|-------|------|--------|----------|
| SettingsWindow | engine_settings_ui.cpp | SettingsWnd | 750×675 |
| DisplaysWindow | engine_displays_ui.cpp | Displays | 400×350 |
| SongInfoWindow | engine_songinfo_ui.cpp | SongInfo | 380×400 |
| HotkeysWindow | engine_hotkeys_ui.cpp | HotkeysWnd | 560×480 |
| MidiWindow | engine_midi_ui.cpp | MidiWnd | 580×550 |
| PresetsWindow | engine_presets_ui.cpp | PresetsWnd | 420×400 |
| AnnotationsWindow | engine_annotations_ui.cpp | AnnotationsWnd | 500×400 |
| PresetEditorWindow | engine_preset_editor_ui.cpp | PresetEditWnd | 620×480 |
| SpritesWindow | engine_sprites_ui.cpp | SpritesWnd | 500×700 |
| MessagesWindow | engine_messages_ui.cpp | MessagesWnd | 420×480 |
| ButtonBoardWindow | engine_board_ui.cpp | BoardWnd | 300×250 |
| MixerWindow | engine_mixer_ui.cpp | MixerWnd | 640×900 |
| VideoEffectsWindow | engine_video_effects_ui.cpp | VideoFX | 380×550 |
| VFXProfileWindow | engine_vfx_profiles_ui.cpp | VFXProfiles | 280×350 |
| VFXKeepPromptWindow | engine_vfx_prompt_ui.cpp | VFXKeepPrompt | 400×210 |
| CustomShadersWindow | engine_custom_shaders_ui.cpp | CustomShaders | 400×620 |
| ShaderImportWindow | engine_shader_import_ui.cpp | ShaderImport | 350×450 |
| ShaderEditorWindow | engine_shader_import_ui.cpp | ShaderEditor | 500×400 |
| WelcomeWindow | engine_shader_import_ui.cpp | Welcome | 300×400 |
| TextAnimWindow | engine_textanim_ui.cpp | TextAnimWnd | 520×750 |
| ScriptWindow | engine_script_ui.cpp | ScriptWnd | 380×500 |
| RemoteWindow | engine_remote_ui.cpp | RemoteWnd | 260×300 |
| VisualWindow | engine_visual_ui.cpp | VisualWnd | 400×600 |
| ColorsWindow | engine_colors_ui.cpp | ColorsWnd | 380×400 |
| ControllerWindow | engine_controller_ui.cpp | ControllerWnd | 400×500 |
| WorkspaceLayoutWindow | engine_workspace_layout_ui.cpp | WorkspaceLayout | 380×700 |

`WelcomeWindow` passes `0` for its three control IDs — it has no pin or font
buttons. It is the one window that does; a normal window needs all three.

## FAQ

### Why are all controls BS_OWNERDRAW?

For dark theme support. Standard Win32 buttons/checkboxes cannot be custom-painted in dark mode without owner-draw. The base WndProc handles `WM_DRAWITEM` and calls `DrawOwnerCheckbox()`, `DrawOwnerRadio()`, `DrawOwnerButton()` in `engine.cpp` to render controls with the current theme colors.

### Why does each window need its own PIN / FONT_PLUS / FONT_MINUS IDs?

Win32 `GetDlgItem(hwnd, id)` identifies controls by integer ID within a single window. The base class WndProc compares `id == GetPinControlID()` to handle the pin button. If two windows shared the same ID, there would be no ambiguity (different HWNDs), but the virtual method pattern allows the base class to dispatch correctly without knowing the concrete subclass. Each window's IDs are defined in `engine_helpers.h` in a reserved range.

### How does radio group auto-toggle work?

Each `CreateRadio()` call can take an optional `radioGroup` parameter. The base class stores this as a `"RadioGroup"` window property. When a radio is clicked, the base `WndProc` finds all tracked controls (`m_childCtrls`) with the same `RadioGroup` value and unchecks them, then checks the clicked one. Use `radioGroup=0` (the default) to opt out of auto-toggle for non-ToolWindow contexts.

### How does font sync work?

When the user clicks font +/- in any window, that window calls `Engine::BroadcastFontSync(hSender)`, which posts `WM_MW_REBUILD_FONTS` to all other open tool windows. Each window then calls `RebuildFonts()`, which destroys all children and calls `DoBuildControls()` again with the new font size. The font size is stored in `Engine::m_nSettingsFontSize`.

### How do I read checkbox state in ApplyLayout / Save / etc.?

```cpp
bool isEnabled = IsChecked(IDC_MY_CHECKBOX);   // correct
bool isEnabled = IsDlgButtonChecked(...);       // WRONG — always returns 0
```

### Can I use `SetChecked()` to programmatically change checkbox state?

Yes. `SetChecked(id, true/false)` sets the `"Checked"` property and invalidates the control for redraw. Use this in `LoadPrefs()`, `ResetDefaults()`, etc.

### My window works but does not appear in Workspace Layout. Why?

Step 6 is missing — `ToolWindowForAction` and `IsToolWindowAction` in
`engine_workspace_layout_ui.cpp`. Nothing else fails, which is what makes it
easy to miss.

### My window does not appear on the Settings Tools tab. Why?

Step 2 is missing, or its `HK_DEF` row is not in `HKCAT_TOOLS`. That tab is
built by filtering the hotkey table on the category; there is no separate list.
