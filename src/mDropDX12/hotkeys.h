#pragma once

#include <windows.h>

// Action IDs for RegisterHotKey
enum HotkeyAction : int {
    HK_TOGGLE_FULLSCREEN = 1,
    HK_TOGGLE_STRETCH,
    HK_COUNT
};

// A single configurable hotkey binding
struct HotkeyBinding {
    int id;                    // HotkeyAction ID (for RegisterHotKey)
    UINT modifiers;            // MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_WIN
    UINT vk;                   // Virtual key code (0 = disabled)
    wchar_t szAction[64];      // Action name (e.g., L"Toggle Fullscreen")
    wchar_t szIniKey[64];      // INI key prefix (e.g., L"ToggleFullscreen")
};
