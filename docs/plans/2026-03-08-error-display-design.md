# Error Display Enhancement Design

## Problem

Error messages disappear too quickly (~3s default). Users need configurable display settings (duration, font, size, position, color) and a "LOUD" mode for unmissable alerts.

## Approach

Extend the existing overlay notification system. All error rendering stays in one place (overlay GDI). Two fixed modes: Normal and LOUD, both fully configurable via INI and a settings modal.

## Two Modes

### Normal (default for all errors)

| Setting | Default | INI Key |
|---------|---------|---------|
| Duration | 8s | `nErrorDuration` |
| Font face | Segoe UI | `szErrorFontFace` |
| Font size | 20px | `nErrorFontSize` |
| Position | upper-right | `nErrorCorner` (0=UR, 1=UL, 2=LR, 3=LL) |
| Color | white | `nErrorColorR/G/B` |

### LOUD (opt-in, for critical alerts)

| Setting | Default | INI Key |
|---------|---------|---------|
| Duration | 30s | `nLoudDuration` |
| Font size | auto (window height / 6) | `nLoudFontSize` (0=auto) |
| Position | centered | (fixed) |
| Color 1 | red (255,50,50) | `nLoudColorR1/G1/B1` |
| Color 2 | yellow (255,255,50) | `nLoudColorR2/G2/B2` |
| Pulse speed | 2 Hz | `nLoudPulseSpeed` |
| Fade-in | 0.5s | (fixed) |
| Fade-out | 2s | (fixed) |

LOUD pulse: `sin(time * pulseSpeed)` interpolation between Color 1 and Color 2.

## Settings UI

Button on About tab: "Error Display Settings..." opens a modal dialog.

### Modal layout

- **Normal section**: Duration, Font Face, Font Size, Corner (dropdown), Color (R/G/B edits), [Test] button
- **LOUD section**: Duration, Font Size (0=auto), Color 1 (R/G/B), Color 2 (R/G/B), Pulse Speed, [Test] button
- **Footer**: [Reset to Defaults] [OK] [Cancel]

## Implementation Changes

### overlay.h / overlay.cpp

- Extend `OverlayNotification` with: `fontSize`, `corner`, `isLoud`, `loudColor2` (DWORD), `pulseSpeed` (float)
- `RenderOverlayToDIB`: for LOUD notifications, create large centered font, compute pulse color via `sin()`, draw centered with fade

### engine.h

- New Engine members for all INI settings (matching table above)
- `void AddLoudError(wchar_t* msg)` — creates error with LOUD settings

### engine_rendering.cpp

- `AddError` uses `m_nErrorDuration` as default duration instead of hardcoded values
- Overlay marshalling populates new notification fields from Engine settings

### engine_config.cpp

- Load/save new INI keys in existing ReadINI/WriteINI

### engine_settings_ui.cpp

- Add "Error Display Settings..." button to About tab
- New modal dialog class (ErrorDisplayDialog or inline WndProc)
- Test buttons call `AddError`/`AddLoudError` with sample text

## Files Modified

| File | Change |
|------|--------|
| engine.h | Error display setting members, AddLoudError declaration |
| engine_rendering.cpp | Use configurable duration, populate overlay fields |
| overlay.h | Extend OverlayNotification struct |
| overlay.cpp | LOUD rendering (large centered, pulse color, fade) |
| engine_config.cpp | Load/save new INI keys |
| engine_settings_ui.cpp | "Error Display Settings..." button + modal dialog |

## Verification

1. Build with 0 warnings
2. Launch, verify errors now show for 8s by default
3. Open About tab, click "Error Display Settings..."
4. Change Normal settings, click Test — verify appearance matches
5. Change LOUD settings, click Test — verify large centered pulsing text
6. Close and reopen — verify settings persisted in INI
7. Click "Reset to Defaults" — verify factory values restored
