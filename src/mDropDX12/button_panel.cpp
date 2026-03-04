// button_panel.cpp — Owner-drawn button grid control implementation.
//
// Renders a configurable grid of buttons with optional thumbnails,
// label text, hover/press states, and 3D beveled borders matching
// the MDropDX12 dark theme.  Supports paged banking.

#include "button_panel.h"
#include "json_utils.h"
#include "engine.h"
#include "engine_helpers.h"
#include <algorithm>

namespace mdrop {

bool ButtonPanel::s_bClassRegistered = false;

// ─── Construction / Destruction ─────────────────────────────────────────

ButtonPanel::ButtonPanel() = default;

ButtonPanel::~ButtonPanel() {
    Destroy();
}

void ButtonPanel::Destroy() {
    ClearAllThumbnails();
    if (m_hWnd) {
        DestroyWindow(m_hWnd);
        m_hWnd = NULL;
    }
}

// ─── Window Creation ────────────────────────────────────────────────────

bool ButtonPanel::Create(HWND hParent, Engine* pEngine, int ctrlID,
                         int x, int y, int w, int h)
{
    m_pEngine = pEngine;
    m_nCtrlID = ctrlID;

    if (!s_bClassRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc   = WndProc;
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL; // we paint everything
        wc.lpszClassName = L"MDropDX12ButtonPanel";
        if (!RegisterClassExW(&wc))
            return false;
        s_bClassRegistered = true;
    }

    m_hWnd = CreateWindowExW(0, L"MDropDX12ButtonPanel", NULL,
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        x, y, w, h, hParent, (HMENU)(INT_PTR)ctrlID, NULL, this);

    if (!m_hWnd)
        return false;

    EnsureSlotCount();
    return true;
}

// ─── Config / Slots ─────────────────────────────────────────────────────

void ButtonPanel::ApplyConfig() {
    EnsureSlotCount();
    Invalidate();
}

int ButtonPanel::GetTotalSlots() const {
    if (m_config.bScrollable)
        return (int)m_slots.size(); // grows as needed
    return m_config.nBanks * m_config.nRows * m_config.nCols;
}

void ButtonPanel::EnsureSlotCount() {
    int needed = m_config.nBanks * m_config.nRows * m_config.nCols;
    if ((int)m_slots.size() < needed)
        m_slots.resize(needed);
}

ButtonSlot& ButtonPanel::Slot(int i) {
    EnsureSlotCount();
    return m_slots[i];
}

const ButtonSlot& ButtonPanel::Slot(int i) const {
    return m_slots[i];
}

// ─── Banking ────────────────────────────────────────────────────────────

void ButtonPanel::SetActiveBank(int bank) {
    if (bank < 0) bank = 0;
    if (bank >= m_config.nBanks) bank = m_config.nBanks - 1;
    if (m_config.nActiveBank != bank) {
        m_config.nActiveBank = bank;
        m_nHoverSlot = -1;
        m_nPressedSlot = -1;
        Invalidate();
    }
}

void ButtonPanel::NextBank() {
    SetActiveBank((m_config.nActiveBank + 1) % m_config.nBanks);
}

void ButtonPanel::PrevBank() {
    SetActiveBank((m_config.nActiveBank + m_config.nBanks - 1) % m_config.nBanks);
}

// ─── Thumbnails ─────────────────────────────────────────────────────────

void ButtonPanel::SetSlotThumbnail(int idx, HBITMAP hBmp, int w, int h) {
    if (idx < 0 || idx >= (int)m_slots.size()) return;
    if (m_slots[idx].hThumbnail)
        DeleteObject(m_slots[idx].hThumbnail);
    m_slots[idx].hThumbnail = hBmp;
    m_slots[idx].thumbW = w;
    m_slots[idx].thumbH = h;
    Invalidate();
}

void ButtonPanel::ClearSlotThumbnail(int idx) {
    if (idx < 0 || idx >= (int)m_slots.size()) return;
    if (m_slots[idx].hThumbnail) {
        DeleteObject(m_slots[idx].hThumbnail);
        m_slots[idx].hThumbnail = NULL;
        m_slots[idx].thumbW = m_slots[idx].thumbH = 0;
    }
}

void ButtonPanel::ClearAllThumbnails() {
    for (auto& s : m_slots) {
        if (s.hThumbnail) {
            DeleteObject(s.hThumbnail);
            s.hThumbnail = NULL;
            s.thumbW = s.thumbH = 0;
        }
    }
}

// ─── Layout / Repaint ───────────────────────────────────────────────────

void ButtonPanel::Reposition(int x, int y, int w, int h) {
    if (m_hWnd)
        MoveWindow(m_hWnd, x, y, w, h, TRUE);
}

void ButtonPanel::Invalidate() {
    if (m_hWnd)
        InvalidateRect(m_hWnd, NULL, FALSE);
}

void ButtonPanel::SetFont(HFONT hFont) {
    m_hFont = hFont;
    Invalidate();
}

void ButtonPanel::SetThemeColors(COLORREF bg, COLORREF btnFace, COLORREF btnHi,
                                  COLORREF btnShadow, COLORREF text, COLORREF ctrlBg) {
    m_colBg        = bg;
    m_colBtnFace   = btnFace;
    m_colBtnHi     = btnHi;
    m_colBtnShadow = btnShadow;
    m_colText      = text;
    m_colEmpty     = ctrlBg;
    Invalidate();
}

// ─── Geometry ───────────────────────────────────────────────────────────

RECT ButtonPanel::GetButtonRect(int localIndex) const {
    RECT rcClient;
    GetClientRect(m_hWnd, &rcClient);

    int pad = m_config.nButtonPadding;
    int totalW = rcClient.right - rcClient.left;
    int totalH = rcClient.bottom - rcClient.top;

    int cellW = (totalW - pad) / m_config.nCols;
    int cellH = (totalH - pad) / m_config.nRows;

    int row = localIndex / m_config.nCols;
    int col = localIndex % m_config.nCols;

    RECT rc;
    rc.left   = pad + col * cellW;
    rc.top    = pad + row * cellH;
    rc.right  = rc.left + cellW - pad;
    rc.bottom = rc.top  + cellH - pad;
    return rc;
}

int ButtonPanel::HitTest(POINT pt) const {
    int perBank = m_config.nRows * m_config.nCols;
    for (int i = 0; i < perBank; i++) {
        RECT rc = GetButtonRect(i);
        if (PtInRect(&rc, pt))
            return i;
    }
    return -1;
}

int ButtonPanel::LocalToGlobal(int localIndex) const {
    return m_config.nActiveBank * (m_config.nRows * m_config.nCols) + localIndex;
}

// ─── WndProc ────────────────────────────────────────────────────────────

LRESULT CALLBACK ButtonPanel::WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    ButtonPanel* self = NULL;

    if (uMsg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lParam;
        self = (ButtonPanel*)cs->lpCreateParams;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (ButtonPanel*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    }

    if (self)
        return self->HandleMessage(hWnd, uMsg, wParam, lParam);

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT ButtonPanel::HandleMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        // Double-buffered paint
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        int w = rcClient.right, h = rcClient.bottom;
        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hBmp = CreateCompatibleBitmap(hdc, w, h);
        HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hBmp);

        OnPaint(hdcMem);

        BitBlt(hdc, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, hOldBmp);
        DeleteObject(hBmp);
        DeleteDC(hdcMem);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // handled in WM_PAINT

    case WM_MOUSEMOVE: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        int newHover = HitTest(pt);
        if (newHover != m_nHoverSlot) {
            m_nHoverSlot = newHover;
            Invalidate();
        }
        // Request WM_MOUSELEAVE
        TRACKMOUSEEVENT tme = {};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hWnd;
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (m_nHoverSlot >= 0) {
            m_nHoverSlot = -1;
            Invalidate();
        }
        return 0;

    case WM_LBUTTONDOWN: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        m_nPressedSlot = HitTest(pt);
        if (m_nPressedSlot >= 0) {
            SetCapture(hWnd);
            Invalidate();
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (m_nPressedSlot >= 0) {
            ReleaseCapture();
            POINT pt = { LOWORD(lParam), HIWORD(lParam) };
            int slot = HitTest(pt);
            int released = m_nPressedSlot;
            m_nPressedSlot = -1;
            Invalidate();
            if (slot == released && OnLeftClick) {
                int gi = LocalToGlobal(slot);
                if (gi < (int)m_slots.size())
                    OnLeftClick(gi);
            }
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        // Treat double-click same as single click (trigger action)
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        int slot = HitTest(pt);
        if (slot >= 0 && OnLeftClick) {
            int gi = LocalToGlobal(slot);
            if (gi < (int)m_slots.size())
                OnLeftClick(gi);
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        int slot = HitTest(pt);
        if (slot >= 0 && OnRightClick) {
            POINT screen = pt;
            ClientToScreen(hWnd, &screen);
            int gi = LocalToGlobal(slot);
            if (gi < (int)m_slots.size())
                OnRightClick(gi, screen);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (m_config.bScrollable) {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            m_nScrollOffset -= (delta > 0) ? 1 : -1;
            if (m_nScrollOffset < 0) m_nScrollOffset = 0;
            Invalidate();
        } else {
            // Scroll wheel changes bank in paged mode
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (delta > 0) PrevBank(); else NextBank();
        }
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

// ─── Painting ───────────────────────────────────────────────────────────

void ButtonPanel::OnPaint(HDC hdc) {
    RECT rcClient;
    GetClientRect(m_hWnd, &rcClient);

    // Fill background
    HBRUSH hBrBg = CreateSolidBrush(m_colBg);
    FillRect(hdc, &rcClient, hBrBg);
    DeleteObject(hBrBg);

    HFONT hOldFont = NULL;
    if (m_hFont)
        hOldFont = (HFONT)SelectObject(hdc, m_hFont);

    int perBank = m_config.nRows * m_config.nCols;
    for (int i = 0; i < perBank; i++) {
        RECT rc = GetButtonRect(i);
        bool hover  = (i == m_nHoverSlot);
        bool press  = (i == m_nPressedSlot);
        PaintButton(hdc, i, rc, hover, press);
    }

    if (hOldFont)
        SelectObject(hdc, hOldFont);
}

void ButtonPanel::PaintButton(HDC hdc, int localIndex, const RECT& rc,
                               bool hover, bool pressed)
{
    int gi = LocalToGlobal(localIndex);
    bool hasSlot = (gi < (int)m_slots.size());
    const ButtonSlot* slot = hasSlot ? &m_slots[gi] : NULL;
    bool empty = (!slot || slot->action == ButtonAction::None);

    // Button face color
    COLORREF faceFill = empty ? m_colEmpty : m_colBtnFace;
    if (hover && !pressed)
        faceFill = RGB(
            (std::min)(GetRValue(faceFill) + 20, 255),
            (std::min)(GetGValue(faceFill) + 20, 255),
            (std::min)(GetBValue(faceFill) + 20, 255));

    HBRUSH hBr = CreateSolidBrush(faceFill);
    FillRect(hdc, &rc, hBr);
    DeleteObject(hBr);

    // 3D beveled border
    draw3DEdge(hdc, rc, m_colBtnHi, m_colBtnShadow, !pressed);

    // Content area (inset by 2px for border)
    RECT rcContent = { rc.left + 2, rc.top + 2, rc.right - 2, rc.bottom - 2 };
    if (pressed) {
        rcContent.left++;
        rcContent.top++;
    }

    // Draw thumbnail if available
    if (slot && slot->hThumbnail && slot->thumbW > 0 && slot->thumbH > 0) {
        HDC hdcThumb = CreateCompatibleDC(hdc);
        HBITMAP hOld = (HBITMAP)SelectObject(hdcThumb, slot->hThumbnail);

        // Fit thumbnail into content area maintaining aspect ratio
        int cw = rcContent.right - rcContent.left;
        int ch = rcContent.bottom - rcContent.top;
        float scaleX = (float)cw / slot->thumbW;
        float scaleY = (float)ch / slot->thumbH;
        float scale = (std::min)(scaleX, scaleY);
        int dw = (int)(slot->thumbW * scale);
        int dh = (int)(slot->thumbH * scale);
        int dx = rcContent.left + (cw - dw) / 2;
        int dy = rcContent.top  + (ch - dh) / 2;

        SetStretchBltMode(hdc, HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, NULL);
        StretchBlt(hdc, dx, dy, dw, dh,
                   hdcThumb, 0, 0, slot->thumbW, slot->thumbH, SRCCOPY);

        SelectObject(hdcThumb, hOld);
        DeleteDC(hdcThumb);

        // Draw label below thumbnail if room
        if (!slot->label.empty()) {
            RECT rcLabel = { rc.left, rc.bottom - 16, rc.right, rc.bottom - 2 };
            if (pressed) { rcLabel.left++; rcLabel.top++; }
            // Semi-transparent background for readability
            HBRUSH hBrLabel = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hdc, &rcLabel, hBrLabel);
            DeleteObject(hBrLabel);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, m_colText);
            DrawTextW(hdc, slot->label.c_str(), -1, &rcLabel,
                      DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    } else if (slot && !slot->label.empty()) {
        // No thumbnail — draw label centered
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, m_colText);
        DrawTextW(hdc, slot->label.c_str(), -1, &rcContent,
                  DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else {
        // Empty slot — draw slot number or "+"
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(80, 80, 80));
        wchar_t buf[16];
        swprintf(buf, 16, L"%d", gi + 1);
        DrawTextW(hdc, buf, -1, &rcContent,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
}

// ─── INI Persistence ────────────────────────────────────────────────────

void ButtonPanel::SaveToINI(const wchar_t* iniPath, const wchar_t* section) const {
    wchar_t buf[32];

    swprintf(buf, 32, L"%d", m_config.nRows);
    WritePrivateProfileStringW(section, L"Rows", buf, iniPath);
    swprintf(buf, 32, L"%d", m_config.nCols);
    WritePrivateProfileStringW(section, L"Cols", buf, iniPath);
    swprintf(buf, 32, L"%d", m_config.nBanks);
    WritePrivateProfileStringW(section, L"Banks", buf, iniPath);
    swprintf(buf, 32, L"%d", m_config.nActiveBank);
    WritePrivateProfileStringW(section, L"ActiveBank", buf, iniPath);
    swprintf(buf, 32, L"%d", m_config.bScrollable ? 1 : 0);
    WritePrivateProfileStringW(section, L"Scrollable", buf, iniPath);

    int total = m_config.nBanks * m_config.nRows * m_config.nCols;
    swprintf(buf, 32, L"%d", total);
    WritePrivateProfileStringW(section, L"SlotCount", buf, iniPath);

    for (int i = 0; i < total && i < (int)m_slots.size(); i++) {
        const auto& s = m_slots[i];
        if (s.action == ButtonAction::None)
            continue;

        // Format: action|payload|label  (payload uses \x7C for literal pipes)
        wchar_t key[32];
        swprintf(key, 32, L"Slot_%d", i);

        // Escape pipes in payload: | -> \x7C
        std::wstring escapedPayload = s.payload;
        for (size_t pos = 0; (pos = escapedPayload.find(L'|', pos)) != std::wstring::npos; pos += 4)
            escapedPayload.replace(pos, 1, L"\\x7C");

        std::wstring val = std::to_wstring((int)s.action) + L"|" + escapedPayload + L"|" + s.label;
        WritePrivateProfileStringW(section, key, val.c_str(), iniPath);

        // Save image path if present
        if (!s.imagePath.empty()) {
            wchar_t imgKey[32];
            swprintf(imgKey, 32, L"SlotImg_%d", i);
            WritePrivateProfileStringW(section, imgKey, s.imagePath.c_str(), iniPath);
        }

        // Save hotkey binding if present
        if (s.vk != 0) {
            wchar_t hkKey[32], hkVal[64];
            swprintf(hkKey, 32, L"SlotHK_%d", i);
            swprintf(hkVal, 64, L"%u|%u|%d", s.modifiers, s.vk, s.scope);
            WritePrivateProfileStringW(section, hkKey, hkVal, iniPath);
        }
    }
}

void ButtonPanel::LoadFromINI(const wchar_t* iniPath, const wchar_t* section) {
    m_config.nRows       = GetPrivateProfileIntW(section, L"Rows", 3, iniPath);
    m_config.nCols       = GetPrivateProfileIntW(section, L"Cols", 5, iniPath);
    m_config.nBanks      = GetPrivateProfileIntW(section, L"Banks", 3, iniPath);
    m_config.nActiveBank = GetPrivateProfileIntW(section, L"ActiveBank", 0, iniPath);
    m_config.bScrollable = GetPrivateProfileIntW(section, L"Scrollable", 0, iniPath) != 0;

    // Clamp
    if (m_config.nRows < 1) m_config.nRows = 1;
    if (m_config.nCols < 1) m_config.nCols = 1;
    if (m_config.nBanks < 1) m_config.nBanks = 1;
    if (m_config.nActiveBank >= m_config.nBanks) m_config.nActiveBank = 0;

    EnsureSlotCount();

    int total = GetPrivateProfileIntW(section, L"SlotCount", -1, iniPath);
    if (total < 0) {
        // No saved config — populate defaults mirroring Milkwave Remote layout
        PopulateDefaults();
        return;
    }
    for (int i = 0; i < total && i < (int)m_slots.size(); i++) {
        wchar_t key[32], val[2048];
        swprintf(key, 32, L"Slot_%d", i);
        GetPrivateProfileStringW(section, key, L"", val, 2048, iniPath);
        if (val[0] == 0) continue;

        // Parse: action|payload|label
        std::wstring line(val);
        size_t p1 = line.find(L'|');
        if (p1 == std::wstring::npos) continue;
        size_t p2 = line.find(L'|', p1 + 1);
        if (p2 == std::wstring::npos) continue;

        int act = _wtoi(line.substr(0, p1).c_str());
        std::wstring payload = line.substr(p1 + 1, p2 - p1 - 1);
        std::wstring label = line.substr(p2 + 1);

        // Unescape pipes: \x7C -> |
        for (size_t pos = 0; (pos = payload.find(L"\\x7C", pos)) != std::wstring::npos;)
            payload.replace(pos, 4, L"|");

        if (act >= (int)ButtonAction::None && act <= (int)ButtonAction::LaunchApp) {
            m_slots[i].action  = (ButtonAction)act;
            m_slots[i].payload = payload;
            m_slots[i].label   = label;
        }

        // Load optional image path
        wchar_t imgKey[32];
        swprintf(imgKey, 32, L"SlotImg_%d", i);
        wchar_t imgVal[MAX_PATH];
        GetPrivateProfileStringW(section, imgKey, L"", imgVal, MAX_PATH, iniPath);
        if (imgVal[0])
            m_slots[i].imagePath = imgVal;

        // Load optional hotkey binding
        wchar_t hkKey[32];
        swprintf(hkKey, 32, L"SlotHK_%d", i);
        wchar_t hkVal[128];
        GetPrivateProfileStringW(section, hkKey, L"", hkVal, 128, iniPath);
        if (hkVal[0]) {
            // Format: modifiers|vk|scope
            UINT mod = 0, vk = 0;
            int sc = 0;
            swscanf_s(hkVal, L"%u|%u|%d", &mod, &vk, &sc);
            m_slots[i].modifiers = mod;
            m_slots[i].vk = vk;
            m_slots[i].scope = sc;
        }
    }
}

// ─── JSON Layout Export/Import ──────────────────────────────────────────

// Action type ↔ string name (Milkwave-style readable names)
static const wchar_t* ActionToString(ButtonAction a) {
    switch (a) {
    case ButtonAction::LoadPreset:    return L"LoadPreset";
    case ButtonAction::PushSprite:    return L"PushSprite";
    case ButtonAction::ScriptCommand: return L"ScriptCommand";
    case ButtonAction::LaunchMessage: return L"LaunchMessage";
    case ButtonAction::RunScript:     return L"RunScript";
    case ButtonAction::LaunchApp:     return L"LaunchApp";
    default:                          return L"None";
    }
}

static ButtonAction StringToAction(const std::wstring& s) {
    if (s == L"LoadPreset"    || s == L"1") return ButtonAction::LoadPreset;
    if (s == L"PushSprite"    || s == L"2") return ButtonAction::PushSprite;
    if (s == L"ScriptCommand" || s == L"3") return ButtonAction::ScriptCommand;
    if (s == L"LaunchMessage" || s == L"4") return ButtonAction::LaunchMessage;
    if (s == L"RunScript"     || s == L"5") return ButtonAction::RunScript;
    if (s == L"LaunchApp"     || s == L"6") return ButtonAction::LaunchApp;
    return ButtonAction::None;
}

bool ButtonPanel::SaveLayoutJSON(const wchar_t* filePath) const {
    JsonWriter w;
    w.BeginObject();
    w.Int(L"rows", m_config.nRows);
    w.Int(L"cols", m_config.nCols);
    w.Int(L"banks", m_config.nBanks);
    w.BeginArray(L"slots");

    int total = m_config.nBanks * m_config.nRows * m_config.nCols;
    for (int i = 0; i < total && i < (int)m_slots.size(); i++) {
        const auto& s = m_slots[i];
        if (s.action == ButtonAction::None) continue;
        w.BeginObject();
        w.Int(L"index", i);
        w.String(L"action", ActionToString(s.action));
        w.String(L"payload", s.payload);
        w.String(L"displayName", s.label);
        if (!s.imagePath.empty())
            w.String(L"image", s.imagePath);
        if (s.vk != 0) {
            w.Int(L"keyModifiers", (int)s.modifiers);
            w.Int(L"keyCode", (int)s.vk);
            w.Int(L"keyScope", s.scope);
        }
        w.EndObject();
    }

    w.EndArray();
    w.EndObject();
    return w.SaveToFile(filePath);
}

bool ButtonPanel::LoadLayoutJSON(const wchar_t* filePath) {
    JsonValue doc = JsonLoadFile(filePath);
    if (!doc.isObject()) return false;

    m_config.nRows  = doc[L"rows"].asInt(3);
    m_config.nCols  = doc[L"cols"].asInt(5);
    m_config.nBanks = doc[L"banks"].asInt(3);
    if (m_config.nRows < 1) m_config.nRows = 1;
    if (m_config.nCols < 1) m_config.nCols = 1;
    if (m_config.nBanks < 1) m_config.nBanks = 1;
    m_config.nActiveBank = 0;

    EnsureSlotCount();
    for (auto& s : m_slots) {
        s.action = ButtonAction::None;
        s.payload.clear();
        s.label.clear();
    }

    // Parse slot entries
    const auto& slots = doc[L"slots"];
    for (size_t i = 0; i < slots.size(); i++) {
        const auto& obj = slots.at(i);
        int idx = obj[L"index"].asInt(-1);
        if (idx < 0 || idx >= (int)m_slots.size()) continue;

        // Accept both string action names and legacy int values
        m_slots[idx].action  = StringToAction(obj[L"action"].asString());
        m_slots[idx].payload = obj[L"payload"].asString();
        // Accept both "displayName" (Milkwave style) and legacy "label"
        std::wstring name = obj[L"displayName"].asString();
        if (name.empty()) name = obj[L"label"].asString();
        m_slots[idx].label = name;
        m_slots[idx].imagePath = obj[L"image"].asString();

        // Load optional hotkey binding
        m_slots[idx].modifiers = (UINT)obj[L"keyModifiers"].asInt(0);
        m_slots[idx].vk        = (UINT)obj[L"keyCode"].asInt(0);
        m_slots[idx].scope     = obj[L"keyScope"].asInt(0);
    }

    ApplyConfig();
    return true;
}

void ButtonPanel::ResetToDefaults() {
    m_config.nRows = 3;
    m_config.nCols = 5;
    m_config.nBanks = 3;
    m_config.nActiveBank = 0;
    m_config.bScrollable = false;
    EnsureSlotCount();
    for (auto& s : m_slots) {
        s.action = ButtonAction::None;
        s.payload.clear();
        s.label.clear();
    }
    PopulateDefaults();
    ApplyConfig();
}

// ─── Default Layout (mirrors Milkwave Remote 3x5 grid) ─────────────────

void ButtonPanel::PopulateDefaults() {
    // Only populate bank 0 (slots 0–14 for 3x5)
    struct DefaultSlot { const wchar_t* label; const wchar_t* payload; };
    static const DefaultSlot defaults[] = {
        // Row 1: Navigation
        { L"Next Preset",     L"ACTION=NextPreset" },
        { L"Prev Preset",     L"ACTION=PrevPreset" },
        { L"Sound Info",      L"ACTION=DebugInfo" },
        { L"Song Info",       L"ACTION=OpenSongInfo" },
        { L"Preset Lock",     L"LOCK" },
        // Row 2: Window
        { L"Fullscreen",      L"ACTION=ToggleFullscreen" },
        { L"Change FPS",      L"ACTION=FPSCycle" },
        { L"Preset Info",     L"ACTION=ShowPresetInfo" },
        { L"Always On Top",   L"ACTION=AlwaysOnTop" },
        { L"Toggle Spout",    L"ACTION=SpoutToggle" },
        // Row 3: Mode
        { L"Sprite/Msg",      L"ACTION=SpriteMode" },
        { L"Clear Sprites",   L"ACTION=KillSprites" },
        { L"Clear Texts",     L"ACTION=KillSupertexts" },
        { L"Transparency",    L"ACTION=TransparencyMode" },
        { L"Play/Pause",      L"ACTION=MediaPlayPause" },
    };

    int count = sizeof(defaults) / sizeof(defaults[0]);
    if (count > (int)m_slots.size()) count = (int)m_slots.size();

    for (int i = 0; i < count; i++) {
        m_slots[i].action  = ButtonAction::ScriptCommand;
        m_slots[i].payload = defaults[i].payload;
        m_slots[i].label   = defaults[i].label;
    }
}

} // namespace mdrop
