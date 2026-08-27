// engine_video_effects_ui.cpp — Video Effects window (3 tabs: Transform, Effects, Audio)
//
// Real-time parameter editing for video input compositing effects.

#include "engine.h"
#include "tool_window.h"
#include "engine_helpers.h"
#include "audio_profile_store.h"
#include "utility.h"
#include <shlwapi.h>
#include <commctrl.h>

namespace mdrop {

void Engine::MarkVideoFXSaved() {
  m_videoFXSaved = m_videoFX;
}

bool Engine::IsVideoFXDirty() const {
  if (m_szCurrentVFXProfile[0] == 0) return false;   // nothing to compare against
  if (!m_videoFX.Equals(m_videoFXSaved)) return true;
  return false;
}

VideoEffectsWindow::VideoEffectsWindow(Engine* pEngine)
    : ToolWindow(pEngine, 420, 640) {}

// ---------------------------------------------------------------------------
// Open / Close wrappers on Engine
// ---------------------------------------------------------------------------
void Engine::OpenVideoEffectsWindow()
{
    if (!m_pVideoEffectsWindow)
        m_pVideoEffectsWindow = new VideoEffectsWindow(this);
    m_pVideoEffectsWindow->Open();
}

void Engine::CloseVideoEffectsWindow()
{
    if (m_pVideoEffectsWindow) {
        m_pVideoEffectsWindow->Close();
        delete m_pVideoEffectsWindow;
        m_pVideoEffectsWindow = nullptr;
    }
}

// ---------------------------------------------------------------------------
// DoBuildControls — tabbed layout with 3 pages
// ---------------------------------------------------------------------------
void VideoEffectsWindow::DoBuildControls()
{
    auto base = BuildBaseControls();
    int y = base.y, lineH = base.lineH, gap = base.gap;
    int x = base.x, rw = base.rw;

    // Profile save/load buttons (above tabs), and beside them the one setting
    // that governs when a profile is written without being asked. It used to
    // live in the profile picker, two windows deep and below the bottom edge
    // of the size older installs had saved.
    int profileBtnW = MulDiv(100, lineH, 26);
    CreateBtn(m_hWnd, L"Save Profile...", IDC_MW_VFX_SAVE_PROFILE, x, y, profileBtnW, lineH, m_hFont);
    CreateBtn(m_hWnd, L"Profiles...", IDC_MW_VFX_LOAD_PROFILE, x + profileBtnW + 8, y, profileBtnW, lineH, m_hFont);
    {
        int chkX = x + (profileBtnW + 8) * 2;
        CreateCheck(m_hWnd, L"Save on exit", IDC_MW_VFX_SAVE_ON_EXIT, chkX, y, rw - (chkX - x), lineH,
                    m_hFont, m_pEngine->m_bEnableVFXSaveOnExit);
    }
    y += lineH + gap;

    RECT rc;
    GetClientRect(m_hWnd, &rc);
    int clientH = rc.bottom;

    // Tab control
    static const wchar_t* tabNames[] = { L"Transform", L"Effects", L"Audio" };
    RECT rcTab = BuildTabControl(IDC_MW_VFX_TAB, tabNames, 3,
                                 x, y, rw, clientH - y - gap);
    int tabX = rcTab.left;
    int tabY = rcTab.top;
    int tabRW = rcTab.right - rcTab.left;

    BuildTransformPage(tabX, tabY, tabRW, lineH, gap);
    BuildEffectsPage(tabX, tabY, tabRW, lineH, gap);
    BuildAudioPage(tabX, tabY, tabRW, lineH, gap);

    SelectInitialTab();

    // Controls were just recreated, so the property the painter reads is gone.
    // Force it to be recomputed rather than assuming clean.
    HWND hBtn = GetDlgItem(m_hWnd, IDC_MW_VFX_SAVE_PROFILE);
    if (hBtn) SetPropW(hBtn, L"AccentBtn", (HANDLE)(intptr_t)!m_pEngine->IsVideoFXDirty());
    RefreshDirtyIndicator();
}

// ---------------------------------------------------------------------------
// DoMessage — repaint the Save button when parameters change from elsewhere
// (IPC, the profile picker), not just from this window's own controls.
// ---------------------------------------------------------------------------
LRESULT VideoEffectsWindow::DoMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_MW_VFX_DIRTY_CHANGED) {
        RefreshDirtyIndicator();
        return 0;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// SaveFX — record a parameter edit
//
// Despite the name this does NOT write a profile. It persists live state to
// videofx/current.json and updates the Save button's red state. Writing a
// named profile is an explicit act, on the Save Profile button.
// ---------------------------------------------------------------------------
void VideoEffectsWindow::SaveFX()
{
    m_pEngine->OnVideoFXChanged();
    RefreshDirtyIndicator();
}

// ---------------------------------------------------------------------------
// RefreshDirtyIndicator — red Save Profile button when there is something to save
//
// The colour is carried on the control as a window property, which is how the
// shared owner-draw painter already distinguishes pin buttons, checkboxes and
// radios from plain buttons.
// ---------------------------------------------------------------------------
void VideoEffectsWindow::RefreshDirtyIndicator()
{
    HWND hBtn = GetDlgItem(m_hWnd, IDC_MW_VFX_SAVE_PROFILE);
    if (!hBtn) return;

    const bool bDirty = m_pEngine->IsVideoFXDirty();
    const bool bWas = (bool)(intptr_t)GetPropW(hBtn, L"AccentBtn");
    if (bDirty == bWas) return;   // no change, no repaint

    SetPropW(hBtn, L"AccentBtn", (HANDLE)(intptr_t)bDirty);

    // A trailing asterisk alongside the colour, so the state still reads in
    // a light theme and for anyone who cannot pick out the red. The label
    // stays short -- the button is a fixed width and a profile name would
    // clip at larger font sizes.
    SetWindowTextW(hBtn, bDirty ? L"Save Profile *" : L"Save Profile...");
    InvalidateRect(hBtn, NULL, TRUE);
}

// ---------------------------------------------------------------------------
// Transform tab (page 0)
// ---------------------------------------------------------------------------
void VideoEffectsWindow::BuildTransformPage(int x, int y, int rw, int lineH, int gap)
{
    HWND hw = m_hWnd;
    HFONT hFont = m_hFont;
    HFONT hFontBold = m_hFontBold;
    auto& fx = m_pEngine->m_videoFX;

    #define P_TC(page, expr) TrackPageControl(page, (expr))
    int slLbl = MulDiv(80, lineH, 26);
    int valW  = MulDiv(56, lineH, 26);
    int slW   = rw - slLbl - 4 - valW;

    P_TC(0, CreateLabel(hw, L"Transform", x, y, rw, lineH, hFontBold));
    y += lineH + gap;

    // Position X: -100..100 mapped to -1..1
    P_TC(0, CreateLabel(hw, L"Position X:", x, y, slLbl, lineH, hFont));
    P_TC(0, CreateSlider(hw, IDC_MW_VFX_POSX, x + slLbl + 4, y, slW, lineH, -100, 100, (int)(fx.posX * 100)));
    { wchar_t b[32]; swprintf(b, 32, L"%.2f", fx.posX);
    P_TC(0, CreateLabel(hw, b, x + rw - valW, y, valW, lineH, hFont)); }
    SetWindowLongPtrW(m_childCtrls.back(), GWLP_ID, IDC_MW_VFX_POSX_LBL);
    y += lineH + gap;

    // Position Y
    P_TC(0, CreateLabel(hw, L"Position Y:", x, y, slLbl, lineH, hFont));
    P_TC(0, CreateSlider(hw, IDC_MW_VFX_POSY, x + slLbl + 4, y, slW, lineH, -100, 100, (int)(fx.posY * 100)));
    { wchar_t b[32]; swprintf(b, 32, L"%.2f", fx.posY);
    P_TC(0, CreateLabel(hw, b, x + rw - valW, y, valW, lineH, hFont)); }
    SetWindowLongPtrW(m_childCtrls.back(), GWLP_ID, IDC_MW_VFX_POSY_LBL);
    y += lineH + gap;

    // Scale: 10..500 mapped to 0.1..5.0
    P_TC(0, CreateLabel(hw, L"Scale:", x, y, slLbl, lineH, hFont));
    P_TC(0, CreateSlider(hw, IDC_MW_VFX_SCALE, x + slLbl + 4, y, slW, lineH, 10, 500, (int)(fx.scale * 100)));
    { wchar_t b[32]; swprintf(b, 32, L"%.2f", fx.scale);
    P_TC(0, CreateLabel(hw, b, x + rw - valW, y, valW, lineH, hFont)); }
    SetWindowLongPtrW(m_childCtrls.back(), GWLP_ID, IDC_MW_VFX_SCALE_LBL);
    y += lineH + gap;

    // Rotation: 0..360
    P_TC(0, CreateLabel(hw, L"Rotation:", x, y, slLbl, lineH, hFont));
    P_TC(0, CreateSlider(hw, IDC_MW_VFX_ROTATION, x + slLbl + 4, y, slW, lineH, 0, 360, (int)fx.rotation));
    { wchar_t b[32]; swprintf(b, 32, L"%d\xB0", (int)fx.rotation);
    P_TC(0, CreateLabel(hw, b, x + rw - valW, y, valW, lineH, hFont)); }
    SetWindowLongPtrW(m_childCtrls.back(), GWLP_ID, IDC_MW_VFX_ROTATION_LBL);
    y += lineH + gap;

    // Mirror H / V
    int halfW = (rw - 8) / 2;
    P_TC(0, CreateCheck(hw, L"Mirror H", IDC_MW_VFX_MIRRORH, x, y, halfW, lineH, hFont, fx.mirrorH));
    P_TC(0, CreateCheck(hw, L"Mirror V", IDC_MW_VFX_MIRRORV, x + halfW + 8, y, halfW, lineH, hFont, fx.mirrorV));
    y += lineH + gap;

    // Blend mode combo
    P_TC(0, CreateLabel(hw, L"Blend:", x, y, slLbl, lineH, hFont));
    HWND hBlend = CreateWindowExW(0, L"COMBOBOX", NULL,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
        x + slLbl + 4, y, rw - slLbl - 4, lineH * 8, hw,
        (HMENU)(INT_PTR)IDC_MW_VFX_BLENDMODE, GetModuleHandle(NULL), NULL);
    if (hBlend && hFont) SendMessage(hBlend, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hBlend, CB_ADDSTRING, 0, (LPARAM)L"Alpha");
    SendMessageW(hBlend, CB_ADDSTRING, 0, (LPARAM)L"Additive");
    SendMessageW(hBlend, CB_ADDSTRING, 0, (LPARAM)L"Multiply");
    SendMessageW(hBlend, CB_ADDSTRING, 0, (LPARAM)L"Screen");
    SendMessageW(hBlend, CB_ADDSTRING, 0, (LPARAM)L"Overlay");
    SendMessageW(hBlend, CB_ADDSTRING, 0, (LPARAM)L"Difference");
    SendMessage(hBlend, CB_SETCURSEL, fx.blendMode, 0);
    P_TC(0, hBlend);
    y += lineH + gap;

    // Reset. Wider than the 100 units the other buttons use: "Reset Transform"
    // is the longest label on any of these and clipped its own last character.
    int btnW = MulDiv(140, lineH, 26);
    P_TC(0, CreateBtn(hw, L"Reset Transform", IDC_MW_VFX_RESET_XFORM, x, y, btnW, lineH, hFont));
    #undef P_TC
}

// ---------------------------------------------------------------------------
// Effects tab (page 1)
// ---------------------------------------------------------------------------
void VideoEffectsWindow::BuildEffectsPage(int x, int y, int rw, int lineH, int gap)
{
    HWND hw = m_hWnd;
    HFONT hFont = m_hFont;
    HFONT hFontBold = m_hFontBold;
    auto& fx = m_pEngine->m_videoFX;

    #define P_TC(page, expr) TrackPageControl(page, (expr))
    int slLbl = MulDiv(90, lineH, 26);
    int valW  = MulDiv(56, lineH, 26);
    int slW   = rw - slLbl - 4 - valW;

    P_TC(1, CreateLabel(hw, L"Color & Effects", x, y, rw, lineH, hFontBold));
    y += lineH + gap;

    // Tint R: 0..200 → 0.0..2.0
    P_TC(1, CreateLabel(hw, L"Tint Red:", x, y, slLbl, lineH, hFont));
    P_TC(1, CreateSlider(hw, IDC_MW_VFX_TINTR, x + slLbl + 4, y, slW, lineH, 0, 200, (int)(fx.tintR * 100)));
    { wchar_t b[32]; swprintf(b, 32, L"%.2f", fx.tintR);
    P_TC(1, CreateLabel(hw, b, x + rw - valW, y, valW, lineH, hFont)); }
    SetWindowLongPtrW(m_childCtrls.back(), GWLP_ID, IDC_MW_VFX_TINTR_LBL);
    y += lineH + gap;

    // Tint G
    P_TC(1, CreateLabel(hw, L"Tint Green:", x, y, slLbl, lineH, hFont));
    P_TC(1, CreateSlider(hw, IDC_MW_VFX_TINTG, x + slLbl + 4, y, slW, lineH, 0, 200, (int)(fx.tintG * 100)));
    { wchar_t b[32]; swprintf(b, 32, L"%.2f", fx.tintG);
    P_TC(1, CreateLabel(hw, b, x + rw - valW, y, valW, lineH, hFont)); }
    SetWindowLongPtrW(m_childCtrls.back(), GWLP_ID, IDC_MW_VFX_TINTG_LBL);
    y += lineH + gap;

    // Tint B
    P_TC(1, CreateLabel(hw, L"Tint Blue:", x, y, slLbl, lineH, hFont));
    P_TC(1, CreateSlider(hw, IDC_MW_VFX_TINTB, x + slLbl + 4, y, slW, lineH, 0, 200, (int)(fx.tintB * 100)));
    { wchar_t b[32]; swprintf(b, 32, L"%.2f", fx.tintB);
    P_TC(1, CreateLabel(hw, b, x + rw - valW, y, valW, lineH, hFont)); }
    SetWindowLongPtrW(m_childCtrls.back(), GWLP_ID, IDC_MW_VFX_TINTB_LBL);
    y += lineH + gap;

    // Brightness: -100..100 → -1..1
    P_TC(1, CreateLabel(hw, L"Brightness:", x, y, slLbl, lineH, hFont));
    P_TC(1, CreateSlider(hw, IDC_MW_VFX_BRIGHTNESS, x + slLbl + 4, y, slW, lineH, -100, 100, (int)(fx.brightness * 100)));
    { wchar_t b[32]; swprintf(b, 32, L"%.2f", fx.brightness);
    P_TC(1, CreateLabel(hw, b, x + rw - valW, y, valW, lineH, hFont)); }
    SetWindowLongPtrW(m_childCtrls.back(), GWLP_ID, IDC_MW_VFX_BRIGHTNESS_LBL);
    y += lineH + gap;

    // Contrast: 0..300 → 0..3
    P_TC(1, CreateLabel(hw, L"Contrast:", x, y, slLbl, lineH, hFont));
    P_TC(1, CreateSlider(hw, IDC_MW_VFX_CONTRAST, x + slLbl + 4, y, slW, lineH, 0, 300, (int)(fx.contrast * 100)));
    { wchar_t b[32]; swprintf(b, 32, L"%.2f", fx.contrast);
    P_TC(1, CreateLabel(hw, b, x + rw - valW, y, valW, lineH, hFont)); }
    SetWindowLongPtrW(m_childCtrls.back(), GWLP_ID, IDC_MW_VFX_CONTRAST_LBL);
    y += lineH + gap;

    // Saturation: 0..300 → 0..3
    P_TC(1, CreateLabel(hw, L"Saturation:", x, y, slLbl, lineH, hFont));
    P_TC(1, CreateSlider(hw, IDC_MW_VFX_SATURATION, x + slLbl + 4, y, slW, lineH, 0, 300, (int)(fx.saturation * 100)));
    { wchar_t b[32]; swprintf(b, 32, L"%.2f", fx.saturation);
    P_TC(1, CreateLabel(hw, b, x + rw - valW, y, valW, lineH, hFont)); }
    SetWindowLongPtrW(m_childCtrls.back(), GWLP_ID, IDC_MW_VFX_SATURATION_LBL);
    y += lineH + gap;

    // Hue Shift: 0..360
    P_TC(1, CreateLabel(hw, L"Hue Shift:", x, y, slLbl, lineH, hFont));
    P_TC(1, CreateSlider(hw, IDC_MW_VFX_HUESHIFT, x + slLbl + 4, y, slW, lineH, 0, 360, (int)fx.hueShift));
    { wchar_t b[32]; swprintf(b, 32, L"%d\xB0", (int)fx.hueShift);
    P_TC(1, CreateLabel(hw, b, x + rw - valW, y, valW, lineH, hFont)); }
    SetWindowLongPtrW(m_childCtrls.back(), GWLP_ID, IDC_MW_VFX_HUESHIFT_LBL);
    y += lineH + gap;

    // Invert + Edge Detect checkboxes
    int halfW = (rw - 8) / 2;
    P_TC(1, CreateCheck(hw, L"Invert", IDC_MW_VFX_INVERT, x, y, halfW, lineH, hFont, fx.invert));
    P_TC(1, CreateCheck(hw, L"Edge Detect", IDC_MW_VFX_EDGEDETECT, x + halfW + 8, y, halfW, lineH, hFont, fx.edgeDetect));
    y += lineH + gap;

    // Pixelation: 0..100 → 0..1
    P_TC(1, CreateLabel(hw, L"Pixelation:", x, y, slLbl, lineH, hFont));
    P_TC(1, CreateSlider(hw, IDC_MW_VFX_PIXELATION, x + slLbl + 4, y, slW, lineH, 0, 100, (int)(fx.pixelation * 100)));
    { wchar_t b[32]; swprintf(b, 32, L"%.2f", fx.pixelation);
    P_TC(1, CreateLabel(hw, b, x + rw - valW, y, valW, lineH, hFont)); }
    SetWindowLongPtrW(m_childCtrls.back(), GWLP_ID, IDC_MW_VFX_PIXELATION_LBL);
    y += lineH + gap;

    // Chromatic: 0..500 → 0..0.05 (slider value / 10000)
    P_TC(1, CreateLabel(hw, L"Chromatic:", x, y, slLbl, lineH, hFont));
    P_TC(1, CreateSlider(hw, IDC_MW_VFX_CHROMATIC, x + slLbl + 4, y, slW, lineH, 0, 500, (int)(fx.chromatic * 10000)));
    { wchar_t b[32]; swprintf(b, 32, L"%.4f", fx.chromatic);
    P_TC(1, CreateLabel(hw, b, x + rw - valW, y, valW, lineH, hFont)); }
    SetWindowLongPtrW(m_childCtrls.back(), GWLP_ID, IDC_MW_VFX_CHROMATIC_LBL);
    y += lineH + gap;

    // Reset
    int btnW = MulDiv(100, lineH, 26);
    P_TC(1, CreateBtn(hw, L"Reset Effects", IDC_MW_VFX_RESET_EFFECTS, x, y, btnW, lineH, hFont));
    #undef P_TC
}

// ---------------------------------------------------------------------------
// Audio tab (page 2) — 7 audio-reactive parameter rows
// ---------------------------------------------------------------------------
void VideoEffectsWindow::BuildAudioPage(int x, int y, int rw, int lineH, int gap)
{
    HWND hw = m_hWnd;
    HFONT hFont = m_hFont;
    HFONT hFontBold = m_hFontBold;
    auto& fx = m_pEngine->m_videoFX;

    #define P_TC(page, expr) TrackPageControl(page, (expr))

    P_TC(2, CreateLabel(hw, L"Audio Reactive", x, y, rw, lineH, hFontBold));
    y += lineH + gap;

    // Helper: each row = label + source combo + intensity slider
    struct ARRow {
        const wchar_t* label;
        int comboID, sliderID;
        AudioLink* link;
    };
    ARRow rows[] = {
        { L"Position X", IDC_MW_VFX_AR_POSX_SRC, IDC_MW_VFX_AR_POSX_INT, &fx.arPosX },
        { L"Position Y", IDC_MW_VFX_AR_POSY_SRC, IDC_MW_VFX_AR_POSY_INT, &fx.arPosY },
        { L"Scale",      IDC_MW_VFX_AR_SCALE_SRC, IDC_MW_VFX_AR_SCALE_INT, &fx.arScale },
        { L"Rotation",   IDC_MW_VFX_AR_ROT_SRC,   IDC_MW_VFX_AR_ROT_INT,   &fx.arRotation },
        { L"Brightness", IDC_MW_VFX_AR_BRIGHT_SRC, IDC_MW_VFX_AR_BRIGHT_INT, &fx.arBrightness },
        { L"Saturation", IDC_MW_VFX_AR_SAT_SRC,   IDC_MW_VFX_AR_SAT_INT,   &fx.arSaturation },
        { L"Chromatic",  IDC_MW_VFX_AR_CHROM_SRC, IDC_MW_VFX_AR_CHROM_INT, &fx.arChromatic },
    };

    int lblW  = MulDiv(80, lineH, 26);
    int comboW = MulDiv(90, lineH, 26);
    int slX   = x + lblW + comboW + 12;
    int slW   = rw - lblW - comboW - 12;

    for (auto& r : rows) {
        P_TC(2, CreateLabel(hw, r.label, x, y, lblW, lineH, hFont));

        HWND hCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            x + lblW + 4, y, comboW, lineH * 6, hw,
            (HMENU)(INT_PTR)r.comboID, GetModuleHandle(NULL), NULL);
        if (hCombo && hFont) SendMessage(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"None");
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Bass");
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Mid");
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Treb");
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Vol");
        SendMessage(hCombo, CB_SETCURSEL, r.link->source, 0);
        P_TC(2, hCombo);

        // Intensity slider: 0..200 → 0.0..2.0
        P_TC(2, CreateSlider(hw, r.sliderID, slX, y, slW, lineH, 0, 200, (int)(r.link->intensity * 100)));
        y += lineH + gap;
    }

    // Reset
    int btnW = MulDiv(100, lineH, 26);
    P_TC(2, CreateBtn(hw, L"Reset Audio", IDC_MW_VFX_RESET_AUDIO, x, y, btnW, lineH, hFont));
    #undef P_TC
}

// ---------------------------------------------------------------------------
// DoCommand — handle buttons, checkboxes, combos
// ---------------------------------------------------------------------------
LRESULT VideoEffectsWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam)
{
    auto& fx = m_pEngine->m_videoFX;

    switch (id) {
    // ── Checkboxes ──
    case IDC_MW_VFX_MIRRORH:
        fx.mirrorH = IsChecked(id);
        SaveFX(); return 0;
    case IDC_MW_VFX_MIRRORV:
        fx.mirrorV = IsChecked(id);
        SaveFX(); return 0;
    case IDC_MW_VFX_INVERT:
        fx.invert = IsChecked(id);
        SaveFX(); return 0;
    case IDC_MW_VFX_EDGEDETECT:
        fx.edgeDetect = IsChecked(id);
        SaveFX(); return 0;

    // ── Blend mode combo ──
    case IDC_MW_VFX_BLENDMODE:
        if (code == CBN_SELCHANGE) {
            fx.blendMode = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
            SaveFX();
        }
        return 0;

    // ── Audio source combos ──
    case IDC_MW_VFX_AR_POSX_SRC:   if (code == CBN_SELCHANGE) { fx.arPosX.source       = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0); SaveFX(); } return 0;
    case IDC_MW_VFX_AR_POSY_SRC:   if (code == CBN_SELCHANGE) { fx.arPosY.source       = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0); SaveFX(); } return 0;
    case IDC_MW_VFX_AR_SCALE_SRC:  if (code == CBN_SELCHANGE) { fx.arScale.source      = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0); SaveFX(); } return 0;
    case IDC_MW_VFX_AR_ROT_SRC:    if (code == CBN_SELCHANGE) { fx.arRotation.source   = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0); SaveFX(); } return 0;
    case IDC_MW_VFX_AR_BRIGHT_SRC: if (code == CBN_SELCHANGE) { fx.arBrightness.source = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0); SaveFX(); } return 0;
    case IDC_MW_VFX_AR_SAT_SRC:    if (code == CBN_SELCHANGE) { fx.arSaturation.source = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0); SaveFX(); } return 0;
    case IDC_MW_VFX_AR_CHROM_SRC:  if (code == CBN_SELCHANGE) { fx.arChromatic.source  = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0); SaveFX(); } return 0;

    // ── Reset buttons ──
    case IDC_MW_VFX_RESET_XFORM:
        fx.posX = 0; fx.posY = 0; fx.scale = 1.0f; fx.rotation = 0;
        fx.mirrorH = false; fx.mirrorV = false; fx.blendMode = 0;
        SaveFX(); RebuildFonts(); return 0;

    case IDC_MW_VFX_RESET_EFFECTS:
        fx.tintR = 1; fx.tintG = 1; fx.tintB = 1;
        fx.brightness = 0; fx.contrast = 1.0f; fx.saturation = 1.0f;
        fx.hueShift = 0; fx.invert = false;
        fx.pixelation = 0; fx.chromatic = 0; fx.edgeDetect = false;
        SaveFX(); RebuildFonts(); return 0;

    case IDC_MW_VFX_RESET_AUDIO:
        fx.arPosX = {}; fx.arPosY = {}; fx.arScale = {}; fx.arRotation = {};
        fx.arBrightness = {}; fx.arSaturation = {}; fx.arChromatic = {};
        SaveFX(); RebuildFonts(); return 0;

    // ── Profile buttons ──
    case IDC_MW_VFX_SAVE_ON_EXIT:
        m_pEngine->m_bEnableVFXSaveOnExit = IsChecked(id);
        m_pEngine->SaveSpoutInputSettings();
        return 0;

    case IDC_MW_VFX_SAVE_PROFILE: {
        // A profile is a named entry in vfxprofiles.json, so this asks for a
        // name rather than putting up a file picker for a file that no longer
        // exists on its own.
        std::vector<std::wstring> existing;
        m_pEngine->m_vfxProfiles.Names(existing);
        std::wstring name = m_pEngine->m_szCurrentVFXProfile;
        if (!PromptForName(m_pEngine, m_hWnd, L"Save VFX Profile",
                           L"Profile name:", name, MAX_VFX_PROFILE_NAME, existing))
            return 0;

        if (m_pEngine->m_vfxProfiles.Exists(name.c_str())) {
            wchar_t msg[512];
            swprintf_s(msg, L"Profile \"%s\" already exists. Replace it?", name.c_str());
            if (MessageBoxW(m_hWnd, msg, L"Save VFX Profile", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
        }

        if (m_pEngine->SaveVideoFXProfile(name.c_str())) {
            wcscpy_s(m_pEngine->m_szCurrentVFXProfile, name.c_str());
            // What was just written becomes the baseline, so the button goes
            // back to normal until the next edit.
            m_pEngine->MarkVideoFXSaved();
            m_pEngine->SaveSpoutInputSettings();
            RefreshDirtyIndicator();
        }
        return 0;
    }

    case IDC_MW_VFX_LOAD_PROFILE:
        m_pEngine->OpenVFXProfileWindow();
        return 0;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// DoHScroll — handle all sliders
// ---------------------------------------------------------------------------
LRESULT VideoEffectsWindow::DoHScroll(HWND hWnd, int id, int pos)
{
    auto& fx = m_pEngine->m_videoFX;
    wchar_t buf[32];

    switch (id) {
    // ── Transform sliders ──
    case IDC_MW_VFX_POSX:
        fx.posX = pos / 100.0f;
        swprintf(buf, 32, L"%.2f", fx.posX);
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VFX_POSX_LBL), buf);
        SaveFX(); return 0;
    case IDC_MW_VFX_POSY:
        fx.posY = pos / 100.0f;
        swprintf(buf, 32, L"%.2f", fx.posY);
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VFX_POSY_LBL), buf);
        SaveFX(); return 0;
    case IDC_MW_VFX_SCALE:
        fx.scale = pos / 100.0f;
        swprintf(buf, 32, L"%.2f", fx.scale);
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VFX_SCALE_LBL), buf);
        SaveFX(); return 0;
    case IDC_MW_VFX_ROTATION:
        fx.rotation = (float)pos;
        swprintf(buf, 32, L"%d\xB0", pos);
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VFX_ROTATION_LBL), buf);
        SaveFX(); return 0;

    // ── Effects sliders ──
    case IDC_MW_VFX_TINTR:
        fx.tintR = pos / 100.0f;
        swprintf(buf, 32, L"%.2f", fx.tintR);
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VFX_TINTR_LBL), buf);
        SaveFX(); return 0;
    case IDC_MW_VFX_TINTG:
        fx.tintG = pos / 100.0f;
        swprintf(buf, 32, L"%.2f", fx.tintG);
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VFX_TINTG_LBL), buf);
        SaveFX(); return 0;
    case IDC_MW_VFX_TINTB:
        fx.tintB = pos / 100.0f;
        swprintf(buf, 32, L"%.2f", fx.tintB);
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VFX_TINTB_LBL), buf);
        SaveFX(); return 0;
    case IDC_MW_VFX_BRIGHTNESS:
        fx.brightness = pos / 100.0f;
        swprintf(buf, 32, L"%.2f", fx.brightness);
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VFX_BRIGHTNESS_LBL), buf);
        SaveFX(); return 0;
    case IDC_MW_VFX_CONTRAST:
        fx.contrast = pos / 100.0f;
        swprintf(buf, 32, L"%.2f", fx.contrast);
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VFX_CONTRAST_LBL), buf);
        SaveFX(); return 0;
    case IDC_MW_VFX_SATURATION:
        fx.saturation = pos / 100.0f;
        swprintf(buf, 32, L"%.2f", fx.saturation);
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VFX_SATURATION_LBL), buf);
        SaveFX(); return 0;
    case IDC_MW_VFX_HUESHIFT:
        fx.hueShift = (float)pos;
        swprintf(buf, 32, L"%d\xB0", pos);
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VFX_HUESHIFT_LBL), buf);
        SaveFX(); return 0;
    case IDC_MW_VFX_PIXELATION:
        fx.pixelation = pos / 100.0f;
        swprintf(buf, 32, L"%.2f", fx.pixelation);
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VFX_PIXELATION_LBL), buf);
        SaveFX(); return 0;
    case IDC_MW_VFX_CHROMATIC:
        fx.chromatic = pos / 10000.0f;
        swprintf(buf, 32, L"%.4f", fx.chromatic);
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VFX_CHROMATIC_LBL), buf);
        SaveFX(); return 0;

    // ── Audio intensity sliders ──
    case IDC_MW_VFX_AR_POSX_INT:   fx.arPosX.intensity       = pos / 100.0f; SaveFX(); return 0;
    case IDC_MW_VFX_AR_POSY_INT:   fx.arPosY.intensity       = pos / 100.0f; SaveFX(); return 0;
    case IDC_MW_VFX_AR_SCALE_INT:  fx.arScale.intensity      = pos / 100.0f; SaveFX(); return 0;
    case IDC_MW_VFX_AR_ROT_INT:    fx.arRotation.intensity   = pos / 100.0f; SaveFX(); return 0;
    case IDC_MW_VFX_AR_BRIGHT_INT: fx.arBrightness.intensity = pos / 100.0f; SaveFX(); return 0;
    case IDC_MW_VFX_AR_SAT_INT:    fx.arSaturation.intensity = pos / 100.0f; SaveFX(); return 0;
    case IDC_MW_VFX_AR_CHROM_INT:  fx.arChromatic.intensity  = pos / 100.0f; SaveFX(); return 0;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// DoDestroy — cleanup
// ---------------------------------------------------------------------------
void VideoEffectsWindow::DoDestroy()
{
    SaveFX();
}

// ---------------------------------------------------------------------------
// Live state <-> the profile store (vfx_profile_store.h)
//
// These two are the whole bridge. The store knows the file and the schema and
// nothing about Engine; these know Engine and nothing about the file.
//
// A profile used to carry the render tunables as well. Those knobs were the
// MD31 stand-ins and are gone, so a profile is video effects only. The store
// still reads and writes a tunable block generically and is simply never given
// one -- and its unknown-member passthrough keeps the keys in any profile a
// previous build wrote.
// ---------------------------------------------------------------------------

void Engine::InitVFXProfileStore()
{
    m_vfxProfiles.SetResourceDir(m_szMilkdrop2Path);
    // Audio profiles live beside the VFX ones and are pointed at the same
    // directory from the same place, so there is one call site to keep right.
    // The built-ins are written here rather than shipped, since Release_x64 is
    // gitignored and a file-only profile would exist on one machine only.
    AudioProfiles().SetResourceDir(m_szMilkdrop2Path);
    AudioProfiles().EnsureBuiltIns();
}

VFXProfileData Engine::CaptureVFXProfile() const
{
    VFXProfileData d;
    d.fx = m_videoFX;
    return d;
}

void Engine::ApplyVFXProfile(const VFXProfileData& d)
{
    m_videoFX = d.fx;
}

// ---------------------------------------------------------------------------
// Scoped VFX — applied because the preset asked, restored when it is left
// ---------------------------------------------------------------------------

void Engine::PushScopedVFXProfile(const std::wstring& name)
{
    if (name.empty()) return;

    // Seeded with live values so sections the stored profile does not carry
    // come back out unchanged -- same reason LoadVideoFXProfile does it.
    VFXProfileData d = CaptureVFXProfile();
    if (!m_vfxProfiles.Load(name.c_str(), d)) {
        DLOG_WARN("Scoped VFX: profile '%ls' not found; leaving state alone",
                  name.c_str());
        return;
    }

    // Only claim the scope if there is not one pending already. A scope that
    // is still active here is one whose edit has not been answered for, and
    // its baseline and name belong to that pending answer -- overwriting them
    // would restore to the wrong place, or save into the wrong profile.
    if (!m_bVFXScopeActive) {
        m_vfxScopeBaseline = CaptureVFXProfile();
        m_bVFXScopeActive  = true;
        m_bVFXScopeDirty   = false;
        wcscpy_s(m_szVFXScopeProfile, name.c_str());
    }

    // fx only. See the header: tunables would write the INI, and setting the
    // current profile would let Save-on-exit overwrite it.
    m_videoFX = d.fx;
}

void Engine::PopScopedVFXProfile()
{
    if (!m_bVFXScopeActive) return;
    if (m_bVFXScopeDirty) {
        // An edit is pending. Keep it on screen and ask, rather than undoing
        // something the user did on purpose. Raised at most once: the window
        // is only created when there is not already one open, so a run of
        // preset changes cannot stack dialogs.
        // Tested on the WINDOW, not the pointer: closing a tool window leaves
        // the object allocated with a null HWND, so a pointer check would mean
        // the prompt never appeared a second time.
        if (!m_pVFXKeepPromptWindow || !m_pVFXKeepPromptWindow->IsOpen())
            OpenVFXKeepPrompt(m_szVFXScopeProfile);
        return;
    }

    m_videoFX = m_vfxScopeBaseline.fx;
    m_bVFXScopeActive = false;
    m_szVFXScopeProfile[0] = 0;
}

void Engine::AnswerScopedVFX(bool bKeep)
{
    if (!m_bVFXScopeActive) return;

    if (bKeep && m_szVFXScopeProfile[0]) {
        // Update only what the scope actually governs. CaptureVFXProfile()
        // takes the live tunables too, and the scope never applied the
        // profile's tunables -- writing the live ones back would silently
        // rewrite parts of the profile the user never touched. So start from
        // what is stored and replace just the fx section.
        VFXProfileData d = CaptureVFXProfile();
        m_vfxProfiles.Load(m_szVFXScopeProfile, d);
        d.fx = m_videoFX;
        if (m_vfxProfiles.Save(m_szVFXScopeProfile, d))
            DLOG_INFO("Scoped VFX: kept the edit in profile '%ls'", m_szVFXScopeProfile);
    }

    m_bVFXScopeDirty = false;   // answered either way
    PopScopedVFXProfile();      // and now the baseline goes back

    // Dismiss the prompt if one is up. Answering over IPC has to leave the
    // same state as clicking, or a stale window sits there offering to decide
    // something already decided. Posted, because the window owns its thread.
    if (m_pVFXKeepPromptWindow && m_pVFXKeepPromptWindow->IsOpen()) {
        HWND h = m_pVFXKeepPromptWindow->GetHWND();
        if (h && IsWindow(h)) PostMessageW(h, WM_CLOSE, 0, 0);
    }
}

void Engine::ForceRestoreScopedVFX()
{
    if (!m_bVFXScopeActive) return;
    if (m_bVFXScopeDirty)
        DLOG_WARN("Scoped VFX: discarding an unanswered edit to '%ls' at shutdown",
                  m_szVFXScopeProfile);
    m_bVFXScopeDirty = false;
    PopScopedVFXProfile();
}

bool Engine::SaveVideoFXProfile(const wchar_t* name)
{
    return m_vfxProfiles.Save(name, CaptureVFXProfile());
}

bool Engine::LoadVideoFXProfile(const wchar_t* name)
{
    // Seeded with the live values, so sections the stored profile does not
    // carry come back out unchanged. See VFXProfileStore::Load.
    VFXProfileData d = CaptureVFXProfile();
    if (!m_vfxProfiles.Load(name, d)) return false;

    ApplyVFXProfile(d);
    wcscpy_s(m_szCurrentVFXProfile, name);
    return true;
}

// Program exit. Writes the loaded profile back only if the user asked for that
// with "Save on exit"; nothing to do when no profile is loaded, or when it
// already matches what is on screen.
void Engine::SaveVideoFXOnExit()
{
    // Put the pre-preset state back BEFORE deciding what to save. Otherwise a
    // scoped profile that happened to be on screen at shutdown would be what
    // gets written into whatever profile is current.
    ForceRestoreScopedVFX();

    if (!m_bEnableVFXSaveOnExit) return;
    if (m_szCurrentVFXProfile[0] == 0) return;
    if (!IsVideoFXDirty()) return;

    if (SaveVideoFXProfile(m_szCurrentVFXProfile)) {
        MarkVideoFXSaved();
        DLOG_INFO("VideoFX: saved profile on exit (Save on exit is enabled)");
    }
}

// ---------------------------------------------------------------------------
// OnVideoFXChanged — call after ANY edit to p->m_videoFX.
//
// Deliberately writes nothing. Editing a parameter used to rewrite a live
// state file, which is how values reappeared after a restart nobody had asked
// to restore anything. All this does now is keep the Save button's red state
// in step with the dirty flag.
// ---------------------------------------------------------------------------
void Engine::OnVideoFXChanged()
{
    // An edit made while a preset's profile is on screen belongs to that
    // profile. Mark it, so the restore stops being automatic and waits for a
    // Keep/Discard answer instead of silently reverting a deliberate change.
    //
    // Safe to do here: the scoped apply and restore assign m_videoFX directly
    // and do NOT come through this function, so a scope cannot mark itself.
    if (m_bVFXScopeActive) m_bVFXScopeDirty = true;

    // The window runs its own thread, so invalidate by posting, never by
    // drawing from here.
    if (m_pVideoEffectsWindow) {
        HWND h = m_pVideoEffectsWindow->GetHWND();
        if (h && IsWindow(h))
            PostMessageW(h, WM_MW_VFX_DIRTY_CHANGED, 0, 0);
    }
}

} // namespace mdrop
