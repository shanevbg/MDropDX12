// engine_messages_ui.cpp — MessagesWindow ToolWindow implementation
// Extracted from Settings page 5 (Messages tab)

#include "engine.h"
#include "tool_window.h"
#include <shellapi.h>

using namespace mdrop;

// ─────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────

MessagesWindow::MessagesWindow(Engine* pEngine)
    : ToolWindow(pEngine, 500, 550) {}

// ─────────────────────────────────────────────────────
// Engine bridge: Open / Close
// ─────────────────────────────────────────────────────

void Engine::OpenMessagesWindow() {
    if (!m_messagesWindow)
        m_messagesWindow = std::make_unique<MessagesWindow>(this);
    m_messagesWindow->Open();
}

void Engine::CloseMessagesWindow() {
    if (m_messagesWindow) {
        m_messagesWindow->Close();
        m_messagesWindow.reset();
    }
}

// ─────────────────────────────────────────────────────
// Build Controls
// ─────────────────────────────────────────────────────

void MessagesWindow::DoBuildControls() {
    HWND hw = m_hWnd;
    Engine* p = m_pEngine;

    auto L = BuildBaseControls();
    int x = L.x, y = L.y, rw = L.rw;
    HFONT hFont = GetFont();
    HFONT hFontBold = GetFontBold();
    m_nTopY = y;

    int lineH = GetLineHeight();
    int gap = 4;
    wchar_t buf[64];

    // Title
    CreateLabel(hw, L"Custom Messages:", x, y, rw, lineH, hFontBold, false);
    y += lineH + 2;

    // Show Messages / Show Sprites toggles
    {
        int halfW = rw / 2 - 2;
        CreateCheck(hw, L"Show Messages", IDC_MW_MSG_SHOW_MESSAGES, x, y, halfW, lineH, hFont, (p->m_nSpriteMessagesMode & 1) != 0, false);
        CreateCheck(hw, L"Show Sprites", IDC_MW_MSG_SHOW_SPRITES, x + halfW + 4, y, halfW, lineH, hFont, (p->m_nSpriteMessagesMode & 2) != 0, false);
    }
    y += lineH + gap;

    // Message listbox
    {
        int listH = 10 * lineH;
        HWND hMsgList = CreateWindowExW(0, L"LISTBOX", L"",
            WS_CHILD | WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
            x, y, rw, listH, hw, (HMENU)(INT_PTR)IDC_MW_MSG_LIST,
            GetModuleHandle(NULL), NULL);
        if (hMsgList && hFont) SendMessage(hMsgList, WM_SETFONT, (WPARAM)hFont, TRUE);
        p->PopulateMsgListBox(hMsgList);
        TrackControl(hMsgList);
    }
    y += 10 * lineH + 4;

    // Button row 1: Push Now, Up, Down, Add, Edit, Delete, Play
    {
        int bx = x, btnGap = 4;
        int pushW = MulDiv(75, lineH, 26), arrowW = MulDiv(30, lineH, 26);
        int smallW = MulDiv(40, lineH, 26), medW = MulDiv(50, lineH, 26);
        CreateBtn(hw, L"Push Now", IDC_MW_MSG_PUSH, bx, y, pushW, lineH, hFont);
        bx += pushW + btnGap;
        CreateBtn(hw, L"\x25B2", IDC_MW_MSG_UP, bx, y, arrowW, lineH, hFont);
        bx += arrowW + btnGap;
        CreateBtn(hw, L"\x25BC", IDC_MW_MSG_DOWN, bx, y, arrowW, lineH, hFont);
        bx += arrowW + btnGap;
        CreateBtn(hw, L"Add", IDC_MW_MSG_ADD, bx, y, smallW, lineH, hFont);
        bx += smallW + btnGap;
        CreateBtn(hw, L"Edit", IDC_MW_MSG_EDIT, bx, y, smallW, lineH, hFont);
        bx += smallW + btnGap;
        CreateBtn(hw, L"Delete", IDC_MW_MSG_DELETE, bx, y, medW, lineH, hFont);
        bx += medW + btnGap;
        CreateBtn(hw, p->m_bMsgAutoplay ? L"Stop" : L"Play", IDC_MW_MSG_PLAY, bx, y, medW, lineH, hFont);
    }
    y += lineH + gap;

    // Button row 2: Reload, Paste, Open INI, Overrides
    {
        int bx = x, btnGap = 4;
        int bw1 = MulDiv(130, lineH, 26), bw2 = MulDiv(55, lineH, 26);
        int bw3 = MulDiv(70, lineH, 26), bw4 = MulDiv(75, lineH, 26);
        CreateBtn(hw, L"Reload from File", IDC_MW_MSG_RELOAD, bx, y, bw1, lineH, hFont); bx += bw1 + btnGap;
        CreateBtn(hw, L"Paste", IDC_MW_MSG_PASTE, bx, y, bw2, lineH, hFont); bx += bw2 + btnGap;
        CreateBtn(hw, L"Open INI", IDC_MW_MSG_OPENINI, bx, y, bw3, lineH, hFont); bx += bw3 + btnGap;
        CreateBtn(hw, L"Overrides", IDC_MW_MSG_OVERRIDES, bx, y, bw4, lineH, hFont);
    }
    y += lineH + gap + 4;

    CreateLabel(hw, L"Font size: 50 = normal, <50 = smaller, >50 = larger (0.01\x2013" L"100)", x, y, rw, lineH, hFont, false);
    y += lineH + gap;

    // Autoplay controls
    CreateCheck(hw, L"Autoplay Messages", IDC_MW_MSG_AUTOPLAY, x, y, rw, lineH, hFont, p->m_bMsgAutoplay, false);
    y += lineH + 2;
    CreateCheck(hw, L"Sequential Order", IDC_MW_MSG_SEQUENTIAL, x, y, rw, lineH, hFont, p->m_bMsgSequential, false);
    y += lineH + 2;
    CreateCheck(hw, L"Auto-size messages to fit screen width", IDC_MW_MSG_AUTOSIZE, x, y, rw, lineH, hFont, p->m_bMessageAutoSize, false);
    y += lineH + gap;

    // Interval + Jitter on same row
    {
        HWND hLbl = CreateLabel(hw, L"Interval (s):", x, y, 90, lineH, hFont, false);
        if (hLbl) SetWindowLongPtr(hLbl, GWL_ID, IDC_MW_MSG_INTERVAL_LBL);
    }
    swprintf(buf, 64, L"%.1f", p->m_fMsgAutoplayInterval);
    CreateEdit(hw, buf, IDC_MW_MSG_INTERVAL, x + 94, y, 60, lineH, hFont, 0);
    {
        HWND hLbl = CreateLabel(hw, L"+/- (s):", x + 170, y, 60, lineH, hFont, false);
        if (hLbl) SetWindowLongPtr(hLbl, GWL_ID, IDC_MW_MSG_JITTER_LBL);
    }
    swprintf(buf, 64, L"%.1f", p->m_fMsgAutoplayJitter);
    CreateEdit(hw, buf, IDC_MW_MSG_JITTER, x + 234, y, 60, lineH, hFont, 0);
    y += lineH + gap;

    // Preview area
    {
        HWND hPrev = CreateWindowExW(0, L"STATIC", L"(select a message to preview)",
            WS_CHILD | SS_LEFT, x, y, rw, lineH * 3, hw,
            (HMENU)(INT_PTR)IDC_MW_MSG_PREVIEW, GetModuleHandle(NULL), NULL);
        if (hPrev && hFont) SendMessage(hPrev, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hPrev);
    }
}

// ─────────────────────────────────────────────────────
// Resize
// ─────────────────────────────────────────────────────

void MessagesWindow::OnResize() {
    RECT rc;
    GetClientRect(m_hWnd, &rc);
    int x = 10, rw = rc.right - 20;
    int y = m_nTopY;
    int lineH = GetLineHeight();
    int gap = 4;

    auto moveCtrl = [&](int id, int cx, int cy, int cw, int ch) {
        HWND h = GetDlgItem(m_hWnd, id);
        if (h) MoveWindow(h, cx, cy, cw, ch, TRUE);
    };

    // Title (label — no ID, skip)
    y += lineH + 2;

    // Show Messages / Show Sprites
    {
        int halfW = rw / 2 - 2;
        moveCtrl(IDC_MW_MSG_SHOW_MESSAGES, x, y, halfW, lineH);
        moveCtrl(IDC_MW_MSG_SHOW_SPRITES, x + halfW + 4, y, halfW, lineH);
    }
    y += lineH + gap;

    // Listbox
    int listH = 10 * lineH;
    moveCtrl(IDC_MW_MSG_LIST, x, y, rw, listH);
    y += listH + 4;

    // Button row 1
    {
        int bx = x, btnGap = 4;
        int pushW = MulDiv(75, lineH, 26), arrowW = MulDiv(30, lineH, 26);
        int smallW = MulDiv(40, lineH, 26), medW = MulDiv(50, lineH, 26);
        moveCtrl(IDC_MW_MSG_PUSH, bx, y, pushW, lineH); bx += pushW + btnGap;
        moveCtrl(IDC_MW_MSG_UP, bx, y, arrowW, lineH); bx += arrowW + btnGap;
        moveCtrl(IDC_MW_MSG_DOWN, bx, y, arrowW, lineH); bx += arrowW + btnGap;
        moveCtrl(IDC_MW_MSG_ADD, bx, y, smallW, lineH); bx += smallW + btnGap;
        moveCtrl(IDC_MW_MSG_EDIT, bx, y, smallW, lineH); bx += smallW + btnGap;
        moveCtrl(IDC_MW_MSG_DELETE, bx, y, medW, lineH); bx += medW + btnGap;
        moveCtrl(IDC_MW_MSG_PLAY, bx, y, medW, lineH);
    }
    y += lineH + gap;

    // Button row 2
    {
        int bx = x, btnGap = 4;
        int bw1 = MulDiv(130, lineH, 26), bw2 = MulDiv(55, lineH, 26);
        int bw3 = MulDiv(70, lineH, 26), bw4 = MulDiv(75, lineH, 26);
        moveCtrl(IDC_MW_MSG_RELOAD, bx, y, bw1, lineH); bx += bw1 + btnGap;
        moveCtrl(IDC_MW_MSG_PASTE, bx, y, bw2, lineH); bx += bw2 + btnGap;
        moveCtrl(IDC_MW_MSG_OPENINI, bx, y, bw3, lineH); bx += bw3 + btnGap;
        moveCtrl(IDC_MW_MSG_OVERRIDES, bx, y, bw4, lineH);
    }
    y += lineH + gap + 4;

    // Font size note (no ID, skip)
    y += lineH + gap;

    // Autoplay checkboxes
    moveCtrl(IDC_MW_MSG_AUTOPLAY, x, y, rw, lineH); y += lineH + 2;
    moveCtrl(IDC_MW_MSG_SEQUENTIAL, x, y, rw, lineH); y += lineH + 2;
    moveCtrl(IDC_MW_MSG_AUTOSIZE, x, y, rw, lineH); y += lineH + gap;

    // Interval + Jitter
    moveCtrl(IDC_MW_MSG_INTERVAL_LBL, x, y, 90, lineH);
    moveCtrl(IDC_MW_MSG_INTERVAL, x + 94, y, 60, lineH);
    moveCtrl(IDC_MW_MSG_JITTER_LBL, x + 170, y, 60, lineH);
    moveCtrl(IDC_MW_MSG_JITTER, x + 234, y, 60, lineH);
    y += lineH + gap;

    // Preview
    moveCtrl(IDC_MW_MSG_PREVIEW, x, y, rw, lineH * 3);
}

// ─────────────────────────────────────────────────────
// Command Handler
// ─────────────────────────────────────────────────────

LRESULT MessagesWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
    Engine* p = m_pEngine;

    // Checkbox handlers
    if (code == BN_CLICKED) {
        bool bChecked = IsDlgButtonChecked(hWnd, id) == BST_CHECKED;
        switch (id) {
        case IDC_MW_MSG_AUTOPLAY:
            p->m_bMsgAutoplay = bChecked;
            SetWindowTextW(GetDlgItem(hWnd, IDC_MW_MSG_PLAY), bChecked ? L"Stop" : L"Play");
            if (bChecked)
                p->ScheduleNextAutoMessage();
            else
                p->m_fNextAutoMsgTime = -1.0f;
            p->SaveMsgAutoplaySettings();
            return 0;
        case IDC_MW_MSG_SEQUENTIAL:
            p->m_bMsgSequential = bChecked;
            p->m_nNextSequentialMsg = 0;
            p->SaveMsgAutoplaySettings();
            return 0;
        case IDC_MW_MSG_AUTOSIZE:
            p->m_bMessageAutoSize = bChecked;
            p->SaveMsgAutoplaySettings();
            return 0;
        case IDC_MW_MSG_SHOW_MESSAGES:
            p->m_nSpriteMessagesMode = (p->m_nSpriteMessagesMode & ~1) | (bChecked ? 1 : 0);
            p->SaveSettingToINI(SET_SPRITES_MESSAGES);
            return 0;
        case IDC_MW_MSG_SHOW_SPRITES:
            p->m_nSpriteMessagesMode = (p->m_nSpriteMessagesMode & ~2) | (bChecked ? 2 : 0);
            p->SaveSettingToINI(SET_SPRITES_MESSAGES);
            return 0;

        case IDC_MW_MSG_PUSH: {
            HWND hMsgList = GetDlgItem(hWnd, IDC_MW_MSG_LIST);
            int sel = hMsgList ? (int)SendMessage(hMsgList, LB_GETCURSEL, 0, 0) : -1;
            if (sel >= 0 && sel < p->m_nMsgAutoplayCount) {
                int msgIdx = p->m_nMsgAutoplayOrder[sel];
                HWND hw = p->GetPluginWindow();
                if (hw) PostMessage(hw, WM_MW_PUSH_MESSAGE, msgIdx, 0);
            }
            return 0;
        }
        case IDC_MW_MSG_UP: {
            HWND hMsgList = GetDlgItem(hWnd, IDC_MW_MSG_LIST);
            int sel = hMsgList ? (int)SendMessage(hMsgList, LB_GETCURSEL, 0, 0) : -1;
            if (sel > 0 && sel < p->m_nMsgAutoplayCount) {
                std::swap(p->m_nMsgAutoplayOrder[sel], p->m_nMsgAutoplayOrder[sel - 1]);
                p->PopulateMsgListBox(hMsgList);
                SendMessage(hMsgList, LB_SETCURSEL, sel - 1, 0);
                p->SaveMsgAutoplaySettings();
            }
            return 0;
        }
        case IDC_MW_MSG_DOWN: {
            HWND hMsgList = GetDlgItem(hWnd, IDC_MW_MSG_LIST);
            int sel = hMsgList ? (int)SendMessage(hMsgList, LB_GETCURSEL, 0, 0) : -1;
            if (sel >= 0 && sel < p->m_nMsgAutoplayCount - 1) {
                std::swap(p->m_nMsgAutoplayOrder[sel], p->m_nMsgAutoplayOrder[sel + 1]);
                p->PopulateMsgListBox(hMsgList);
                SendMessage(hMsgList, LB_SETCURSEL, sel + 1, 0);
                p->SaveMsgAutoplaySettings();
            }
            return 0;
        }
        case IDC_MW_MSG_ADD: {
            int freeSlot = -1;
            for (int i = 0; i < MAX_CUSTOM_MESSAGES; i++) {
                if (p->m_CustomMessage[i].szText[0] == 0) { freeSlot = i; break; }
            }
            if (freeSlot < 0) { MessageBoxW(hWnd, L"All 100 message slots are full.", L"Messages", MB_OK); return 0; }
            if (p->ShowMessageEditDialog(hWnd, freeSlot, true)) {
                p->BuildMsgPlaybackOrder();
                HWND hMsgList = GetDlgItem(hWnd, IDC_MW_MSG_LIST);
                p->PopulateMsgListBox(hMsgList);
                p->WriteCustomMessages();
            }
            return 0;
        }
        case IDC_MW_MSG_EDIT: {
            HWND hMsgList = GetDlgItem(hWnd, IDC_MW_MSG_LIST);
            int sel = hMsgList ? (int)SendMessage(hMsgList, LB_GETCURSEL, 0, 0) : -1;
            if (sel >= 0 && sel < p->m_nMsgAutoplayCount) {
                int msgIdx = p->m_nMsgAutoplayOrder[sel];
                if (p->ShowMessageEditDialog(hWnd, msgIdx, false)) {
                    p->PopulateMsgListBox(hMsgList);
                    SendMessage(hMsgList, LB_SETCURSEL, sel, 0);
                    p->UpdateMsgPreview(hWnd, sel);
                    p->WriteCustomMessages();
                }
            }
            return 0;
        }
        case IDC_MW_MSG_DELETE: {
            HWND hMsgList = GetDlgItem(hWnd, IDC_MW_MSG_LIST);
            int sel = hMsgList ? (int)SendMessage(hMsgList, LB_GETCURSEL, 0, 0) : -1;
            if (sel >= 0 && sel < p->m_nMsgAutoplayCount) {
                int msgIdx = p->m_nMsgAutoplayOrder[sel];
                p->m_CustomMessage[msgIdx].szText[0] = 0;
                p->BuildMsgPlaybackOrder();
                p->PopulateMsgListBox(hMsgList);
                p->WriteCustomMessages();
                SetWindowTextW(GetDlgItem(hWnd, IDC_MW_MSG_PREVIEW), L"");
            }
            return 0;
        }
        case IDC_MW_MSG_RELOAD:
            p->ReadCustomMessages();
            p->BuildMsgPlaybackOrder();
            p->PopulateMsgListBox(GetDlgItem(hWnd, IDC_MW_MSG_LIST));
            SetWindowTextW(GetDlgItem(hWnd, IDC_MW_MSG_PREVIEW), L"");
            return 0;
        case IDC_MW_MSG_OPENINI: {
            SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            HINSTANCE hr = ShellExecuteW(NULL, L"open", p->m_szMsgIniFile, NULL, NULL, SW_SHOWNORMAL);
            if ((INT_PTR)hr <= 32)
                ShellExecuteW(NULL, L"open", L"notepad.exe", p->m_szMsgIniFile, NULL, SW_SHOWNORMAL);
            SetTimer(hWnd, 9999, 500, NULL);
            return 0;
        }
        case IDC_MW_MSG_PASTE: {
            if (!OpenClipboard(hWnd)) return 0;
            HANDLE hData = GetClipboardData(CF_UNICODETEXT);
            if (!hData) { CloseClipboard(); return 0; }
            wchar_t* pText = (wchar_t*)GlobalLock(hData);
            if (!pText) { CloseClipboard(); return 0; }

            std::wstring clipText(pText);
            GlobalUnlock(hData);
            CloseClipboard();

            int added = 0;
            size_t pos = 0;
            while (pos < clipText.size()) {
                size_t end = clipText.find_first_of(L"\r\n", pos);
                if (end == std::wstring::npos) end = clipText.size();
                if (end > pos && (end - pos) < 256) {
                    int freeSlot = -1;
                    for (int i = 0; i < MAX_CUSTOM_MESSAGES; i++) {
                        if (p->m_CustomMessage[i].szText[0] == 0) { freeSlot = i; break; }
                    }
                    if (freeSlot < 0) break;

                    wcsncpy(p->m_CustomMessage[freeSlot].szText, clipText.c_str() + pos, end - pos);
                    p->m_CustomMessage[freeSlot].szText[end - pos] = 0;
                    p->m_CustomMessage[freeSlot].nFont = 0;
                    p->m_CustomMessage[freeSlot].fSize = 50.0f;
                    p->m_CustomMessage[freeSlot].x = 0.5f;
                    p->m_CustomMessage[freeSlot].y = 0.5f;
                    p->m_CustomMessage[freeSlot].randx = 0;
                    p->m_CustomMessage[freeSlot].randy = 0;
                    p->m_CustomMessage[freeSlot].growth = 1.0f;
                    p->m_CustomMessage[freeSlot].fTime = 5.0f;
                    p->m_CustomMessage[freeSlot].fFade = 1.0f;
                    p->m_CustomMessage[freeSlot].fFadeOut = 1.0f;
                    p->m_CustomMessage[freeSlot].fBurnTime = 0;
                    p->m_CustomMessage[freeSlot].nColorR = -1;
                    p->m_CustomMessage[freeSlot].nColorG = -1;
                    p->m_CustomMessage[freeSlot].nColorB = -1;
                    p->m_CustomMessage[freeSlot].nRandR = 0;
                    p->m_CustomMessage[freeSlot].nRandG = 0;
                    p->m_CustomMessage[freeSlot].nRandB = 0;
                    p->m_CustomMessage[freeSlot].bOverrideFace = 0;
                    p->m_CustomMessage[freeSlot].bOverrideBold = 0;
                    p->m_CustomMessage[freeSlot].bOverrideItal = 0;
                    p->m_CustomMessage[freeSlot].bOverrideColorR = 0;
                    p->m_CustomMessage[freeSlot].bOverrideColorG = 0;
                    p->m_CustomMessage[freeSlot].bOverrideColorB = 0;
                    p->m_CustomMessage[freeSlot].bBold = -1;
                    p->m_CustomMessage[freeSlot].bItal = -1;
                    p->m_CustomMessage[freeSlot].szFace[0] = 0;
                    added++;
                }
                pos = end;
                while (pos < clipText.size() && (clipText[pos] == L'\r' || clipText[pos] == L'\n')) pos++;
            }

            if (added > 0) {
                p->BuildMsgPlaybackOrder();
                p->PopulateMsgListBox(GetDlgItem(hWnd, IDC_MW_MSG_LIST));
                p->WriteCustomMessages();
                wchar_t msg[64];
                swprintf(msg, 64, L"Pasted %d message(s) from clipboard.", added);
                MessageBoxW(hWnd, msg, L"Messages", MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBoxW(hWnd, L"No text found on clipboard (or all slots full).", L"Messages", MB_OK);
            }
            return 0;
        }
        case IDC_MW_MSG_PLAY: {
            p->m_bMsgAutoplay = !p->m_bMsgAutoplay;
            SetWindowTextW(GetDlgItem(hWnd, IDC_MW_MSG_PLAY), p->m_bMsgAutoplay ? L"Stop" : L"Play");
            CheckDlgButton(hWnd, IDC_MW_MSG_AUTOPLAY, p->m_bMsgAutoplay ? BST_CHECKED : BST_UNCHECKED);
            if (p->m_bMsgAutoplay)
                p->ScheduleNextAutoMessage();
            else
                p->m_fNextAutoMsgTime = -1.0f;
            p->SaveMsgAutoplaySettings();
            return 0;
        }
        case IDC_MW_MSG_OVERRIDES: {
            p->ShowMsgOverridesDialog(hWnd);
            return 0;
        }
        }
    }

    // Listbox selection
    if (id == IDC_MW_MSG_LIST && code == LBN_SELCHANGE) {
        int sel = (int)SendMessage((HWND)lParam, LB_GETCURSEL, 0, 0);
        p->UpdateMsgPreview(hWnd, sel);
        return 0;
    }

    // Edit control changes (apply on focus lost)
    if (code == EN_KILLFOCUS) {
        wchar_t buf[64];
        GetWindowTextW((HWND)lParam, buf, 64);
        switch (id) {
        case IDC_MW_MSG_INTERVAL: {
            float val = (float)_wtof(buf);
            if (val < 1.0f) val = 1.0f;
            if (val > 9999.0f) val = 9999.0f;
            p->m_fMsgAutoplayInterval = val;
            p->SaveMsgAutoplaySettings();
            return 0;
        }
        case IDC_MW_MSG_JITTER: {
            float val = (float)_wtof(buf);
            if (val < 0.0f) val = 0.0f;
            if (val > 9999.0f) val = 9999.0f;
            p->m_fMsgAutoplayJitter = val;
            p->SaveMsgAutoplaySettings();
            return 0;
        }
        }
    }

    return 0;
}

// ─────────────────────────────────────────────────────
// Destroy
// ─────────────────────────────────────────────────────

void MessagesWindow::DoDestroy() {
    // Nothing extra to clean up
}
