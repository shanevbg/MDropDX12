// engine_board_ui.cpp — Button Board ToolWindow implementation.
//
// Standalone window hosting a configurable grid of buttons that trigger
// presets, sprites, script commands, or messages.

#include "engine.h"
#include "button_panel.h"
#include "utility.h"
#include <CommCtrl.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

namespace mdrop {

// ─── Open / Close (Engine methods) ──────────────────────────────────────

void Engine::OpenBoardWindow() {
    if (!m_boardWindow)
        m_boardWindow = std::make_unique<ButtonBoardWindow>(this);
    m_boardWindow->Open();
}

void Engine::CloseBoardWindow() {
    if (m_boardWindow)
        m_boardWindow->Close();
}

// ─── Constructor ────────────────────────────────────────────────────────

ButtonBoardWindow::ButtonBoardWindow(Engine* pEngine)
    : ToolWindow(pEngine, 480, 380)
{
}

// ─── Build Controls ─────────────────────────────────────────────────────

void ButtonBoardWindow::DoBuildControls() {
    HWND hw = m_hWnd;
    Engine* p = m_pEngine;
    RECT rcClient;
    GetClientRect(hw, &rcClient);

    auto L = BuildBaseControls();
    int x = L.x, y = L.y, rw = L.rw;
    HFONT hFont = GetFont();
    m_nTopY = y; // save for LayoutControls

    // Bank navigation row
    int btnW = 28, btnH = 22;
    m_hBankPrev = CreateBtn(hw, L"\x25C0", IDC_MW_BOARD_BANK_PREV, x, y, btnW, btnH, hFont);
    m_hBankLabel = CreateLabel(hw, L"Bank 1/3", x + btnW + 4, y, 80, btnH, hFont);
    m_hBankNext = CreateBtn(hw, L"\x25B6", IDC_MW_BOARD_BANK_NEXT, x + btnW + 88, y, btnW, btnH, hFont);
    m_hConfigBtn = CreateBtn(hw, L"Config", IDC_MW_BOARD_CONFIG, rcClient.right - 8 - 60, y, 60, btnH, hFont);
    y += btnH + 6;

    // Create the button panel filling the rest of the window
    m_pPanel = new ButtonPanel();

    // Load saved config from INI
    const wchar_t* ini = p->GetConfigIniFile();
    m_pPanel->LoadFromINI(ini, L"ButtonBoard");

    m_pPanel->Create(hw, p, IDC_MW_BOARD_PANEL, x, y,
                     rcClient.right - 16, rcClient.bottom - y - 4);

    // Apply theme colors
    if (p->IsDarkTheme()) {
        m_pPanel->SetThemeColors(
            p->m_colSettingsBg, p->m_colSettingsBtnFace,
            p->m_colSettingsBtnHi, p->m_colSettingsBtnShadow,
            p->m_colSettingsText, p->m_colSettingsCtrlBg);
    }
    m_pPanel->SetFont(hFont);

    // Set callbacks
    m_pPanel->OnLeftClick = [this](int gi) { ExecuteSlot(gi); };
    m_pPanel->OnRightClick = [this](int gi, POINT pt) { ShowSlotContextMenu(gi, pt); };

    UpdateBankLabel();
}

// ─── Layout on Resize ───────────────────────────────────────────────────

void ButtonBoardWindow::OnResize() {
    LayoutControls();
}

void ButtonBoardWindow::LayoutControls() {
    if (!m_hWnd || !m_pPanel) return;
    RECT rc;
    GetClientRect(m_hWnd, &rc);

    int x = 8;
    int y = m_nTopY; // below font +/- and pin
    int rw = rc.right - 16;
    int btnW = 28, btnH = 22;

    MoveWindow(m_hBankPrev, x, y, btnW, btnH, TRUE);
    MoveWindow(m_hBankLabel, x + btnW + 4, y, 80, btnH, TRUE);
    MoveWindow(m_hBankNext, x + btnW + 88, y, btnW, btnH, TRUE);
    MoveWindow(m_hConfigBtn, rc.right - 8 - 60, y, 60, btnH, TRUE);
    y += btnH + 6;

    m_pPanel->Reposition(x, y, rc.right - 16, rc.bottom - y - 4);
}

// ─── Commands ───────────────────────────────────────────────────────────

LRESULT ButtonBoardWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
    if (code != BN_CLICKED) return -1;

    switch (id) {
    case IDC_MW_BOARD_BANK_PREV:
        if (m_pPanel) {
            m_pPanel->PrevBank();
            UpdateBankLabel();
            SaveBoard();
        }
        return 0;

    case IDC_MW_BOARD_BANK_NEXT:
        if (m_pPanel) {
            m_pPanel->NextBank();
            UpdateBankLabel();
            SaveBoard();
        }
        return 0;

    case IDC_MW_BOARD_CONFIG:
        ShowConfigMenu();
        return 0;
    }

    return -1;
}

// ─── Destroy ────────────────────────────────────────────────────────────

void ButtonBoardWindow::DoDestroy() {
    if (m_pPanel) {
        SaveBoard();
        delete m_pPanel;
        m_pPanel = NULL;
    }
}

// ─── Bank Label ─────────────────────────────────────────────────────────

void ButtonBoardWindow::UpdateBankLabel() {
    if (!m_pPanel || !m_hBankLabel) return;
    wchar_t buf[32];
    swprintf(buf, 32, L"Bank %d/%d",
             m_pPanel->GetActiveBank() + 1,
             m_pPanel->Config().nBanks);
    SetWindowTextW(m_hBankLabel, buf);
}

// ─── Execute Slot Action ────────────────────────────────────────────────

void ButtonBoardWindow::ExecuteSlot(int globalIndex) {
    if (!m_pPanel || globalIndex < 0 || globalIndex >= m_pPanel->GetTotalSlots())
        return;

    const ButtonSlot& slot = m_pPanel->Slot(globalIndex);
    if (slot.action == ButtonAction::None)
        return;

    Engine* p = m_pEngine;
    HWND hRender = p->GetPluginWindow();
    if (!hRender) return;

    switch (slot.action) {
    case ButtonAction::LoadPreset: {
        // Send preset path via IPC message: "PRESET=<path>"
        std::wstring cmd = L"PRESET=" + slot.payload;
        wchar_t* copy = new wchar_t[cmd.size() + 1];
        wcscpy(copy, cmd.c_str());
        PostMessage(hRender, WM_MW_IPC_MESSAGE, 1, (LPARAM)copy);
        break;
    }
    case ButtonAction::PushSprite: {
        int sprNum = _wtoi(slot.payload.c_str());
        PostMessage(hRender, WM_MW_PUSH_SPRITE, sprNum, -1);
        break;
    }
    case ButtonAction::ScriptCommand: {
        // Send pipe-chained commands via IPC
        wchar_t* copy = new wchar_t[slot.payload.size() + 1];
        wcscpy(copy, slot.payload.c_str());
        PostMessage(hRender, WM_MW_IPC_MESSAGE, 1, (LPARAM)copy);
        break;
    }
    case ButtonAction::LaunchMessage: {
        // Full MSG|text=...|font=... string via IPC
        wchar_t* copy = new wchar_t[slot.payload.size() + 1];
        wcscpy(copy, slot.payload.c_str());
        PostMessage(hRender, WM_MW_IPC_MESSAGE, 1, (LPARAM)copy);
        break;
    }
    default:
        break;
    }
}

// ─── Right-Click Context Menu ───────────────────────────────────────────

void ButtonBoardWindow::ShowSlotContextMenu(int globalIndex, POINT screenPt) {
    if (!m_pPanel) return;

    const ButtonSlot& slot = m_pPanel->Slot(globalIndex);
    bool hasAction = (slot.action != ButtonAction::None);

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, 1, L"Assign Current Preset");
    AppendMenuW(hMenu, MF_STRING, 2, L"Set Script Command...");
    AppendMenuW(hMenu, MF_STRING, 3, L"Set Sprite...");
    AppendMenuW(hMenu, MF_STRING, 4, L"Set Message...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING | (hasAction ? 0 : MF_GRAYED), 5, L"Edit...");
    AppendMenuW(hMenu, MF_STRING | (hasAction ? 0 : MF_GRAYED), 6, L"Clear Slot");

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY,
                             screenPt.x, screenPt.y, 0, m_hWnd, NULL);
    DestroyMenu(hMenu);

    Engine* p = m_pEngine;
    ButtonSlot& s = m_pPanel->Slot(globalIndex);

    switch (cmd) {
    case 1: // Assign Current Preset
        s.action  = ButtonAction::LoadPreset;
        s.payload = p->m_szCurrentPresetFile;
        // Extract display name from path
        {
            const wchar_t* name = wcsrchr(s.payload.c_str(), L'\\');
            if (!name) name = wcsrchr(s.payload.c_str(), L'/');
            s.label = name ? name + 1 : s.payload;
            // Strip extension
            size_t dot = s.label.rfind(L'.');
            if (dot != std::wstring::npos) s.label.erase(dot);
        }
        m_pPanel->Invalidate();
        SaveBoard();
        break;

    case 2: // Script Command
        ShowSlotEditDialog(globalIndex);
        break;

    case 3: { // Sprite — show sprite list popup
        HMENU hSprMenu = CreatePopupMenu();
        for (int i = 0; i < (int)p->m_spriteEntries.size(); i++) {
            wchar_t buf[256];
            const wchar_t* imgName = wcsrchr(p->m_spriteEntries[i].szImg, L'\\');
            if (!imgName) imgName = wcsrchr(p->m_spriteEntries[i].szImg, L'/');
            imgName = imgName ? imgName + 1 : p->m_spriteEntries[i].szImg;
            swprintf(buf, 256, L"[%d] %s", p->m_spriteEntries[i].nIndex, imgName);
            AppendMenuW(hSprMenu, MF_STRING, 100 + i, buf);
        }
        int sprCmd = TrackPopupMenu(hSprMenu, TPM_RETURNCMD | TPM_NONOTIFY,
                                     screenPt.x, screenPt.y, 0, m_hWnd, NULL);
        DestroyMenu(hSprMenu);
        if (sprCmd >= 100) {
            int idx = sprCmd - 100;
            s.action  = ButtonAction::PushSprite;
            s.payload = std::to_wstring(p->m_spriteEntries[idx].nIndex);
            const wchar_t* imgName = wcsrchr(p->m_spriteEntries[idx].szImg, L'\\');
            if (!imgName) imgName = wcsrchr(p->m_spriteEntries[idx].szImg, L'/');
            s.label = imgName ? imgName + 1 : p->m_spriteEntries[idx].szImg;
            m_pPanel->Invalidate();
            SaveBoard();
        }
        break;
    }

    case 4: // Message — for now, open edit dialog
        s.action = ButtonAction::LaunchMessage;
        ShowSlotEditDialog(globalIndex);
        break;

    case 5: // Edit
        ShowSlotEditDialog(globalIndex);
        break;

    case 6: // Clear
        s.action = ButtonAction::None;
        s.payload.clear();
        s.label.clear();
        m_pPanel->ClearSlotThumbnail(globalIndex);
        m_pPanel->Invalidate();
        SaveBoard();
        break;
    }
}

// ─── Config Menu ────────────────────────────────────────────────────────

void ButtonBoardWindow::ShowConfigMenu() {
    if (!m_pPanel) return;
    auto& cfg = m_pPanel->Config();

    HMENU hMenu = CreatePopupMenu();
    HMENU hSize = CreatePopupMenu();
    AppendMenuW(hSize, MF_STRING | (cfg.nRows==3 && cfg.nCols==3 ? MF_CHECKED : 0), 10, L"3 x 3");
    AppendMenuW(hSize, MF_STRING | (cfg.nRows==3 && cfg.nCols==5 ? MF_CHECKED : 0), 11, L"3 x 5");
    AppendMenuW(hSize, MF_STRING | (cfg.nRows==4 && cfg.nCols==4 ? MF_CHECKED : 0), 12, L"4 x 4");
    AppendMenuW(hSize, MF_STRING | (cfg.nRows==5 && cfg.nCols==5 ? MF_CHECKED : 0), 13, L"5 x 5");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSize, L"Grid Size");

    HMENU hBanks = CreatePopupMenu();
    AppendMenuW(hBanks, MF_STRING | (cfg.nBanks==1 ? MF_CHECKED : 0), 20, L"1 Bank");
    AppendMenuW(hBanks, MF_STRING | (cfg.nBanks==2 ? MF_CHECKED : 0), 21, L"2 Banks");
    AppendMenuW(hBanks, MF_STRING | (cfg.nBanks==3 ? MF_CHECKED : 0), 22, L"3 Banks");
    AppendMenuW(hBanks, MF_STRING | (cfg.nBanks==5 ? MF_CHECKED : 0), 23, L"5 Banks");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hBanks, L"Banks");

    RECT rc;
    GetWindowRect(m_hConfigBtn, &rc);
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY,
                             rc.left, rc.bottom, 0, m_hWnd, NULL);
    DestroyMenu(hMenu);

    bool changed = false;
    switch (cmd) {
    case 10: cfg.nRows = 3; cfg.nCols = 3; changed = true; break;
    case 11: cfg.nRows = 3; cfg.nCols = 5; changed = true; break;
    case 12: cfg.nRows = 4; cfg.nCols = 4; changed = true; break;
    case 13: cfg.nRows = 5; cfg.nCols = 5; changed = true; break;
    case 20: cfg.nBanks = 1; changed = true; break;
    case 21: cfg.nBanks = 2; changed = true; break;
    case 22: cfg.nBanks = 3; changed = true; break;
    case 23: cfg.nBanks = 5; changed = true; break;
    }

    if (changed) {
        if (cfg.nActiveBank >= cfg.nBanks)
            cfg.nActiveBank = 0;
        m_pPanel->ApplyConfig();
        UpdateBankLabel();
        SaveBoard();
    }
}

// ─── Slot Edit Dialog ───────────────────────────────────────────────────

void ButtonBoardWindow::ShowSlotEditDialog(int globalIndex) {
    if (!m_pPanel || globalIndex < 0) return;

    ButtonSlot& slot = m_pPanel->Slot(globalIndex);

    // Build in-memory dialog template
    struct DlgData {
        ButtonSlot* pSlot;
        Engine* pEngine;
        bool accepted;
    } data = { &slot, m_pEngine, false };

    // Simple in-memory DLGTEMPLATE
    __declspec(align(4)) BYTE buf[1024] = {};
    DLGTEMPLATE* pDlg = (DLGTEMPLATE*)buf;
    pDlg->style = DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER;
    pDlg->cx = 220;
    pDlg->cy = 130;
    // Title embedded after DLGTEMPLATE (menu=0, class=0, title)
    WORD* pw = (WORD*)(pDlg + 1);
    *pw++ = 0; // menu
    *pw++ = 0; // class
    const wchar_t* title = L"Edit Button Slot";
    while (*title) *pw++ = *title++;
    *pw++ = 0;

    INT_PTR result = DialogBoxIndirectParamW(
        NULL, pDlg, m_hWnd,
        [](HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) -> INT_PTR {
            DlgData* d = (DlgData*)GetWindowLongPtrW(hDlg, GWLP_USERDATA);

            switch (msg) {
            case WM_INITDIALOG: {
                d = (DlgData*)lParam;
                SetWindowLongPtrW(hDlg, GWLP_USERDATA, (LONG_PTR)d);

                // Apply dark theme
                Engine* p = d->pEngine;
                if (p->IsDarkTheme()) {
                    BOOL useDark = TRUE;
                    DwmSetWindowAttribute(hDlg, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &useDark, sizeof(useDark));
                }

                HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                int y = 10, x = 10, w = 300, lineH = 22;

                // Action type combo
                CreateWindowExW(0, L"STATIC", L"Action Type:", WS_CHILD | WS_VISIBLE,
                    x, y, 80, lineH, hDlg, NULL, NULL, NULL);
                HWND hType = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
                    WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                    x + 85, y, 200, 200, hDlg, (HMENU)100, NULL, NULL);
                SendMessageW(hType, WM_SETFONT, (WPARAM)hFont, TRUE);
                SendMessageW(hType, CB_ADDSTRING, 0, (LPARAM)L"None");
                SendMessageW(hType, CB_ADDSTRING, 0, (LPARAM)L"Load Preset");
                SendMessageW(hType, CB_ADDSTRING, 0, (LPARAM)L"Push Sprite");
                SendMessageW(hType, CB_ADDSTRING, 0, (LPARAM)L"Script Command");
                SendMessageW(hType, CB_ADDSTRING, 0, (LPARAM)L"Launch Message");
                SendMessageW(hType, CB_SETCURSEL, (int)d->pSlot->action, 0);
                y += lineH + 6;

                // Label
                CreateWindowExW(0, L"STATIC", L"Label:", WS_CHILD | WS_VISIBLE,
                    x, y, 80, lineH, hDlg, NULL, NULL, NULL);
                HWND hLabel = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", d->pSlot->label.c_str(),
                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                    x + 85, y, 200, lineH, hDlg, (HMENU)101, NULL, NULL);
                SendMessageW(hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
                y += lineH + 6;

                // Payload (multiline for script commands)
                CreateWindowExW(0, L"STATIC", L"Payload:", WS_CHILD | WS_VISIBLE,
                    x, y, 80, lineH, hDlg, NULL, NULL, NULL);

                // Convert pipes to newlines for display
                std::wstring displayPayload = d->pSlot->payload;
                for (size_t pos = 0; (pos = displayPayload.find(L'|', pos)) != std::wstring::npos; pos += 2)
                    displayPayload.replace(pos, 1, L"\r\n");

                HWND hPayload = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", displayPayload.c_str(),
                    WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL,
                    x + 85, y, 200, lineH * 3, hDlg, (HMENU)102, NULL, NULL);
                SendMessageW(hPayload, WM_SETFONT, (WPARAM)hFont, TRUE);
                y += lineH * 3 + 10;

                // OK / Cancel
                HWND hOK = CreateWindowExW(0, L"BUTTON", L"OK",
                    WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                    x + 85, y, 75, 26, hDlg, (HMENU)IDOK, NULL, NULL);
                HWND hCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    x + 170, y, 75, 26, hDlg, (HMENU)IDCANCEL, NULL, NULL);
                SendMessageW(hOK, WM_SETFONT, (WPARAM)hFont, TRUE);
                SendMessageW(hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

                // Size the dialog to fit
                RECT rcDlg = { 0, 0, x + 295, y + 36 };
                AdjustWindowRectEx(&rcDlg, (DWORD)GetWindowLongPtrW(hDlg, GWL_STYLE), FALSE,
                                   (DWORD)GetWindowLongPtrW(hDlg, GWL_EXSTYLE));
                SetWindowPos(hDlg, NULL, 0, 0,
                    rcDlg.right - rcDlg.left, rcDlg.bottom - rcDlg.top,
                    SWP_NOMOVE | SWP_NOZORDER);

                // Dark theme for child controls
                if (p->IsDarkTheme()) {
                    auto setDark = [&](HWND h) {
                        SendMessageW(h, WM_SETFONT, (WPARAM)hFont, TRUE);
                    };
                    setDark(hType);
                    setDark(hLabel);
                    setDark(hPayload);
                }

                return TRUE;
            }

            case WM_COMMAND:
                if (LOWORD(wParam) == IDOK) {
                    // Read back values
                    HWND hType = GetDlgItem(hDlg, 100);
                    HWND hLabel = GetDlgItem(hDlg, 101);
                    HWND hPayload = GetDlgItem(hDlg, 102);

                    int sel = (int)SendMessageW(hType, CB_GETCURSEL, 0, 0);
                    d->pSlot->action = (ButtonAction)sel;

                    // Read label
                    int labelLen = GetWindowTextLengthW(hLabel);
                    std::vector<wchar_t> labelBuf(labelLen + 1);
                    GetWindowTextW(hLabel, labelBuf.data(), labelLen + 1);
                    d->pSlot->label = labelBuf.data();

                    // Read payload (multiline → convert newlines to pipes)
                    int payLen = GetWindowTextLengthW(hPayload);
                    std::vector<wchar_t> payBuf(payLen + 1);
                    GetWindowTextW(hPayload, payBuf.data(), payLen + 1);
                    std::wstring payload(payBuf.data());
                    // Replace \r\n with |
                    size_t pos = 0;
                    while ((pos = payload.find(L"\r\n", pos)) != std::wstring::npos)
                        payload.replace(pos, 2, L"|");
                    // Also replace bare \n
                    pos = 0;
                    while ((pos = payload.find(L'\n', pos)) != std::wstring::npos)
                        payload.replace(pos, 1, L"|");
                    d->pSlot->payload = payload;

                    d->accepted = true;
                    EndDialog(hDlg, IDOK);
                    return TRUE;
                }
                if (LOWORD(wParam) == IDCANCEL) {
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;
                }
                break;

            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX:
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORDLG:
                if (d && d->pEngine && d->pEngine->IsDarkTheme()) {
                    HDC hdc = (HDC)wParam;
                    SetTextColor(hdc, d->pEngine->m_colSettingsText);
                    SetBkColor(hdc, d->pEngine->m_colSettingsCtrlBg);
                    static HBRUSH hBrDark = NULL;
                    if (!hBrDark) hBrDark = CreateSolidBrush(d->pEngine->m_colSettingsCtrlBg);
                    if (msg == WM_CTLCOLORDLG) {
                        static HBRUSH hBrBg = NULL;
                        if (!hBrBg) hBrBg = CreateSolidBrush(d->pEngine->m_colSettingsBg);
                        return (INT_PTR)hBrBg;
                    }
                    return (INT_PTR)hBrDark;
                }
                break;
            }
            return FALSE;
        },
        (LPARAM)&data);

    if (data.accepted) {
        m_pPanel->Invalidate();
        SaveBoard();
    }
}

// ─── Save ───────────────────────────────────────────────────────────────

void ButtonBoardWindow::SaveBoard() {
    if (!m_pPanel || !m_pEngine) return;
    const wchar_t* ini = m_pEngine->GetConfigIniFile();
    m_pPanel->SaveToINI(ini, L"ButtonBoard");
}

} // namespace mdrop
