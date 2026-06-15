// fps_caps.h — Single source of truth for FPS cap options.
#ifndef MDROP_FPS_CAPS_H
#define MDROP_FPS_CAPS_H

struct FpsCapOption {
    int value;                  // 0 = unlimited
    const wchar_t* uiLabel;     // combo box text
    const wchar_t* notifyLabel; // hotkey notification text
};

// UI order, hotkey cycle (F3), and SetFPSCap sync all use this table.
static constexpr FpsCapOption kFpsCapOptions[] = {
    {  30, L"30",        L"30 fps" },
    {  40, L"40",        L"40 fps" },
    {  60, L"60",        L"60 fps" },
    {  90, L"90",        L"90 fps" },
    { 120, L"120",       L"120 fps" },
    { 144, L"144",       L"144 fps" },
    { 240, L"240",       L"240 fps" },
    { 360, L"360",       L"360 fps" },
    { 720, L"720",       L"720 fps" },
    {   0, L"Unlimited", L"Unlimited fps" },
};

static constexpr int kFpsCapOptionCount = (int)(sizeof(kFpsCapOptions) / sizeof(kFpsCapOptions[0]));

inline int FpsCapFindIndex(int fps) {
    for (int i = 0; i < kFpsCapOptionCount; i++)
        if (kFpsCapOptions[i].value == fps) return i;
    return -1;
}

inline int FpsCapDefaultIndex() {
    return FpsCapFindIndex(0); // unlimited
}

#endif