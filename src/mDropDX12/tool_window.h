#pragma once
/*
  ToolWindow — reusable base class for standalone tool windows on their own threads.
  Provides: thread + message pump, dark theme painting, pin button (always-on-top),
  font +/- buttons with cross-window sync, window size persistence, owner-draw rendering.

  Subclass to create specific windows (Displays, Sticky Notes, etc.).
*/

#include <Windows.h>
#include <climits>
#include <deque>
#include <initializer_list>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include "engine_helpers.h"
#include "display_panel.h"
#include "midi_input.h"
#include "button_panel.h"   // ButtonAction
#include "hotkeys.h"        // HotkeyScope
#include "sci_editor.h"     // SciEditor, CodeLang (Preset Editor)

namespace mdrop {

class Engine;  // forward declaration — full type in engine.h

class ToolWindow {
protected:
  Engine*     m_pEngine;
  HWND        m_hWnd = NULL;
  std::thread m_thread;
  std::atomic<bool> m_bThreadRunning{false};
  bool        m_bOnTop = false;
  bool        m_bWasMaximized = false;  // last WM_SIZE state, for relayout/persist
  HFONT       m_hFont = NULL;
  HFONT       m_hFontBold = NULL;
  HFONT       m_hPinFont = NULL;       // Segoe MDL2 Assets for pin icon
  // The same face, 10% smaller, for the other glyph buttons in the title row.
  // The refresh wheel fills its em box more than the pin does, so at one size
  // it reads as the taller of the two even though both are square.
  HFONT       m_hGlyphFont = NULL;
  int         m_nWndW, m_nWndH;        // current (persisted) size
  int         m_nDefaultW, m_nDefaultH; // default size if no INI
  // Persisted position. INT_MIN means "never saved" -- plain -1 cannot mean
  // that, because a monitor placed left of or above the primary one gives real
  // windows negative coordinates.
  int         m_nPosX = INT_MIN, m_nPosY = INT_MIN;
  std::vector<HWND> m_childCtrls;      // all child HWNDs (for rebuild + dark theme)
  std::vector<HWND> m_tooltips;        // owned popups; children teardown misses these
  // Backing store for AttachTip. TTM_ADDTOOLW keeps the POINTER it is handed
  // rather than copying the text, so a caller's temporary would leave the
  // tooltip reading freed memory. A deque and not a vector: pushing another
  // string must not move the ones already handed out.
  std::deque<std::wstring> m_tipText;
  bool        m_bFirstBuild = true;    // cleared once DoBuildControls has run

  // Anchored layout. m_anchorClient is the client size the rectangles were
  // measured at, so the deltas are always relative to the build, not to the
  // last resize -- rounding cannot accumulate across a hundred mouse-moves.
  struct AnchoredControl {
    HWND hwnd = NULL;
    unsigned edges = 0;
    RECT rect = {};
  };
  std::vector<AnchoredControl> m_anchors;
  SIZE m_anchorClient = { 0, 0 };

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

  // Extra WS_* bits for the frame (e.g. WS_MINIMIZEBOX | WS_MAXIMIZEBOX).
  virtual DWORD GetExtraWindowStyle() const { return 0; }

  // Tool-window frame: small caption, no taskbar button -- and Windows hides
  // the minimise/maximise buttons on one no matter what styles are set. A
  // window that wants those must return false here as well as asking for the
  // styles above.
  virtual bool UsesToolWindowFrame() const { return true; }

  // Called on WM_SIZE.
  //
  // A window that has anchored its controls is laid out; one that has not is
  // rebuilt from scratch, which is what every window used to do. See
  // AnchorControl for how a window moves from the second to the first.
  virtual void OnResize() {
    if (HasAnchors()) ApplyAnchors();
    else RebuildFonts();
  }

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

  // ── Preset loading from a tool window ──
  // Every tool window pumps messages on its own thread, so calling
  // Engine::LoadPreset() from a handler runs it off the render thread. That
  // races the render thread's LoadPresetTick() over m_presetLoadThread — two
  // threads joining or reassigning one std::thread calls std::terminate(),
  // which aborts the process (WER 0xC0000409 / FAST_FAIL_FATAL_APP_EXIT) with
  // no chance for any handler to log it. These marshal onto the render thread
  // instead; use them and never call LoadPreset/NextPreset/PrevPreset here.
  // nPresetIndex >= 0 loads by index; pass -1 with szPath to load a full path.
  void RequestLoadPreset(int nPresetIndex, const wchar_t* szPath = nullptr,
                         float fBlendTime = -1.0f);
  // +1 = next, -1 = previous. fBlendTime < 0 uses the user's configured
  // blend time (the default every existing caller relies on); pass an
  // explicit value (e.g. 0.0f) for a caller that needs an instant cut.
  void RequestNavPreset(int nDirection, float fBlendTime = -1.0f);

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


  // ── Anchored layout: resizing without rebuilding ──
  //
  // The rule is the one every GUI toolkit uses, and stating it plainly explains
  // every layout that follows:
  //
  //   anchored to ONE edge   -> the control keeps its distance from that edge,
  //                             so it MOVES as the window grows;
  //   anchored to BOTH edges -> it keeps its distance from both, so it
  //                             STRETCHES.
  //
  // A control that is never anchored simply stays where it was built, which is
  // what every control did before this existed -- so a window adopts anchoring
  // one control at a time, and a window that adopts none behaves exactly as it
  // always has.
  //
  // Once a window anchors anything, OnResize lays it out instead of destroying
  // and rebuilding it: no flicker, no lost text, no re-reading files, and one
  // DeferWindowPos batch instead of a teardown per mouse-move.
  enum ToolAnchor : unsigned {
    kAnchorLeft   = 1u << 0,
    kAnchorTop    = 1u << 1,
    kAnchorRight  = 1u << 2,
    kAnchorBottom = 1u << 3,

    kAnchorTopLeft       = kAnchorLeft | kAnchorTop,      // stay put (the default)
    kAnchorTopRight      = kAnchorTop | kAnchorRight,     // slide with the right edge
    kAnchorBottomLeft    = kAnchorLeft | kAnchorBottom,   // slide with the bottom
    kAnchorBottomRight   = kAnchorRight | kAnchorBottom,
    kAnchorStretchWide   = kAnchorLeft | kAnchorTop | kAnchorRight,
    kAnchorStretchBottom = kAnchorLeft | kAnchorRight | kAnchorBottom,
    kAnchorFill          = kAnchorLeft | kAnchorTop | kAnchorRight | kAnchorBottom,
  };

  // Register a control with the layout. Call it in DoBuildControls, straight
  // after creating the control: the rectangle it has at that moment, and the
  // client size at that moment, are what the anchors are measured against.
  void AnchorControl(HWND h, unsigned edges);

  // Move this window back onto the render window's monitor if it has ended up
  // elsewhere. Only acts in testing mode, and only for a window that places
  // itself there (PlacesItselfInTestingMode).
  //
  // Placement is decided once, when the window opens, and at startup that can
  // be too early: the render window has been created but has not necessarily
  // reached its final display yet, so "the render monitor" is answered
  // correctly and becomes wrong a moment later. The window then sits on the
  // display the render window was PASSING THROUGH. Re-checking costs two
  // MonitorFrom calls a second and is the only thing that does not depend on
  // knowing the exact startup ordering.
  //
  // Outside testing mode this does nothing, so it can never fight a window the
  // user deliberately dragged to another screen.
  void EnsureOnRenderMonitor();

  // Track and anchor in one call -- the usual form:
  //     TrackAnchored(CreateSlider(...), kAnchorStretchWide);
  HWND TrackAnchored(HWND h, unsigned edges) {
    TrackControl(h);
    AnchorControl(h, edges);
    return h;
  }

  // Enable or disable every control this window has built.
  //
  // For a window with a master switch: with the subsystem off there is nothing
  // behind its controls, and offering them anyway means clicks that quietly do
  // nothing. `keep` names the ids that stay live -- normally just the switch
  // itself.
  //
  // The window's own furniture is always exempt: the pin, the two font
  // buttons and the tab strip. Being able to look at the other tabs before
  // turning something on is the point of having tabs.
  //
  // Owner-drawn controls are painted by HandleDarkDrawItem, which honours
  // ODS_DISABLED, and disabled labels are dimmed in HandleDarkCtlColor -- so a
  // control that ignores clicks also looks like it does.
  void SetControlsEnabled(bool enable, std::initializer_list<int> keep = {});

  // Show a particular page, whether or not the window is open yet.
  //
  // The page is written to the INI first, so a window that has not been built
  // lands on it when SelectInitialTab runs; a window that IS open is told
  // through a posted message, because tool windows each run on their own
  // thread and switching a tab touches its controls.
  //
  // On the base class deliberately: "here is the setting you want, in the
  // window that owns it" is a link any tool window may want to offer, not a
  // one-off.
  void ShowTab(int page);

  // Reveal one control and put the caret on it, wherever it lives.
  //
  // The caller does not have to know which tab holds it: the page is found by
  // looking the control up in the per-page lists, switched to, and only then
  // focused. That is what makes "take me to that setting" answerable from a
  // remote, which cannot see the window at all.
  void ShowControl(int ctrlId);

  // Press a control, as a click would. Answers the same way ShowControl does.
  void ClickControl(int ctrlId);

  // Move the window. `centreOnRender` puts it in the middle of whichever
  // display the visualiser is on, which is the one thing a caller cannot work
  // out for itself -- it does not know the window's size or where the render
  // window went.
  void MoveTo(int x, int y);
  void CentreOnRender();

  // Top-left for a window centred on the render window's display.
  void RenderCentredPos(int& outX, int& outY) const;

 private:
  // A control asked for before the window had been built.
  //
  // A tool window is created on its OWN thread, so for a moment after Open()
  // returns it has no controls at all. Waiting for it from the calling thread
  // was the first attempt and it was fragile -- the Settings window takes
  // longer to build than any wait worth holding an IPC thread for, and a
  // request for a control it certainly has came back "unknown". The intent is
  // remembered instead and applied the moment the window is ready, and the
  // WINDOW reports what happened rather than the caller guessing.
  int m_pendingShowCtrl = 0;

  // This window was positioned by testing mode, so its position is disposable
  // and must never be saved -- not even after testing mode has ended.
  // Testing mode normally moves every tool window to the middle of the render
  // window's monitor and throws that placement away afterwards, because a test
  // must not repossess the user's layout. A window can opt out:
  //
  //   * if it was OPEN when the app was last used, its own position is kept --
  //     read, never written, so the arrangement survives the run untouched;
  //   * otherwise it opens in the LOWER CORNER of the render display rather
  //     than over the middle of it, so it never covers the frame being
  //     measured.
  //
  // Only the Remote window wants this today. Everything else still centres,
  // which is what a window a test opens on purpose should do.
  virtual bool PlacesItselfInTestingMode() const { return false; }

  bool m_bPlacedByTestingMode = false;
  // Was this window open when the app was last used? Persisted as WasOpen,
  // written on create and cleared on an explicit close -- nothing closes tool
  // windows when the program exits, so a window still open at shutdown leaves
  // the flag set, which is exactly the question being asked.
  bool m_bWasOpenLastSession = false;

  // Broadcast the outcome of a UI_SHOW request, from the window's own thread
  // once it has actually acted.
  void ReportUiShown(int ctrlId, bool found);
  void ReportUiClicked(int ctrlId, bool pressed);

 public:


  // Has this window anchored anything? OnResize uses it to decide between
  // laying out and rebuilding.
  bool HasAnchors() const { return !m_anchors.empty(); }

  // Move every anchored control to suit the current client size.
  void ApplyAnchors();

  // Compute line height from current font
  int GetLineHeight();

  // The line height a given font size produces, measured WITHOUT a window.
  //
  // GetLineHeight needs m_hWnd and m_hFont, and the size a window opens at is
  // decided before either exists -- which is precisely where the font-blindness
  // of issue 157 lives. Measured on a screen DC rather than guessed: the font
  // is created with an explicit cell height, so the answer does not depend on
  // which monitor's DC it is taken from.
  static int LineHeightForFontSize(int nFontSize);

  // GetMinWidth/GetMinHeight scaled to the font in use.
  //
  // The declared minimum of every tool window is a constant chosen against a
  // line height of 26, the same baseline every MulDiv(n, lineH, 26) in the
  // layouts is relative to. At a larger font the layout grows and the minimum
  // does not, so the "minimum" stops meaning what it says -- a window at it has
  // controls outside itself. Scaling keeps the promise the number was making.
  int ScaledMinWidth();
  int ScaledMinHeight();

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

  // Points the window-geometry store at the resources directory and
  // moves any [*Wnd] sections still in settings.ini into it. Called
  // once at startup, before any tool window reads its position.
  static void InitWindowStore(const wchar_t* resourceDir);

  // Resize so every control fits at the current font size. Bound to the
  // refresh button beside the pin, which every tool window now has. Mainly for
  // a window that has been shrunk by accident: its controls are still laid out
  // past the client edge, so the fit finds them and grows back to them.
  //
  // Pressed by hand, so it also restores the window's DESIGNED size, scaled to
  // the font: the button means "put this window right", not merely "stop
  // cutting things off".
  void FitToContents();

  // The same measurement, run once when the window opens, growing ONLY what is
  // actually hanging outside the client edge (issue 157).
  //
  // A tool window's size comes either from what the user last left it at or
  // from a default constant, and neither knows anything about the font. The
  // layouts do: every one of them is built from MulDiv(n, lineH, 26), so they
  // grow with the font while the frame does not. At a large font four windows
  // opened with controls past their own edge, and one of them -- Controller --
  // put its only three buttons below the client bottom, where they cannot be
  // clicked at all. A window that comes up unusable cannot be repaired with a
  // button inside it.
  //
  // Deliberately NOT the button's behaviour. This is a repair and nothing
  // more: a dimension moves only when something overflows it, so a window the
  // user has deliberately made small is left exactly as it is.
  void FitClippedContentsOnOpen();

  // Both of the above. bRestoreDesignedSize is the only difference.
  void FitContents(bool bRestoreDesignedSize);

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

  // ── Rebuilding without taking the user's work with it ──
  //
  // RebuildFonts destroys every control and calls DoBuildControls again. That
  // is how a font-size change is applied, and how most windows handle a resize.
  // It used to throw away whatever the user was in the middle of: text typed
  // into an edit, the row selected in a list, how far a list was scrolled,
  // which control had focus. Resizing the Shader Editor replaced pasted GLSL
  // with the text the shader pass held when the window opened; resizing the
  // Controller window reverted the JSON to the last saved copy.
  //
  // CaptureControlState reads that back out of the controls before they are
  // destroyed, keyed by control ID, and RestoreControlState puts it back
  // afterwards. Generic on purpose -- every window gets it without having to
  // remember, and a window added tomorrow gets it too.
  //
  // Text is only restored when the user actually typed it (EM_GETMODIFY), or
  // when the rebuilt control came back empty and we had something. That
  // matters: plenty of edits are refreshed from live state by DoBuildControls,
  // and putting the old text back over a deliberate refresh would be its own
  // bug.
  struct ControlState {
    int id = 0;
    std::wstring cls;      // only restored onto a control of the same class
    std::wstring text;
    bool hasText = false;
    bool userModified = false;        // EM_GETMODIFY: the user typed this
    int selStart = -1, selEnd = -1;   // caret / selection inside an edit
    int selection = -1;               // chosen row in a list or combo
    int topIndex = -1;                // first visible row, i.e. scroll position
    bool hadFocus = false;
  };
  std::vector<ControlState> CaptureControlState() const;

  // Restoring happens in two steps with OnRebuilt() between them, and the order
  // is the point:
  //   1. put the list selections and scroll positions back;
  //   2. let the window bring its own members into line with them (OnRebuilt);
  //   3. put the user's unsaved typing back LAST, so a window refreshing its
  //      edits from the newly-selected row cannot overwrite it.
  void RestoreControlSelections(const std::vector<ControlState>& saved);
  void RestoreControlText(const std::vector<ControlState>& saved);
  HWND MatchingControl(const ControlState& s) const;

  // Called after a rebuild has recreated, themed and restored the controls.
  // Override when the window keeps state in MEMBERS that DoBuildControls
  // resets -- restoring the control is not enough if the window also has to
  // agree with it about which row is selected.
  virtual void OnRebuilt() {}

  // Called after ShowPage has made a page visible, tab switch included.
  //
  // Override when a page holds controls that must NOT all be shown together --
  // two views sharing one rectangle, say. ShowPage shows every control it was
  // given, so a window that hides one of them has to say so again afterwards.
  virtual void OnPageShown(int /*page*/) {}

  // Register an owned tooltip so a rebuild destroys it.
  //
  // Tooltips are created WS_POPUP with the tool window as their OWNER, not as
  // children, so the GW_CHILD teardown in RebuildFonts never saw them: every
  // rebuild leaked one window per tooltip, and BuildBaseControls makes one for
  // the pin button on all 25 tool windows.
  void TrackTooltip(HWND h) { if (h) m_tooltips.push_back(h); }

  // Give one control a tooltip, and own the text.
  //
  // Shared because there were six hand-rolled copies of the same eleven lines,
  // and because issue 159 is about to need many more: a label shortened to fit
  // its box keeps its full wording here rather than losing it -- Shane, asked
  // how the detail should survive the shortening, answered "Maybe tooltips".
  //
  // One tooltip control per control, deliberately. They used to be shared, and
  // in the Presets window that meant moving the Copy button took the Edit
  // button's tooltip with it. TrackTooltip owns the teardown either way.
  void AttachTip(HWND hCtrl, const wchar_t* tip);
  void AttachTip(HWND hCtrl, const std::wstring& tip) { AttachTip(hCtrl, tip.c_str()); }

  // A label, with its full wording as a tooltip (issue 159).
  //
  // The pairing is the point: the short form is what fits, the long form is
  // what it means, and neither is any use without the other.
  HWND CreateLabelTip(HWND hw, const wchar_t* text, const wchar_t* tip,
                      int x, int y, int w, int h, HFONT hFont);

  // True while the window is being built for the FIRST time. Windows whose
  // DoBuildControls loads a document from disk (Sprites re-reads sprites.ini)
  // must only do that on the first build; on a rebuild it would discard
  // everything the user has changed but not yet saved.
  bool IsFirstBuild() const { return m_bFirstBuild; }

  // Create a report-mode ListView with standard styles. Does NOT call TrackControl().
  // When sortable=true, column headers are clickable (omits LVS_NOSORTHEADER).
  HWND CreateThemedListView(int id, int x, int y, int w, int h,
                            bool visible = true, bool sortable = false);

  // Common control setup: creates fonts, font +/- buttons, pin button with tooltip.
  // Returns the Y position below the header row for subclasses to continue from.
  // Populates lineH, gap, x, rw, clientW for the caller.
  struct BaseLayout { int y, lineH, gap, x, rw, clientW; };
  BaseLayout BuildBaseControls();
  // Re-anchors the right-edge base controls (the pin) after a resize. Called
  // from WM_SIZE before OnResize, so subclasses never have to know about it.
  void LayoutBaseControls();

private:
  void CreateOnThread();
  void LoadWindowPosition();
  void SaveWindowPosition();
  void SaveOpenState(bool open);

  // Pull a saved position back onto a monitor that is actually attached.
  // Displays get unplugged and desktops get rearranged; without this the
  // window opens at coordinates that no longer belong to any screen.
  static void ClampToVisibleMonitor(int& posX, int& posY, int w, int h);

  // The single shared WndProc dispatches to virtual methods
  // The registered procedure is a thin SEH guard; the real one is Impl.
  // Separate functions because __try cannot share a frame with C++ unwinding.
  // See seh_guard.h -- a fault here is otherwise unreportable and fatal.
  static LRESULT CALLBACK BaseWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
  static LRESULT CALLBACK BaseWndProcImpl(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
  static const wchar_t* SehContextFor(HWND hWnd, UINT uMsg);

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
  LRESULT DoMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override;
  // The outputs page keeps the layout panel and the detail list in the same
  // rectangle, so a page that has just been shown needs one of them hidden
  // again. Without this, switching tabs drew both.
  void OnPageShown(int page) override;

private:
  void BuildOutputsPage(int x, int y, int rw, int lineH, int gap);
  void BuildVideoInputPage(int x, int y, int rw, int lineH, int gap);
  // Last thing the list said about the children, so the timer can tell whether
  // rebuilding it would change anything.
  std::wstring m_lastChildSig;
  // The arrangement view. The list box is still there underneath it and is
  // still where the selection lives; only one of the two is visible.
  DisplayPanel m_layoutPanel;
  void ApplyDisplayViewMode();
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
  LRESULT DoHScroll(HWND hWnd, int id, int pos) override;
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

  // The build path selects row 0 and fills the detail panes from it, so after a
  // rebuild the window believed row 0 was current while the list showed the row
  // the user had actually chosen. RestoreControlSelections has already put the
  // selection back by the time this runs; read it and agree with it.
  void OnRebuilt() override;
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
  LRESULT DoMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) override;
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
  HWND m_hBtnPrev = NULL, m_hBtnNext = NULL, m_hBtnCopy = NULL, m_hBtnEdit = NULL;
  // Segoe MDL2 Assets, for every glyph button on this window -- not just Copy,
  // which is what it held when it was called m_hCopyFont.
  HFONT m_hIconBtnFont = NULL;
  // A themed glyph button with a tooltip. See the comment on IconBtnW in
  // engine_presets_ui.cpp for why these rows use icons at all.
  HWND IconBtn(HWND hw, const wchar_t* glyph, int id, int x, int y, int w, int h,
               const wchar_t* tip);
  HWND m_hBtnUp = NULL, m_hBtnInto = NULL, m_hBtnFilter = NULL, m_hBtnSubdir = NULL;
  HWND m_hLblTag = NULL, m_hTagFilter = NULL, m_hBtnImportTags = NULL;
  HWND m_hLblListName = NULL, m_hPresetListCombo = NULL, m_hBtnListSave = NULL, m_hBtnListClear = NULL;
  HWND m_hLblSens = NULL, m_hEditSens = NULL;
  HWND m_hLblBlend = NULL, m_hEditBlend = NULL;
  HWND m_hLblTime = NULL, m_hEditTime = NULL;
  HWND m_hChkHardCuts = NULL, m_hChkLock = NULL, m_hChkSeq = NULL;
  // The LIVE lock, distinct from m_hChkLock's startup preference. Kept in
  // step with the engine by the 500 ms WM_TIMER, because a hotkey, the game
  // controller, IPC and the silence auto-lock can all change it while this
  // window is open.
  HWND m_hChkLockNow = NULL;
  HWND m_hStartupCombo = NULL, m_hLblStartup = NULL;
  HWND m_hStartupPath = NULL, m_hStartupBrowse = NULL;
  // Shows which preset szPresetStartup actually names, or (random).
  void UpdateStartupPresetField();
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
  HWND m_hBtnEdit = NULL;   // opens the Preset Editor on the selected preset
  HWND m_hBtnImport = NULL;
  HWND m_hBtnScan = NULL;
  HWND m_hBtnResetUse = NULL;
  // Per-preset override slots for the selected row. Three states each, and the
  // middle one is the one that gets forgotten:
  //   index 0  (inherit from tags)  member absent
  //   index 1  (none)               member present and empty -- suppresses the tag rule
  //   index 2+ a name               member present and named
  HWND m_hShaderCombo = NULL;
  HWND m_hVfxCombo = NULL;
  HWND m_hAudioCombo = NULL;
  HWND m_hCanvasCombo = NULL;
  HWND m_hLblCanvas = NULL;
  HWND m_hDampCombo = NULL;
  HWND m_hLblDamp = NULL;
  // Labels are tracked so LayoutControls can move them too. Moving only the
  // combos on resize left the labels behind at their build-time positions.
  HWND m_hLblFilter = NULL;
  HWND m_hLblShader = NULL;
  HWND m_hLblVfx = NULL;
  HWND m_hLblAudio = NULL;
  std::vector<std::wstring> m_shaderNames;  // parallel to m_hShaderCombo, from index 2
  std::vector<std::wstring> m_vfxProfNames; // parallel to m_hVfxCombo, from index 2
  std::vector<std::wstring> m_audioProfNames; // parallel to m_hAudioCombo, from index 2
  HWND m_hBtnSearchClear = NULL;   // the "x" that empties the search box
  HWND m_hBtnFindCopies  = NULL;   // hash-scans the tree for duplicate files
  HWND m_hBtnPurgeMissing = NULL;  // drops entries whose preset file is gone
  int  m_nTopY = 0;
  int  m_nFilterMode = 0; // 0=All, 1=Favorite, 2=Error, 3=Skip, 4=Broken, 5=Duplicates
  HWND m_hSearchEdit = NULL;
  HWND m_hLblSearch  = NULL;
  // THIS WINDOW OUTLIVES ITS HWND.  Engine holds the AnnotationsWindow in a
  // unique_ptr for the life of the program and only the window handle is
  // destroyed on close, so everything below survives a close/reopen while the
  // controls are built fresh and empty.  Any state here that a control also
  // displays MUST be pushed back into that control in DoBuildControls, or the
  // window reopens showing one thing and filtering by another -- which is
  // exactly how an empty search box ended up driving a live filter that
  // backspace could not clear, because an empty edit sends no EN_CHANGE.
  std::wstring m_searchQuery;     // live; substring, or glob if it has * or ?
  int  m_nSortColumn = 0;         // an ANNOT_COL_* index
  bool m_bSortAscending = true;

  void LayoutControls();
  // Column widths are computed from the live client width and the HUD font,
  // never stored as pixel constants: eight columns of fixed pixels truncated
  // their own headers the moment the font grew, and never followed a resize.
  void AutoSizeListColumns();
  void RefreshOverrideCombos();
  // Per-preset actions for the right-clicked row. Mirrors the Presets browser
  // menu so the same data is managed the same way in both windows.
  void ShowRowContextMenu(int screenX, int screenY);
  // 0 = shader, 1 = VFX, 2 = audio. An index rather than a bool because
  // there are three slots now.
  void ApplyOverrideCombo(int slot);
  void RefreshList();
  void ShowDetailsDialog();
  void ShowImportDialog();
  void DoScanPresets();
  // Hash-scan the preset tree for files that are the same preset, then show the
  // report. Runs the walk on a worker thread and pumps a small modal while it
  // goes, because it reads every preset file under the root.
  void DoFindCopies();
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
  LRESULT DoMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) override;
  void    DoDestroy() override;

private:
  void BuildTransformPage(int x, int y, int rw, int lineH, int gap);
  void BuildEffectsPage(int x, int y, int rw, int lineH, int gap);
  void BuildAudioPage(int x, int y, int rw, int lineH, int gap);
  void SaveFX();

  // Red Save Profile button while live parameters differ from the profile.
  void RefreshDirtyIndicator();
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
  std::vector<std::wstring> m_profileNames;  // profile names, indexed parallel to listbox
};

// ── Concrete subclass: Custom Shaders window ──
//
// Manages the shader override store (shader_overrides.h): named overrides, the
// ordered tag rules that select them, and applying one to the running preset by
// hand.  Shader TEXT is edited outside this window -- the files sit in
// resources/shaders and Edit opens them in the system editor, which is the
// point of keeping them as files rather than JSON strings.

class CustomShadersWindow : public ToolWindow {
public:
  CustomShadersWindow(Engine* pEngine);

  void RefreshAll();

protected:
  TOOLWINDOW_META(L"Custom Shaders", L"MDropDX12CustomShadersWnd", L"CustomShaders",
                  IDC_MW_CSHADER_PIN, IDC_MW_CSHADER_FONT_PLUS, IDC_MW_CSHADER_FONT_MINUS,
                  400, 620)

  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;

private:
  void RefreshOverrideList();
  void RefreshRuleList();
  void RefreshRuleVFXCombo();
  void RefreshStatus();
  std::wstring SelectedOverride();
  int  SelectedRule();
  void EditShaderFile(bool warp);

  std::vector<std::wstring> m_overrideNames;  // parallel to the listbox
  std::vector<std::wstring> m_vfxNames;       // parallel to the VFX combo;
                                              // index 0 is "" = (none)
  HWND m_hStatus = NULL;
};

// ── Concrete subclass: Preset Editor window ──
//
// Edits the RUNNING preset's code sections with syntax highlighting.
//
// Text is staged here and pushed to the render thread with
// RenderCmd::ApplyPresetCode -- never applied inline, because CState, the
// shaders and the DX12 PSOs belong to the render thread (see the
// RequestLoadPreset note above).
//
// While this window is open, Engine::m_bPresetEditorOpen suppresses preset
// auto-advance, so an unsaved edit cannot be discarded by a preset change.

class PresetEditorWindow : public ToolWindow {
public:
  PresetEditorWindow(Engine* pEngine);
  ~PresetEditorWindow() override;

protected:
  TOOLWINDOW_META(L"Preset Editor", L"MDropDX12PresetEditWnd", L"PresetEditWnd",
                  IDC_MW_PEDIT_PIN, IDC_MW_PEDIT_FONT_PLUS, IDC_MW_PEDIT_FONT_MINUS,
                  620, 480)

  // Code needs room, and an editor is a window you park and come back to, so
  // it gets a full frame with minimise/maximise/restore and a taskbar button
  // rather than the usual tool-window chrome.
  DWORD GetExtraWindowStyle() const override { return WS_MINIMIZEBOX | WS_MAXIMIZEBOX; }
  bool  UsesToolWindowFrame() const override { return false; }
  DWORD GetCommonControlFlags() const override;   // adds ICC_TREEVIEW_CLASSES

  void    OnResize() override;
  void    RebuildFonts() override;
  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoNotify(HWND hWnd, NMHDR* pnm) override;
  LRESULT DoContextMenu(HWND hWnd, int x, int y) override;
  LRESULT DoMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override;
  void    DoDestroy() override;

  // Window-local pseudo-sections. Negative so they can never be mistaken for a
  // PresetCodeSection and reach ApplyPresetCodeSection.
  enum {
    PSEUDO_WHOLE_PRESET = -2,   // the whole .milk in [section] block form
    PSEUDO_RAW_FILE     = -3,   // the whole .milk exactly as written
  };

private:
  struct SectionRow {
    std::wstring label;
    int          section;   // PresetCodeSection, or a PSEUDO_* value
    int          index;     // wave/shape index, else 0
    CodeLang     lang;
  };

  void LayoutControls();
  void BuildSectionList();
  void PopulateTree();
  void LoadSectionIntoEditor(int nRow);   // CState (or the stash) -> editor
  void StashEditorText();                 // editor -> m_staged[m_nCurRow]
  // Returns false when nothing was queued (e.g. a .milk2 raw view, which
  // Import cannot read). The caller must not then report success.
  bool EnqueueApply(int nRow);
  void ExpandCurrentSection();
  void ApplyCurrentSection();
  void RevertCurrentSection();
  void SavePreset(bool bSaveAs);
  void InsertTemplate(int nTemplateId);
  void RefreshStatusFromLastError();
  void SetStatus(const wchar_t* text);
  std::string ReadSectionFromState(const SectionRow& r) const;
  std::string BuildWholePresetText(bool bBlocks) const;  // via CState::Export
  void SyncSideTabs();            // show/hide the Preset 1 / Preset 2 selector
  void OnSideChanged(int nSide);  // reload every section for the new side
  void LoadViewOptions();      // line numbers / folding, from the INI
  void SaveViewOptions();
  bool CurrentIsWholeFile() const;

  // Preset 1 / Preset 2 selector, created only for a frozen .milk2 (which is
  // the only case where two presets are on screen at once).
  HWND      m_hSideTabs = NULL;
  int       m_nSide = 0;          // a PresetSide value
  bool      m_bTwoPresets = false;
  HWND      m_hTree = NULL;
  HWND      m_hApply = NULL, m_hRevert = NULL;
  HWND      m_hSave = NULL, m_hSaveAs = NULL, m_hStatus = NULL;
  HWND      m_hFallbackEdit = NULL;   // plain EDIT if Scintilla failed to init
  SciEditor m_sci;
  std::vector<SectionRow>  m_sections;
  std::vector<std::string> m_staged;   // parallel to m_sections
  std::vector<bool>        m_dirty;    // parallel to m_sections
  int  m_nCurRow = 0;
  int  m_nTopY = 0;
  bool m_bOptLineNumbers = true;
  bool m_bOptFolding = true;
  bool m_bPollingSave = false;   // the pending poll is for a Save, not an Apply
  std::wstring m_wWatchedPreset; // last preset seen, to notice a load underneath us
};

// ── Concrete subclass: scoped-VFX Keep / Discard prompt ──
//
// Shown when a preset is left after its VFX profile was edited. It runs on its
// own thread like every other tool window, so the render loop never stalls
// waiting for an answer -- which matters because presets can advance every few
// seconds and a modal here would freeze the visualiser.
//
// Both buttons call Engine::AnswerScopedVFX, the same function VFX_SCOPED_KEEP
// uses, so what is tested is what is clicked.

class VFXKeepPromptWindow : public ToolWindow {
public:
  VFXKeepPromptWindow(Engine* pEngine);

  // The profile the pending edit belongs to, shown in the message.
  void SetProfileName(const std::wstring& name) { m_profile = name; }

protected:
  TOOLWINDOW_META(L"Keep video effect changes?", L"MDropDX12VFXPromptWnd",
                  L"VFXKeepPrompt",
                  IDC_MW_VFXPROMPT_PIN, IDC_MW_VFXPROMPT_FONT_PLUS,
                  IDC_MW_VFXPROMPT_FONT_MINUS, 400, 210)

  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;

private:
  std::wstring m_profile;
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

  // The build path selects row 0 and fills the detail panes from it, so after a
  // rebuild the window believed row 0 was current while the list showed the row
  // the user had actually chosen. RestoreControlSelections has already put the
  // selection back by the time this runs; read it and agree with it.
  void OnRebuilt() override;
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
  // 260x300 minimum, half the old 520x600.
  //
  // The old value was the DEFAULT size repeated as the MINIMUM, so the window
  // could not be made smaller by anyone -- not by the user dragging its edge
  // and not by the code below. That is why it always appeared large.
  //
  // Safe to shrink because this window's layout is width-relative: every
  // control is placed from the layout struct's clientW rather than at fixed
  // pixels, and the window rebuilds its controls on resize, so the content
  // reflows instead of being clipped.
  TOOLWINDOW_META(L"Remote", L"MDropDX12RemoteWnd", L"RemoteWnd",
                  IDC_MW_REMOTEWIN_PIN, IDC_MW_REMOTEWIN_FONT_PLUS, IDC_MW_REMOTEWIN_FONT_MINUS, 260, 300)

  bool    PlacesItselfInTestingMode() const override { return true; }

  DWORD   GetCommonControlFlags() const override;
  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoNotify(HWND hWnd, NMHDR* pnm) override;
  LRESULT DoMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override;
  void    DoDestroy() override;

private:
  int m_lastSeenIPCSeq = 0;
  bool m_bRefreshingList = false; // guard against LVN_ITEMCHANGED during list rebuild
  void RefreshIPCList();
  void RefreshDeviceList();
  void RefreshTcpStatus();
};

// ── Concrete subclass: Audio Mixer window ──

class MixerWindow : public ToolWindow {
public:
  MixerWindow(Engine* pEngine);

protected:
  TOOLWINDOW_META(L"Audio Mixer", L"MDropDX12MixerWnd", L"MixerWnd",
                  IDC_MW_MIXERWIN_PIN, IDC_MW_MIXERWIN_FONT_PLUS,
                  IDC_MW_MIXERWIN_FONT_MINUS, 640, 900)

  DWORD   GetCommonControlFlags() const override;
  void    DoBuildControls() override;
  LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
  LRESULT DoNotify(HWND hWnd, NMHDR* pnm) override;
  LRESULT DoHScroll(HWND hWnd, int id, int pos) override;
  LRESULT DoMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override;
  // Right-click on a Mixer tab fader row: hide it, or show the hidden ones.
  // Must be this override and not a DoMessage case -- the base class routes
  // WM_CONTEXTMENU here and DoMessage never sees it.
  LRESULT DoContextMenu(HWND hWnd, int x, int y) override;
  void    OnResize() override;
  // The client size the pages were last laid out for. Compared on resize so a
  // relayout happens when -- and only when -- the size actually changed.
  int     m_builtClientW = -1;
  int     m_builtClientH = -1;
  // Move Up / Move Down take the whole channel. Not persisted: it is a way
  // of working for the next few clicks, not a preference to come back to.
  // Indices into m_orderKeys that the order list actually shows. The full
  // order is the model and is what gets saved; this is the view of it, so
  // hiding a fader cannot drop it from storage nor put it in the list.
  std::vector<int> m_orderVisible;
  bool    m_orderMoveGroup = false;

  // The selected fader's own preferences, mirrored so the Hide button can say
  // what it will DO and the name box can show what is set. Refreshed when the
  // selection moves; both are meaningless with nothing selected, and the
  // controls read as "Hide fader" with an empty box, which is what a fresh
  // selection of an ordinary fader looks like anyway (forgejo#50, forgejo#52).
  bool         m_orderSelHidden = false;
  // The fader the Mixer tab's context menu was opened on, as "<ch>|<id>".
  // Captured when the menu is built rather than looked up when the command
  // arrives: by then the click position is gone, and a rebuild between the two
  // would have renumbered the rows.
  std::wstring m_ctxFaderKey;
  std::wstring m_orderSelShort;
  // Raised while the order list is being filled. Ticking a box in a ListView
  // and having the code tick it both arrive as LVN_ITEMCHANGED, so without
  // this the population pass reads as the user clearing every box and writes
  // an empty group back over the real one.
  bool    m_fillingOrderList = false;
  void    DoDestroy() override;

private:
  // The subscription follows ACTIVATION, not merely being open. Shane asked for
  // levels to be read "only if asked or mixer tool window open and active", so
  // a window sitting behind the visualiser must cost nothing.
  //
  // It is deliberately NOT keyed on visibility, though the refresh below is.
  // Subscribing polls every provider, Sonar over HTTP included; tying that to
  // a window merely being on screen made switching headsets crawl (#328).
  bool m_subscribed = false;

  // Ticks since the last refresh while visible but not in front. The window
  // ticks once a second; unfocused it refreshes on every fifth, which is what
  // keeps the battery and the failover list current without subscribing.
  int m_idleTicks = 0;

  // Slider id minus IDC_MW_MIXER_FADER_BASE indexes these, which is how a
  // WM_HSCROLL finds the channel and fader it belongs to. Rebuilt with the
  // controls, because the provider list can change while the window is open.
  std::vector<std::pair<std::wstring, std::wstring>> m_faders;

  // Backing store for the fader tooltips. TTM_ADDTOOLW stores the pointer it
  // is given rather than copying the text, so a local would leave the tooltip
  // reading freed memory.
  std::vector<std::wstring> m_faderNames;

  // The user's fader order, as "channel|fader" keys, and the same list as it
  // is shown in the Options tab. Shane asked to "put controls I use most at
  // top and those I don't use can be at bottom": with two dozen endpoints and
  // room for ten, the order decides what is reachable without resizing.
  std::vector<std::wstring> m_orderKeys;
  // The arrangement this window is currently LAID OUT for.
  //
  // RefreshValues rewrites the rows that exist; it cannot notice that there
  // are different rows, or the same rows in a different order. A failover
  // device connecting is exactly that -- the engine pins it to the top and no
  // fader value changes -- so the window went on drawing the old arrangement
  // until Refresh was pressed by hand. Comparing this against
  // Engine::MixerOrderRev() on the poll tick is what makes the relayout
  // happen on its own.
  std::wstring m_viewRev;

  // The Profiles tab. The list SHOWS display names and ACTS on file leaves,
  // and the two differ whenever a typed name needed sanitising -- so the row
  // index maps into this rather than the visible text being re-derived.
  std::vector<std::wstring> m_profileLeaves;
  // What the last save/load/delete did, shown under the buttons. A load that
  // skipped a device has something worth saying and no other place to say it.
  std::wstring m_profileStatus;

  // Segoe MDL2 Assets, sized to the row rather than to the title bar, for the
  // muted-speaker indicator. Its own font because the pin button's is sized
  // for the pin.
  HFONT m_hMuteFont = NULL;

  std::vector<std::wstring> m_routeIds;    // route combo index -> route id
  std::vector<std::wstring> m_deviceIds;   // device combo index -> device id
  std::vector<std::wstring> m_allowIds;    // allowlist row -> device id

  // id and stored name per allowlist row, so the one-second tick can re-render
  // a row in place -- rebuilding the page would throw away the selection the
  // user is working with.
  struct AllowRow { std::wstring id, name; };
  std::vector<AllowRow> m_allowRows;
  std::wstring m_routeId;                  // the route the Failover tab edits

  // Pulls the current values into the controls without rebuilding them, so a
  // slider the user is dragging is not yanked out from under them.
  void RefreshValues();
  void SendMixer(const std::wstring& command);
  // Reconciles the subscription with whether anyone is actually looking.
  void SetActive(bool active);
  bool LooksActive() const;
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
    // A thin SEH guard; the body is Impl. See seh_guard.h -- a fault inside
    // a window procedure is otherwise fatal and leaves no trace at all.
    static LRESULT CALLBACK ModalWndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ModalWndProcImpl(HWND, UINT, WPARAM, LPARAM);
};

// ── Ask the user to name something ────────────────────────────────────
// Returns false if cancelled. `text` carries the initial value in and the
// entered value out. Replaces GetSaveFileNameW wherever a thing is named
// rather than saved to a file of its own.
//
// `choices` are the names that already exist. They are offered in a dropdown
// the user can also type into: a save box that only accepts a fresh name makes
// overwriting one mean retyping it exactly, which is both tedious and how
// duplicates-by-typo get made.
bool PromptForName(Engine* pEngine, HWND hParent, const wchar_t* title,
                   const wchar_t* prompt, std::wstring& text, size_t maxLen,
                   const std::vector<std::wstring>& choices);

// ── Clipboard ─────────────────────────────────────────────────────────
// Retries: another process can hold the clipboard for a few milliseconds and
// OpenClipboard fails rather than waiting.
bool CopyTextToClipboard(HWND owner, const wchar_t* text);

// ── Shader errors ─────────────────────────────────────────────────────
// Drop D3DCompile's `<dir>\Shader@0x<address>` source-name prefix, which names
// no real file and changes every run, leaving `(line,col): error X....`.
std::wstring StripShaderErrorPrefix(const std::wstring& line);

// Clipboard form of a recorded shader error: the preset's full path, when the
// error was captured, then one error per line.  capturedAt carries the date so
// that a pasted error cannot be mistaken for one from the running build --
// these records outlive the build that produced them.
std::wstring FormatShaderErrorForClipboard(const std::wstring& presetPath,
                                           const std::wstring& errorText,
                                           const std::wstring& capturedAt = std::wstring());

// Modal viewer for a preset's recorded shader error, with a Copy button that
// puts FormatShaderErrorForClipboard()'s text on the clipboard.
void ShowShaderErrorDialog(Engine* pEngine, HWND hParent,
                           const std::wstring& presetPath,
                           const std::wstring& errorText,
                           const std::wstring& capturedAt = std::wstring());

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

    // Which entry is BEING edited, so the live conflict line does not report
    // the action clashing with itself. -1 for "not one of these", which is
    // what the Button Board passes -- it edits buttons, not hotkey table rows.
    int selfHotkeyId = -1;      // HotkeyAction id of the built-in under edit
    int selfUserId   = -1;      // UserHotkey id of the user entry under edit

    // Context
    Engine* pEngine = nullptr;
    bool accepted = false;
};

// Show the shared action edit dialog.  Returns true if user pressed OK.
bool ShowActionEditDialog(HWND hParent, ActionEditData& data);

} // namespace mdrop
