#pragma once
/*
  ToolWindow — reusable base class for standalone tool windows on their own threads.
  Provides: thread + message pump, dark theme painting, pin button (always-on-top),
  font +/- buttons with cross-window sync, window size persistence, owner-draw rendering.

  Subclass to create specific windows (Displays, Sticky Notes, etc.).
*/

#include <Windows.h>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include "engine_helpers.h"
#include "midi_input.h"
#include "button_panel.h"   // ButtonAction
#include "hotkeys.h"        // HotkeyScope

namespace mdrop {

class Engine;  // forward declaration — full type in engine.h

class ToolWindow {
protected:
  Engine*     m_pEngine;
  HWND        m_hWnd = NULL;
  std::thread m_thread;
  std::atomic<bool> m_bThreadRunning{false};
  bool        m_bOnTop = false;
  HFONT       m_hFont = NULL;
  HFONT       m_hFontBold = NULL;
  HFONT       m_hPinFont = NULL;       // Segoe MDL2 Assets for pin icon
  int         m_nWndW, m_nWndH;        // current (persisted) size
  int         m_nDefaultW, m_nDefaultH; // default size if no INI
  int         m_nPosX = -1, m_nPosY = -1; // persisted position (-1 = center on screen)
  std::vector<HWND> m_childCtrls;      // all child HWNDs (for rebuild + dark theme)

  // ── Tab control support (optional — used by tabbed subclasses) ──
  HWND        m_hTab = NULL;
  int         m_nActivePage = 0;
  std::vector<std::vector<HWND>> m_pageCtrls;  // per-page control tracking

  // ── Subclass must override these ──

  // Window identity
  virtual const wchar_t* GetWindowTitle() const = 0;
  virtual const wchar_t* GetWindowClass() const = 0;
  virtual const wchar_t* GetINISection() const = 0;

  // Control IDs for pin, font +/-
  virtual int GetPinControlID() const = 0;
  virtual int GetFontPlusControlID() const = 0;
  virtual int GetFontMinusControlID() const = 0;

  // Minimum resize dimensions
  virtual int GetMinWidth() const { return 400; }
  virtual int GetMinHeight() const { return 350; }

  // Called on WM_SIZE — default destroys/rebuilds all controls.
  // Settings overrides to reposition controls without rebuild.
  virtual void OnResize() { RebuildFonts(); }

  // ICC flags for InitCommonControlsEx. Override to add ICC_LISTVIEW_CLASSES etc.
  virtual DWORD GetCommonControlFlags() const;

  // Whether window accepts drag-and-drop files (DragAcceptFiles)
  virtual bool AcceptsDragDrop() const { return false; }

  // Whether the message pump forwards ALL keyboard input to the render window
  // (not just F-keys and Ctrl/Alt combos).  Override to true for windows with
  // no text edits (e.g. Button Board) so the VJ can use hotkeys while the
  // window has focus.  Escape and Ctrl+Shift+F2 are always kept local.
  virtual bool ForwardAllKeys() const { return false; }

  // Called when Open() finds window already visible. Default: SetForegroundWindow.
  // Settings overrides to move off fullscreen monitor.
  virtual void OnAlreadyOpen();

  // Build all child controls (called after window creation and on rebuild)
  virtual void DoBuildControls() = 0;

  // Handle WM_COMMAND. Return 0 if handled, -1 if not.
  virtual LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) { return -1; }

  // Handle WM_HSCROLL slider changes. Return 0 if handled, -1 if not.
  virtual LRESULT DoHScroll(HWND hWnd, int id, int pos) { return -1; }

  // Handle WM_NOTIFY. Return 0 if handled, -1 if not.
  virtual LRESULT DoNotify(HWND hWnd, NMHDR* pnm) { return -1; }

  // Handle WM_CONTEXTMENU. x/y are screen coordinates. Return 0 if handled, -1 if not.
  virtual LRESULT DoContextMenu(HWND hWnd, int x, int y) { return -1; }

  // Catch-all for messages BaseWndProc doesn't handle (WM_TIMER, WM_DROPFILES, etc.)
  virtual LRESULT DoMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) { return -1; }

  // Called from WM_DESTROY before cleanup (subclass releases its resources)
  virtual void DoDestroy() {}

public:
  ToolWindow(Engine* pEngine, int defaultW, int defaultH);
  virtual ~ToolWindow();

  // Open the window (creates thread, or brings to front if already open)
  void Open();

  // Close the window and join the thread
  void Close();

  // Two-phase close: signal first, join later (for parallel shutdown)
  void SignalClose();
  void WaitClose();

  // Is the window currently open?
  bool IsOpen() const;

  // Get the HWND (may be NULL if not open)
  HWND GetHWND() const { return m_hWnd; }

  // Destroy all children and rebuild controls at the current font size
  virtual void RebuildFonts();

  // Reset to default size, centered on primary display
  void ResetPosition();

  // Apply dark theme to the window and all children
  void ApplyDarkTheme();

  // Compute line height from current font
  int GetLineHeight();

  // Helper for subclasses to track child controls for dark theme + rebuild
  void TrackControl(HWND h) { if (h) m_childCtrls.push_back(h); }

  // Read owner-draw checkbox/radio state. Use instead of IsDlgButtonChecked()
  // which does NOT work with BS_OWNERDRAW controls (always returns 0).
  bool IsChecked(int controlID) const;

  // Set owner-draw checkbox/radio state. Use instead of CheckDlgButton()
  // which does NOT work with BS_OWNERDRAW controls (silently fails).
  void SetChecked(int controlID, bool checked);

  // Track control on a specific tab page (adds to m_pageCtrls[page] + m_childCtrls)
  void TrackPageControl(int page, HWND h);

  // Create TCS_OWNERDRAWFIXED tab control with dark theme subclass.
  // Returns the content area rect (below tab headers).
  RECT BuildTabControl(int tabCtrlID, const wchar_t* const* tabNames, int numPages,
                       int x, int y, int w, int h);

  // Show/hide page controls + persist active tab to INI
  void ShowPage(int page);

  // Restore persisted active tab from INI (call at end of DoBuildControls)
  void SelectInitialTab();

  // Access fonts for control creation
  HFONT GetFont() const { return m_hFont; }
  HFONT GetFontBold() const { return m_hFontBold; }

  // Create a report-mode ListView with standard styles. Does NOT call TrackControl().
  // When sortable=true, column headers are clickable (omits LVS_NOSORTHEADER).
  HWND CreateThemedListView(int id, int x, int y, int w, int h,
                            bool visible = true, bool sortable = false);

  // Common control setup: creates fonts, font +/- buttons, pin button with tooltip.
  // Returns the Y position below the header row for subclasses to continue from.
  // Populates lineH, gap, x, rw, clientW for the caller.
  struct BaseLayout { int y, lineH, gap, x, rw, clientW; };
  BaseLayout BuildBaseControls();

private:
  void CreateOnThread();
  void LoadWindowPosition();
  void SaveWindowPosition();

  // The single shared WndProc dispatches to virtual methods
  static LRESULT CALLBACK BaseWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

  // Tab control dark background subclass (shared by all tabbed windows)
  static LRESULT CALLBACK TabSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
};

// ── Macro to eliminate boilerplate overrides in ToolWindow subclasses ──
// Each subclass needs: title, window class, INI section, 3 control IDs, min size.
// Usage: place inside the `protected:` section of the subclass declaration.
#define TOOLWINDOW_META(title, cls, ini, pinID, fpID, fmID, minW, minH) \
  const wchar_t* GetWindowTitle() const override { return title; }     \
  const wchar_t* GetWindowClass() const override { return cls; }       \
  const wchar_t* GetINISection() const override  { return ini; }       \
  int GetPinControlID() const override       { return pinID; }         \
  int GetFontPlusControlID() const override  { return fpID; }          \
  int GetFontMinusControlID() const override { return fmID; }          \
  int GetMinWidth() const override  { return minW; }                   \
  int GetMinHeight() const override { return minH; }

// ── Concrete subclass: Spout / Displays window ──

class DisplaysWindow : public ToolWindow {
public:
  DisplaysWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"Spout / Displays", L"MDropDX12DisplaysWnd", L"Displays",
                  IDC_MW_DISPLAYS_PIN, IDC_MW_DISP_FONT_PLUS, IDC_MW_DISP_FONT_MINUS, 400, 350)

  void DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoHScroll(HWND hWnd, int id, int pos) override;
  LRESULT DoNotify(HWND hWnd, NMHDR* pnm) override;

private:
  void BuildOutputsPage(int x, int y, int rw, int lineH, int gap);
  void BuildVideoInputPage(int x, int y, int rw, int lineH, int gap);
};

// ── Concrete subclass: Song Info window ──

class SongInfoWindow : public ToolWindow {
public:
  SongInfoWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"Song Info", L"MDropDX12SongInfoWnd", L"SongInfo",
                  IDC_MW_SONGINFO_PIN, IDC_MW_SONGINFO_FONT_PLUS, IDC_MW_SONGINFO_FONT_MINUS, 380, 400)

  void DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
};

// ── Concrete subclass: Settings window ──

class SettingsWindow : public ToolWindow {
public:
  SettingsWindow(Engine* pEngine);
  void EnsureVisible();  // called from Engine on WM_SIZE

protected:
  TOOLWINDOW_META(L"MDropDX12 Settings", L"MDropDX12SettingsWnd", L"SettingsWnd",
                  IDC_MW_SETTINGS_PIN, IDC_MW_FONT_PLUS, IDC_MW_FONT_MINUS, 750, 675)

  DWORD GetCommonControlFlags() const override;
  bool  AcceptsDragDrop() const override { return true; }
  void  OnAlreadyOpen() override;
  void  OnResize() override;
  void  RebuildFonts() override;

  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoNotify(HWND hWnd, NMHDR* pnm) override;
  LRESULT DoMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override;
  void    DoDestroy() override;

private:
  void LayoutControls();
  void ResetToFactory();
  void ResetToUserDefaults();
};

// ── Concrete subclass: Hotkeys window ──

class HotkeysWindow : public ToolWindow {
public:
  HotkeysWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"Hotkeys", L"MDropDX12HotkeysWnd", L"HotkeysWnd",
                  IDC_MW_HOTKEYS_PIN, IDC_MW_HOTKEYS_FONT_PLUS, IDC_MW_HOTKEYS_FONT_MINUS, 560, 480)

  void    OnResize() override;
  DWORD   GetCommonControlFlags() const override;
  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoNotify(HWND hWnd, NMHDR* pnm) override;

private:
  HWND m_hList = NULL;
  HWND m_hBtnAdd = NULL;
  HWND m_hBtnDelete = NULL;
  HWND m_hBtnEdit = NULL;
  HWND m_hBtnClearKey = NULL;
  HWND m_hBtnReset = NULL;
  int  m_headerH = 0;     // height of title + header area
  int  m_buttonBarH = 0;  // height of bottom button row
  void LayoutControls();
  void OpenEditDialog(int lvItem);
  void UpdateDeleteButton();
  void BuildBindingsPage(int x, int y, int rw, int lineH, int gap);
  void BuildHelpOrderPage(int x, int y, int rw, int lineH, int gap);
  void RefreshCatOrderList();
  HWND m_hCatList = NULL;
  HWND m_hBtnCatUp = NULL;
  HWND m_hBtnCatDown = NULL;
  HWND m_hBtnCatReset = NULL;
};

// ── Concrete subclass: MIDI window ──

class MidiWindow : public ToolWindow {
public:
  MidiWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"MIDI", L"MDropDX12MidiWnd", L"MidiWnd",
                  IDC_MW_MIDI_PIN, IDC_MW_MIDI_FONT_PLUS, IDC_MW_MIDI_FONT_MINUS, 580, 550)

  DWORD   GetCommonControlFlags() const override;
  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoNotify(HWND hWnd, NMHDR* pnm) override;
  LRESULT DoMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override;
  void    DoDestroy() override;

private:
  HWND m_hList = NULL;
  HWND m_hDeviceCombo = NULL;
  HWND m_hTypeCombo = NULL;
  HWND m_hActionCombo = NULL;
  HWND m_hLabelEdit = NULL;
  HWND m_hIncrementEdit = NULL;
  bool m_bLearning = false;
  int  m_nLearnRow = -1;
  int  m_nSelectedRow = -1;

  void PopulateListView();
  void UpdateListViewRow(int idx);
  void UpdateEditControls(int sel);
  void SaveEditControls();
  void PopulateDeviceCombo();
  void PopulateActionCombo(MidiActionType type);
  void StartLearn();
  void StopLearn();
  void OnMidiData(LPARAM lParam);
  static const wchar_t* KnobActionName(MidiKnobAction id);
};

// ── Concrete subclass: Sprites window ──

class SpritesWindow : public ToolWindow {
public:
  SpritesWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"Sprites", L"MDropDX12SpritesWnd", L"SpritesWnd",
                  IDC_MW_SPRITES_WIN_PIN, IDC_MW_SPRITES_WIN_FONT_PLUS, IDC_MW_SPRITES_WIN_FONT_MINUS, 500, 700)

  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoNotify(HWND hWnd, NMHDR* pnm) override;
  void    DoDestroy() override;

private:
  HWND m_hList = NULL;
  int  m_nTopY = 0;
};

// ── Concrete subclass: Messages window ──

class MessagesWindow : public ToolWindow {
public:
  MessagesWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"Messages", L"MDropDX12MessagesWnd", L"MessagesWnd",
                  IDC_MW_MESSAGES_WIN_PIN, IDC_MW_MESSAGES_WIN_FONT_PLUS, IDC_MW_MESSAGES_WIN_FONT_MINUS, 420, 480)

  void    OnResize() override;
  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  void    DoDestroy() override;

private:
  int  m_nTopY = 0;
};

// ── Concrete subclass: Presets window ──

class PresetsWindow : public ToolWindow {
public:
  PresetsWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"Presets", L"MDropDX12PresetsWnd", L"PresetsWnd",
                  IDC_MW_PRESETS_PIN, IDC_MW_PRESETS_FONT_PLUS, IDC_MW_PRESETS_FONT_MINUS, 420, 400)

  void    OnResize() override;
  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoContextMenu(HWND hWnd, int x, int y) override;
  LRESULT DoMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override;
  void    DoDestroy() override;

private:
  HWND m_hCurrentPreset = NULL, m_hBrowsePreset = NULL;
  HWND m_hPresetDir = NULL, m_hBrowseDir = NULL;
  HWND m_hLblPreset = NULL, m_hLblDir = NULL;
  HWND m_hList = NULL;
  HWND m_hBtnPrev = NULL, m_hBtnNext = NULL, m_hBtnCopy = NULL;
  HWND m_hBtnUp = NULL, m_hBtnInto = NULL, m_hBtnFilter = NULL, m_hBtnSubdir = NULL;
  HWND m_hLblTag = NULL, m_hTagFilter = NULL, m_hBtnImportTags = NULL;
  HWND m_hLblListName = NULL, m_hPresetListCombo = NULL, m_hBtnListSave = NULL, m_hBtnListClear = NULL;
  HWND m_hLblSens = NULL, m_hEditSens = NULL;
  HWND m_hLblBlend = NULL, m_hEditBlend = NULL;
  HWND m_hLblTime = NULL, m_hEditTime = NULL;
  HWND m_hChkHardCuts = NULL, m_hChkLock = NULL, m_hChkSeq = NULL;
  HWND m_hStartupCombo = NULL, m_hLblStartup = NULL;
  int  m_nTopY = 0;

  void LayoutControls();
  void RefreshPresetList();
  void SyncListBoxToCurrentPreset();
  void UpdateCurrentPresetDisplay();
  void UpdatePresetDirDisplay();
  void NavigatePresetDirUp();
  void NavigatePresetDirInto(int sel);
  bool ShowNoteDialog(HWND hParent, const wchar_t* presetName, wchar_t* szNote, int nMaxNote);
  int  m_nContextSel = -1;  // listbox index for context menu
  int  m_nLastPresetCount = -1; // for detecting scan completion
};

// ── Concrete subclass: Annotations window ──

class AnnotationsWindow : public ToolWindow {
public:
  AnnotationsWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"Annotations", L"MDropDX12AnnotationsWnd", L"AnnotationsWnd",
                  IDC_MW_ANNOTWIN_PIN, IDC_MW_ANNOTWIN_FONT_PLUS, IDC_MW_ANNOTWIN_FONT_MINUS, 500, 400)

  void    OnResize() override;
  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoNotify(HWND hWnd, NMHDR* pnm) override;

private:
  HWND m_hListView = NULL;
  HWND m_hFilterCombo = NULL;
  HWND m_hBtnLoad = NULL;
  HWND m_hBtnRemove = NULL;
  HWND m_hBtnDetails = NULL;
  HWND m_hBtnImport = NULL;
  HWND m_hBtnScan = NULL;
  int  m_nTopY = 0;
  int  m_nFilterMode = 0; // 0=All, 1=Favorite, 2=Error, 3=Skip, 4=Broken

  void LayoutControls();
  void RefreshList();
  void ShowDetailsDialog();
  void ShowImportDialog();
  void DoScanPresets();
  std::wstring GetSelectedFilename();
};

// ── Concrete subclass: Button Board window ──

class ButtonBoardWindow : public ToolWindow {
public:
  ButtonBoardWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"Button Board", L"MDropDX12BoardWnd", L"BoardWnd",
                  IDC_MW_BOARD_PIN, IDC_MW_BOARD_FONT_PLUS, IDC_MW_BOARD_FONT_MINUS, 300, 250)
  bool ForwardAllKeys() const override { return true; }
  bool AcceptsDragDrop() const override { return true; }

  void    OnResize() override;
  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) override;
  void    DoDestroy() override;

private:
  ButtonPanel* m_pPanel = NULL; // owned, heap-allocated (avoids header include)
  HWND m_hBankPrev  = NULL;
  HWND m_hBankNext  = NULL;
  HWND m_hBankLabel = NULL;
  HWND m_hConfigBtn = NULL;
  int  m_nTopY = 0; // Y below base controls, for LayoutControls

  void LayoutControls();
  void UpdateBankLabel();
  void ExecuteSlot(int globalIndex);
  void ShowSlotContextMenu(int globalIndex, POINT screenPt);
  void ShowConfigMenu();
  void ShowSlotEditDialog(int globalIndex);
  void SaveBoard();
  void LoadSlotImages();
  void SetSlotImage(int globalIndex, const std::wstring& path);
  HMENU BuildActionSubMenu();
};

// ── Concrete subclass: Video Effects window ──

class VideoEffectsWindow : public ToolWindow {
public:
  VideoEffectsWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"Video Effects", L"MDropDX12VideoFXWnd", L"VideoFX",
                  IDC_MW_VFX_PIN, IDC_MW_VFX_FONT_PLUS, IDC_MW_VFX_FONT_MINUS, 380, 550)

  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoHScroll(HWND hWnd, int id, int pos) override;
  void    DoDestroy() override;

private:
  void BuildTransformPage(int x, int y, int rw, int lineH, int gap);
  void BuildEffectsPage(int x, int y, int rw, int lineH, int gap);
  void BuildAudioPage(int x, int y, int rw, int lineH, int gap);
  void SaveFX();
};

// ── Concrete subclass: VFX Profile Picker window ──

class VFXProfileWindow : public ToolWindow {
public:
  VFXProfileWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"VFX Profiles", L"MDropDX12VFXProfileWnd", L"VFXProfiles",
                  IDC_MW_VFXP_PIN, IDC_MW_VFXP_FONT_PLUS, IDC_MW_VFXP_FONT_MINUS, 280, 350)

  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  void    DoDestroy() override;

private:
  void RefreshProfileList();
  void ApplySelectedProfile();
  std::vector<std::wstring> m_profilePaths;  // full paths, indexed parallel to listbox
};

// ── Concrete subclass: Text Animations window ──

class TextAnimWindow : public ToolWindow {
public:
  TextAnimWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"Text Animations", L"MDropDX12TextAnimWnd", L"TextAnimWnd",
                  IDC_MW_TEXTANIM_PIN, IDC_MW_TEXTANIM_FONT_PLUS, IDC_MW_TEXTANIM_FONT_MINUS, 520, 750)

  DWORD   GetCommonControlFlags() const override;
  void    OnResize() override;
  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoNotify(HWND hWnd, NMHDR* pnm) override;
  void    DoDestroy() override;

private:
  HWND m_hList = NULL;
  int  m_nTopY = 0;
  int  m_nSelectedRow = -1;
  static COLORREF s_acrCustColors[16];  // ChooseColor custom colors

  void PopulateListView();
  void UpdateListViewRow(int idx);
  void UpdateEditControls(int sel);
  void SaveEditControls();
  void SelectProfile(int idx);
  void UpdateColorSwatch(int ctrlID, int r, int g, int b);
  void UpdateFontPreview();
};

// ── Concrete subclass: Script window ──

class ScriptWindow : public ToolWindow {
public:
  ScriptWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"Script", L"MDropDX12ScriptWnd", L"ScriptWnd",
                  IDC_MW_SCRIPTWIN_PIN, IDC_MW_SCRIPTWIN_FONT_PLUS, IDC_MW_SCRIPTWIN_FONT_MINUS, 380, 500)

  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
};

// ── Concrete subclass: Remote window ──

class RemoteWindow : public ToolWindow {
public:
  RemoteWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"Remote", L"MDropDX12RemoteWnd", L"RemoteWnd",
                  IDC_MW_REMOTEWIN_PIN, IDC_MW_REMOTEWIN_FONT_PLUS, IDC_MW_REMOTEWIN_FONT_MINUS, 380, 500)

  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override;
  void    DoDestroy() override;

private:
  int m_lastSeenIPCSeq = 0;
  void RefreshIPCList();
};

// ── Concrete subclass: Visual window ──

class VisualWindow : public ToolWindow {
public:
  VisualWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"Visual", L"MDropDX12VisualWnd", L"VisualWnd",
                  IDC_MW_VISUALWIN_PIN, IDC_MW_VISUALWIN_FONT_PLUS, IDC_MW_VISUALWIN_FONT_MINUS, 400, 600)

  DWORD GetCommonControlFlags() const override;

  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoHScroll(HWND hWnd, int id, int pos) override;
};

// ── Concrete subclass: Colors window ──

class ColorsWindow : public ToolWindow {
public:
  ColorsWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"Colors", L"MDropDX12ColorsWnd", L"ColorsWnd",
                  IDC_MW_COLORSWIN_PIN, IDC_MW_COLORSWIN_FONT_PLUS, IDC_MW_COLORSWIN_FONT_MINUS, 380, 400)

  DWORD GetCommonControlFlags() const override;

  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoHScroll(HWND hWnd, int id, int pos) override;
};

// ── Concrete subclass: Controller window ──

class ControllerWindow : public ToolWindow {
public:
  ControllerWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"Controller", L"MDropDX12ControllerWnd", L"ControllerWnd",
                  IDC_MW_CONTROLLERWIN_PIN, IDC_MW_CONTROLLERWIN_FONT_PLUS, IDC_MW_CONTROLLERWIN_FONT_MINUS, 400, 500)

  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
};

// ── Channel input sources for Shadertoy passes ──

enum ChannelSource {
  CHAN_NOISE_LQ = 0,     // sampler_noise_lq (256x256)
  CHAN_NOISE_MQ,         // sampler_noise_mq (256x256)
  CHAN_NOISE_HQ,         // sampler_noise_hq (256x256)
  CHAN_FEEDBACK,         // sampler_feedback (Buffer A output / self-feedback)
  CHAN_NOISEVOL_LQ,      // sampler_noisevol_lq (3D 32x32x32)
  CHAN_NOISEVOL_HQ,      // sampler_noisevol_hq (3D 32x32x32)
  CHAN_IMAGE_PREV,       // sampler_image (Image previous frame output)
  CHAN_AUDIO,            // sampler_audio (512x2 audio FFT + waveform)
  CHAN_RANDOM_TEX,       // sampler_rand00 (random texture from disk)
  CHAN_BUFFER_B,         // sampler_bufferB (Buffer B output)
  CHAN_BUFFER_C,         // sampler_bufferC (Buffer C output)
  CHAN_BUFFER_D,         // sampler_bufferD (Buffer D output)
  CHAN_TEXTURE_FILE,     // sampler_chtex0..3 (user-selected texture file)
  CHAN_COUNT
};

// ── Shared data for shader import passes ──

struct ShaderPass {
  std::wstring name;       // "Image", "Buffer A"
  std::string  glslSource; // Raw GLSL text (narrow)
  std::string  hlslOutput; // Converted HLSL (narrow, with LINEFEED_CONTROL_CHAR)
  std::string  notes;      // User comments/notes (narrow)
  int channels[4] = {CHAN_NOISE_LQ, CHAN_NOISE_LQ, CHAN_NOISE_MQ, CHAN_NOISE_HQ};
  std::wstring channelTexPaths[4]; // File paths for CHAN_TEXTURE_FILE channels
  bool channelsFromJSON = false;   // true = channels loaded from .milk3 JSON, skip auto-detect
};

// ── Concrete subclass: Shader Editor window (GLSL + HLSL code editor) ──

class ShaderImportWindow;  // forward decl

class ShaderEditorWindow : public ToolWindow {
public:
  ShaderEditorWindow(Engine* pEngine, ShaderImportWindow* pImport);

  void SetGLSL(const std::string& glsl);
  std::string GetGLSL();
  void SetHLSL(const std::string& hlsl);
  std::string GetHLSL();
  void SetNotes(const std::string& notes);
  std::string GetNotes();
  void SetPassName(const std::wstring& name);
  void SetPendingData(const ShaderPass& pass);  // Store data for DoBuildControls to load

protected:
  TOOLWINDOW_META(L"Shader Editor", L"MDropDX12ShaderEditorWnd", L"ShaderEditor",
                  IDC_MW_SHEDITOR_PIN, IDC_MW_SHEDITOR_FONT_PLUS, IDC_MW_SHEDITOR_FONT_MINUS, 500, 400)

  void    OnResize() override;
  void    DoBuildControls() override;
  void    DoDestroy() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;

private:
  int m_nTopY = 0;
  std::wstring m_passName = L"Image";
  ShaderImportWindow* m_pImportWindow = nullptr;
  // Pending data — set before Open(), loaded by DoBuildControls when controls are ready
  std::string m_pendingGlsl, m_pendingHlsl, m_pendingNotes;
  std::wstring m_pendingPassName;
};

// ── Concrete subclass: Shader Import window (control panel) ──

class ShaderImportWindow : public ToolWindow {
public:
  ShaderImportWindow(Engine* pEngine);
  ~ShaderImportWindow();

  void SyncEditorToPass();          // Editor text → m_passes[m_nSelectedPass]
  void OnEditorClosing(const std::string& glsl, const std::string& hlsl, const std::string& notes);  // Called by editor before destroy
  void ConvertGLSLtoHLSL(int passOverride = -1);  // Convert pass GLSL→HLSL
  void ConvertAndApply();           // Convert all passes, then apply
  void OnPasteGLSL(const std::string& glsl);  // Paste intelligence: detect pass type + channels
  std::wstring ImportFromFile(const wchar_t* path);  // Headless: load JSON, convert, apply — returns status
  std::wstring ImportFromGLSL(const std::string& glsl, bool applyToEngine = true);  // Headless: convert GLSL, optionally apply
  std::wstring SavePresetToFile(const wchar_t* path);  // Headless: save current passes as .milk3

protected:
  TOOLWINDOW_META(L"Shader Import", L"MDropDX12ShaderImportWnd", L"ShaderImport",
                  IDC_MW_SHIMPORT_PIN, IDC_MW_SHIMPORT_FONT_PLUS, IDC_MW_SHIMPORT_FONT_MINUS, 350, 450)

  void    OnResize() override;
  void    DoBuildControls() override;
  void    DoDestroy() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override;

private:
  int m_nTopY = 0;
  std::vector<ShaderPass> m_passes;
  int m_nSelectedPass = 0;
  std::wstring m_lastProjectPath;  // Last loaded/saved .json project path
  std::unique_ptr<ShaderEditorWindow> m_editorWindow;

  void LayoutControls();
  void SyncPassToEditor();    // m_passes[m_nSelectedPass] → editor text
  void SyncChannelCombos();   // Update channel combos from m_passes[m_nSelectedPass]
  void RebuildPassList();
  void OpenEditor();
  void ApplyShader();
  void SaveAsPreset();
  void SaveImportProject();
  void LoadImportProject();
  int  GetSelectedPass();     // 0=Image, 1=Buffer A

  void AnalyzeChannels(ShaderPass& pass, bool jsonLoaded = false);  // Infer channel types from GLSL source

  // Conversion helpers (ported from Milkwave Remote ShaderHelper.cs)
  static std::string ReplaceVarName(const std::string& oldName, const std::string& newName, const std::string& input);
  static int FindClosingBracket(const std::string& input, char open, char close, int startLevel);
  static std::string FixMatrixMultiplication(const std::string& line);
  static std::string FixFloatNumberOfArguments(const std::string& line, const std::string& fullContext);
  static std::string FixAtan(const std::string& line);
  static std::string BasicFormatShaderCode(const std::string& code);
};

// ── Concrete subclass: Workspace Layout window ──

class WorkspaceLayoutWindow : public ToolWindow {
public:
  WorkspaceLayoutWindow(Engine* pEngine);

  void ApplyLayout();
  void SetAutoApply() { m_bAutoApply = true; } // apply layout after window builds

protected:
  TOOLWINDOW_META(L"Workspace Layout", L"MDropDX12WorkspaceLayoutWnd", L"WorkspaceLayout",
                  IDC_MW_WSLAYOUT_PIN, IDC_MW_WSLAYOUT_FONT_PLUS, IDC_MW_WSLAYOUT_FONT_MINUS, 380, 700)

  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoHScroll(HWND hWnd, int id, int pos) override;

private:
  void LoadLayoutPrefs();
  void SaveLayoutPrefs();
  void ResetDefaults();
  void UpdateSizeLabel();
  void UpdateModeState();
  bool m_bAutoApply = false;
};

// ── Concrete subclass: Welcome window (no-presets prompt) ──

class WelcomeWindow : public ToolWindow {
public:
  WelcomeWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"Welcome", L"MDropDX12WelcomeWnd", L"Welcome", 0, 0, 0, 300, 400)

  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
};

// ── ModalDialog — lightweight base class for modal popup dialogs ──────
// No thread, no INI persistence, no pin/font buttons.  Shares the same
// dark theme helpers as ToolWindow so popups get correct theming for free.

class ModalDialog {
protected:
    Engine*     m_pEngine;
    HWND        m_hWnd = NULL;
    HWND        m_hParent = NULL;
    HFONT       m_hFont = NULL;
    std::vector<HWND> m_childCtrls;
    bool        m_bDone = false;
    bool        m_bResult = false;

    virtual const wchar_t* GetDialogTitle() const = 0;
    virtual const wchar_t* GetDialogClass() const = 0;
    virtual void DoBuildControls(int clientW, int clientH) = 0;
    virtual LRESULT DoCommand(int id, int code, LPARAM lParam) { return -1; }
    virtual LRESULT DoNotify(NMHDR* pnm) { return -1; }
    virtual LRESULT DoMessage(UINT msg, WPARAM wParam, LPARAM lParam) { return -1; }

    // Layout metrics — computed from actual font, consistent with ToolWindow
    struct BaseLayout { int lineH, gap, margin, labelW; };
    BaseLayout GetBaseLayout();

    // Resize window to fit content height (call at end of DoBuildControls)
    void FitToContent(int clientW, int contentH);

public:
    ModalDialog(Engine* pEngine) : m_pEngine(pEngine) {}
    virtual ~ModalDialog() {}

    bool Show(HWND hParent, int clientW, int clientH);
    void EndDialog(bool result) { m_bResult = result; m_bDone = true; }
    void TrackControl(HWND h) { if (h) m_childCtrls.push_back(h); }
    bool IsChecked(int id) const;
    void SetChecked(int id, bool checked);
    int  GetLineHeight();
    HFONT GetFont() const { return m_hFont; }
    HWND GetHWND() const { return m_hWnd; }

private:
    static LRESULT CALLBACK ModalWndProc(HWND, UINT, WPARAM, LPARAM);
};

// ── Shared dark theme helpers ─────────────────────────────────────────
// Used by both ToolWindow::BaseWndProc and ModalDialog::ModalWndProc
// to avoid duplicating theme painting across popup dialogs.

// Handle WM_CTLCOLOREDIT/LISTBOX/STATIC/BTN/DLG. Returns brush LRESULT if dark, 0 if not.
LRESULT HandleDarkCtlColor(Engine* p, UINT msg, WPARAM wParam, LPARAM lParam);

// Handle WM_DRAWITEM for ODT_TAB, ODT_BUTTON (checkbox/radio/button), ODT_STATIC (swatch).
// Does NOT handle pin button (ToolWindow-specific). Returns TRUE if painted, FALSE if not.
LRESULT HandleDarkDrawItem(Engine* p, DRAWITEMSTRUCT* pDIS);

// Handle WM_ERASEBKGND — fills with dark or light bg. Returns 1.
LRESULT HandleDarkEraseBkgnd(Engine* p, HWND hWnd, HDC hdc);

// Apply DWM dark mode attributes to a window (title bar, border, caption color).
void ApplyDarkThemeToWindow(Engine* p, HWND hWnd);

// Apply SetWindowTheme to tracked child controls (tab, listview, hotkey, etc).
void ApplyDarkThemeToChildren(Engine* p, const std::vector<HWND>& ctrls);

// Dark tab background subclass — apply to tab controls for dark theme support.
LRESULT CALLBACK DarkTabSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

// Paint a ListView header in dark theme via NM_CUSTOMDRAW.
// Returns LRESULT to return from WndProc; sets *pHandled=true if the notification was handled.
LRESULT PaintDarkListViewHeader(NMHDR* pnm, LPARAM lParam, HWND hListView,
                                COLORREF colBg, COLORREF colBorder, COLORREF colText,
                                bool* pHandled);

// ── Shared Action Edit Dialog ────────────────────────────────────────────
// Used by both ButtonBoardWindow and HotkeysWindow for editing actions.
// Supports: action type dropdown, label, payload/command, file browse,
// optional hotkey binding (key + scope).

struct ActionEditData {
    // Action configuration
    ButtonAction actionType = ButtonAction::None;
    std::wstring label;
    std::wstring payload;       // command string, file path, etc.

    // Key binding (shown when showKeyBinding == true)
    bool showKeyBinding = true;
    UINT modifiers = 0;         // Local binding: MOD_ALT, MOD_CONTROL, MOD_SHIFT
    UINT vk = 0;                // Local binding: virtual key code (0 = unbound)
    HotkeyScope scope = ::HKSCOPE_LOCAL;  // For user hotkeys (single-binding mode)

    // Global binding (built-in hotkeys only; separate from local)
    UINT globalMod = 0;         // Global binding modifiers
    UINT globalVK = 0;          // Global binding VK (0 = unbound)

    // Built-in hotkey mode: action type is read-only, only key + scope editable
    bool isBuiltInHotkey = false;
    std::wstring actionName;    // display name for built-in (read-only)

    // Context
    Engine* pEngine = nullptr;
    bool accepted = false;
};

// Show the shared action edit dialog.  Returns true if user pressed OK.
bool ShowActionEditDialog(HWND hParent, ActionEditData& data);

} // namespace mdrop
