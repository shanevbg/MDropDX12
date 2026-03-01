// engine_controller.cpp — Game controller support
//
// Polls game controllers via winmm.dll (joyGetPosEx) on a 50ms timer,
// maps button presses to visualizer commands via a JSON config file.

#include "engine.h"
#include "utility.h"
#include "render_commands.h"
#include <mmsystem.h>
#include <dwmapi.h>
#include <fstream>
#include <sstream>

namespace mdrop {

// ---------------------------------------------------------------------------
// GetDefaultControllerJSON — returns the default button mapping JSON
// ---------------------------------------------------------------------------
std::string Engine::GetDefaultControllerJSON()
{
    return
        "// Button-to-command mapping for game controllers\r\n"
        "// Commands: NEXT, PREV, LOCK, RAND, HARDCUT, MASHUP,\r\n"
        "//           FULLSCREEN, STRETCH, MIRROR, RESET,\r\n"
        "//           PRESETINFO, SETTINGS, SEND=<vk>\r\n"
        "// Example values for a DualSense controller:\r\n"
        "{\r\n"
        "  \"1\": \"NEXT\",\r\n"
        "  \"2\": \"PREV\",\r\n"
        "  \"3\": \"LOCK\",\r\n"
        "  \"4\": \"RAND\",\r\n"
        "  \"5\": \"PREV\",\r\n"
        "  \"6\": \"NEXT\",\r\n"
        "  \"7\": \"\",\r\n"
        "  \"8\": \"\",\r\n"
        "  \"9\": \"PRESETINFO\",\r\n"
        "  \"10\": \"SETTINGS\",\r\n"
        "  \"11\": \"FULLSCREEN\",\r\n"
        "  \"12\": \"HARDCUT\",\r\n"
        "  \"13\": \"MASHUP\",\r\n"
        "  \"14\": \"\"\r\n"
        "}\r\n";
}

// ---------------------------------------------------------------------------
// ParseControllerJSON — parse flat {"key": "value"} JSON into config map
// ---------------------------------------------------------------------------
void Engine::ParseControllerJSON(const std::string& jsonText)
{
    m_controllerConfig.clear();

    // Strip comment lines and parse simple key-value JSON
    std::istringstream stream(jsonText);
    std::string line;
    while (std::getline(stream, line)) {
        // Trim leading whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        // Skip comment lines
        if (line.size() >= 2 && line[0] == '/' && line[1] == '/') continue;
        // Skip braces
        if (line[0] == '{' || line[0] == '}') continue;

        // Parse "key": "value" pattern
        size_t q1 = line.find('"');
        if (q1 == std::string::npos) continue;
        size_t q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;
        std::string key = line.substr(q1 + 1, q2 - q1 - 1);

        size_t q3 = line.find('"', q2 + 1);
        if (q3 == std::string::npos) continue;
        size_t q4 = line.find('"', q3 + 1);
        if (q4 == std::string::npos) continue;
        std::string value = line.substr(q3 + 1, q4 - q3 - 1);

        int buttonNum = 0;
        try { buttonNum = std::stoi(key); } catch (...) { continue; }
        if (buttonNum > 0 && buttonNum <= 32 && !value.empty()) {
            m_controllerConfig[buttonNum] = value;
        }
    }
}

// ---------------------------------------------------------------------------
// LoadControllerJSON — read controller.json from disk
// ---------------------------------------------------------------------------
void Engine::LoadControllerJSON()
{
    wchar_t szPath[MAX_PATH];
    swprintf(szPath, MAX_PATH, L"%scontroller.json", m_szBaseDir);

    std::ifstream file(szPath);
    if (!file.is_open()) {
        // No file on disk — use defaults
        m_szControllerJSONText = GetDefaultControllerJSON();
        ParseControllerJSON(m_szControllerJSONText);
        return;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    m_szControllerJSONText = ss.str();
    file.close();

    ParseControllerJSON(m_szControllerJSONText);
}

// ---------------------------------------------------------------------------
// SaveControllerJSON — write JSON text to controller.json
// ---------------------------------------------------------------------------
void Engine::SaveControllerJSON(const std::string& jsonText)
{
    m_szControllerJSONText = jsonText;
    ParseControllerJSON(jsonText);

    wchar_t szPath[MAX_PATH];
    swprintf(szPath, MAX_PATH, L"%scontroller.json", m_szBaseDir);

    std::ofstream file(szPath);
    if (file.is_open()) {
        file << jsonText;
        file.close();
    }
}

// ---------------------------------------------------------------------------
// LoadControllerSettings / SaveControllerSettings — INI persistence
// ---------------------------------------------------------------------------
void Engine::LoadControllerSettings()
{
    wchar_t* pIni = GetConfigIniFile();
    m_bControllerEnabled = GetPrivateProfileIntW(L"Controller", L"Enabled", 0, pIni) != 0;
    m_nControllerDeviceID = GetPrivateProfileIntW(L"Controller", L"DeviceID", -1, pIni);
    GetPrivateProfileStringW(L"Controller", L"DeviceName", L"", m_szControllerName, 256, pIni);
}

void Engine::SaveControllerSettings()
{
    wchar_t* pIni = GetConfigIniFile();
    wchar_t buf[32];

    WritePrivateProfileStringW(L"Controller", L"Enabled",
        m_bControllerEnabled ? L"1" : L"0", pIni);
    swprintf(buf, 32, L"%d", m_nControllerDeviceID);
    WritePrivateProfileStringW(L"Controller", L"DeviceID", buf, pIni);
    WritePrivateProfileStringW(L"Controller", L"DeviceName", m_szControllerName, pIni);
}

// ---------------------------------------------------------------------------
// EnumerateControllers — populate a combo box with available controllers
// ---------------------------------------------------------------------------
void Engine::EnumerateControllers(HWND hCombo)
{
    if (!hCombo) return;

    SendMessage(hCombo, CB_RESETCONTENT, 0, 0);

    UINT numDevs = joyGetNumDevs();
    int found = 0;

    for (UINT id = 0; id < numDevs && id < 16; id++) {
        JOYCAPSW caps = {};
        if (joyGetDevCapsW(id, &caps, sizeof(caps)) == JOYERR_NOERROR) {
            // Verify the device is actually connected by trying to get its state
            JOYINFOEX info = {};
            info.dwSize = sizeof(info);
            info.dwFlags = JOY_RETURNBUTTONS;
            if (joyGetPosEx(id, &info) == JOYERR_NOERROR) {
                int idx = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)caps.szPname);
                SendMessage(hCombo, CB_SETITEMDATA, idx, (LPARAM)id);
                found++;
            }
        }
    }

    if (found == 0) {
        int idx = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"(No controllers found)");
        SendMessage(hCombo, CB_SETITEMDATA, idx, (LPARAM)-1);
    }

    // Try to select the saved controller by name
    if (m_szControllerName[0]) {
        int count = (int)SendMessage(hCombo, CB_GETCOUNT, 0, 0);
        for (int i = 0; i < count; i++) {
            wchar_t name[256];
            SendMessageW(hCombo, CB_GETLBTEXT, i, (LPARAM)name);
            if (_wcsicmp(name, m_szControllerName) == 0) {
                SendMessage(hCombo, CB_SETCURSEL, i, 0);
                m_nControllerDeviceID = (int)SendMessage(hCombo, CB_GETITEMDATA, i, 0);
                return;
            }
        }
    }

    // If saved name not found, select by saved device ID
    if (m_nControllerDeviceID >= 0) {
        int count = (int)SendMessage(hCombo, CB_GETCOUNT, 0, 0);
        for (int i = 0; i < count; i++) {
            if ((int)SendMessage(hCombo, CB_GETITEMDATA, i, 0) == m_nControllerDeviceID) {
                SendMessage(hCombo, CB_SETCURSEL, i, 0);
                return;
            }
        }
    }

    // Default to first item
    SendMessage(hCombo, CB_SETCURSEL, 0, 0);
    if (found > 0)
        m_nControllerDeviceID = (int)SendMessage(hCombo, CB_GETITEMDATA, 0, 0);
}

// ---------------------------------------------------------------------------
// PollController — called from WM_TIMER on the message pump thread
// ---------------------------------------------------------------------------
void Engine::PollController()
{
    if (!m_bControllerEnabled || m_nControllerDeviceID < 0)
        return;

    JOYINFOEX info = {};
    info.dwSize = sizeof(info);
    info.dwFlags = JOY_RETURNBUTTONS;

    if (joyGetPosEx(m_nControllerDeviceID, &info) != JOYERR_NOERROR) {
        // Controller disconnected or error — skip silently
        return;
    }

    DWORD changed = info.dwButtons & ~m_dwLastControllerButtons; // rising edges
    m_dwLastControllerButtons = info.dwButtons;

    if (changed == 0) return;

    for (int bit = 0; bit < 32; bit++) {
        if (changed & (1u << bit)) {
            int buttonNum = bit + 1; // buttons are 1-based in JSON
            auto it = m_controllerConfig.find(buttonNum);
            if (it != m_controllerConfig.end()) {
                ExecuteControllerCommand(it->second);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// ExecuteControllerCommand — dispatch a command string to an action
// ---------------------------------------------------------------------------
void Engine::ExecuteControllerCommand(const std::string& cmdRaw)
{
    if (cmdRaw.empty()) return;

    // Trim leading/trailing whitespace and convert to uppercase
    std::string cmd = cmdRaw;
    size_t start = cmd.find_first_not_of(" \t\r\n");
    size_t end = cmd.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return;
    cmd = cmd.substr(start, end - start + 1);
    for (auto& c : cmd) c = (char)toupper((unsigned char)c);

    HWND hwnd = GetPluginWindow();

    if (cmd == "NEXT") {
        RenderCommand rc;
        rc.cmd = RenderCmd::NextPreset;
        rc.fParam = m_fBlendTimeUser;
        EnqueueRenderCmd(std::move(rc));
    }
    else if (cmd == "PREV") {
        RenderCommand rc;
        rc.cmd = RenderCmd::PrevPreset;
        rc.fParam = m_fBlendTimeUser;
        EnqueueRenderCmd(std::move(rc));
    }
    else if (cmd == "HARDCUT") {
        RenderCommand rc;
        rc.cmd = RenderCmd::NextPreset;
        rc.fParam = 0.0f;
        EnqueueRenderCmd(std::move(rc));
    }
    else if (cmd == "LOCK") {
        m_bPresetLockedByUser = !m_bPresetLockedByUser;
        AddNotification(m_bPresetLockedByUser ? L"Preset locked" : L"Preset unlocked");
    }
    else if (cmd == "RAND") {
        m_bSequentialPresetOrder = !m_bSequentialPresetOrder;
        AddNotification(m_bSequentialPresetOrder ? L"Sequential order" : L"Random order");
    }
    else if (cmd == "MASHUP") {
        if (hwnd) PostMessage(hwnd, WM_KEYDOWN, 'A', 0);
    }
    else if (cmd == "FULLSCREEN") {
        if (hwnd) PostMessage(hwnd, WM_SYSKEYDOWN, VK_RETURN, (1 << 29));
    }
    else if (cmd == "PRESETINFO") {
        m_bShowPresetInfo = !m_bShowPresetInfo;
    }
    else if (cmd == "SETTINGS") {
        if (hwnd) PostMessage(hwnd, WM_KEYDOWN, VK_F8, 0);
    }
    else if (cmd == "STRETCH") {
        if (hwnd) PostMessage(hwnd, WM_MW_TOGGLE_STRETCH_MODE, 0, 0);
    }
    else if (cmd == "MIRROR") {
        if (hwnd) PostMessage(hwnd, WM_MW_TOGGLE_MIRROR_MODE, 0, 0);
    }
    else if (cmd == "RESET") {
        if (hwnd) PostMessage(hwnd, WM_MW_RESET_WINDOW, 0, 0);
    }
    else if (cmd.substr(0, 5) == "SEND=") {
        std::string val = cmd.substr(5);
        int vk = 0;
        try {
            if (val.size() > 2 && val[0] == '0' && (val[1] == 'x' || val[1] == 'X'))
                vk = std::stoi(val, nullptr, 16);
            else
                vk = std::stoi(val);
        } catch (...) { return; }
        if (vk > 0 && hwnd)
            PostMessage(hwnd, WM_KEYDOWN, (WPARAM)vk, 0);
    }
}

// ---------------------------------------------------------------------------
// Controller Help Popup — custom-painted non-blocking window
// ---------------------------------------------------------------------------
struct ControllerHelpData {
    bool darkTheme;
    int  fontSize;  // base font size from settings window
};

static LRESULT CALLBACK ControllerHelpWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    ControllerHelpData* data = (ControllerHelpData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);

    switch (uMsg) {
    case WM_CREATE: {
        CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { DestroyWindow(hWnd); return 0; }
        break;
    case WM_PAINT: {
        if (!data) break;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);

        bool dark = data->darkTheme;

        // Colors
        COLORREF clrBg       = dark ? RGB(30, 30, 30)    : RGB(255, 255, 255);
        COLORREF clrText     = dark ? RGB(220, 220, 220)  : RGB(30, 30, 30);
        COLORREF clrTitle    = dark ? RGB(80, 200, 255)   : RGB(0, 90, 180);
        COLORREF clrHeader   = dark ? RGB(0, 200, 120)    : RGB(0, 130, 80);
        COLORREF clrRowAlt   = dark ? RGB(40, 40, 40)     : RGB(240, 245, 250);
        COLORREF clrRowNorm  = clrBg;
        COLORREF clrGrid     = dark ? RGB(60, 60, 60)     : RGB(200, 200, 200);
        COLORREF clrAccent   = dark ? RGB(255, 200, 80)   : RGB(180, 100, 0);
        COLORREF clrDimText  = dark ? RGB(140, 140, 140)  : RGB(120, 120, 120);
        COLORREF clrWarnBg   = dark ? RGB(50, 35, 20)     : RGB(255, 248, 230);
        COLORREF clrWarnBdr  = dark ? RGB(180, 130, 50)   : RGB(200, 160, 60);

        // Fill background
        HBRUSH hBrBg = CreateSolidBrush(clrBg);
        FillRect(hdc, &rcClient, hBrBg);
        DeleteObject(hBrBg);

        SetBkMode(hdc, TRANSPARENT);

        // Font sizes scale from settings window base font size
        int fs = data->fontSize > 0 ? data->fontSize : 16;
        int fsTitle = MulDiv(fs, 140, 100);  // 140% of base
        int fsHeader = MulDiv(fs, 110, 100); // 110% of base
        int fsBody = fs;
        int fsSmall = MulDiv(fs, 90, 100);   // 90% of base

        HFONT hFontTitle = CreateFontW(-fsTitle, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT hFontHeader = CreateFontW(-fsHeader, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT hFontBody = CreateFontW(-fsBody, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT hFontSmall = CreateFontW(-fsSmall, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

        int pad = MulDiv(20, fs, 16);
        int y = pad;
        int contentW = rcClient.right - 2 * pad;
        int titleH = fsTitle + 6;
        int rowH = fsBody + 10;

        // Title
        HFONT hOld = (HFONT)SelectObject(hdc, hFontTitle);
        SetTextColor(hdc, clrTitle);
        RECT rcTitle = { pad, y, rcClient.right - pad, y + titleH };
        DrawTextW(hdc, L"Xbox Wireless Controller \u2014 Button Map", -1, &rcTitle, DT_LEFT | DT_SINGLELINE);
        y += titleH + 6;

        // Subtitle
        SelectObject(hdc, hFontSmall);
        SetTextColor(hdc, clrDimText);
        RECT rcSub = { pad, y, rcClient.right - pad, y + fsSmall + 4 };
        DrawTextW(hdc, L"Model 1797  \u2022  via Windows Multimedia API (winmm.dll / joyGetPosEx)", -1, &rcSub, DT_LEFT | DT_SINGLELINE);
        y += fsSmall + 10;

        // Separator line
        HPEN hPenGrid = CreatePen(PS_SOLID, 1, clrGrid);
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPenGrid);
        MoveToEx(hdc, pad, y, NULL);
        LineTo(hdc, rcClient.right - pad, y);
        y += 8;

        // Table header
        int colBtn = pad;
        int colBtnW = MulDiv(80, fs, 16);
        int colCtrl = colBtn + colBtnW;
        int colCtrlW = contentW - colBtnW;

        SelectObject(hdc, hFontHeader);
        SetTextColor(hdc, clrHeader);
        RECT rcH1 = { colBtn, y, colBtn + colBtnW, y + rowH };
        RECT rcH2 = { colCtrl, y, colCtrl + colCtrlW, y + rowH };
        DrawTextW(hdc, L"Button #", -1, &rcH1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(hdc, L"Xbox Control", -1, &rcH2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += rowH;

        // Separator under header
        MoveToEx(hdc, pad, y, NULL);
        LineTo(hdc, rcClient.right - pad, y);
        y += 2;

        // Table rows
        struct BtnRow { const wchar_t* num; const wchar_t* label; };
        BtnRow rows[] = {
            { L"1",  L"A" },
            { L"2",  L"B" },
            { L"3",  L"X" },
            { L"4",  L"Y" },
            { L"5",  L"LB  (Left Bumper)" },
            { L"6",  L"RB  (Right Bumper)" },
            { L"7",  L"Back / View  \u29C9" },
            { L"8",  L"Start / Menu  \u2630" },
            { L"9",  L"Left Stick Press  (L3)" },
            { L"10", L"Right Stick Press  (R3)" },
        };

        SelectObject(hdc, hFontBody);
        for (int i = 0; i < 10; i++) {
            COLORREF rowBg = (i % 2) ? clrRowAlt : clrRowNorm;
            HBRUSH hBrRow = CreateSolidBrush(rowBg);
            RECT rcRow = { pad, y, rcClient.right - pad, y + rowH };
            FillRect(hdc, &rcRow, hBrRow);
            DeleteObject(hBrRow);

            // Button number (accent color)
            SetTextColor(hdc, clrAccent);
            RECT rc1 = { colBtn + 8, y, colBtn + colBtnW, y + rowH };
            DrawTextW(hdc, rows[i].num, -1, &rc1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Control name
            SetTextColor(hdc, clrText);
            RECT rc2 = { colCtrl, y, colCtrl + colCtrlW, y + rowH };
            DrawTextW(hdc, rows[i].label, -1, &rc2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            y += rowH;
        }

        // Bottom separator
        MoveToEx(hdc, pad, y, NULL);
        LineTo(hdc, rcClient.right - pad, y);
        y += 12;

        // Warning box: "Not Available" section
        int warnLineH = fsSmall + 4;
        int boxH = 8 + (fsHeader + 6) + warnLineH * 3 + 8;
        RECT rcBox = { pad, y, rcClient.right - pad, y + boxH };
        HBRUSH hBrWarn = CreateSolidBrush(clrWarnBg);
        FillRect(hdc, &rcBox, hBrWarn);
        DeleteObject(hBrWarn);
        HPEN hPenWarn = CreatePen(PS_SOLID, 1, clrWarnBdr);
        SelectObject(hdc, hPenWarn);
        HBRUSH hBrNull = (HBRUSH)GetStockObject(NULL_BRUSH);
        SelectObject(hdc, hBrNull);
        Rectangle(hdc, rcBox.left, rcBox.top, rcBox.right, rcBox.bottom);

        int bx = pad + 12;
        int by = y + 8;

        SelectObject(hdc, hFontHeader);
        SetTextColor(hdc, clrAccent);
        RECT rcWarnTitle = { bx, by, rcBox.right - 8, by + fsHeader + 4 };
        DrawTextW(hdc, L"\u26A0  Not Available as Buttons", -1, &rcWarnTitle, DT_LEFT | DT_SINGLELINE);
        by += fsHeader + 8;

        SelectObject(hdc, hFontSmall);
        SetTextColor(hdc, clrText);
        RECT rcW1 = { bx, by, rcBox.right - 8, by + warnLineH };
        DrawTextW(hdc, L"D-Pad \u2014 Reported as POV hat (dwPOV), not discrete buttons", -1, &rcW1, DT_LEFT | DT_SINGLELINE);
        by += warnLineH;
        RECT rcW2 = { bx, by, rcBox.right - 8, by + warnLineH };
        DrawTextW(hdc, L"LT / RT Triggers \u2014 Reported as analog axes (dwZpos), not buttons", -1, &rcW2, DT_LEFT | DT_SINGLELINE);
        by += warnLineH;
        RECT rcW3 = { bx, by, rcBox.right - 8, by + warnLineH };
        DrawTextW(hdc, L"Xbox Button \u2014 Not exposed through the winmm API", -1, &rcW3, DT_LEFT | DT_SINGLELINE);

        y += boxH + 12;

        // Footer tip
        SelectObject(hdc, hFontSmall);
        SetTextColor(hdc, clrDimText);
        RECT rcFoot = { pad, y, rcClient.right - pad, y + warnLineH };
        DrawTextW(hdc, L"Tip: Button numbers may vary by controller. Use a gamepad tester to verify.", -1, &rcFoot, DT_LEFT | DT_SINGLELINE);

        // Cleanup
        SelectObject(hdc, hOldPen);
        SelectObject(hdc, hOld);
        DeleteObject(hPenGrid);
        DeleteObject(hPenWarn);
        DeleteObject(hFontTitle);
        DeleteObject(hFontHeader);
        DeleteObject(hFontBody);
        DeleteObject(hFontSmall);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // We handle background in WM_PAINT
    case WM_DESTROY:
        if (data) delete data;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

void Engine::ShowControllerHelpPopup(HWND hParent)
{
    static bool classRegistered = false;
    static const wchar_t* className = L"MDropControllerHelp";

    if (!classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ControllerHelpWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = className;
        RegisterClassExW(&wc);
        classRegistered = true;
    }

    // Close any existing help popup
    HWND hExisting = FindWindowW(className, NULL);
    if (hExisting) { DestroyWindow(hExisting); }

    ControllerHelpData* data = new ControllerHelpData();
    data->darkTheme = m_bSettingsDarkTheme;
    data->fontSize = abs(m_nSettingsFontSize);
    if (data->fontSize < 12) data->fontSize = 16;

    // Scale window size relative to font size
    int fs = data->fontSize;
    int winW = MulDiv(520, fs, 16);
    int winH = MulDiv(560, fs, 16);

    // Position near parent window
    RECT rcParent = {};
    if (hParent) GetWindowRect(hParent, &rcParent);
    int px = rcParent.right + 8;
    int py = rcParent.top;

    // Ensure it fits on screen
    HMONITOR hMon = MonitorFromWindow(hParent, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(hMon, &mi);
    if (px + winW > mi.rcWork.right) px = rcParent.left - winW - 8;
    if (py + winH > mi.rcWork.bottom) py = mi.rcWork.bottom - winH;
    if (px < mi.rcWork.left) px = mi.rcWork.left;
    if (py < mi.rcWork.top) py = mi.rcWork.top;

    HWND hPopup = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        className,
        L"Controller Button Map",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        px, py, winW, winH,
        hParent, NULL, GetModuleHandle(NULL), data);

    if (hPopup) {
        // Apply dark mode title bar
        BOOL bDark = m_bSettingsDarkTheme ? TRUE : FALSE;
        DwmSetWindowAttribute(hPopup, 20, &bDark, sizeof(bDark));
        if (m_bSettingsDarkTheme) {
            DwmSetWindowAttribute(hPopup, 35, &m_colSettingsBg, sizeof(m_colSettingsBg));
            DwmSetWindowAttribute(hPopup, 34, &m_colSettingsBorder, sizeof(m_colSettingsBorder));
            DwmSetWindowAttribute(hPopup, 36, &m_colSettingsText, sizeof(m_colSettingsText));
        }
        ShowWindow(hPopup, SW_SHOWNORMAL);
    }
}

} // namespace mdrop
