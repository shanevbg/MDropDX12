// engine_board_ui.cpp — Button Board ToolWindow implementation.
//
// Standalone window hosting a configurable grid of buttons that trigger
// presets, sprites, script commands, or messages.

#include "tool_window.h"
#include "engine.h"
#include "button_panel.h"
#include "utility.h"
#include <CommCtrl.h>
#include <commdlg.h>
#include <shellapi.h>

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

    // Load thumbnail images from saved paths
    LoadSlotImages();

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

// Helper: post a wide string to the render window via WM_MW_IPC_MESSAGE.
// Allocates with malloc() to match the free() in the message handler.
static void PostIPCMessage(HWND hRender, const std::wstring& msg) {
    size_t cb = (msg.size() + 1) * sizeof(wchar_t);
    wchar_t* copy = (wchar_t*)malloc(cb);
    if (!copy) return;
    memcpy(copy, msg.c_str(), cb);
    if (!PostMessage(hRender, WM_MW_IPC_MESSAGE, 1, (LPARAM)copy))
        free(copy);
}

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
    case ButtonAction::LoadPreset:
        PostIPCMessage(hRender, L"PRESET=" + slot.payload);
        break;
    case ButtonAction::PushSprite: {
        int sprNum = _wtoi(slot.payload.c_str());
        PostMessage(hRender, WM_MW_PUSH_SPRITE, sprNum, -1);
        break;
    }
    case ButtonAction::ScriptCommand:
        // Pipe-chained script commands (NEXT, PREV, LOCK, SEND=0x.., etc.)
        // Routed via IPC → LaunchMessage → ExecuteScriptLine fallback
        PostIPCMessage(hRender, slot.payload);
        break;
    case ButtonAction::LaunchMessage:
        PostIPCMessage(hRender, slot.payload);
        break;
    case ButtonAction::RunScript:
        // Load and execute a script file (same as FILE= in script system)
        PostIPCMessage(hRender, L"FILE=" + slot.payload);
        break;
    case ButtonAction::LaunchApp:
        // Launch or focus an application (same as hotkey Launch type)
        PostIPCMessage(hRender, L"LAUNCH=" + slot.payload);
        break;
    default:
        break;
    }
}

// ─── "Assign Action..." cascading submenu ───────────────────────────────

HMENU ButtonBoardWindow::BuildActionSubMenu() {
    Engine* p = m_pEngine;
    HMENU hTop = CreatePopupMenu();

    for (int cat = HKCAT_NAVIGATION; cat <= HKCAT_MISC; cat++) {
        HMENU hCatMenu = CreatePopupMenu();
        int itemCount = 0;

        if (cat == HKCAT_VISUAL) {
            // Sub-group by first word of action name (Opacity, Wave, Zoom, ...)
            struct PrefixGroup { std::wstring prefix; std::vector<int> indices; };
            std::vector<PrefixGroup> groups;

            for (int i = 0; i < NUM_HOTKEYS; i++) {
                if ((int)p->m_hotkeys[i].category != cat) continue;
                std::wstring action = p->m_hotkeys[i].szAction;
                size_t sp = action.find(L' ');
                std::wstring prefix = (sp != std::wstring::npos) ? action.substr(0, sp) : action;
                bool found = false;
                for (auto& g : groups) {
                    if (g.prefix == prefix) { g.indices.push_back(i); found = true; break; }
                }
                if (!found) groups.push_back({ prefix, { i } });
            }

            for (auto& g : groups) {
                if (g.indices.size() >= 2) {
                    HMENU hSub = CreatePopupMenu();
                    for (int idx : g.indices) {
                        std::wstring text = p->m_hotkeys[idx].szAction;
                        if (p->m_hotkeys[idx].vk != 0)
                            text += L"\t" + p->FormatHotkeyDisplay(p->m_hotkeys[idx].modifiers, p->m_hotkeys[idx].vk);
                        AppendMenuW(hSub, MF_STRING, 1000 + idx, text.c_str());
                    }
                    AppendMenuW(hCatMenu, MF_POPUP, (UINT_PTR)hSub, g.prefix.c_str());
                } else {
                    int idx = g.indices[0];
                    std::wstring text = p->m_hotkeys[idx].szAction;
                    if (p->m_hotkeys[idx].vk != 0)
                        text += L"\t" + p->FormatHotkeyDisplay(p->m_hotkeys[idx].modifiers, p->m_hotkeys[idx].vk);
                    AppendMenuW(hCatMenu, MF_STRING, 1000 + idx, text.c_str());
                }
                itemCount++;
            }
        } else {
            for (int i = 0; i < NUM_HOTKEYS; i++) {
                if ((int)p->m_hotkeys[i].category != cat) continue;
                std::wstring text = p->m_hotkeys[i].szAction;
                if (p->m_hotkeys[i].vk != 0)
                    text += L"\t" + p->FormatHotkeyDisplay(p->m_hotkeys[i].modifiers, p->m_hotkeys[i].vk);
                AppendMenuW(hCatMenu, MF_STRING, 1000 + i, text.c_str());
                itemCount++;
            }
        }

        if (itemCount > 0)
            AppendMenuW(hTop, MF_POPUP, (UINT_PTR)hCatMenu, kCategoryNames[cat]);
        else
            DestroyMenu(hCatMenu);
    }

    return hTop;
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
    HMENU hActionMenu = BuildActionSubMenu();
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hActionMenu, L"Assign Action...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, 7, L"Set Image...");
    bool hasImage = !slot.imagePath.empty();
    AppendMenuW(hMenu, MF_STRING | (hasImage ? 0 : MF_GRAYED), 8, L"Clear Image");
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
        s.imagePath.clear();
        m_pPanel->ClearSlotThumbnail(globalIndex);
        m_pPanel->Invalidate();
        SaveBoard();
        break;

    case 7: { // Set Image
        wchar_t szFile[MAX_PATH] = {};
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = m_hWnd;
        ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.ico\0All Files\0*.*\0";
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) {
            SetSlotImage(globalIndex, szFile);
            SaveBoard();
        }
        break;
    }

    case 8: // Clear Image
        SetSlotImage(globalIndex, L"");
        SaveBoard();
        break;
    }

    // Handle "Assign Action..." menu items (ID = 1000 + hotkey index)
    if (cmd >= 1000 && cmd < 1000 + NUM_HOTKEYS) {
        int hkIdx = cmd - 1000;
        s.action  = ButtonAction::ScriptCommand;
        s.payload = std::wstring(L"ACTION=") + p->m_hotkeys[hkIdx].szIniKey;
        s.label   = p->m_hotkeys[hkIdx].szAction;
        m_pPanel->Invalidate();
        SaveBoard();
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

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, 30, L"Save Layout...");
    AppendMenuW(hMenu, MF_STRING, 31, L"Load Layout...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, 40, L"Reset to Defaults");

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
    case 30: // Save Layout
    {
        wchar_t szFile[MAX_PATH] = L"board_layout.json";
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = m_hWnd;
        ofn.lpstrFilter = L"JSON Files (*.json)\0*.json\0All Files\0*.*\0";
        ofn.lpstrFile   = szFile;
        ofn.nMaxFile    = MAX_PATH;
        ofn.lpstrDefExt = L"json";
        ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        // Start in base dir
        std::wstring initDir(m_pEngine->m_szBaseDir);
        ofn.lpstrInitialDir = initDir.c_str();
        if (GetSaveFileNameW(&ofn)) {
            if (m_pPanel->SaveLayoutJSON(szFile))
                m_pEngine->AddNotification(L"Board layout saved");
        }
        break;
    }
    case 31: // Load Layout
    {
        wchar_t szFile[MAX_PATH] = L"";
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = m_hWnd;
        ofn.lpstrFilter = L"JSON Files (*.json)\0*.json\0All Files\0*.*\0";
        ofn.lpstrFile   = szFile;
        ofn.nMaxFile    = MAX_PATH;
        ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        std::wstring initDir(m_pEngine->m_szBaseDir);
        ofn.lpstrInitialDir = initDir.c_str();
        if (GetOpenFileNameW(&ofn)) {
            if (m_pPanel->LoadLayoutJSON(szFile)) {
                UpdateBankLabel();
                SaveBoard();
                m_pEngine->AddNotification(L"Board layout loaded");
            }
        }
        break;
    }
    case 40: // Reset to Defaults
        m_pPanel->ResetToDefaults();
        UpdateBankLabel();
        SaveBoard();
        m_pEngine->AddNotification(L"Board reset to defaults");
        break;
    }

    if (changed) {
        if (cfg.nActiveBank >= cfg.nBanks)
            cfg.nActiveBank = 0;
        m_pPanel->ApplyConfig();
        UpdateBankLabel();
        SaveBoard();
    }
}

// ─── Slot Edit Dialog (uses shared ActionEditDialog) ────────────────────

void ButtonBoardWindow::ShowSlotEditDialog(int globalIndex) {
    if (!m_pPanel || globalIndex < 0) return;

    ButtonSlot& slot = m_pPanel->Slot(globalIndex);

    // Populate shared dialog data from the button slot
    ActionEditData data;
    data.actionType     = slot.action;
    data.label          = slot.label;
    data.payload        = slot.payload;
    data.showKeyBinding = true;
    data.modifiers      = slot.modifiers;
    data.vk             = slot.vk;
    data.scope          = (HotkeyScope)slot.scope;
    data.isBuiltInHotkey = false;
    data.pEngine        = m_pEngine;

    if (ShowActionEditDialog(m_hWnd, data)) {
        slot.action    = data.actionType;
        slot.label     = data.label;
        slot.payload   = data.payload;
        slot.modifiers = data.modifiers;
        slot.vk        = data.vk;
        slot.scope     = (int)data.scope;
        m_pPanel->Invalidate();
        SaveBoard();
    }
}

// ─── Image Support ──────────────────────────────────────────────────────

static bool IsImageFile(const wchar_t* path) {
    const wchar_t* ext = wcsrchr(path, L'.');
    if (!ext) return false;
    return (_wcsicmp(ext, L".png") == 0  || _wcsicmp(ext, L".jpg") == 0 ||
            _wcsicmp(ext, L".jpeg") == 0 || _wcsicmp(ext, L".bmp") == 0 ||
            _wcsicmp(ext, L".gif") == 0  || _wcsicmp(ext, L".tif") == 0 ||
            _wcsicmp(ext, L".tiff") == 0 || _wcsicmp(ext, L".ico") == 0);
}

void ButtonBoardWindow::SetSlotImage(int globalIndex, const std::wstring& path) {
    if (!m_pPanel || globalIndex < 0 || globalIndex >= m_pPanel->SlotCount()) return;
    ButtonSlot& s = m_pPanel->Slot(globalIndex);

    // Clear old thumbnail
    m_pPanel->ClearSlotThumbnail(globalIndex);

    if (path.empty()) {
        s.imagePath.clear();
    } else {
        s.imagePath = path;
        // Load via WIC (reuse engine's thumbnail loader, 128x128 max for button icons)
        HBITMAP hBmp = m_pEngine->LoadThumbnailWIC(path.c_str(), 128, 128);
        if (hBmp) {
            BITMAP bm = {};
            GetObject(hBmp, sizeof(bm), &bm);
            m_pPanel->SetSlotThumbnail(globalIndex, hBmp, bm.bmWidth, bm.bmHeight);
        }
    }
    m_pPanel->Invalidate();
}

void ButtonBoardWindow::LoadSlotImages() {
    if (!m_pPanel) return;
    for (int i = 0; i < m_pPanel->SlotCount(); i++) {
        const ButtonSlot& s = m_pPanel->Slot(i);
        if (!s.imagePath.empty())
            SetSlotImage(i, s.imagePath);
    }
}

// ─── Drag-and-Drop ──────────────────────────────────────────────────────

LRESULT ButtonBoardWindow::DoMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_DROPFILES && m_pPanel) {
        HDROP hDrop = (HDROP)wParam;
        UINT nFiles = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);

        // Get drop point and convert to panel client coords
        POINT pt;
        DragQueryPoint(hDrop, &pt);
        // pt is in client coords of the window that received WM_DROPFILES
        // Convert to panel client coords
        POINT panelPt = pt;
        MapWindowPoints(m_hWnd, m_pPanel->GetHWND(), &panelPt, 1);
        int hitSlot = m_pPanel->HitTest(panelPt);

        if (hitSlot >= 0) {
            int gi = m_pPanel->LocalToGlobal(hitSlot);
            // Process first image file found
            for (UINT i = 0; i < nFiles; i++) {
                wchar_t szFile[MAX_PATH];
                DragQueryFileW(hDrop, i, szFile, MAX_PATH);
                if (IsImageFile(szFile)) {
                    SetSlotImage(gi, szFile);
                    SaveBoard();
                    break;
                }
            }
        }
        DragFinish(hDrop);
        return 0;
    }
    return -1; // not handled
}

// ─── Save ───────────────────────────────────────────────────────────────

void ButtonBoardWindow::SaveBoard() {
    if (!m_pPanel || !m_pEngine) return;
    const wchar_t* ini = m_pEngine->GetConfigIniFile();
    m_pPanel->SaveToINI(ini, L"ButtonBoard");
}

} // namespace mdrop
