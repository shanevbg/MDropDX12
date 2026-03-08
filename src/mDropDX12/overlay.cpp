#include "overlay.h"
#include "utility.h"
#include <cstdio>
#include <algorithm>

bool COverlayThread::s_bClassRegistered = false;

COverlayThread::COverlayThread() {}

COverlayThread::~COverlayThread() {
    Shutdown();
}

bool COverlayThread::Init(HWND hParentWnd, int width, int height) {
    if (!hParentWnd || width <= 0 || height <= 0) return false;
    m_hParentWnd = hParentWnd;
    m_texWidth = (UINT)width;
    m_texHeight = (UINT)height;

    m_bShutdown.store(false);
    m_bDataAvailable.store(false);
    m_bNeedsResize.store(false);
    m_bNeedsReposition.store(false);
    m_bAlive.store(true);

    m_thread = std::thread(&COverlayThread::ThreadFunc, this);
    return true;
}

void COverlayThread::RequestShutdown() {
    m_bShutdown.store(true);
}

void COverlayThread::Shutdown() {
    m_bShutdown.store(true);
    if (m_thread.joinable())
        m_thread.join();
    m_bAlive.store(false);
    m_hParentWnd = nullptr;
}

void COverlayThread::OnResize(int newW, int newH) {
    if (newW > 0 && newH > 0) {
        m_newWidth.store(newW);
        m_newHeight.store(newH);
        m_bNeedsResize.store(true);
    }
}

void COverlayThread::OnParentMove() {
    m_bNeedsReposition.store(true, std::memory_order_release);
}

void COverlayThread::UpdateData(const OverlayData& data) {
    if (!m_bDataAvailable.load(std::memory_order_relaxed)) {
        m_data = data;
        m_bDataAvailable.store(true, std::memory_order_release);
    }
}

// ---------------------------------------------------------------------------
// Layered window management (overlay thread)
// ---------------------------------------------------------------------------
bool COverlayThread::CreateOverlayWindow() {
    if (!s_bClassRegistered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = OverlayWndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"MDropDX12_OverlayClass";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassW(&wc)) {
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                return false;
        }
        s_bClassRegistered = true;
    }

    // Get parent client area in screen coords
    RECT clientRect;
    GetClientRect(m_hParentWnd, &clientRect);
    POINT topLeft = { 0, 0 };
    ClientToScreen(m_hParentWnd, &topLeft);

    int cw = clientRect.right - clientRect.left;
    int ch = clientRect.bottom - clientRect.top;

    // No owner (nullptr) to avoid deadlock: Shutdown() joins this thread,
    // and DestroyWindow on an owned popup sends sync messages to the owner's
    // thread which would be blocked in join(). We manage z-order ourselves.
    m_hOverlayWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"MDropDX12_OverlayClass",
        L"",
        WS_POPUP,
        topLeft.x, topLeft.y, cw, ch,
        nullptr,
        nullptr,
        GetModuleHandle(nullptr),
        nullptr);

    if (!m_hOverlayWnd) return false;

    // Place overlay in front of parent in z-order.
    // SetWindowPos(A, B, ...) places A *behind* B, so we need to find
    // the window just above parent and insert after that instead.
    BOOL isTopmost = (GetWindowLong(m_hParentWnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (isTopmost) {
        SetWindowPos(m_hOverlayWnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        HWND hAbove = GetWindow(m_hParentWnd, GW_HWNDPREV);
        SetWindowPos(m_hOverlayWnd, hAbove ? hAbove : HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    ShowWindow(m_hOverlayWnd, SW_SHOWNOACTIVATE);
    return true;
}

void COverlayThread::DestroyOverlayWindow() {
    if (m_hOverlayWnd) {
        DestroyWindow(m_hOverlayWnd);
        m_hOverlayWnd = nullptr;
    }
}

void COverlayThread::UpdateOverlayPosition() {
    if (!m_hOverlayWnd || !m_hParentWnd) return;
    if (!IsWindow(m_hParentWnd)) return;

    // Hide overlay when parent is minimized or invisible
    if (IsIconic(m_hParentWnd) || !IsWindowVisible(m_hParentWnd)) {
        if (IsWindowVisible(m_hOverlayWnd))
            ShowWindow(m_hOverlayWnd, SW_HIDE);
        return;
    }

    RECT clientRect;
    GetClientRect(m_hParentWnd, &clientRect);
    POINT topLeft = { 0, 0 };
    ClientToScreen(m_hParentWnd, &topLeft);

    int cw = clientRect.right - clientRect.left;
    int ch = clientRect.bottom - clientRect.top;

    // Place overlay in front of parent in z-order, matching its client area.
    // SetWindowPos(A, B, ...) places A *behind* B, so insert after the
    // window above parent to end up just in front of parent.
    BOOL isTopmost = (GetWindowLong(m_hParentWnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (isTopmost) {
        SetWindowPos(m_hOverlayWnd, HWND_TOPMOST, topLeft.x, topLeft.y, cw, ch,
                     SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    } else {
        HWND hAbove = GetWindow(m_hParentWnd, GW_HWNDPREV);
        SetWindowPos(m_hOverlayWnd, hAbove ? hAbove : HWND_TOP, topLeft.x, topLeft.y, cw, ch,
                     SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    }

    if (!IsWindowVisible(m_hOverlayWnd))
        ShowWindow(m_hOverlayWnd, SW_SHOWNOACTIVATE);
}

void COverlayThread::PresentToLayeredWindow() {
    if (!m_hOverlayWnd || !m_hMemDC || !m_pDIBBits) return;

    POINT ptSrc = { 0, 0 };
    SIZE sz = { (LONG)m_texWidth, (LONG)m_texHeight };

    RECT overlayRect;
    GetWindowRect(m_hOverlayWnd, &overlayRect);
    POINT ptDst = { overlayRect.left, overlayRect.top };

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(m_hOverlayWnd, nullptr, &ptDst, &sz,
                        m_hMemDC, &ptSrc, 0, &blend, ULW_ALPHA);
}

LRESULT CALLBACK COverlayThread::OverlayWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// GDI DIB creation/destruction (overlay thread only)
// ---------------------------------------------------------------------------
void COverlayThread::CreateDIB(UINT w, UINT h) {
    ReleaseDIB();

    m_hMemDC = CreateCompatibleDC(nullptr);
    if (!m_hMemDC) return;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = (LONG)w;
    bmi.bmiHeader.biHeight      = -(LONG)h;  // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    m_hDIB = CreateDIBSection(m_hMemDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!m_hDIB || !bits) {
        DeleteDC(m_hMemDC);
        m_hMemDC = nullptr;
        return;
    }
    m_pDIBBits = (BYTE*)bits;
    SelectObject(m_hMemDC, m_hDIB);

    // Create font: DPI-adjusted, matching help texture pattern
    int dpiY = GetDeviceCaps(m_hMemDC, LOGPIXELSY);
    if (dpiY <= 0) dpiY = 96;
    int fontSize = max(20, (int)h / 32);
    int fontRequest = MulDiv(fontSize, 96, dpiY);

    m_hFont = CreateFontW(
        -fontRequest, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
    if (m_hFont)
        SelectObject(m_hMemDC, m_hFont);

    // HUD font: Segoe UI, proportional to window height, for preset name / song title
    m_hudFontH = max(20, (int)h / 30);
    m_hHUDFont = CreateFontW(
        -m_hudFontH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    SetBkMode(m_hMemDC, TRANSPARENT);
}

void COverlayThread::ReleaseDIB() {
    if (m_hMenuFont) { DeleteObject(m_hMenuFont); m_hMenuFont = nullptr; m_menuFontHeight = 0; }
    if (m_hHUDFont)  { DeleteObject(m_hHUDFont);  m_hHUDFont = nullptr;  m_hudFontH = 0; }
    if (m_hFont) { DeleteObject(m_hFont); m_hFont = nullptr; }
    if (m_hDIB)  { DeleteObject(m_hDIB);  m_hDIB = nullptr; }
    if (m_hMemDC){ DeleteDC(m_hMemDC);    m_hMemDC = nullptr; }
    m_pDIBBits = nullptr;
}

// ---------------------------------------------------------------------------
// Thread entry point — wrapped in SEH for crash isolation
// ---------------------------------------------------------------------------
void COverlayThread::ThreadFunc() {
    __try {
        if (!CreateOverlayWindow()) {
            m_bAlive.store(false, std::memory_order_release);
            return;
        }

        CreateDIB(m_texWidth, m_texHeight);

        while (!m_bShutdown.load(std::memory_order_relaxed)) {
            // Handle resize
            if (m_bNeedsResize.load(std::memory_order_relaxed)) {
                UINT nw = (UINT)m_newWidth.load();
                UINT nh = (UINT)m_newHeight.load();
                if (nw > 0 && nh > 0) {
                    ReleaseDIB();
                    m_texWidth = nw;
                    m_texHeight = nh;
                    CreateDIB(nw, nh);
                    UpdateOverlayPosition();
                }
                m_bNeedsResize.store(false, std::memory_order_release);
            }

            // Handle parent move (reposition overlay)
            if (m_bNeedsReposition.load(std::memory_order_relaxed)) {
                UpdateOverlayPosition();
                m_bNeedsReposition.store(false, std::memory_order_release);
            }

            // Render if new data available
            if (m_bDataAvailable.load(std::memory_order_acquire)) {
                m_currentData = m_data;
                m_bDataAvailable.store(false, std::memory_order_release);

                if (m_hMemDC && m_pDIBBits &&
                    (m_currentData.bShowFPS || m_currentData.bShowDebugInfo || m_currentData.bShowMenu ||
                     m_currentData.bShowPresetName || m_currentData.bShowRating ||
                     m_currentData.bShowSongTitle  || m_currentData.nNotifications > 0)) {
                    RenderOverlayToDIB();
                    PostProcessAlpha();
                    PresentToLayeredWindow();
                } else if (m_hMemDC && m_pDIBBits) {
                    // Nothing to show — present fully transparent
                    memset(m_pDIBBits, 0, (size_t)m_texWidth * m_texHeight * 4);
                    PresentToLayeredWindow();
                }
            }

            // Menu needs ~60fps for responsive feel; debug is fine at ~15fps
            Sleep(m_currentData.bShowMenu ? 16 : 66);
        }

        ReleaseDIB();
        DestroyOverlayWindow();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Overlay thread crashed — mark as dead, main rendering continues
        m_bAlive.store(false, std::memory_order_release);
        ReleaseDIB();
        DestroyOverlayWindow();
        DebugLogA("COverlayThread: SEH exception caught, overlay disabled", LOG_ERROR);
    }
}

// ---------------------------------------------------------------------------
// GDI text rendering to DIB
// ---------------------------------------------------------------------------
void COverlayThread::DrawShadowText(const wchar_t* text, bool alignRight,
                                     int marginX, int* pY, int rightEdge,
                                     bool fromBottom, DWORD rgbColor) {
    if (!m_hMemDC || !text || !text[0]) return;

    // Measure text
    RECT rCalc = { 0, 0, rightEdge - marginX, 2048 };
    ::DrawTextW(m_hMemDC, text, -1, &rCalc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    int textW = rCalc.right - rCalc.left;
    int textH = rCalc.bottom - rCalc.top;
    if (textH <= 0) return;

    // Position rect
    RECT r;
    if (alignRight) {
        if (fromBottom) {
            *pY -= textH;
            SetRect(&r, rightEdge - textW, *pY, rightEdge, *pY + textH);
        } else {
            SetRect(&r, rightEdge - textW, *pY, rightEdge, *pY + textH);
            *pY += textH;
        }
    } else {
        if (fromBottom) {
            *pY -= textH;
            SetRect(&r, marginX, *pY, marginX + textW, *pY + textH);
        } else {
            SetRect(&r, marginX, *pY, marginX + textW, *pY + textH);
            *pY += textH;
        }
    }

    // Shadow pass: offset (1,1), dark gray (not black — black = A=0 after post-process)
    RECT rShadow = r;
    OffsetRect(&rShadow, 1, 1);
    SetTextColor(m_hMemDC, RGB(64, 64, 64));
    ::DrawTextW(m_hMemDC, text, -1, &rShadow, DT_SINGLELINE | DT_NOPREFIX);

    // Main text: caller-specified color (default white)
    BYTE cr = (BYTE)((rgbColor >> 16) & 0xFF);
    BYTE cg = (BYTE)((rgbColor >> 8) & 0xFF);
    BYTE cb = (BYTE)(rgbColor & 0xFF);
    SetTextColor(m_hMemDC, RGB(cr, cg, cb));
    ::DrawTextW(m_hMemDC, text, -1, &r, DT_SINGLELINE | DT_NOPREFIX);
}

void COverlayThread::RenderOverlayToDIB() {
    UINT w = m_texWidth;
    UINT h = m_texHeight;
    if (w == 0 || h == 0) return;

    // Clear DIB to transparent black
    memset(m_pDIBBits, 0, (size_t)w * h * 4);

    int margin = max(10, (int)w / 100);
    int upperRightY = margin;
    int upperLeftY = margin;
    int lowerRightY = (int)h - margin;
    int lowerLeftY  = (int)h - margin;

    wchar_t buf[256];

    // --- HUD: Preset name (top-right, above FPS) ---
    if (m_currentData.bShowPresetName && m_currentData.szPresetName[0] && m_hHUDFont) {
        // Measure at normal HUD font size; shrink if too wide for available area
        int availW = (int)w - margin * 2;
        HFONT hUseFont = m_hHUDFont;
        HFONT hShrunk = NULL;

        HFONT hPrev = (HFONT)SelectObject(m_hMemDC, m_hHUDFont);
        RECT rCalc = { 0, 0, availW, 2048 };
        ::DrawTextW(m_hMemDC, m_currentData.szPresetName, -1, &rCalc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        if (rCalc.right - rCalc.left > availW && m_hudFontH > 10) {
            // Compute scaled font height to fit
            int shrunkH = (int)(m_hudFontH * (float)availW / (float)(rCalc.right - rCalc.left));
            if (shrunkH < 10) shrunkH = 10;
            hShrunk = CreateFontW(-shrunkH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            if (hShrunk) {
                hUseFont = hShrunk;
                SelectObject(m_hMemDC, hUseFont);
            }
        }

        DrawShadowText(m_currentData.szPresetName, true, margin, &upperRightY,
                       (int)w - margin, false, m_currentData.presetNameColor);
        SelectObject(m_hMemDC, hPrev);
        if (hShrunk) DeleteObject(hShrunk);
    }

    // --- HUD: Rating (top-right, below preset name, above FPS) ---
    if (m_currentData.bShowRating && m_currentData.szRating[0]) {
        DrawShadowText(m_currentData.szRating, true, margin, &upperRightY, (int)w - margin);
    }

    // FPS display (upper-right)
    if (m_currentData.bShowFPS) {
        swprintf(buf, 128, L"FPS: %4.2f ", m_currentData.fps);
        DrawShadowText(buf, true, margin, &upperRightY, (int)w - margin);
    }

    // Debug info (upper-left)
    if (m_currentData.bShowDebugInfo) {
        swprintf(buf, 128, L"  %6.2f pf_monitor", m_currentData.pfMonitor);
        DrawShadowText(buf, false, margin, &upperLeftY, (int)w - margin);

        if (!m_currentData.bPresetLocked) {
            swprintf(buf, 128, L"  %6.2f time", m_currentData.presetTime);
            DrawShadowText(buf, false, margin, &upperLeftY, (int)w - margin);
        }

        // bass
        swprintf(buf, 128, L"%s %6.2f bass", m_currentData.bass_imm_rel >= 1.3f ? L"+" : L" ", m_currentData.bass);
        DrawShadowText(buf, false, margin, &upperLeftY, (int)w - margin);
        swprintf(buf, 128, L"%s %6.2f bass_att", m_currentData.bass_avg_rel >= 1.3f ? L"+" : L" ", m_currentData.bass_att);
        DrawShadowText(buf, false, margin, &upperLeftY, (int)w - margin);
        swprintf(buf, 128, L"%s %6.2f bass_smooth", m_currentData.bass_smooth_rel >= 1.3f ? L"+" : L" ", m_currentData.bass_smooth);
        DrawShadowText(buf, false, margin, &upperLeftY, (int)w - margin);

        // mid
        swprintf(buf, 128, L"%s %6.2f mid", m_currentData.mid_imm_rel >= 1.3f ? L"+" : L" ", m_currentData.mid);
        DrawShadowText(buf, false, margin, &upperLeftY, (int)w - margin);
        swprintf(buf, 128, L"%s %6.2f mid_att", m_currentData.mid_avg_rel >= 1.3f ? L"+" : L" ", m_currentData.mid_att);
        DrawShadowText(buf, false, margin, &upperLeftY, (int)w - margin);
        swprintf(buf, 128, L"%s %6.2f mid_smooth", m_currentData.mid_smooth_rel >= 1.3f ? L"+" : L" ", m_currentData.mid_smooth);
        DrawShadowText(buf, false, margin, &upperLeftY, (int)w - margin);

        // treb
        swprintf(buf, 128, L"%s %6.2f treb", m_currentData.treb_imm_rel >= 1.3f ? L"+" : L" ", m_currentData.treb);
        DrawShadowText(buf, false, margin, &upperLeftY, (int)w - margin);
        swprintf(buf, 128, L"%s %6.2f treb_att", m_currentData.treb_avg_rel >= 1.3f ? L"+" : L" ", m_currentData.treb_att);
        DrawShadowText(buf, false, margin, &upperLeftY, (int)w - margin);
        swprintf(buf, 128, L"%s %6.2f treb_smooth", m_currentData.treb_smooth_rel >= 1.3f ? L"+" : L" ", m_currentData.treb_smooth);
        DrawShadowText(buf, false, margin, &upperLeftY, (int)w - margin);

        // quality + color shift (lower-right)
        swprintf(buf, 128, L"q=%.2f hue=%.2f sat=%.2f bri=%.2f",
                 m_currentData.fRenderQuality, m_currentData.fColShiftHue,
                 m_currentData.fColShiftSaturation, m_currentData.fColShiftBrightness);
        DrawShadowText(buf, true, margin, &lowerRightY, (int)w - margin, true);

        // mouse info (lower-right)
        if (m_currentData.bEnableMouseInteraction) {
            swprintf(buf, 128, L"mouse x=%0.2f y=%0.2f z=%s ",
                     m_currentData.mouseX, m_currentData.mouseY,
                     m_currentData.mouseDown ? L"1" : L"0");
            DrawShadowText(buf, true, margin, &lowerRightY, (int)w - margin, true);
        }
    }

    // --- HUD: Song title (bottom-left) ---
    if (m_currentData.bShowSongTitle && m_currentData.szSongTitle[0] && m_hHUDFont) {
        HFONT hPrev = (HFONT)SelectObject(m_hMemDC, m_hHUDFont);
        DrawShadowText(m_currentData.szSongTitle, false, margin, &lowerLeftY,
                       (int)w - margin, true);
        SelectObject(m_hMemDC, hPrev);
    }

    // --- HUD: Notifications (per-corner stacking) ---
    if (m_currentData.nNotifications > 0) {
        int notifY[4] = { upperRightY, upperLeftY, lowerRightY, lowerLeftY };
        for (int i = 0; i < m_currentData.nNotifications; i++) {
            const auto& n = m_currentData.notifications[i];
            if (!n.text[0]) continue;
            int  c       = n.corner;
            if (c < 0 || c > 3) c = 0;
            bool right   = (c == 0 || c == 2);
            bool fromBot = (c == 2 || c == 3);
            DrawShadowText(n.text, right, margin, &notifY[c], (int)w - margin, fromBot, n.color);
        }
    }

    // Menu overlay
    if (m_currentData.bShowMenu) {
        RenderMenuToDIB();
    }
}

void COverlayThread::PostProcessAlpha() {
    UINT w = m_texWidth;
    UINT h = m_texHeight;
    BYTE* pixels = m_pDIBBits;
    if (!pixels) return;

    // Menu dark box: composite box alpha (0xD0) with text alpha inside box region.
    // Outside box: A = max(R,G,B) — text coverage only
    // Inside box:  A = maxRGB + (0xD0 * (255 - maxRGB)) / 255
    //   Empty pixel → A = 0xD0 (dark semitransparent background)
    //   White text  → A = 255  (fully opaque)
    //   Gray shadow → A ≈ 220  (blended)
    bool hasBox = m_currentData.bShowMenu &&
                  m_currentData.menuBox.right > m_currentData.menuBox.left &&
                  m_currentData.menuBox.bottom > m_currentData.menuBox.top;
    int boxL = m_currentData.menuBox.left;
    int boxT = m_currentData.menuBox.top;
    int boxR = m_currentData.menuBox.right;
    int boxB = m_currentData.menuBox.bottom;

    for (UINT y = 0; y < h; y++) {
        for (UINT x = 0; x < w; x++) {
            UINT idx = (y * w + x) * 4;
            BYTE b = pixels[idx + 0];
            BYTE g = pixels[idx + 1];
            BYTE r = pixels[idx + 2];
            BYTE maxRGB = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);

            if (hasBox && (int)x >= boxL && (int)x < boxR && (int)y >= boxT && (int)y < boxB) {
                pixels[idx + 3] = (BYTE)(maxRGB + (0xD0 * (255 - maxRGB)) / 255);
            } else {
                pixels[idx + 3] = maxRGB;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Menu rendering to DIB (overlay thread)
// ---------------------------------------------------------------------------
void COverlayThread::RenderMenuToDIB() {
    if (!m_hMemDC || !m_currentData.bShowMenu) return;

    // Create/recreate menu font if size changed
    int desiredH = m_currentData.menuFontHeight;
    if (desiredH <= 0) desiredH = 20;
    if (!m_hMenuFont || m_menuFontHeight != desiredH) {
        if (m_hMenuFont) DeleteObject(m_hMenuFont);
        m_hMenuFont = CreateFontW(
            -desiredH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        m_menuFontHeight = desiredH;
    }
    if (!m_hMenuFont) return;

    // Select menu font (save current font to restore later)
    HFONT hPrevFont = (HFONT)SelectObject(m_hMemDC, m_hMenuFont);

    // Draw each menu line
    for (int i = 0; i < m_currentData.nMenuLines; i++) {
        const OverlayMenuLine& line = m_currentData.menuLines[i];
        if (!line.text[0]) continue;

        // Extract RGB from ARGB color
        DWORD argb = line.color;
        BYTE r = (BYTE)((argb >> 16) & 0xFF);
        BYTE g = (BYTE)((argb >> 8) & 0xFF);
        BYTE b = (BYTE)(argb & 0xFF);
        SetTextColor(m_hMemDC, RGB(r, g, b));

        RECT rc = { line.x, line.y, (LONG)m_texWidth, line.y + desiredH + 4 };
        ::DrawTextW(m_hMemDC, line.text, -1, &rc, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    // Tooltip (drawn below menu box)
    if (m_currentData.bShowTooltip && m_currentData.tooltip[0]) {
        SetTextColor(m_hMemDC, RGB(0xBB, 0xBB, 0xCC));  // TOOLTIP_COLOR
        RECT rc = { m_currentData.tooltipX, m_currentData.tooltipY,
                     (LONG)m_texWidth - 4, (LONG)m_texHeight };
        ::DrawTextW(m_hMemDC, m_currentData.tooltip, -1, &rc,
                    DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    // Restore previous font
    SelectObject(m_hMemDC, hPrevFont);
}
