#pragma once
/*
  ToolWindow — reusable base class for standalone tool windows on their own threads.
  Provides: thread + message pump, dark theme painting, pin button (always-on-top),
  font +/- buttons with cross-window sync, window size persistence, owner-draw rendering.

  Subclass to create specific windows (Displays, Sticky Notes, etc.).
*/

#include <Windows.h>
#include <vector>
#include <thread>
#include <atomic>
#include "engine_helpers.h"
#include "midi_input.h"

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

// ── Concrete subclass: Spout / Displays window ──

class DisplaysWindow : public ToolWindow {
public:
  DisplaysWindow(Engine* pEngine);

protected:
  const wchar_t* GetWindowTitle() const override { return L"Spout / Displays"; }
  const wchar_t* GetWindowClass() const override { return L"MDropDX12DisplaysWnd"; }
  const wchar_t* GetINISection() const override  { return L"Displays"; }
  int GetPinControlID() const override       { return IDC_MW_DISPLAYS_PIN; }
  int GetFontPlusControlID() const override  { return IDC_MW_DISP_FONT_PLUS; }
  int GetFontMinusControlID() const override { return IDC_MW_DISP_FONT_MINUS; }
  int GetMinWidth() const override  { return 400; }
  int GetMinHeight() const override { return 350; }

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
  const wchar_t* GetWindowTitle() const override { return L"Song Info"; }
  const wchar_t* GetWindowClass() const override { return L"MDropDX12SongInfoWnd"; }
  const wchar_t* GetINISection() const override  { return L"SongInfo"; }
  int GetPinControlID() const override       { return IDC_MW_SONGINFO_PIN; }
  int GetFontPlusControlID() const override  { return IDC_MW_SONGINFO_FONT_PLUS; }
  int GetFontMinusControlID() const override { return IDC_MW_SONGINFO_FONT_MINUS; }
  int GetMinWidth() const override  { return 380; }
  int GetMinHeight() const override { return 400; }

  void DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
};

// ── Concrete subclass: Settings window ──

class SettingsWindow : public ToolWindow {
public:
  SettingsWindow(Engine* pEngine);
  void EnsureVisible();  // called from Engine on WM_SIZE

protected:
  const wchar_t* GetWindowTitle() const override { return L"MDropDX12 Settings"; }
  const wchar_t* GetWindowClass() const override { return L"MDropDX12SettingsWnd"; }
  const wchar_t* GetINISection() const override  { return L"Settings"; }
  int GetPinControlID() const override       { return IDC_MW_SETTINGS_PIN; }
  int GetFontPlusControlID() const override  { return IDC_MW_FONT_PLUS; }
  int GetFontMinusControlID() const override { return IDC_MW_FONT_MINUS; }
  int GetMinWidth() const override  { return 500; }
  int GetMinHeight() const override { return 450; }

  DWORD GetCommonControlFlags() const override;
  bool  AcceptsDragDrop() const override { return true; }
  void  OnAlreadyOpen() override;
  void  OnResize() override;
  void  RebuildFonts() override;

  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoHScroll(HWND hWnd, int id, int pos) override;
  LRESULT DoNotify(HWND hWnd, NMHDR* pnm) override;
  LRESULT DoMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override;
  void    DoDestroy() override;

private:
  int  m_lastSeenIPCSeq = 0;

  void LayoutControls();
  void RefreshIPCList();
  void UpdateVisualUI();
  void UpdateColorsUI();
  void ResetToFactory();
  void ResetToUserDefaults();
};

// ── Concrete subclass: Hotkeys window ──

class HotkeysWindow : public ToolWindow {
public:
  HotkeysWindow(Engine* pEngine);

protected:
  const wchar_t* GetWindowTitle() const override { return L"Hotkeys"; }
  const wchar_t* GetWindowClass() const override { return L"MDropDX12HotkeysWnd"; }
  const wchar_t* GetINISection() const override  { return L"HotkeysWnd"; }
  int GetPinControlID() const override       { return IDC_MW_HOTKEYS_PIN; }
  int GetFontPlusControlID() const override  { return IDC_MW_HOTKEYS_FONT_PLUS; }
  int GetFontMinusControlID() const override { return IDC_MW_HOTKEYS_FONT_MINUS; }
  int GetMinWidth() const override  { return 560; }
  int GetMinHeight() const override { return 480; }

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
};

// ── Concrete subclass: MIDI window ──

class MidiWindow : public ToolWindow {
public:
  MidiWindow(Engine* pEngine);

protected:
  const wchar_t* GetWindowTitle() const override { return L"MIDI"; }
  const wchar_t* GetWindowClass() const override { return L"MDropDX12MidiWnd"; }
  const wchar_t* GetINISection() const override  { return L"MidiWnd"; }
  int GetPinControlID() const override       { return IDC_MW_MIDI_PIN; }
  int GetFontPlusControlID() const override  { return IDC_MW_MIDI_FONT_PLUS; }
  int GetFontMinusControlID() const override { return IDC_MW_MIDI_FONT_MINUS; }
  int GetMinWidth() const override  { return 580; }
  int GetMinHeight() const override { return 550; }

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
  const wchar_t* GetWindowTitle() const override { return L"Sprites"; }
  const wchar_t* GetWindowClass() const override { return L"MDropDX12SpritesWnd"; }
  const wchar_t* GetINISection() const override  { return L"SpritesWnd"; }
  int GetPinControlID() const override       { return IDC_MW_SPRITES_WIN_PIN; }
  int GetFontPlusControlID() const override  { return IDC_MW_SPRITES_WIN_FONT_PLUS; }
  int GetFontMinusControlID() const override { return IDC_MW_SPRITES_WIN_FONT_MINUS; }
  int GetMinWidth() const override  { return 480; }
  int GetMinHeight() const override { return 500; }

  void    OnResize() override;
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
  const wchar_t* GetWindowTitle() const override { return L"Messages"; }
  const wchar_t* GetWindowClass() const override { return L"MDropDX12MessagesWnd"; }
  const wchar_t* GetINISection() const override  { return L"MessagesWnd"; }
  int GetPinControlID() const override       { return IDC_MW_MESSAGES_WIN_PIN; }
  int GetFontPlusControlID() const override  { return IDC_MW_MESSAGES_WIN_FONT_PLUS; }
  int GetFontMinusControlID() const override { return IDC_MW_MESSAGES_WIN_FONT_MINUS; }
  int GetMinWidth() const override  { return 420; }
  int GetMinHeight() const override { return 480; }

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
  const wchar_t* GetWindowTitle() const override { return L"Presets"; }
  const wchar_t* GetWindowClass() const override { return L"MDropDX12PresetsWnd"; }
  const wchar_t* GetINISection() const override  { return L"PresetsWnd"; }
  int GetPinControlID() const override       { return IDC_MW_PRESETS_PIN; }
  int GetFontPlusControlID() const override  { return IDC_MW_PRESETS_FONT_PLUS; }
  int GetFontMinusControlID() const override { return IDC_MW_PRESETS_FONT_MINUS; }
  int GetMinWidth() const override  { return 420; }
  int GetMinHeight() const override { return 400; }

  void    OnResize() override;
  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override;
  void    DoDestroy() override;

private:
  HWND m_hCurrentPreset = NULL, m_hBrowsePreset = NULL;
  HWND m_hPresetDir = NULL, m_hBrowseDir = NULL;
  HWND m_hLblPreset = NULL, m_hLblDir = NULL;
  HWND m_hList = NULL;
  HWND m_hBtnPrev = NULL, m_hBtnNext = NULL, m_hBtnCopy = NULL;
  HWND m_hBtnUp = NULL, m_hBtnInto = NULL, m_hBtnFilter = NULL;
  HWND m_hLblSens = NULL, m_hEditSens = NULL;
  HWND m_hLblBlend = NULL, m_hEditBlend = NULL;
  HWND m_hLblTime = NULL, m_hEditTime = NULL;
  HWND m_hChkHardCuts = NULL, m_hChkLock = NULL, m_hChkSeq = NULL;
  int  m_nTopY = 0;

  void LayoutControls();
  void RefreshPresetList();
  void UpdateCurrentPresetDisplay();
  void UpdatePresetDirDisplay();
  void NavigatePresetDirUp();
  void NavigatePresetDirInto(int sel);
};

// ── Concrete subclass: Button Board window ──

class ButtonPanel; // forward (defined in button_panel.h)

class ButtonBoardWindow : public ToolWindow {
public:
  ButtonBoardWindow(Engine* pEngine);

protected:
  const wchar_t* GetWindowTitle() const override { return L"Button Board"; }
  const wchar_t* GetWindowClass() const override { return L"MDropDX12BoardWnd"; }
  const wchar_t* GetINISection() const override  { return L"BoardWnd"; }
  int GetPinControlID() const override       { return IDC_MW_BOARD_PIN; }
  int GetFontPlusControlID() const override  { return IDC_MW_BOARD_FONT_PLUS; }
  int GetFontMinusControlID() const override { return IDC_MW_BOARD_FONT_MINUS; }
  int GetMinWidth() const override  { return 300; }
  int GetMinHeight() const override { return 250; }

  void    OnResize() override;
  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
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
};

// Paint a ListView header in dark theme via NM_CUSTOMDRAW.
// Returns LRESULT to return from WndProc; sets *pHandled=true if the notification was handled.
LRESULT PaintDarkListViewHeader(NMHDR* pnm, LPARAM lParam, HWND hListView,
                                COLORREF colBg, COLORREF colBorder, COLORREF colText,
                                bool* pHandled);

} // namespace mdrop
