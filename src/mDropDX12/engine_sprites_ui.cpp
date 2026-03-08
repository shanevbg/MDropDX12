// engine_sprites_ui.cpp — Sprites ToolWindow implementation.
//
// Standalone window for sprite management, extracted from Settings page 6.
// Uses Engine's m_hSpriteList / m_hSpriteImageList so the existing Engine
// methods (PopulateSpriteListView, UpdateSpriteProperties, etc.) work.

#include "engine.h"
#include "utility.h"
#include <CommCtrl.h>
#include <ShObjIdl.h>
#include <set>
#include <sstream>

// Static helpers (duplicated from engine_settings_ui.cpp to keep both files self-contained)
static void FormatSpriteSection(wchar_t* buf, int bufSize, int index) {
    if (index < 100) swprintf(buf, bufSize, L"img%02d", index);
    else             swprintf(buf, bufSize, L"img%d", index);
}

static std::wstring MakeRelativeSpritePath(const wchar_t* szAbsPath,
                                           const wchar_t* szContentBasePath,
                                           const wchar_t* szMilkdrop2Path) {
    if (!szAbsPath || !szAbsPath[0]) return L"";
    if (szAbsPath[1] != L':') return szAbsPath;
    const wchar_t* bases[] = { szContentBasePath, szMilkdrop2Path };
    for (auto base : bases) {
        if (!base || !base[0]) continue;
        size_t baseLen = wcslen(base);
        if (_wcsnicmp(szAbsPath, base, baseLen) == 0)
            return szAbsPath + baseLen;
    }
    return szAbsPath;
}

namespace mdrop {

// ─── Open / Close ───────────────────────────────────────────────────────

void Engine::OpenSpritesWindow() {
    if (!m_spritesWindow)
        m_spritesWindow = std::make_unique<SpritesWindow>(this);
    m_spritesWindow->Open();
}

void Engine::CloseSpritesWindow() {
    if (m_spritesWindow)
        m_spritesWindow->Close();
}

// ─── Constructor ────────────────────────────────────────────────────────

SpritesWindow::SpritesWindow(Engine* pEngine)
    : ToolWindow(pEngine, 560, 820)
{
}

// ─── Build Controls ─────────────────────────────────────────────────────

void SpritesWindow::DoBuildControls() {
    HWND hw = m_hWnd;
    Engine* p = m_pEngine;

    auto L = BuildBaseControls();
    int x = L.x, y = L.y, rw = L.rw;
    HFONT hFont = GetFont();
    HFONT hFontBold = GetFontBold();
    m_nTopY = y;

    int lineH = GetLineHeight();
    int gap = 4;

    // Title
    CreateLabel(hw, L"Sprites (sprites.ini):", x, y, rw, lineH, hFontBold);
    y += lineH + 2;

    // ListView
    {
        int listH = 6 * lineH;
        m_hList = CreateThemedListView(IDC_MW_SPR_LIST, x, y, rw, listH);

        // ImageList for thumbnails
        p->m_hSpriteImageList = (void*)ImageList_Create(32, 32, ILC_COLOR32, 100, 10);
        p->m_hSpriteList = m_hList;
        SendMessageW(m_hList, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)p->m_hSpriteImageList);

        // Columns
        LVCOLUMNW col = { LVCF_TEXT | LVCF_WIDTH, LVCFMT_LEFT, MulDiv(60, lineH, 26) };
        col.pszText = (LPWSTR)L"Index";
        SendMessageW(m_hList, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
        col.cx = MulDiv(120, lineH, 26); col.pszText = (LPWSTR)L"File";
        SendMessageW(m_hList, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
        col.cx = rw - 240; col.pszText = (LPWSTR)L"Path";
        SendMessageW(m_hList, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);

        p->LoadSpritesFromINI();
        p->PopulateSpriteListView();
        y += listH + gap;
    }

    // Button row 1: Push, Kill, Kill All, Defaults
    {
        int bx = x, bg = 4;
        int bw = MulDiv(55, lineH, 26), bwL = MulDiv(65, lineH, 26);
        CreateBtn(hw, L"Push", IDC_MW_SPR_PUSH, bx, y, bw, lineH, hFont); bx += bw + bg;
        CreateBtn(hw, L"Kill", IDC_MW_SPR_KILL, bx, y, bw, lineH, hFont); bx += bw + bg;
        CreateBtn(hw, L"Kill All", IDC_MW_SPR_KILLALL, bx, y, bwL, lineH, hFont); bx += bwL + bg;
        CreateBtn(hw, L"\u267B Defaults", IDC_MW_SPR_DEFAULTS, bx, y, bwL + MulDiv(14, lineH, 26), lineH, hFont);
    }
    y += lineH + gap;

    // Button row 2: Add, Delete, Save, Reload, Open INI
    {
        int bx = x, bg = 4;
        int bw1 = MulDiv(45, lineH, 26), bw2 = MulDiv(55, lineH, 26);
        int bw3 = MulDiv(55, lineH, 26), bw4 = MulDiv(58, lineH, 26), bw5 = MulDiv(70, lineH, 26);
        CreateBtn(hw, L"Add",     IDC_MW_SPR_ADD,     bx, y, bw1, lineH, hFont); bx += bw1 + bg;
        CreateBtn(hw, L"Delete",  IDC_MW_SPR_DELETE,  bx, y, bw3, lineH, hFont); bx += bw3 + bg;
        CreateBtn(hw, L"Save",    IDC_MW_SPR_SAVE,    bx, y, bw1, lineH, hFont); bx += bw1 + bg;
        CreateBtn(hw, L"Reload",  IDC_MW_SPR_RELOAD,  bx, y, bw4, lineH, hFont); bx += bw4 + bg;
        CreateBtn(hw, L"Open INI",IDC_MW_SPR_OPENINI, bx, y, bw5, lineH, hFont);
    }
    y += lineH + gap + 2;

    // Image path
    {
        int imgLblW = MulDiv(50, lineH, 26), browseW = MulDiv(60, lineH, 26);
        CreateLabel(hw, L"Image:", x, y, imgLblW, lineH, hFont);
        CreateEdit(hw, L"", IDC_MW_SPR_IMG_PATH, x + imgLblW + 4, y, rw - imgLblW - browseW - 8, lineH, hFont, ES_READONLY);
        CreateBtn(hw, L"Browse", IDC_MW_SPR_IMG_BROWSE, x + rw - browseW, y, browseW, lineH, hFont);
    }
    y += lineH + gap;

    // Properties: Blend + Layer
    {
        int propLblW = MulDiv(50, lineH, 26), propComboW = MulDiv(100, lineH, 26);
        int propCol2 = x + rw / 2;
        CreateLabel(hw, L"Blend:", x, y, propLblW, lineH, hFont);
        HWND hBlend = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            x + propLblW, y, propComboW, 200, hw, (HMENU)(INT_PTR)IDC_MW_SPR_BLENDMODE,
            GetModuleHandle(NULL), NULL);
        if (hBlend && hFont) SendMessage(hBlend, WM_SETFONT, (WPARAM)hFont, TRUE);
        const wchar_t* blendNames[] = { L"0: Blend", L"1: Decal", L"2: Additive", L"3: SrcColor", L"4: ColorKey" };
        for (int i = 0; i < 5; i++) SendMessageW(hBlend, CB_ADDSTRING, 0, (LPARAM)blendNames[i]);

        CreateLabel(hw, L"Layer:", propCol2, y, propLblW, lineH, hFont);
        HWND hLayer = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            propCol2 + propLblW, y, propComboW + MulDiv(20, lineH, 26), 200, hw, (HMENU)(INT_PTR)IDC_MW_SPR_LAYER,
            GetModuleHandle(NULL), NULL);
        if (hLayer && hFont) SendMessage(hLayer, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hLayer, CB_ADDSTRING, 0, (LPARAM)L"0: Behind Text");
        SendMessageW(hLayer, CB_ADDSTRING, 0, (LPARAM)L"1: On Top of Text");
    }
    y += lineH + gap;

    // Properties: Position, Scale, Rotation, Colorkey
    {
        int propLblW = MulDiv(80, lineH, 26), propEditW = MulDiv(55, lineH, 26);
        int propCol2 = x + rw / 2;
        CreateLabel(hw, L"Position X:", x, y, propLblW, lineH, hFont);
        CreateEdit(hw, L"0.5", IDC_MW_SPR_X, x + propLblW, y, propEditW, lineH, hFont);
        CreateLabel(hw, L"Position Y:", propCol2, y, propLblW, lineH, hFont);
        CreateEdit(hw, L"0.5", IDC_MW_SPR_Y, propCol2 + propLblW, y, propEditW, lineH, hFont);
        y += lineH + gap;

        CreateLabel(hw, L"Scale X:", x, y, propLblW, lineH, hFont);
        CreateEdit(hw, L"1.0", IDC_MW_SPR_SX, x + propLblW, y, propEditW, lineH, hFont);
        CreateLabel(hw, L"Scale Y:", propCol2, y, propLblW, lineH, hFont);
        CreateEdit(hw, L"1.0", IDC_MW_SPR_SY, propCol2 + propLblW, y, propEditW, lineH, hFont);
        y += lineH + gap;

        CreateLabel(hw, L"Rotation:", x, y, propLblW, lineH, hFont);
        CreateEdit(hw, L"0", IDC_MW_SPR_ROT, x + propLblW, y, propEditW, lineH, hFont);
        int ckLblW = MulDiv(70, lineH, 26), ckEditW = MulDiv(75, lineH, 26);
        CreateLabel(hw, L"Colorkey:", propCol2, y, ckLblW, lineH, hFont);
        CreateEdit(hw, L"0x000000", IDC_MW_SPR_COLORKEY, propCol2 + ckLblW, y, ckEditW, lineH, hFont);
    }
    y += lineH + gap;

    // Color: R, G, B, A
    {
        int clrLblW = MulDiv(45, lineH, 26), clrEditW = MulDiv(40, lineH, 26), clrGap = 6;
        int colW = clrLblW + clrEditW + clrGap;
        CreateLabel(hw, L"Red:", x, y, clrLblW, lineH, hFont);
        CreateEdit(hw, L"1", IDC_MW_SPR_R, x + clrLblW, y, clrEditW, lineH, hFont);
        CreateLabel(hw, L"Green:", x + colW, y, clrLblW, lineH, hFont);
        CreateEdit(hw, L"1", IDC_MW_SPR_G, x + colW + clrLblW, y, clrEditW, lineH, hFont);
        CreateLabel(hw, L"Blue:", x + 2 * colW, y, clrLblW, lineH, hFont);
        CreateEdit(hw, L"1", IDC_MW_SPR_B, x + 2 * colW + clrLblW, y, clrEditW, lineH, hFont);
        CreateLabel(hw, L"Alpha:", x + 3 * colW, y, clrLblW, lineH, hFont);
        CreateEdit(hw, L"1", IDC_MW_SPR_A, x + 3 * colW + clrLblW, y, clrEditW, lineH, hFont);
    }
    y += lineH + gap;

    // Flip X, Flip Y, Burn, Repeat X, Y
    {
        int chkW = MulDiv(55, lineH, 26), chkGap = 4;
        int cx = x;
        CreateCheck(hw, L"Flip X", IDC_MW_SPR_FLIPX, cx, y, chkW, lineH, hFont, false); cx += chkW + chkGap;
        CreateCheck(hw, L"Flip Y", IDC_MW_SPR_FLIPY, cx, y, chkW, lineH, hFont, false); cx += chkW + chkGap;
        CreateCheck(hw, L"Burn",   IDC_MW_SPR_BURN,  cx, y, chkW, lineH, hFont, false);

        int propCol2 = x + rw / 2;
        int repLblW = MulDiv(70, lineH, 26), repEditW = MulDiv(40, lineH, 26);
        CreateLabel(hw, L"Repeat X:", propCol2, y, repLblW, lineH, hFont);
        CreateEdit(hw, L"1", IDC_MW_SPR_REPEATX, propCol2 + repLblW, y, repEditW, lineH, hFont);
        int repYx = propCol2 + repLblW + repEditW + 8;
        CreateLabel(hw, L"Repeat Y:", repYx, y, repLblW, lineH, hFont);
        CreateEdit(hw, L"1", IDC_MW_SPR_REPEATY, repYx + repLblW, y, repEditW, lineH, hFont);
    }
    y += lineH + gap + 2;

    // Init Code
    CreateLabel(hw, L"Init", x, y, 30, lineH, hFont);
    y += lineH;
    {
        int codeH = 3 * lineH;
        CreateEdit(hw, L"", IDC_MW_SPR_INIT_CODE, x, y, rw, codeH, hFont,
            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL);
        y += codeH + gap;
    }

    // Per-Frame Code
    CreateLabel(hw, L"Per-Frame", x, y, 80, lineH, hFont);
    y += lineH;
    {
        int codeH = 3 * lineH;
        CreateEdit(hw, L"", IDC_MW_SPR_FRAME_CODE, x, y, rw, codeH, hFont,
            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL);
    }
}

// ─── Commands ───────────────────────────────────────────────────────────

LRESULT SpritesWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
    Engine* p = m_pEngine;

    if (id == IDC_MW_SPR_ADD && code == BN_CLICKED) {
        SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        IFileOpenDialog* pDlg = NULL;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg));
        if (SUCCEEDED(hr) && pDlg) {
            COMDLG_FILTERSPEC filters[] = {
                { L"Image Files", L"*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.dds;*.gif" },
                { L"All Files", L"*.*" }
            };
            pDlg->SetFileTypes(2, filters);
            pDlg->SetTitle(L"Add Sprite Image");
            if (SUCCEEDED(pDlg->Show(hWnd))) {
                IShellItem* pItem = NULL;
                if (SUCCEEDED(pDlg->GetResult(&pItem))) {
                    LPWSTR pPath = NULL;
                    if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pPath))) {
                        std::set<int> used;
                        for (auto& e : p->m_spriteEntries) used.insert(e.nIndex);
                        int newIdx = 0;
                        while (used.count(newIdx) && newIdx < 100000) newIdx++;
                        if (newIdx < 100000) {
                            Engine::SpriteEntry entry = {};
                            entry.nIndex = newIdx;
                            std::wstring relPath = MakeRelativeSpritePath(pPath, p->m_szContentBasePath, p->m_szMilkdrop2Path);
                            wcscpy_s(entry.szImg, relPath.c_str());
                            entry.nColorkey = 0;
                            entry.szInitCode = "blendmode = 0;\r\nx = 0.5; y = 0.5;\r\nsx = 0.5; sy = 0.5; rot = 0;\r\nr = 1; g = 1; b = 1; a = 1;";
                            p->m_spriteEntries.push_back(entry);
                            p->PopulateSpriteListView();
                            int newSel = (int)p->m_spriteEntries.size() - 1;
                            ListView_SetItemState(p->m_hSpriteList, newSel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                            ListView_EnsureVisible(p->m_hSpriteList, newSel, FALSE);
                        }
                        CoTaskMemFree(pPath);
                    }
                    pItem->Release();
                }
            }
            pDlg->Release();
        }
        if (m_bOnTop) SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;
    }

    if (id == IDC_MW_SPR_DELETE && code == BN_CLICKED) {
        if (p->m_nSpriteSelected >= 0 && p->m_nSpriteSelected < (int)p->m_spriteEntries.size()) {
            p->m_spriteEntries.erase(p->m_spriteEntries.begin() + p->m_nSpriteSelected);
            p->m_nSpriteSelected = -1;
            p->PopulateSpriteListView();
        }
        return 0;
    }

    if (id == IDC_MW_SPR_PUSH && code == BN_CLICKED) {
        if (p->m_nSpriteSelected >= 0 && p->m_nSpriteSelected < (int)p->m_spriteEntries.size()) {
            p->SaveCurrentSpriteProperties();
            p->SaveSpritesToINI();
            // Flush INI cache
            WritePrivateProfileStringW(NULL, NULL, NULL, p->m_szImgIniFile);
            HWND hRender = p->GetPluginWindow();
            if (hRender)
                PostMessage(hRender, WM_MW_PUSH_SPRITE, p->m_spriteEntries[p->m_nSpriteSelected].nIndex, -1);
        }
        return 0;
    }

    if (id == IDC_MW_SPR_KILL && code == BN_CLICKED) {
        if (p->m_nSpriteSelected >= 0 && p->m_nSpriteSelected < (int)p->m_spriteEntries.size()) {
            HWND hRender = p->GetPluginWindow();
            if (hRender)
                PostMessage(hRender, WM_MW_KILL_SPRITE, p->m_spriteEntries[p->m_nSpriteSelected].nIndex, 0);
        }
        return 0;
    }

    if (id == IDC_MW_SPR_KILLALL && code == BN_CLICKED) {
        HWND hRender = p->GetPluginWindow();
        if (hRender) {
            for (int i = 0; i < 100; i++)
                PostMessage(hRender, WM_MW_KILL_SPRITE, i, 0);
        }
        return 0;
    }

    if (id == IDC_MW_SPR_DEFAULTS && code == BN_CLICKED) {
        if (p->m_nSpriteSelected >= 0 && p->m_nSpriteSelected < (int)p->m_spriteEntries.size()) {
            auto& e = p->m_spriteEntries[p->m_nSpriteSelected];
            e.szInitCode = "blendmode = 0;\r\nx = 0.5; y = 0.5;\r\nsx = 0.5; sy = 0.5; rot = 0;\r\nr = 1; g = 1; b = 1; a = 1;";
            e.szFrameCode.clear();
            p->UpdateSpriteProperties(p->m_nSpriteSelected);
        }
        return 0;
    }

    if (id == IDC_MW_SPR_SAVE && code == BN_CLICKED) {
        p->SaveCurrentSpriteProperties();
        p->SaveSpritesToINI();
        return 0;
    }

    if (id == IDC_MW_SPR_RELOAD && code == BN_CLICKED) {
        p->LoadSpritesFromINI();
        p->PopulateSpriteListView();
        p->m_nSpriteSelected = -1;
        return 0;
    }

    if (id == IDC_MW_SPR_OPENINI && code == BN_CLICKED) {
        HINSTANCE hr = ShellExecuteW(NULL, L"open", p->m_szImgIniFile, NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)hr <= 32)
          ShellExecuteW(NULL, L"open", L"notepad.exe", p->m_szImgIniFile, NULL, SW_SHOWNORMAL);
        return 0;
    }

    if (id == IDC_MW_SPR_IMG_BROWSE && code == BN_CLICKED) {
        if (p->m_nSpriteSelected < 0) return 0;
        SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        IFileOpenDialog* pDlg = NULL;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg));
        if (SUCCEEDED(hr) && pDlg) {
            COMDLG_FILTERSPEC filters[] = {
                { L"Image Files", L"*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.dds;*.gif" },
                { L"All Files", L"*.*" }
            };
            pDlg->SetFileTypes(2, filters);
            pDlg->SetTitle(L"Select Sprite Image");
            if (SUCCEEDED(pDlg->Show(hWnd))) {
                IShellItem* pItem = NULL;
                if (SUCCEEDED(pDlg->GetResult(&pItem))) {
                    LPWSTR pPath = NULL;
                    if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pPath))) {
                        std::wstring relPath = MakeRelativeSpritePath(pPath, p->m_szContentBasePath, p->m_szMilkdrop2Path);
                        wcscpy_s(p->m_spriteEntries[p->m_nSpriteSelected].szImg, relPath.c_str());
                        SetDlgItemTextW(hWnd, IDC_MW_SPR_IMG_PATH, relPath.c_str());
                        // Update thumbnail in ListView
                        HBITMAP hBmp = p->LoadThumbnailWIC(p->m_spriteEntries[p->m_nSpriteSelected].szImg, 32, 32);
                        if (hBmp && p->m_hSpriteImageList) {
                            int imgIdx = ImageList_Add((HIMAGELIST)p->m_hSpriteImageList, hBmp, NULL);
                            DeleteObject(hBmp);
                            LVITEMW lvi = {};
                            lvi.mask = LVIF_IMAGE;
                            lvi.iItem = p->m_nSpriteSelected;
                            lvi.iImage = imgIdx;
                            SendMessageW(p->m_hSpriteList, LVM_SETITEMW, 0, (LPARAM)&lvi);
                        }
                        CoTaskMemFree(pPath);
                    }
                    pItem->Release();
                }
            }
            pDlg->Release();
        }
        if (m_bOnTop) SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;
    }

    return -1;
}

// ─── Notify (ListView selection) ────────────────────────────────────────

LRESULT SpritesWindow::DoNotify(HWND hWnd, NMHDR* pnm) {
    if (pnm->idFrom == IDC_MW_SPR_LIST && pnm->code == LVN_ITEMCHANGED) {
        NMLISTVIEW* pnmv = (NMLISTVIEW*)pnm;
        if ((pnmv->uNewState & LVIS_SELECTED) && !(pnmv->uOldState & LVIS_SELECTED)) {
            Engine* p = m_pEngine;
            if (p->m_nSpriteSelected >= 0)
                p->SaveCurrentSpriteProperties();
            p->m_nSpriteSelected = pnmv->iItem;
            p->UpdateSpriteProperties(pnmv->iItem);
        }
    }
    return -1;
}

// ─── Resize ─────────────────────────────────────────────────────────────

void SpritesWindow::OnResize() {
    // Controls are fixed-position; only the ListView could resize but
    // the rebuild-on-resize pattern handles this automatically.
}

// ─── Destroy ────────────────────────────────────────────────────────────

void SpritesWindow::DoDestroy() {
    Engine* p = m_pEngine;
    // Save current properties before closing
    if (p->m_nSpriteSelected >= 0)
        p->SaveCurrentSpriteProperties();
    // Clean up sprite list and image list (same pattern as SettingsWindow)
    if (p->m_hSpriteImageList) {
        ImageList_Destroy((HIMAGELIST)p->m_hSpriteImageList);
        p->m_hSpriteImageList = NULL;
    }
    p->m_hSpriteList = NULL;
}

} // namespace mdrop
