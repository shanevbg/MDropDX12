/*
  Plugin module: Script Engine
  Milkwave-compatible script file playback system.
  Parses text files with beat-driven command sequences and executes them on a timer.
*/

#include "engine.h"
#include "engine_helpers.h"
#include "utility.h"
#include <fstream>
#include <sstream>
#include <algorithm>

extern mdrop::Engine g_engine;

namespace mdrop {

void Engine::LoadScript(const wchar_t* path) {
  StopScript();
  m_script.lines.clear();
  m_script.filePath = path;

  std::wifstream file(path);
  if (!file.is_open()) {
    wchar_t msg[512];
    swprintf_s(msg, L"Script: failed to open '%s'", path);
    AddNotification(msg);
    return;
  }

  std::wstring line;
  while (std::getline(file, line)) {
    // Trim trailing whitespace
    while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n' || line.back() == L' ' || line.back() == L'\t'))
      line.pop_back();

    // Skip comment lines (# at start)
    if (!line.empty() && line[0] == L'#')
      continue;

    // Keep blank lines as beat delays, keep everything else
    m_script.lines.push_back(line);
  }

  // Reset default message style
  m_script.defaultFont = L"Arial";
  m_script.defaultSize = 20;
  m_script.defaultR = 255;
  m_script.defaultG = 255;
  m_script.defaultB = 255;

  wchar_t msg[512];
  swprintf_s(msg, L"Script loaded: %d lines", (int)m_script.lines.size());
  AddNotification(msg);

  SyncScriptUI();
}

void Engine::StartScript() {
  if (m_script.lines.empty())
    return;

  m_script.playing = true;
  m_script.currentLine = 0;
  m_script.lastLineTime = GetTime();

  // Execute the first line immediately
  ExecuteScriptLine(0);

  SyncScriptUI();
}

void Engine::StopScript() {
  m_script.playing = false;
  m_script.currentLine = -1;
  SyncScriptUI();
}

void Engine::UpdateScript() {
  if (!m_script.playing || m_script.lines.empty())
    return;

  double elapsed = GetTime() - m_script.lastLineTime;
  double interval = (60.0 / m_script.bpm) * m_script.beats;

  if (elapsed >= interval) {
    // Advance to next line
    m_script.currentLine++;

    if (m_script.currentLine >= (int)m_script.lines.size()) {
      if (m_script.loop) {
        m_script.currentLine = 0;
      } else {
        StopScript();
        AddNotification((wchar_t*)L"Script finished");
        return;
      }
    }

    m_script.lastLineTime = GetTime();
    ExecuteScriptLine(m_script.currentLine);
    SyncScriptUI();
  }
}

void Engine::ExecuteScriptLine(int lineIndex) {
  if (lineIndex < 0 || lineIndex >= (int)m_script.lines.size())
    return;

  const std::wstring& line = m_script.lines[lineIndex];

  // Blank lines are just beat delays
  if (line.empty())
    return;

  // Split by | and execute each token
  std::wstringstream ss(line);
  std::wstring token;
  while (std::getline(ss, token, L'|')) {
    // Trim leading/trailing whitespace
    size_t start = token.find_first_not_of(L" \t");
    size_t end = token.find_last_not_of(L" \t");
    if (start == std::wstring::npos)
      continue;
    token = token.substr(start, end - start + 1);

    if (!token.empty())
      ExecuteScriptCommand(token);
  }
}

void Engine::ExecuteScriptCommand(const std::wstring& cmd) {
  // --- Sequencing ---
  if (cmd == L"NEXT") {
    NextPreset(0);
  }
  else if (cmd == L"PREV") {
    PrevPreset(0);
  }
  else if (cmd == L"STOP") {
    StopScript();
  }
  else if (cmd == L"RESET") {
    m_script.lastLineTime = GetTime();
  }
  else if (_wcsnicmp(cmd.c_str(), L"BPM=", 4) == 0) {
    double val = _wtof(cmd.c_str() + 4);
    if (val > 0) m_script.bpm = val;
  }
  else if (_wcsnicmp(cmd.c_str(), L"BEATS=", 6) == 0) {
    int val = _wtoi(cmd.c_str() + 6);
    if (val > 0) m_script.beats = val;
  }
  else if (_wcsnicmp(cmd.c_str(), L"LINE=", 5) == 0) {
    std::wstring arg = cmd.substr(5);
    if (arg == L"CURR") {
      // Re-execute current line (no-op for advancement)
    } else if (arg == L"NEXT") {
      if (m_script.currentLine + 1 < (int)m_script.lines.size())
        m_script.currentLine++;
    } else if (arg == L"PREV") {
      if (m_script.currentLine > 0)
        m_script.currentLine--;
    } else {
      int n = _wtoi(arg.c_str());
      if (n >= 0 && n < (int)m_script.lines.size())
        m_script.currentLine = n;
    }
    m_script.lastLineTime = GetTime();
  }
  else if (_wcsnicmp(cmd.c_str(), L"FILE=", 5) == 0) {
    std::wstring path = cmd.substr(5);
    // Support relative paths (relative to current script dir)
    if (path.find(L':') == std::wstring::npos && path[0] != L'\\' && path[0] != L'/') {
      std::wstring baseDir = m_script.filePath;
      size_t lastSlash = baseDir.find_last_of(L"\\/");
      if (lastSlash != std::wstring::npos)
        baseDir = baseDir.substr(0, lastSlash + 1);
      else
        baseDir = std::wstring(m_szBaseDir);
      path = baseDir + path;
    }
    LoadScript(path.c_str());
    StartScript();
  }
  // --- Preset Control ---
  else if (_wcsnicmp(cmd.c_str(), L"PRESET=", 7) == 0) {
    std::wstring msg = L"PRESET=" + cmd.substr(7);
    LaunchMessage((wchar_t*)msg.c_str());
  }
  // --- Messages ---
  else if (_wcsnicmp(cmd.c_str(), L"MSG=", 4) == 0) {
    // Convert semicolons to pipes for LaunchMessage format
    std::wstring params = cmd.substr(4);
    std::replace(params.begin(), params.end(), L';', L'|');
    std::wstring msg = L"MSG|" + params;
    LaunchMessage((wchar_t*)msg.c_str());
  }
  else if (_wcsnicmp(cmd.c_str(), L"COLOR=", 6) == 0) {
    // Parse R,G,B
    int r = 255, g = 255, b = 255;
    swscanf_s(cmd.c_str() + 6, L"%d,%d,%d", &r, &g, &b);
    m_script.defaultR = std::clamp(r, 0, 255);
    m_script.defaultG = std::clamp(g, 0, 255);
    m_script.defaultB = std::clamp(b, 0, 255);
  }
  else if (_wcsnicmp(cmd.c_str(), L"FONT=", 5) == 0) {
    m_script.defaultFont = cmd.substr(5);
  }
  else if (_wcsnicmp(cmd.c_str(), L"SIZE=", 5) == 0) {
    int val = _wtoi(cmd.c_str() + 5);
    if (val > 0) m_script.defaultSize = val;
  }
  else if (cmd == L"CLEARSPRITES") {
    LaunchMessage((wchar_t*)L"CLEARSPRITES");
  }
  else if (cmd == L"CLEARTEXTS") {
    LaunchMessage((wchar_t*)L"CLEARTEXTS");
  }
  else if (cmd == L"CLEARPARAMS") {
    m_script.defaultFont = L"Arial";
    m_script.defaultSize = 20;
    m_script.defaultR = 255;
    m_script.defaultG = 255;
    m_script.defaultB = 255;
  }
  else if (_wcsnicmp(cmd.c_str(), L"SEND=", 5) == 0) {
    std::wstring val = cmd.substr(5);
    HWND hWnd = GetPluginWindow();
    if (hWnd) {
      if (val.size() >= 2 && val[0] == L'0' && (val[1] == L'x' || val[1] == L'X')) {
        // Hex keycode
        int keyCode = (int)wcstol(val.c_str(), nullptr, 16);
        PostMessage(hWnd, WM_KEYDOWN, keyCode, 0);
        PostMessage(hWnd, WM_KEYUP, keyCode, 0);
      } else {
        // Send as WM_CHAR sequence
        for (wchar_t ch : val)
          PostMessage(hWnd, WM_CHAR, ch, 0);
      }
    }
  }
  // --- Visual Parameters (mapped to LaunchMessage) ---
  else if (_wcsnicmp(cmd.c_str(), L"TIME=", 5) == 0) {
    std::wstring msg = L"VAR_TIME=" + cmd.substr(5);
    LaunchMessage((wchar_t*)msg.c_str());
  }
  else if (_wcsnicmp(cmd.c_str(), L"FRAME=", 6) == 0) {
    std::wstring msg = L"VAR_FRAME=" + cmd.substr(6);
    LaunchMessage((wchar_t*)msg.c_str());
  }
  else if (_wcsnicmp(cmd.c_str(), L"FPS=", 4) == 0) {
    std::wstring msg = L"VAR_FPS=" + cmd.substr(4);
    LaunchMessage((wchar_t*)msg.c_str());
  }
  else if (_wcsnicmp(cmd.c_str(), L"INTENSITY=", 10) == 0) {
    std::wstring msg = L"VAR_INTENSITY=" + cmd.substr(10);
    LaunchMessage((wchar_t*)msg.c_str());
  }
  else if (_wcsnicmp(cmd.c_str(), L"SHIFT=", 6) == 0) {
    std::wstring msg = L"VAR_SHIFT=" + cmd.substr(6);
    LaunchMessage((wchar_t*)msg.c_str());
  }
  else if (_wcsnicmp(cmd.c_str(), L"VERSION=", 8) == 0) {
    std::wstring msg = L"VAR_VERSION=" + cmd.substr(8);
    LaunchMessage((wchar_t*)msg.c_str());
  }
  else if (_wcsnicmp(cmd.c_str(), L"QUALITY=", 8) == 0) {
    std::wstring msg = L"VAR_QUALITY=" + cmd.substr(8);
    LaunchMessage((wchar_t*)msg.c_str());
  }
  else if (_wcsnicmp(cmd.c_str(), L"HUE=", 4) == 0) {
    std::wstring msg = L"COL_HUE=" + cmd.substr(4);
    LaunchMessage((wchar_t*)msg.c_str());
  }
  else if (_wcsnicmp(cmd.c_str(), L"SATURATION=", 11) == 0) {
    std::wstring msg = L"COL_SATURATION=" + cmd.substr(11);
    LaunchMessage((wchar_t*)msg.c_str());
  }
  else if (_wcsnicmp(cmd.c_str(), L"BRIGHTNESS=", 11) == 0) {
    std::wstring msg = L"COL_BRIGHTNESS=" + cmd.substr(11);
    LaunchMessage((wchar_t*)msg.c_str());
  }
  // --- Toggles ---
  else if (cmd == L"RAND") {
    m_bSequentialPresetOrder = !m_bSequentialPresetOrder;
    AddNotification(m_bSequentialPresetOrder ? (wchar_t*)L"Sequential order" : (wchar_t*)L"Random order");
  }
  else if (cmd == L"LOCK") {
    m_bPresetLockedByUser = !m_bPresetLockedByUser;
    AddNotification(m_bPresetLockedByUser ? (wchar_t*)L"Preset locked" : (wchar_t*)L"Preset unlocked");
  }
  else if (cmd == L"PRESETINFO") {
    m_bShowPresetInfo = !m_bShowPresetInfo;
  }
  else if (cmd == L"FULLSCREEN") {
    HWND hWnd = GetPluginWindow();
    if (hWnd)
      PostMessage(hWnd, WM_KEYDOWN, VK_F11, 0);
  }
  // --- Media ---
  else if (cmd == L"MEDIA_PLAY") {
    keybd_event(VK_MEDIA_PLAY_PAUSE, 0, 0, 0);
    keybd_event(VK_MEDIA_PLAY_PAUSE, 0, KEYEVENTF_KEYUP, 0);
  }
  else if (cmd == L"MEDIA_STOP") {
    keybd_event(VK_MEDIA_STOP, 0, 0, 0);
    keybd_event(VK_MEDIA_STOP, 0, KEYEVENTF_KEYUP, 0);
  }
  // --- Milkwave-specific (ignore gracefully) ---
  else if (_wcsnicmp(cmd.c_str(), L"STYLE=", 6) == 0 ||
           _wcsnicmp(cmd.c_str(), L"BTN=", 4) == 0 ||
           _wcsnicmp(cmd.c_str(), L"EXEC=", 5) == 0 ||
           cmd == L"SONGINFO" || cmd == L"SOUNDINFO") {
    // Milkwave Remote-only commands — not applicable to native playback
  }
  // --- Default: treat as message text ---
  else {
    // Build a message using current default style
    std::wstring text = cmd;
    // Replace // with newlines (Milkwave convention)
    size_t pos;
    while ((pos = text.find(L"//")) != std::wstring::npos)
      text.replace(pos, 2, L"\n");

    wchar_t msgBuf[2048];
    swprintf_s(msgBuf, L"MSG|font=%s|size=%d|color_r=%d|color_g=%d|color_b=%d|text=%s",
      m_script.defaultFont.c_str(),
      m_script.defaultSize,
      m_script.defaultR, m_script.defaultG, m_script.defaultB,
      text.c_str());
    LaunchMessage(msgBuf);
  }
}

void Engine::SyncScriptUI() {
  if (!m_hSettingsWnd || !IsWindow(m_hSettingsWnd))
    return;

  // Update listbox selection
  HWND hList = GetDlgItem(m_hSettingsWnd, IDC_MW_SCRIPT_LIST);
  if (hList) {
    if (m_script.currentLine >= 0)
      SendMessage(hList, LB_SETCURSEL, m_script.currentLine, 0);
    else
      SendMessage(hList, LB_SETCURSEL, (WPARAM)-1, 0);
  }

  // Update line label
  HWND hLine = GetDlgItem(m_hSettingsWnd, IDC_MW_SCRIPT_LINE);
  if (hLine) {
    if (m_script.playing && m_script.currentLine >= 0) {
      wchar_t buf[64];
      swprintf_s(buf, L"Line %d of %d", m_script.currentLine + 1, (int)m_script.lines.size());
      SetWindowTextW(hLine, buf);
    } else if (!m_script.lines.empty()) {
      wchar_t buf[64];
      swprintf_s(buf, L"%d lines loaded", (int)m_script.lines.size());
      SetWindowTextW(hLine, buf);
    } else {
      SetWindowTextW(hLine, L"No script loaded");
    }
  }

  // Update BPM/Beats edit fields
  HWND hBpm = GetDlgItem(m_hSettingsWnd, IDC_MW_SCRIPT_BPM);
  if (hBpm) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f", m_script.bpm);
    SetWindowTextW(hBpm, buf);
  }
  HWND hBeats = GetDlgItem(m_hSettingsWnd, IDC_MW_SCRIPT_BEATS);
  if (hBeats) {
    wchar_t buf[32];
    swprintf_s(buf, L"%d", m_script.beats);
    SetWindowTextW(hBeats, buf);
  }
}

} // namespace mdrop
