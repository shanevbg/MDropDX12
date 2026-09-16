// display_panel.cpp — see display_panel.h for what this is and why.

#include "engine.h"
#include "display_panel.h"
#include "engine_helpers.h"
#include "format_to.h"

#include <algorithm>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

namespace mdrop {

bool DisplayPanel::s_bClassRegistered = false;

// Room for the Spout row under the monitors, as a fraction of the panel. Spout
// senders are few and their tiles carry two short lines, so they get a strip
// rather than an equal share -- the monitors are what the eye is looking for.
static const float kSpoutRowFraction = 0.28f;
static const int   kPad = 6;

bool DisplayPanel::Create(HWND hParent, Engine* pEngine, int ctrlID,
                          int x, int y, int w, int h)
{
    m_pEngine = pEngine;
    m_nCtrlID = ctrlID;

    if (!s_bClassRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = WndProc;
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;            // painted whole, every time
        wc.lpszClassName = L"MDropDX12DisplayPanel";
        if (!RegisterClassExW(&wc))
            return false;
        s_bClassRegistered = true;
    }

    m_hWnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"MDropDX12DisplayPanel", NULL,
        WS_CHILD | WS_VISIBLE, x, y, w, h, hParent,
        (HMENU)(INT_PTR)ctrlID, NULL, this);
    return m_hWnd != NULL;
}

LRESULT CALLBACK DisplayPanel::WndProc(HWND hWnd, UINT msg, WPARAM wParam,
                                       LPARAM lParam)
{
    DisplayPanel* self = (DisplayPanel*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lParam;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        ((DisplayPanel*)cs->lpCreateParams)->m_hWnd = hWnd;
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    if (!self) return DefWindowProcW(hWnd, msg, wParam, lParam);

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;                            // OnPaint fills every pixel

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        // Double-buffered, like ButtonPanel: this redraws once a second while a
        // child is starting, and drawing straight to the DC flickers.
        RECT rc; GetClientRect(hWnd, &rc);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
        self->OnPaint(mem);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        const int idx = self->HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (idx >= 0) {
            // Drive the LIST BOX, not our own state. Everything below this
            // panel reads the selection from there, and LBN_SELCHANGE is what
            // repopulates it -- so the panel stays a second view of one list
            // rather than a second source of truth that can disagree with it.
            HWND hParent = GetParent(hWnd);
            HWND hList = GetDlgItem(hParent, IDC_MW_DISP_LIST);
            if (hList) {
                SendMessage(hList, LB_SETCURSEL, (WPARAM)idx, 0);
                SendMessage(hParent, WM_COMMAND,
                            MAKEWPARAM(IDC_MW_DISP_LIST, LBN_SELCHANGE),
                            (LPARAM)hList);
            }
            self->Invalidate();
        }
        return 0;
    }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int DisplayPanel::HitTest(int x, int y) const {
    POINT pt = { x, y };
    for (auto& t : m_tiles)
        if (PtInRect(&t.rc, pt)) return t.outputIndex;
    return -1;
}

// One tile: a filled rectangle, a border, and as much text as fits.
static void DrawTile(HDC hdc, const RECT& rc, COLORREF fill, COLORREF edge,
                     COLORREF text, bool selected, HFONT hBig, HFONT hSmall,
                     const wchar_t* big, const wchar_t* line1,
                     const wchar_t* line2)
{
    HBRUSH br = CreateSolidBrush(fill);
    FillRect(hdc, &rc, br);
    DeleteObject(br);

    // Selection is a thicker, brighter frame rather than a different fill: the
    // fill already carries the state, and two meanings in one channel is how a
    // legend becomes necessary.
    HPEN pen = CreatePen(PS_SOLID, selected ? 3 : 1, edge);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, text);

    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int kLine = 15;

    // Lay the text out from the BOTTOM UP, reserving its band before the number
    // is placed. Positioning the number by the tile's centre and the lines by
    // its bottom edge independently is what put "OWN PRESET" through the middle
    // of the "1" on a short tile -- two claims on the same pixels, never
    // compared.
    int reserved = 0;
    const bool wantL1 = (line1 && line1[0] && h >= 30);
    const bool wantL2 = (line2 && line2[0] && h >= 30 + kLine);
    if (wantL1) reserved += kLine;
    if (wantL2) reserved += kLine;

    HFONT oldF = (HFONT)SelectObject(hdc, hSmall);
    RECT r = rc;
    r.left += 3; r.right -= 3;
    int ty = rc.bottom - 3 - reserved;
    if (wantL1) {
        RECT r1 = r; r1.top = ty; r1.bottom = ty + kLine;
        DrawTextW(hdc, line1, -1, &r1,
                  DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        ty += kLine;
    }
    if (wantL2) {
        RECT r2 = r; r2.top = ty; r2.bottom = ty + kLine;
        DrawTextW(hdc, line2, -1, &r2,
                  DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    SelectObject(hdc, oldF);

    // Whatever is left over, and only if the number can be read in it.
    if (big && big[0] && w > 34) {
        RECT rn = rc;
        rn.bottom -= reserved + 3;
        if (rn.bottom - rn.top >= 20) {
            HFONT oldB = (HFONT)SelectObject(hdc, hBig);
            DrawTextW(hdc, big, -1, &rn,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, oldB);
        }
    }
}

void DisplayPanel::OnPaint(HDC hdc)
{
    m_tiles.clear();
    Engine* p = m_pEngine;
    if (!p) return;

    RECT rcAll; GetClientRect(m_hWnd, &rcAll);
    HBRUSH bg = CreateSolidBrush(p->m_colSettingsBg);
    FillRect(hdc, &rcAll, bg);
    DeleteObject(bg);

    HFONT hSmall = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT hBig = CreateFontW(-max(14, (rcAll.bottom - rcAll.top) / 7), 0, 0, 0,
                             FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    const int selected = (int)SendMessage(
        GetDlgItem(GetParent(m_hWnd), IDC_MW_DISP_LIST), LB_GETCURSEL, 0, 0);

    wchar_t renderDev[32] = {};
    p->GetRenderMonitorDevice(renderDev);

    // Split the panel: monitors above, Spout senders below when there are any.
    int spoutCount = 0;
    for (auto& o : p->m_displayOutputs)
        if (o.config.type == DisplayOutputType::Spout) spoutCount++;

    RECT rcMon = rcAll;
    RECT rcSpout = rcAll;
    if (spoutCount > 0) {
        const int split = (int)((rcAll.bottom - rcAll.top) * (1.0f - kSpoutRowFraction));
        rcMon.bottom = rcAll.top + split;
        rcSpout.top = rcMon.bottom;
    } else {
        rcSpout.top = rcSpout.bottom;
    }

    // ── Monitors, to scale, in their desktop arrangement ──────────────────
    //
    // The virtual desktop's bounding box mapped into the panel with one
    // uniform scale, so relative size and position both survive: a 4K screen
    // beside a 1080p one looks like one, and a monitor above another is drawn
    // above it.
    RECT vd = { LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN };
    bool anyMon = false;
    for (auto& o : p->m_displayOutputs) {
        if (o.config.type != DisplayOutputType::Monitor) continue;
        const RECT& m = o.config.rcMonitor;
        if (m.right <= m.left || m.bottom <= m.top) continue;
        vd.left = min(vd.left, m.left);   vd.top = min(vd.top, m.top);
        vd.right = max(vd.right, m.right); vd.bottom = max(vd.bottom, m.bottom);
        anyMon = true;
    }

    if (anyMon) {
        const double vw = (double)(vd.right - vd.left);
        const double vh = (double)(vd.bottom - vd.top);
        const int availW = (rcMon.right - rcMon.left) - kPad * 2;
        const int availH = (rcMon.bottom - rcMon.top) - kPad * 2;
        const double scale = min(availW / vw, availH / vh);
        const int offX = rcMon.left + kPad + (int)((availW - vw * scale) / 2);
        const int offY = rcMon.top + kPad + (int)((availH - vh * scale) / 2);

        for (size_t i = 0; i < p->m_displayOutputs.size(); i++) {
            auto& out = p->m_displayOutputs[i];
            auto& cfg = out.config;
            if (cfg.type != DisplayOutputType::Monitor) continue;
            const RECT& m = cfg.rcMonitor;
            if (m.right <= m.left || m.bottom <= m.top) continue;

            RECT t;
            t.left   = offX + (int)((m.left - vd.left) * scale);
            t.top    = offY + (int)((m.top - vd.top) * scale);
            t.right  = offX + (int)((m.right - vd.left) * scale);
            t.bottom = offY + (int)((m.bottom - vd.top) * scale);
            // A hairline between adjacent screens, so two tiles do not read as
            // one wide one.
            t.right -= 2; t.bottom -= 2;

            const bool isRender = renderDev[0] &&
                                  wcscmp(renderDev, cfg.szDeviceName) == 0;
            ChildInstance ci;
            const bool haveChild = cfg.bOwnProcess &&
                                   p->GetChildInfo(cfg.szDeviceName, ci);

            // Colour IS the state. Nothing here needs a legend: dim means off,
            // green means it is running its own preset, blue means it mirrors,
            // red means a child failed, and the render window's own screen is
            // called out because it is the one that cannot be a child.
            COLORREF fill, edge, text = RGB(235, 235, 235);
            const wchar_t* state;
            if (isRender)                       { fill = RGB(58, 58, 72); edge = RGB(150, 150, 190); state = L"MAIN WINDOW"; }
            else if (!cfg.bEnabled)             { fill = RGB(38, 38, 38); edge = RGB(80, 80, 80);    state = L"OFF"; text = RGB(140, 140, 140); }
            else if (cfg.bOwnProcess && haveChild && ci.bFailed)
                                                { fill = RGB(90, 34, 34); edge = RGB(210, 90, 90);   state = L"FAILED"; }
            else if (cfg.bOwnProcess && haveChild && ci.bReady)
                                                { fill = RGB(28, 78, 40); edge = RGB(90, 210, 120);  state = L"OWN PRESET"; }
            else if (cfg.bOwnProcess)           { fill = RGB(70, 62, 26); edge = RGB(200, 180, 80);  state = L"STARTING"; }
            // Its own preset, in this process. Same green as a ready child --
            // it is showing its own preset, which is what the colour means --
            // and the label says where it renders. Without this a display set
            // to the middle tier still read MIRROR on the tile, which is the
            // first thing Shane checked and the reason it looked like the mode
            // had not taken.
            else if (cfg.bOwnPreset)            { fill = RGB(28, 78, 40); edge = RGB(90, 210, 120);  state = L"OWN PRESET"; }
            else                                { fill = RGB(30, 54, 82); edge = RGB(90, 150, 220);  state = L"MIRROR"; }

            wchar_t num[8] = L"";
            const int n = Engine::DisplayNumberFromDevice(cfg.szDeviceName);
            if (n > 0) FormatTo(num, L"%d", n);

            const wchar_t* detail = L"";
            if (haveChild && ci.szCurrentPreset[0]) {
                const wchar_t* slash = wcsrchr(ci.szCurrentPreset, L'\\');
                detail = slash ? slash + 1 : ci.szCurrentPreset;
            }

            DrawTile(hdc, t, fill, edge, text, (int)i == selected,
                     hBig, hSmall, num, state, detail);
            m_tiles.push_back({ t, (int)i });
        }
    }

    // ── Spout senders, on their own row ───────────────────────────────────
    //
    // Not placed on the desktop, because they are not on it. Each is drawn in
    // its configured aspect ratio -- for a sender the dimensions ARE the
    // geometry, and a row of identical boxes would say nothing.
    if (spoutCount > 0) {
        const int availW = (rcSpout.right - rcSpout.left) - kPad * 2;
        const int availH = (rcSpout.bottom - rcSpout.top) - kPad * 2;
        const int cellW = max(40, availW / spoutCount);
        int cx = rcSpout.left + kPad;

        for (size_t i = 0; i < p->m_displayOutputs.size(); i++) {
            auto& cfg = p->m_displayOutputs[i].config;
            if (cfg.type != DisplayOutputType::Spout) continue;

            int sw = cfg.nWidth > 0 ? cfg.nWidth : 1920;
            int sh = cfg.nHeight > 0 ? cfg.nHeight : 1080;
            double s = min((double)(cellW - kPad) / sw, (double)availH / sh);
            int tw = max(30, (int)(sw * s));
            int th = max(20, (int)(sh * s));

            RECT t;
            t.left = cx + (cellW - tw) / 2;
            t.top = rcSpout.top + kPad + (availH - th) / 2;
            t.right = t.left + tw;
            t.bottom = t.top + th;

            COLORREF fill = cfg.bEnabled ? RGB(64, 40, 74) : RGB(38, 38, 38);
            COLORREF edge = cfg.bEnabled ? RGB(180, 120, 210) : RGB(80, 80, 80);
            COLORREF text = cfg.bEnabled ? RGB(235, 235, 235) : RGB(140, 140, 140);

            wchar_t dims[32];
            FormatTo(dims, L"%dx%d", sw, sh);
            DrawTile(hdc, t, fill, edge, text, (int)i == selected,
                     hBig, hSmall, L"", cfg.szName[0] ? cfg.szName : L"Spout",
                     dims);
            m_tiles.push_back({ t, (int)i });
            cx += cellW;
        }
    }

    DeleteObject(hBig);
}

} // namespace mdrop
