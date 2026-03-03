#pragma once
// button_panel.h — Owner-drawn button grid control for the Button Board window.
//
// A single custom HWND that renders a configurable grid of buttons with
// optional thumbnails, label text, and 3D beveled borders matching the
// MDropDX12 dark theme.  Supports paged banking or scrollable mode.

#include <Windows.h>
#include <vector>
#include <string>
#include <functional>

namespace mdrop {

class Engine; // forward

// Action types for button panel slots
enum class ButtonAction : int {
    None = 0,
    LoadPreset    = 1, // payload = preset file path
    PushSprite    = 2, // payload = sprite index (as string)
    ScriptCommand = 3, // payload = pipe-chained IPC commands
    LaunchMessage = 4, // payload = MSG|text=...|font=... string
};

// Data for a single button slot
struct ButtonSlot {
    ButtonAction action  = ButtonAction::None;
    std::wstring payload;    // action-specific data
    std::wstring label;      // display name
    HBITMAP      hThumbnail = NULL; // owned 32bpp DIBSection, or NULL
    int          thumbW = 0;
    int          thumbH = 0;
};

// Grid configuration
struct ButtonPanelConfig {
    int  nRows         = 3;
    int  nCols         = 5;
    int  nBanks        = 3;
    int  nActiveBank   = 0; // 0-based
    int  nButtonPadding = 4; // pixels between cells
    bool bScrollable   = false; // false = paged banking
};

class ButtonPanel {
public:
    ButtonPanel();
    ~ButtonPanel();

    // Create the child window.  Returns true on success.
    bool Create(HWND hParent, Engine* pEngine, int ctrlID, int x, int y, int w, int h);
    void Destroy();

    HWND GetHWND() const { return m_hWnd; }

    // Configuration -------------------------------------------------------
    ButtonPanelConfig& Config() { return m_config; }
    const ButtonPanelConfig& Config() const { return m_config; }
    void ApplyConfig(); // call after modifying config to resize slot vector + repaint

    // Slot access ---------------------------------------------------------
    int  GetSlotsPerBank() const { return m_config.nRows * m_config.nCols; }
    int  GetTotalSlots()   const;
    ButtonSlot&       Slot(int globalIndex);
    const ButtonSlot& Slot(int globalIndex) const;

    // Banking -------------------------------------------------------------
    void SetActiveBank(int bank);
    int  GetActiveBank() const { return m_config.nActiveBank; }
    void NextBank();
    void PrevBank();

    // Thumbnails ----------------------------------------------------------
    void SetSlotThumbnail(int globalIndex, HBITMAP hBmp, int w, int h);
    void ClearSlotThumbnail(int globalIndex);
    void ClearAllThumbnails();

    // Reposition / repaint ------------------------------------------------
    void Reposition(int x, int y, int w, int h);
    void Invalidate();
    void SetFont(HFONT hFont);

    // Theme colors (called by parent after ApplyDarkTheme) ----------------
    void SetThemeColors(COLORREF bg, COLORREF btnFace, COLORREF btnHi,
                        COLORREF btnShadow, COLORREF text, COLORREF ctrlBg);

    // INI persistence -----------------------------------------------------
    void SaveToINI(const wchar_t* iniPath, const wchar_t* section) const;
    void LoadFromINI(const wchar_t* iniPath, const wchar_t* section);

    // Callbacks (set by the owning window) ---------------------------------
    std::function<void(int globalIndex)>                 OnLeftClick;
    std::function<void(int globalIndex, POINT screenPt)> OnRightClick;

private:
    HWND    m_hWnd    = NULL;
    Engine* m_pEngine = NULL;
    int     m_nCtrlID = 0;
    HFONT   m_hFont   = NULL; // not owned, set by parent

    ButtonPanelConfig        m_config;
    std::vector<ButtonSlot>  m_slots;

    // Theme
    COLORREF m_colBg        = RGB(30, 30, 30);
    COLORREF m_colBtnFace   = RGB(60, 60, 60);
    COLORREF m_colBtnHi     = RGB(90, 90, 90);
    COLORREF m_colBtnShadow = RGB(35, 35, 35);
    COLORREF m_colText      = RGB(0, 220, 0);
    COLORREF m_colEmpty     = RGB(45, 45, 45);

    // Interaction state
    int m_nHoverSlot   = -1; // local index under cursor (-1 = none)
    int m_nPressedSlot = -1;
    int m_nScrollOffset = 0; // for scrollable mode (row offset)

    // Geometry
    RECT GetButtonRect(int localIndex) const;
    int  HitTest(POINT clientPt) const;
    int  LocalToGlobal(int localIndex) const;

    void EnsureSlotCount();

    // Window class
    static bool s_bClassRegistered;
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);

    // Painting
    void OnPaint(HDC hdc);
    void PaintButton(HDC hdc, int localIndex, const RECT& rc, bool hover, bool pressed);
};

} // namespace mdrop
