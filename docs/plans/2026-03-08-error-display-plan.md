# Error Display Enhancement Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make error messages configurable (duration, font, size, position, color) with a "LOUD" mode for unmissable alerts, controlled from a settings modal on the About tab.

**Architecture:** Extend the overlay notification system with per-notification font size, corner, and LOUD mode fields. Add Engine members for all settings, persisted via INI. A modal dialog on the About tab configures both Normal and LOUD modes with test buttons.

**Tech Stack:** Win32 GDI (overlay), Win32 dialog (settings modal), INI persistence

---

### Task 1: Add Engine members and INI persistence for error display settings

**Files:**
- Modify: `src/mDropDX12/engine.h:607` (after `m_HideNotificationsWhenRemoteActive`)
- Modify: `src/mDropDX12/engine.cpp` (MyReadConfig ~line 1437, MyWriteConfig ~line 1710)

**Step 1: Add member variables to engine.h**

After line 607 (`bool m_HideNotificationsWhenRemoteActive = false;`), add:

```cpp
  // Error Display Settings — Normal mode
  float   m_ErrorDuration       = 8.0f;     // seconds
  int     m_ErrorFontSize       = 20;        // pixels (0 = auto)
  int     m_ErrorCorner         = 0;         // 0=UR, 1=UL, 2=LR, 3=LL
  int     m_ErrorColorR         = 255;
  int     m_ErrorColorG         = 255;
  int     m_ErrorColorB         = 255;
  wchar_t m_szErrorFontFace[128] = L"Segoe UI";

  // Error Display Settings — LOUD mode
  float   m_LoudDuration        = 30.0f;
  int     m_LoudFontSize        = 0;         // 0 = auto (window height / 6)
  int     m_LoudColorR1         = 255;
  int     m_LoudColorG1         = 50;
  int     m_LoudColorB1         = 50;
  int     m_LoudColorR2         = 255;
  int     m_LoudColorG2         = 255;
  int     m_LoudColorB2         = 50;
  int     m_LoudPulseSpeed      = 2;         // cycles per second
```

**Step 2: Add AddLoudError declaration to engine.h**

After `AddError` declaration (line 808), add:

```cpp
  void AddLoudError(wchar_t* szMsg);
```

**Step 3: Add INI reads in MyReadConfig (engine.cpp)**

After the `m_HideNotificationsWhenRemoteActive` read (around line 1437), add reads following the existing pattern:

```cpp
  m_ErrorDuration   = GetPrivateProfileFloatW(L"Milkwave", L"ErrorDuration", m_ErrorDuration, pIni);
  m_ErrorFontSize   = GetPrivateProfileIntW(L"Milkwave", L"ErrorFontSize", m_ErrorFontSize, pIni);
  m_ErrorCorner     = GetPrivateProfileIntW(L"Milkwave", L"ErrorCorner", m_ErrorCorner, pIni);
  m_ErrorColorR     = GetPrivateProfileIntW(L"Milkwave", L"ErrorColorR", m_ErrorColorR, pIni);
  m_ErrorColorG     = GetPrivateProfileIntW(L"Milkwave", L"ErrorColorG", m_ErrorColorG, pIni);
  m_ErrorColorB     = GetPrivateProfileIntW(L"Milkwave", L"ErrorColorB", m_ErrorColorB, pIni);
  GetPrivateProfileStringW(L"Milkwave", L"ErrorFontFace", m_szErrorFontFace, m_szErrorFontFace, _countof(m_szErrorFontFace), pIni);
  m_LoudDuration    = GetPrivateProfileFloatW(L"Milkwave", L"LoudDuration", m_LoudDuration, pIni);
  m_LoudFontSize    = GetPrivateProfileIntW(L"Milkwave", L"LoudFontSize", m_LoudFontSize, pIni);
  m_LoudColorR1     = GetPrivateProfileIntW(L"Milkwave", L"LoudColorR1", m_LoudColorR1, pIni);
  m_LoudColorG1     = GetPrivateProfileIntW(L"Milkwave", L"LoudColorG1", m_LoudColorG1, pIni);
  m_LoudColorB1     = GetPrivateProfileIntW(L"Milkwave", L"LoudColorB1", m_LoudColorB1, pIni);
  m_LoudColorR2     = GetPrivateProfileIntW(L"Milkwave", L"LoudColorR2", m_LoudColorR2, pIni);
  m_LoudColorG2     = GetPrivateProfileIntW(L"Milkwave", L"LoudColorG2", m_LoudColorG2, pIni);
  m_LoudColorB2     = GetPrivateProfileIntW(L"Milkwave", L"LoudColorB2", m_LoudColorB2, pIni);
  m_LoudPulseSpeed  = GetPrivateProfileIntW(L"Milkwave", L"LoudPulseSpeed", m_LoudPulseSpeed, pIni);
```

**Step 4: Add INI writes in MyWriteConfig (engine.cpp)**

After the corresponding writes section (around line 1710), add:

```cpp
  { wchar_t b[32]; swprintf(b, 32, L"%.1f", m_ErrorDuration);
    WritePrivateProfileStringW(L"Milkwave", L"ErrorDuration", b, pIni); }
  WritePrivateProfileIntW(m_ErrorFontSize, L"ErrorFontSize", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_ErrorCorner, L"ErrorCorner", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_ErrorColorR, L"ErrorColorR", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_ErrorColorG, L"ErrorColorG", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_ErrorColorB, L"ErrorColorB", pIni, L"Milkwave");
  WritePrivateProfileStringW(L"Milkwave", L"ErrorFontFace", m_szErrorFontFace, pIni);
  { wchar_t b[32]; swprintf(b, 32, L"%.1f", m_LoudDuration);
    WritePrivateProfileStringW(L"Milkwave", L"LoudDuration", b, pIni); }
  WritePrivateProfileIntW(m_LoudFontSize, L"LoudFontSize", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_LoudColorR1, L"LoudColorR1", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_LoudColorG1, L"LoudColorG1", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_LoudColorB1, L"LoudColorB1", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_LoudColorR2, L"LoudColorR2", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_LoudColorG2, L"LoudColorG2", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_LoudColorB2, L"LoudColorB2", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_LoudPulseSpeed, L"LoudPulseSpeed", pIni, L"Milkwave");
```

**Step 5: Build**

Run: `powershell -ExecutionPolicy Bypass -File build.ps1 Release x64`
Expected: 0 warnings, 0 errors

**Step 6: Commit**

```
git add src/mDropDX12/engine.h src/mDropDX12/engine.cpp
git commit -m "Add error display settings members and INI persistence"
```

---

### Task 2: Wire error display settings into overlay notifications

**Files:**
- Modify: `src/mDropDX12/overlay.h:40-44` (OverlayNotification struct)
- Modify: `src/mDropDX12/engine_rendering.cpp:338-540` (AddError, AddLoudError, marshalling)

**Step 1: Extend OverlayNotification struct in overlay.h**

Replace the existing struct (lines 40-44) with:

```cpp
struct OverlayNotification {
    wchar_t text[256];
    DWORD   color;      // 0x00RRGGBB
    int     corner;     // MTO_UPPER_RIGHT=0, MTO_UPPER_LEFT=1, MTO_LOWER_RIGHT=2, MTO_LOWER_LEFT=3
    int     fontSize;   // 0 = use default
    bool    isLoud;     // true = LOUD mode (centered, large, pulsing)
    DWORD   loudColor2; // second pulse color (0x00RRGGBB)
    int     pulseSpeed; // cycles per second
    wchar_t fontFace[128]; // font face name (empty = default)
};
```

**Step 2: Update AddError to use configurable duration**

In `engine_rendering.cpp` line 338, the `AddError` function takes `fDuration` as a parameter — callers pass their own duration, so no change needed to the function itself. But we need to update call sites that hardcode short durations.

Search for `AddError(` calls across the codebase and change any that pass `3.0f` or `2.5f` durations to use `m_ErrorDuration` instead. Leave calls with intentionally different durations (like `m_SongInfoDisplaySeconds`) as-is.

**Step 3: Implement AddLoudError in engine_rendering.cpp**

After the `AddError` function (around line 353), add:

```cpp
void Engine::AddLoudError(wchar_t* szMsg) {
  DebugLogW(szMsg, LOG_WARN);
  ErrorMsg x;
  x.msg = szMsg;
  x.birthTime = GetTime();
  x.expireTime = GetTime() + m_LoudDuration;
  x.category = ERR_MISC;
  x.bBold = true;
  x.bSentToRemote = false;
  x.color = 0xFFFFFFFF; // sentinel for LOUD mode
  m_errors.push_back(x);
}
```

**Step 4: Update error marshalling to overlay**

In the notification marshalling loop (lines 521-540), update the non-BOTTOM_EXTRA branch to populate the new fields:

```cpp
  else if (!m_errors[i].bSentToRemote || !m_HideNotificationsWhenRemoteActive) {
    auto& n = od.notifications[od.nNotifications++];
    swprintf(n.text, 256, L"%s ", m_errors[i].msg.c_str());

    bool isLoud = (m_errors[i].color == 0xFFFFFFFF);
    n.isLoud = isLoud;
    if (isLoud) {
      n.color = ((DWORD)m_LoudColorR1 << 16) | ((DWORD)m_LoudColorG1 << 8) | (DWORD)m_LoudColorB1;
      n.loudColor2 = ((DWORD)m_LoudColorR2 << 16) | ((DWORD)m_LoudColorG2 << 8) | (DWORD)m_LoudColorB2;
      n.pulseSpeed = m_LoudPulseSpeed;
      n.fontSize = m_LoudFontSize;
      n.corner = -1; // centered
      n.fontFace[0] = 0;
    } else {
      n.color = m_errors[i].color ? (m_errors[i].color & 0x00FFFFFF) :
        ((DWORD)m_ErrorColorR << 16) | ((DWORD)m_ErrorColorG << 8) | (DWORD)m_ErrorColorB;
      n.loudColor2 = 0;
      n.pulseSpeed = 0;
      n.fontSize = m_ErrorFontSize;
      n.corner = m_ErrorCorner;
      wcscpy_s(n.fontFace, m_szErrorFontFace);
      n.isLoud = false;
    }
  }
```

**Step 5: Build**

Run: `powershell -ExecutionPolicy Bypass -File build.ps1 Release x64`
Expected: 0 warnings, 0 errors

**Step 6: Commit**

```
git add src/mDropDX12/overlay.h src/mDropDX12/engine_rendering.cpp
git commit -m "Wire error display settings into overlay notifications"
```

---

### Task 3: Implement LOUD and configurable rendering in overlay

**Files:**
- Modify: `src/mDropDX12/overlay.cpp:220-494` (font creation, DrawShadowText, notification rendering)

**Step 1: Add a DrawCenteredText helper function**

After the existing `DrawShadowText` function (around line 367), add a new function for LOUD centered rendering:

```cpp
void COverlayThread::DrawCenteredLoudText(const wchar_t* text, int fontSize,
                                           DWORD color1, DWORD color2, int pulseSpeed,
                                           int w, int h) {
    if (!m_hMemDC || !text || !text[0]) return;

    // Calculate pulse color
    DWORD drawColor = color1;
    if (pulseSpeed > 0 && color2 != 0) {
        double t = (double)GetTickCount64() / 1000.0;
        float s = (sinf((float)(t * pulseSpeed * 2.0 * 3.14159)) + 1.0f) * 0.5f; // 0..1
        BYTE r = (BYTE)((1 - s) * ((color1 >> 16) & 0xFF) + s * ((color2 >> 16) & 0xFF));
        BYTE g = (BYTE)((1 - s) * ((color1 >> 8) & 0xFF) + s * ((color2 >> 8) & 0xFF));
        BYTE b = (BYTE)((1 - s) * (color1 & 0xFF) + s * (color2 & 0xFF));
        drawColor = ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
    }

    // Create large font
    int fs = (fontSize > 0) ? fontSize : max(40, (int)h / 6);
    int dpiY = GetDeviceCaps(m_hMemDC, LOGPIXELSY);
    if (dpiY <= 0) dpiY = 96;
    int fontRequest = MulDiv(fs, 96, dpiY);
    HFONT hLoud = CreateFontW(-fontRequest, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!hLoud) return;
    HFONT hOld = (HFONT)SelectObject(m_hMemDC, hLoud);

    // Measure and center
    RECT rCalc = { 0, 0, (int)w - 40, (int)h };
    ::DrawTextW(m_hMemDC, text, -1, &rCalc, DT_CALCRECT | DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    int tw = rCalc.right - rCalc.left;
    int th = rCalc.bottom - rCalc.top;
    int cx = ((int)w - tw) / 2;
    int cy = ((int)h - th) / 2;

    RECT r = { cx, cy, cx + tw, cy + th };

    // Shadow
    RECT rShadow = r;
    OffsetRect(&rShadow, 2, 2);
    SetTextColor(m_hMemDC, RGB(32, 32, 32));
    ::DrawTextW(m_hMemDC, text, -1, &rShadow, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);

    // Main text
    BYTE cr = (BYTE)((drawColor >> 16) & 0xFF);
    BYTE cg = (BYTE)((drawColor >> 8) & 0xFF);
    BYTE cb = (BYTE)(drawColor & 0xFF);
    SetTextColor(m_hMemDC, RGB(cr, cg, cb));
    ::DrawTextW(m_hMemDC, text, -1, &r, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);

    SelectObject(m_hMemDC, hOld);
    DeleteObject(hLoud);
}
```

**Step 2: Update notification rendering in RenderOverlayToDIB**

In the notification rendering loop (lines 482-494), split into LOUD and normal paths:

```cpp
if (m_currentData.nNotifications > 0) {
    int notifY[4] = { upperRightY, upperLeftY, lowerRightY, lowerLeftY };

    // First pass: render LOUD notifications (centered, on top)
    for (int i = 0; i < m_currentData.nNotifications; i++) {
        const auto& n = m_currentData.notifications[i];
        if (!n.text[0] || !n.isLoud) continue;
        DrawCenteredLoudText(n.text, n.fontSize, n.color, n.loudColor2, n.pulseSpeed, (int)w, (int)h);
    }

    // Second pass: render normal notifications (per-corner stacking)
    for (int i = 0; i < m_currentData.nNotifications; i++) {
        const auto& n = m_currentData.notifications[i];
        if (!n.text[0] || n.isLoud) continue;

        // Create per-notification font if custom size/face specified
        HFONT hCustom = NULL;
        HFONT hOldFont = NULL;
        if (n.fontSize > 0 || n.fontFace[0]) {
            int fs = (n.fontSize > 0) ? n.fontSize : max(20, (int)h / 32);
            int dpiY = GetDeviceCaps(m_hMemDC, LOGPIXELSY);
            if (dpiY <= 0) dpiY = 96;
            int fontReq = MulDiv(fs, 96, dpiY);
            const wchar_t* face = n.fontFace[0] ? n.fontFace : L"Consolas";
            hCustom = CreateFontW(-fontReq, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
            if (hCustom) hOldFont = (HFONT)SelectObject(m_hMemDC, hCustom);
        }

        int c = n.corner;
        if (c < 0 || c > 3) c = 0;
        bool right   = (c == 0 || c == 2);
        bool fromBot = (c == 2 || c == 3);
        DrawShadowText(n.text, right, margin, &notifY[c], (int)w - margin, fromBot, n.color);

        if (hCustom) {
            SelectObject(m_hMemDC, hOldFont);
            DeleteObject(hCustom);
        }
    }
}
```

**Step 3: Build**

Run: `powershell -ExecutionPolicy Bypass -File build.ps1 Release x64`
Expected: 0 warnings, 0 errors

**Step 4: Commit**

```
git add src/mDropDX12/overlay.cpp
git commit -m "Add LOUD and configurable error rendering in overlay"
```

---

### Task 4: Add "Error Display Settings..." button to About tab

**Files:**
- Modify: `src/mDropDX12/engine_helpers.h` (add control ID)
- Modify: `src/mDropDX12/engine_settings_ui.cpp:2455` (after Workspace Layout section)

**Step 1: Add control ID in engine_helpers.h**

After `IDC_MW_OPEN_WORKSPACE_LAYOUT` (line 846), add:

```cpp
#define IDC_MW_ERROR_DISPLAY_SETTINGS 9500  // Button: Error Display Settings (About tab)
```

**Step 2: Add button to About tab in engine_settings_ui.cpp**

After line 2455 (the Workspace Layout description label), add:

```cpp
  y += lineH + 8;

  // Error Display Settings button
  PAGE_CTRL(SP_ABOUT, CreateLabel(hw, L"Error Display:", x, y, lw, lineH, hFont, false));
  {
    int btnW = MulDiv(200, lineH, 26);
    PAGE_CTRL(SP_ABOUT, CreateBtn(hw, L"Error Display Settings...", IDC_MW_ERROR_DISPLAY_SETTINGS, x + lw + 4, y, btnW, lineH, hFont, false));
  }
  y += lineH + 2;
  PAGE_CTRL(SP_ABOUT, CreateLabel(hw, L"(Configure error message appearance, duration, and LOUD mode)", x + lw + 4, y, rw - lw - 4, lineH, hFont, false));
```

**Step 3: Handle button click in DoCommand**

Find the `IDC_MW_FILE_ASSOC` case in `SettingsWindow::DoCommand` and add a new case nearby for `IDC_MW_ERROR_DISPLAY_SETTINGS`. For now, just show a placeholder MessageBox — the actual dialog will be implemented in Task 5:

```cpp
case IDC_MW_ERROR_DISPLAY_SETTINGS:
  // Will be replaced with modal dialog in Task 5
  MessageBoxW(hWnd, L"Error Display Settings (coming soon)", L"MDropDX12", MB_OK);
  break;
```

**Step 4: Build**

Run: `powershell -ExecutionPolicy Bypass -File build.ps1 Release x64`
Expected: 0 warnings, 0 errors

**Step 5: Commit**

```
git add src/mDropDX12/engine_helpers.h src/mDropDX12/engine_settings_ui.cpp
git commit -m "Add Error Display Settings button to About tab"
```

---

### Task 5: Implement the Error Display Settings modal dialog

**Files:**
- Modify: `src/mDropDX12/engine_helpers.h` (add dialog control IDs)
- Modify: `src/mDropDX12/engine_settings_ui.cpp` (add dialog WndProc and launch code)

**Step 1: Add control IDs in engine_helpers.h**

After `IDC_MW_ERROR_DISPLAY_SETTINGS` (added in Task 4), add:

```cpp
// Error Display Settings dialog controls
#define IDC_ERRDLG_NORM_DURATION   9501
#define IDC_ERRDLG_NORM_FONTFACE   9502
#define IDC_ERRDLG_NORM_FONTSIZE   9503
#define IDC_ERRDLG_NORM_CORNER     9504  // Combo box
#define IDC_ERRDLG_NORM_R          9505
#define IDC_ERRDLG_NORM_G          9506
#define IDC_ERRDLG_NORM_B          9507
#define IDC_ERRDLG_NORM_TEST       9508
#define IDC_ERRDLG_LOUD_DURATION   9510
#define IDC_ERRDLG_LOUD_FONTSIZE   9511
#define IDC_ERRDLG_LOUD_R1         9512
#define IDC_ERRDLG_LOUD_G1         9513
#define IDC_ERRDLG_LOUD_B1         9514
#define IDC_ERRDLG_LOUD_R2         9515
#define IDC_ERRDLG_LOUD_G2         9516
#define IDC_ERRDLG_LOUD_B2         9517
#define IDC_ERRDLG_LOUD_PULSE      9518
#define IDC_ERRDLG_LOUD_TEST       9519
#define IDC_ERRDLG_RESET           9520
#define IDC_ERRDLG_OK              9521
#define IDC_ERRDLG_CANCEL          9522
```

**Step 2: Implement the modal dialog**

In `engine_settings_ui.cpp`, add a static dialog WndProc and a helper to launch it. The dialog creates a popup window with edit controls for all settings, two Test buttons, Reset to Defaults, OK, and Cancel. Follow the pattern used by other modal dialogs in the codebase (e.g., the message edit dialog).

Key implementation details:
- Create the window with `WS_POPUP | WS_CAPTION | WS_SYSMENU` style, ~500x520 pixels
- Use `CreateWindowExW` for each control (labels, edits, combo, buttons) — same pattern as ToolWindow controls but without the BS_OWNERDRAW system (since this is a simple modal, standard controls are fine)
- On WM_CREATE: populate edit controls from Engine members
- On IDC_ERRDLG_OK: read all edit values back into Engine members, call `MyWriteConfig()`, destroy window
- On IDC_ERRDLG_CANCEL: destroy window without saving
- On IDC_ERRDLG_RESET: reset all edits to default values
- On IDC_ERRDLG_NORM_TEST: call `g_engine.AddError(L"This is a test error message.", g_engine.m_ErrorDuration, ERR_MISC, true)`
- On IDC_ERRDLG_LOUD_TEST: call `g_engine.AddLoudError(L"LOUD TEST MESSAGE")`
- Modal: disable parent Settings window while open, re-enable on close
- Center the dialog over the Settings window

**Step 3: Replace the placeholder MessageBox from Task 4**

Update the `IDC_MW_ERROR_DISPLAY_SETTINGS` case to call the new dialog launch function.

**Step 4: Build**

Run: `powershell -ExecutionPolicy Bypass -File build.ps1 Release x64`
Expected: 0 warnings, 0 errors

**Step 5: Commit**

```
git add src/mDropDX12/engine_helpers.h src/mDropDX12/engine_settings_ui.cpp
git commit -m "Implement Error Display Settings modal dialog with Normal/LOUD config"
```

---

### Task 6: Update hardcoded error durations across the codebase

**Files:**
- Modify: Various files that call `AddError` with hardcoded durations

**Step 1: Search for AddError calls with short durations**

Search for `AddError(` across `src/mDropDX12/` and identify calls that pass hardcoded float durations (like `3.0f`, `2.5f`, `4.0f`). Replace the duration argument with `m_ErrorDuration` where appropriate — but leave calls that intentionally use different durations (e.g., `m_SongInfoDisplaySeconds` for track info, or very short durations for transient feedback).

Guidelines:
- Replace `3.0f`, `2.5f`, `4.0f` with `m_ErrorDuration` for general error/notification messages
- Keep `m_SongInfoDisplaySeconds` for song info messages
- Keep very short durations (< 2s) for brief acknowledgment messages like "Shuffle ON"

**Step 2: Build**

Run: `powershell -ExecutionPolicy Bypass -File build.ps1 Release x64`
Expected: 0 warnings, 0 errors

**Step 3: Commit**

```
git add -u
git commit -m "Use configurable error duration across the codebase"
```

---

### Task 7: Final verification and cleanup

**Step 1: Full build**

Run: `powershell -ExecutionPolicy Bypass -File build.ps1 Release x64`
Expected: 0 warnings, 0 errors

**Step 2: Manual testing checklist**

1. Launch app — verify errors show for 8s by default (trigger with a bad preset or shader error)
2. Open Settings → About tab → click "Error Display Settings..."
3. Verify modal opens with default values populated
4. Click "Test" in Normal section — verify error appears in correct corner with configured font/color
5. Click "Test" in LOUD section — verify large centered pulsing text appears
6. Change Normal duration to 15s, click Test — verify it stays longer
7. Change Normal corner, click Test — verify position changes
8. Change LOUD colors, click Test — verify pulse cycles between new colors
9. Click OK, close and reopen Settings, reopen dialog — verify values persisted
10. Click "Reset to Defaults" — verify factory values restored
11. Click Cancel — verify changes are NOT saved

**Step 3: Commit any final fixes**

```
git add -u
git commit -m "Error display enhancement: configurable Normal/LOUD modes with settings UI"
```
