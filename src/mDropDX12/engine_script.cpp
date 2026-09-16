/*
  Plugin module: Script Engine
  Milkwave-compatible script file playback system.
  Parses text files with beat-driven command sequences and executes them on a timer.
*/

#include "engine.h"
#include "pipe_server.h"  // MW_FS_TOGGLE
#include "engine_helpers.h"
#include "utility.h"
#include "format_to.h"
#include <fstream>
#include <sstream>
#include <algorithm>

extern mdrop::Engine g_engine;

namespace mdrop {

void Engine::LoadScript(const wchar_t* path) {
  StopScript();
  m_script.lines.clear();
  m_script.filePath = path;
  // Per FILE, not per session (#303): strict is declared by the script that
  // wants it, so a chained FILE= must not inherit it silently -- and the error
  // count belongs to the run being reported, not to everything since launch.
  m_script.strict = false;
  m_script.errors = 0;
  m_script.waitingReady = false;
  m_script.repeatLine = -1;
  m_script.repeatsLeft = 0;
  m_script.checks = 0;
  m_script.checkShaderFails = 0;
  m_script.checkTextureFails = 0;

  std::wifstream file(path);
  if (!file.is_open()) {
    wchar_t msg[512];
    FormatTo(msg, L"Script: failed to open '%s'", path);
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
  FormatTo(msg, L"Script loaded: %d lines", (int)m_script.lines.size());
  AddNotification(msg);

  SyncScriptUI();
}

void Engine::StartScript() {
  if (m_script.lines.empty())
    return;

  m_script.playing = true;
  m_script.currentLine = 0;
  m_script.lastLineTime = GetTime();
  m_script.startedAt = GetTime();

  // Execute the first line immediately
  ExecuteScriptLine(0);

  SyncScriptUI();
}

void Engine::StopScript() {
  // The verdict goes HERE, not where the line list runs out (#303).
  //
  // A script ends three ways -- off the end, on a STOP line, or because someone
  // pressed Stop -- and a test script written to be handed to a user ends with
  // STOP, which is the one path an end-of-lines summary never reaches. That is
  // exactly how the self-test came to pass five of its six checks: every marker
  // was in the log and the verdict was not.
  //
  // Guarded on `playing` so the StopScript that LoadScript calls first cannot
  // print a verdict for a run that never happened.
  if (m_script.playing && m_script.strict) {
    DLOG_ERROR("SCRIPT: finished -- %d error%ls", m_script.errors,
               m_script.errors == 1 ? L"" : L"s");
  }
  m_script.playing = false;
  m_script.currentLine = -1;
  m_script.waitingReady = false;
  SyncScriptUI();
}

void Engine::UpdateScript() {
  if (!m_script.playing || m_script.lines.empty())
    return;

  // WAITREADY holds the line until the preset has actually loaded (#303).
  //
  // Timed out rather than trusted: a preset whose shaders never compile would
  // otherwise stop the script for good, and a repro that hangs tells its reader
  // nothing. Ten seconds is longer than the slowest compile measured here by an
  // order of magnitude, and the timeout is LOGGED -- a script that had to wait
  // that long has found something worth knowing about.
  if (m_script.waitingReady) {
    const double waited = GetTime() - m_script.waitStarted;
    if (m_nLoadingPreset == 0) {
      m_script.waitingReady = false;
      m_script.lastLineTime = GetTime();
      DLOG_ERROR("SCRIPT: ready after %.2fs", waited);
    } else if (waited > 10.0) {
      m_script.waitingReady = false;
      m_script.lastLineTime = GetTime();
      m_script.errors++;
      DLOG_ERROR("SCRIPT: WAITREADY TIMED OUT after %.1fs -- preset still loading",
                 waited);
    } else {
      // Held: do not let the beat interval accumulate while waiting, or the
      // script would sprint through the next several lines to catch up.
      m_script.lastLineTime = GetTime();
      return;
    }
  }

  double elapsed = GetTime() - m_script.lastLineTime;
  double interval = (60.0 / m_script.bpm) * m_script.beats;

  if (elapsed >= interval) {
    // Advance to next line
    m_script.currentLine++;

    if (m_script.currentLine >= (int)m_script.lines.size()) {
      if (m_script.loop) {
        m_script.currentLine = 0;
      } else {
        // StopScript logs the verdict -- every exit route goes through it.
        const int nErr = m_script.errors;
        StopScript();
        if (nErr > 0) {
          wchar_t note[96];
          FormatTo(note, L"Script finished with %d error%ls", nErr,
                   nErr == 1 ? L"" : L"s");
          AddNotification(note);
        } else {
          AddNotification((wchar_t*)L"Script finished");
        }
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
      ExecuteScriptCommand(token, m_script.strict);
  }
}

void Engine::ExecuteScriptLine(const wchar_t* text, bool bStrict) {
  if (!text || !*text) return;
  std::wstringstream ss(text);
  std::wstring token;
  while (std::getline(ss, token, L'|')) {
    size_t start = token.find_first_not_of(L" \t");
    size_t end   = token.find_last_not_of(L" \t");
    if (start == std::wstring::npos) continue;
    token = token.substr(start, end - start + 1);
    if (!token.empty())
      ExecuteScriptCommand(token, bStrict);
  }
}

void Engine::ExecuteScriptCommand(const std::wstring& cmd, bool bStrict) {
  // A script that has declared STRICT stays strict for every token after it,
  // however the token arrived (#303).
  bStrict = bStrict || m_script.strict;

  // --- Driving a test from a script file (#303) ---
  if (cmd == L"STRICT") {
    // Everything after this is checked rather than painted on screen.
    //
    // Without it a test script cannot be trusted: an unrecognised token is
    // DRAWN AS A MESSAGE, so a typo or a verb this build does not have becomes
    // scenery and the script reports success having done nothing. Shane:
    // "Can require use of STRICT flag".
    m_script.strict = true;
    m_script.errors = 0;
    DLOG_ERROR("SCRIPT: strict mode on -- unknown commands are errors from here");
  }
  else if (_wcsnicmp(cmd.c_str(), L"LOG=", 4) == 0) {
    // A marker in debug.log, which is the only output a public repro has.
    //
    // Logged at ERROR deliberately, and it is not an error: it is the one level
    // that survives every LogLevel above Off, so a script handed to someone
    // whose logging is at the default still produces a readable trace. Asking
    // them to change a setting first is a step that gets skipped.
    DLOG_ERROR("SCRIPT: %ls", cmd.c_str() + 4);
  }
  else if (cmd == L"LOGSTATE") {
    // What the engine is actually doing, into the log, so a report says more
    // than "it looked wrong".
    wchar_t cur[MAX_PATH] = {};
    CurrentPresetPath(cur, MAX_PATH);
    DLOG_ERROR("SCRIPT STATE: preset='%ls' fps=%.1f mesh=%dx%d loading=%d "
               "blend=%d mirrors=%d displays=%d",
               cur, GetFps(), m_nGridX, m_nGridY, m_nLoadingPreset,
               (m_pState && m_pState->m_bBlending) ? 1 : 0,
               m_bMirrorsActive ? 1 : 0,
               (int)m_displayOutputs.size());
  }
  else if (cmd == L"CHECK") {
    // Judge the preset that is up, and tally it.
    //
    // Both counters are per-load and were reset when this preset began, so
    // they describe THIS preset and not the run so far -- which is what lets a
    // sweep name the preset that is at fault instead of reporting a total.
    //
    // Texture misses are counted separately from shader errors because they
    // fail differently: a preset with a missing texture compiles, renders, and
    // is wrong. Anything that only asks "did the shaders build" scores it a
    // pass.
    const int shaderErrs = m_nShaderErrorsThisLoad.load();
    const int texMiss    = m_nTextureMissesThisLoad.load();
    wchar_t cur[MAX_PATH] = {};
    CurrentPresetPath(cur, MAX_PATH);
    m_script.checks++;
    if (shaderErrs) m_script.checkShaderFails++;
    if (texMiss)    m_script.checkTextureFails++;
    DLOG_ERROR("SCRIPT CHECK: %ls shader_errors=%d textures_missing=%d  %ls",
               (shaderErrs || texMiss) ? L"FAIL" : L"ok",
               shaderErrs, texMiss, cur);
  }
  else if (cmd == L"LOGSUMMARY") {
    // Elapsed is COMPUTED here even though every log line is timestamped
    // already. Shane: "it's in the logs" -- it is, and a reader of a 200-preset
    // report should not have to find the first and last entry and subtract
    // them to learn how long it took, nor divide to get the per-preset rate.
    // That rate is the number that says whether a library is slow.
    const double elapsed = GetTime() - m_script.startedAt;
    DLOG_ERROR("SCRIPT SUMMARY: checked=%d  shader_failures=%d  "
               "texture_failures=%d  unknown_commands=%d  "
               "elapsed=%.1fs  per_preset=%.2fs",
               m_script.checks, m_script.checkShaderFails,
               m_script.checkTextureFails, m_script.errors,
               elapsed, elapsed / (m_script.checks > 0 ? m_script.checks : 1));
  }
  else if (_wcsnicmp(cmd.c_str(), L"REPEAT=", 7) == 0) {
    // REPEAT=<count>,<line> -- jump back to <line>, at most <count> times.
    //
    // The one bounded backward jump this language has. LINE= is unconditional,
    // so a sweep written with it runs forever and has to be stopped by hand --
    // which means it can never print a summary, and a report that never
    // arrives is not a report.
    //
    // The budget is keyed on the TARGET LINE so two loops in one script do not
    // share a counter, and it is re-armed once exhausted so a script that is
    // played twice behaves the same way the second time.
    int count = 0, line = -1;
    if (swscanf(cmd.c_str() + 7, L"%d,%d", &count, &line) == 2 &&
        count > 0 && line >= 0 && line < (int)m_script.lines.size()) {
      if (m_script.repeatLine != line) {
        m_script.repeatLine = line;
        m_script.repeatsLeft = count;
      }
      if (m_script.repeatsLeft > 0) {
        m_script.repeatsLeft--;
        // line - 1, because UpdateScript INCREMENTS currentLine and then
        // executes it. Setting it to `line` runs line+1 next, which for a
        // two-line loop is the REPEAT line itself: it then span on that one
        // line forever, re-CHECKing the same preset because the NEXT above it
        // was never reached again.
        //
        // That is not a theory. A 4,771-preset sweep reported 4,777 checks,
        // zero failures and 0.02 s/preset -- and the log held ONE distinct
        // preset name, one "ready after" line, and the cache had gained 11
        // files instead of thousands. A clean result produced by not running.
        m_script.currentLine = line - 1;
        m_script.lastLineTime = GetTime();
      } else {
        m_script.repeatLine = -1;      // re-armed for a second play
      }
    } else if (bStrict) {
      m_script.errors++;
      DLOG_ERROR("SCRIPT: REPEAT needs <count>,<line> -- got '%ls'", cmd.c_str());
    }
  }
  else if (cmd == L"WAITREADY") {
    // Hold the script here until the preset has finished loading.
    //
    // Every other form of waiting in this language is a number of BEATS, which
    // is a guess about how long a machine takes to compile a shader -- and the
    // machine a repro matters on is the slow one, where the guess is wrong. The
    // hold times out (see UpdateScript) so a preset that never loads stalls the
    // script for a few seconds rather than forever.
    m_script.waitingReady = true;
    m_script.waitStarted = GetTime();
  }
  // --- Sequencing ---
  else if (cmd == L"NEXT") {
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
    if (val.size() >= 2 && val[0] == L'0' && (val[1] == L'x' || val[1] == L'X')) {
      // Hex keycode
      int keyCode = (int)wcstol(val.c_str(), nullptr, 16);
      // Media keys (VK_MEDIA_NEXT_TRACK..VK_LAUNCH_APP2, 0xB0-0xB7) are
      // system-level virtual keys — they must go through keybd_event so the
      // OS media-key handler picks them up.
      if (keyCode >= 0xB0 && keyCode <= 0xB7) {
        keybd_event((BYTE)keyCode, 0, 0, 0);
        keybd_event((BYTE)keyCode, 0, KEYEVENTF_KEYUP, 0);
      } else {
        HWND hWnd = GetPluginWindow();
        if (hWnd) {
          // lParam carries a REPEAT COUNT of 1 in its low word, because that
          // is what a real WM_KEYDOWN carries and some handlers read it.
          //
          // It was 0, which is not a keystroke any keyboard produces, and the
          // menu is the proof: CMenu::HandleKeydown moves the selection inside
          // `for (rep = 0; rep < LOWORD(lParam); rep++)` (menu.cpp:398), so
          // with a repeat count of zero the loop body never ran. VK_DOWN was
          // accepted, absorbed, and did nothing -- while VK_RIGHT, whose case
          // does not loop, worked. The menu looked selectively deaf; it was
          // being sent malformed key presses.
          //
          // KEYUP sets bit 30 (previous state down) and bit 31 (transition),
          // again as a real one does.
          const LPARAM lpDown = 1;
          const LPARAM lpUp = 1 | (1 << 30) | (1 << 31);
          PostMessage(hWnd, WM_KEYDOWN, keyCode, lpDown);
          PostMessage(hWnd, WM_KEYUP, keyCode, lpUp);
        }
      }
    } else {
      HWND hWnd = GetPluginWindow();
      if (hWnd) {
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
    SetUserPresetLock(!m_bPresetLockedByUser);
    AddNotification(m_bPresetLockedByUser ? (wchar_t*)L"Preset locked" : (wchar_t*)L"Preset unlocked");
  }
  else if (cmd == L"PRESETINFO") {
    m_bShowPresetInfo = !m_bShowPresetInfo;
  }
  else if (cmd == L"FULLSCREEN") {
    HWND hWnd = GetPluginWindow();
    if (hWnd)
      PostMessage(hWnd, WM_MW_FULLSCREEN, MW_FS_TOGGLE, 0);  // forgejo#23
  }
  else if (cmd == L"MIRROR") {
    HWND hWnd = GetPluginWindow();
    if (hWnd)
      PostMessage(hWnd, WM_MW_TOGGLE_MIRROR_MODE, 0, 0);
  }
  else if (cmd == L"STRETCH") {
    HWND hWnd = GetPluginWindow();
    if (hWnd)
      PostMessage(hWnd, WM_MW_TOGGLE_STRETCH_MODE, 0, 0);
  }
  else if (cmd == L"MIRROR_WM" || cmd == L"MIRROR_WATERMARK") {
    HWND hWnd = GetPluginWindow();
    if (hWnd)
      PostMessage(hWnd, WM_MW_MIRROR_WM, 0, 0);
  }
  else if (cmd == L"WATERMARK") {
    HWND hWnd = GetPluginWindow();
    if (hWnd)
      PostMessage(hWnd, WM_MW_WATERMARK, 0, 0);
  }
  else if (cmd == L"MIRROR_INDEPENDENT" || cmd == L"INDEPENDENT") {
    ToggleMirrorIndependentRender();
  }
  else if (cmd == L"ALWAYS_ON_TOP" || cmd == L"AOT") {
    HWND hWnd = GetPluginWindow();
    if (hWnd)
      PostMessage(hWnd, WM_MW_ALWAYS_ON_TOP_TOGGLE, 0, 0);
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
  // --- Trigger hotkey action by tag name (e.g. ACTION=OpenSongInfo) ---
  else if (_wcsnicmp(cmd.c_str(), L"ACTION=", 7) == 0) {
    DispatchHotkeyByTag(cmd.substr(7));
  }
  // --- Launch application, optionally with arguments ---
  //
  //   LAUNCH=C:\Program Files\Foo\foo.exe              the original form
  //   LAUNCH="C:\Python\python.exe" "script.py" --n 20 path quoted, then args
  //
  // A QUOTED path is what turns arguments on, and the unquoted form is left
  // exactly as it was. Splitting on the first space instead would have broken
  // every existing binding: `C:\Program Files\...` is the normal shape of a
  // Windows path and has spaces in it, so the old form has to stay
  // "everything is the path".
  //
  // '|' is not available as a separator here, however natural it looks beside
  // the rest of the vocabulary -- the script tokenizer splits on it before a
  // command is ever dispatched, so `LAUNCH=x|y` arrives as two commands.
  else if (_wcsnicmp(cmd.c_str(), L"LAUNCH=", 7) == 0) {
    std::wstring rest = cmd.substr(7);
    size_t a = rest.find_first_not_of(L" \t");
    if (a == std::wstring::npos) a = rest.size();
    if (a < rest.size() && rest[a] == L'"') {
      const size_t close = rest.find(L'"', a + 1);
      if (close != std::wstring::npos) {
        std::wstring path = rest.substr(a + 1, close - a - 1);
        std::wstring args = rest.substr(close + 1);
        const size_t b = args.find_first_not_of(L" \t");
        args = (b == std::wstring::npos) ? std::wstring() : args.substr(b);
        LaunchOrFocusApp(path, args);
        return;
      }
      // An unclosed quote is a typo, not a path: say so rather than trying to
      // open a file whose name begins with a quotation mark.
      DLOG_ERROR("SCRIPT: LAUNCH= has an unclosed quote: %ls", rest.c_str());
      if (bStrict) m_script.errors++;
      return;
    }
    LaunchOrFocusApp(rest);
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
    // ...unless this connection asked for STRICT (issue 102).
    //
    // THIS is the branch that had to be intercepted, not LaunchMessage's
    // fallback, and getting that wrong broke a test immediately. `SEND=`,
    // `NEXT`, `LOCK` and most of the rest of the vocabulary are not keywords in
    // the dispatcher at all -- they are SCRIPT commands, reached through the
    // fallback on purpose. Rejecting everything the dispatcher did not match
    // therefore rejected half the working vocabulary; `SEND=0x1B` stopped
    // arriving and two ESC presses no longer left testing mode.
    //
    // What a strict caller actually wants refused is what nothing recognises,
    // and in the script engine that is exactly this else: the branch that turns
    // an unknown token into TEXT DRAWN ON THE VISUALIZER. That is the behaviour
    // a harness cannot live with, because a typo lands in the frame it was
    // about to capture.
    if (bStrict) {
      extern PipeServer g_pipeServer;
      wchar_t reply[320];
      FormatTo(reply, L"ERR|UNKNOWN|%s", cmd.c_str());
      g_pipeServer.Send(reply);
      // COUNTED and logged at ERROR, not just answered down the pipe (#303).
      //
      // The reply is for a harness that is listening. A script run by a user
      // has nobody on the other end, so the failure went nowhere: it was
      // suppressed from the screen, which is the point of strict, and then
      // silently discarded. The count is what lets the run report a verdict
      // when the script ends.
      m_script.errors++;
      DLOG_ERROR("SCRIPT: unknown command '%ls' (strict)", cmd.c_str());
      return;
    }
    // Build a message using current default style
    std::wstring text = cmd;
    // Replace // with newlines (Milkwave convention)
    size_t pos;
    while ((pos = text.find(L"//")) != std::wstring::npos)
      text.replace(pos, 2, L"\n");

    wchar_t msgBuf[2048];
    FormatTo(msgBuf, L"MSG|font=%s|size=%d|color_r=%d|color_g=%d|color_b=%d|text=%s",
      m_script.defaultFont.c_str(),
      m_script.defaultSize,
      m_script.defaultR, m_script.defaultG, m_script.defaultB,
      text.c_str());
    LaunchMessage(msgBuf);
  }
}

void Engine::SyncScriptUI() {
  // Try ScriptWindow first, fall back to Settings window
  HWND hw = m_scriptWindow ? m_scriptWindow->GetHWND() : NULL;
  if (!hw || !IsWindow(hw))
    hw = NULL;
  if (!hw)
    return;

  // Update listbox selection
  HWND hList = GetDlgItem(hw, IDC_MW_SCRIPT_LIST);
  if (hList) {
    if (m_script.currentLine >= 0)
      SendMessage(hList, LB_SETCURSEL, m_script.currentLine, 0);
    else
      SendMessage(hList, LB_SETCURSEL, (WPARAM)-1, 0);
  }

  // Update line label
  HWND hLine = GetDlgItem(hw, IDC_MW_SCRIPT_LINE);
  if (hLine) {
    if (m_script.playing && m_script.currentLine >= 0) {
      wchar_t buf[64];
      FormatTo(buf, L"Line %d of %d", m_script.currentLine + 1, (int)m_script.lines.size());
      SetWindowTextW(hLine, buf);
    } else if (!m_script.lines.empty()) {
      wchar_t buf[64];
      FormatTo(buf, L"%d lines loaded", (int)m_script.lines.size());
      SetWindowTextW(hLine, buf);
    } else {
      SetWindowTextW(hLine, L"No script loaded");
    }
  }

  // Update BPM/Beats edit fields
  HWND hBpm = GetDlgItem(hw, IDC_MW_SCRIPT_BPM);
  if (hBpm) {
    wchar_t buf[32];
    FormatTo(buf, L"%.1f", m_script.bpm);
    SetWindowTextW(hBpm, buf);
  }
  HWND hBeats = GetDlgItem(hw, IDC_MW_SCRIPT_BEATS);
  if (hBeats) {
    wchar_t buf[32];
    FormatTo(buf, L"%d", m_script.beats);
    SetWindowTextW(hBeats, buf);
  }
}

} // namespace mdrop
