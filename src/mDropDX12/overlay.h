#ifndef MDROPDX12_OVERLAY_H
#define MDROPDX12_OVERLAY_H

#include <windows.h>
#include <thread>
#include <atomic>

// Menu line entry for overlay rendering
static const int OVERLAY_MAX_MENU_LINES = 48;

struct OverlayMenuLine {
    wchar_t text[256];
    DWORD color;        // ARGB (e.g., 0xFFCCCCCC)
    int x, y;           // position in client pixels
};

// Data snapshot passed from main thread to overlay thread each frame.
// Flat POD struct — no pointers, no allocations, fully self-contained.
struct OverlayData {
    bool bShowFPS;
    bool bShowDebugInfo;
    bool bEnableMouseInteraction;
    bool bPresetLocked;

    // HUD: preset name (top-right, above FPS)
    bool    bShowPresetName;
    DWORD   presetNameColor;            // 0x00RRGGBB — passed to SetTextColor via DrawShadowText
    wchar_t szPresetName[256];          // lock-symbol prefix already included if locked

    // HUD: rating (top-right, below preset name)
    bool    bShowRating;
    wchar_t szRating[64];               // pre-formatted e.g. L" Rating: 3 "

    // HUD: song title (bottom-left)
    bool    bShowSongTitle;
    wchar_t szSongTitle[256];

    // HUD: notifications / error messages
    static const int OVERLAY_MAX_NOTIFICATIONS = 32;
    struct OverlayNotification {
        wchar_t text[256];
        DWORD   color;      // 0x00RRGGBB
        int     corner;     // MTO_UPPER_RIGHT=0, MTO_UPPER_LEFT=1, MTO_LOWER_RIGHT=2, MTO_LOWER_LEFT=3
    };
    int                  nNotifications;
    OverlayNotification  notifications[OVERLAY_MAX_NOTIFICATIONS];

    int clientWidth;
    int clientHeight;

    float fps;
    float fRenderQuality;
    float fColShiftHue;
    float fColShiftSaturation;
    float fColShiftBrightness;

    // Audio values (resolved from double* on main thread)
    float bass, bass_att, bass_smooth;
    float mid, mid_att, mid_smooth;
    float treb, treb_att, treb_smooth;

    // Relative values for +/- beat indicator
    float bass_imm_rel, bass_avg_rel, bass_smooth_rel;
    float mid_imm_rel, mid_avg_rel, mid_smooth_rel;
    float treb_imm_rel, treb_avg_rel, treb_smooth_rel;

    float pfMonitor;
    float presetTime;

    float mouseX, mouseY;
    bool mouseDown;

    // Menu overlay
    bool bShowMenu;
    RECT menuBox;                                       // dark background rect (client pixels)
    int menuFontHeight;                                 // font size for menu text
    int nMenuLines;
    OverlayMenuLine menuLines[OVERLAY_MAX_MENU_LINES];

    // Tooltip (bottom-right area)
    bool bShowTooltip;
    wchar_t tooltip[1024];
    int tooltipX, tooltipY;
};

class COverlayThread {
public:
    COverlayThread();
    ~COverlayThread();

    // Lifecycle (called from main thread)
    bool Init(HWND hParentWnd, int width, int height);
    void Shutdown();

    // Called by main thread each frame — copies data snapshot
    void UpdateData(const OverlayData& data);

    // Called on window resize — signals overlay recreation
    void OnResize(int newW, int newH);

    // Called when parent window moves — signals reposition
    void OnParentMove();

    bool IsAlive() const { return m_bAlive.load(std::memory_order_relaxed); }

private:
    void ThreadFunc();
    void RenderOverlayToDIB();
    void PostProcessAlpha();

    // GDI text helper: draw text with 1px shadow (debug info)
    // rgbColor defaults to white (0xFFFFFF); existing callers need no changes.
    void DrawShadowText(const wchar_t* text, bool alignRight,
                        int marginX, int* pY, int rightEdge,
                        bool fromBottom = false, DWORD rgbColor = 0xFFFFFF);

    // Menu rendering
    void RenderMenuToDIB();

    // Layered window management (overlay thread only)
    bool CreateOverlayWindow();
    void DestroyOverlayWindow();
    void UpdateOverlayPosition();
    void PresentToLayeredWindow();

    static LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    // GDI resource creation/destruction (overlay thread only)
    void CreateDIB(UINT w, UINT h);
    void ReleaseDIB();

    // Thread and synchronization
    std::thread         m_thread;
    std::atomic<bool>   m_bAlive{false};
    std::atomic<bool>   m_bShutdown{false};
    std::atomic<bool>   m_bDataAvailable{false};  // new data snapshot available

    // Data handoff (single buffer, gated by m_bDataAvailable)
    OverlayData         m_data{};
    OverlayData         m_currentData{};  // overlay thread's working copy

    // Window handles
    HWND                m_hParentWnd = nullptr;
    HWND                m_hOverlayWnd = nullptr;

    // GDI resources (overlay thread owns)
    HDC                 m_hMemDC = nullptr;
    HBITMAP             m_hDIB = nullptr;
    BYTE*               m_pDIBBits = nullptr;
    HFONT               m_hFont = nullptr;
    HFONT               m_hHUDFont = nullptr;   // Segoe UI, for preset name / song title
    int                 m_hudFontH = 0;
    HFONT               m_hMenuFont = nullptr;
    int                 m_menuFontHeight = 0;
    UINT                m_texWidth = 0;
    UINT                m_texHeight = 0;

    // Resize/reposition signaling
    std::atomic<bool>   m_bNeedsResize{false};
    std::atomic<bool>   m_bNeedsReposition{false};
    std::atomic<int>    m_newWidth{0};
    std::atomic<int>    m_newHeight{0};

    static bool         s_bClassRegistered;
};

#endif
