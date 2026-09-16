/*
  Plugin module: Preset Management
  Extracted from engine.cpp for maintainability.
  Contains: Preset loading, browsing, file operations, blend pattern, plasma generation
*/

#include "tcp_server.h"  // Must be before engine.h — winsock2.h must precede windows.h
#include "engine.h"
#include "preset_replacements.h"
#include <shlwapi.h>
#include "builtin_preset.h"
#include "seh_guard.h"
#include "engine_helpers.h"
#include "format_to.h"
#include "json_utils.h"
#include "milk3_format.h"
#include "pipe_server.h"
#include "utility.h"
#include "support.h"
#include "resource.h"
#include "defines.h"
#include "shell_defines.h"
#include "wasabi.h"
#include <assert.h>
#include <locale.h>
#include <process.h>
#include <strsafe.h>
#include <Windows.h>
#include "AutoCharFn.h"
#include <sstream>
#include <condition_variable>
#include <algorithm>
#include <map>
#include "tool_window.h"

#define FRAND ((rand() % 7381)/7380.0f)

#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace mdrop {

extern Engine g_engine;

// ---------------------------------------------------------------------------
// SEH crash diagnostics — writes register state and stack trace to
// diag_seh_crash.txt so JIT / EEL crashes can be analyzed post-mortem.
// Called from the __except filter expression (GetExceptionInformation() is
// only valid there).  Returns EXCEPTION_EXECUTE_HANDLER so the handler runs.
// ---------------------------------------------------------------------------
// The general half of this -- registers and a stack walk -- moved to
// seh_guard.cpp when window procedures needed the same report (forgejo#75).
// One implementation, because a second copy of a stack walker is how the two
// drift. What stayed here is the EEL half, which is this module's business:
// g_eelCompileCtx knows which compile phase was running and holds the source
// text, and neither means anything to a window procedure.
static LONG WriteSEHCrashDiag(EXCEPTION_POINTERS* ep, const wchar_t* presetPath)
{
    SehCrashFilter(ep, presetPath);

    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    SYSTEMTIME st;
    GetLocalTime(&st);

    // --- Write EEL-specific diagnostics to diag_eel_error.txt ---
    if (g_eelCompileCtx.phase) {
        FILE* ef = DebugLogDiagOpen(L"diag_eel_error.txt", L"a"); // append — accumulates across crashes
        if (ef) {
            fwprintf(ef, L"\n========== EEL CRASH %04d-%02d-%02d %02d:%02d:%02d ==========\n",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            fwprintf(ef, L"Preset: %s\n", presetPath ? presetPath : L"<unknown>");
            fwprintf(ef, L"Phase:  %hs\n", g_eelCompileCtx.phase);
            fwprintf(ef, L"Exception: 0x%08X at 0x%016llX\n",
                     er->ExceptionCode, (DWORD64)er->ExceptionAddress);

            // Check if crash address is in JIT memory (no module owns it)
            HMODULE hCrashMod = NULL;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)er->ExceptionAddress, &hCrashMod);
            fwprintf(ef, L"JIT crash: %s\n", hCrashMod ? L"NO (in loaded module)" : L"YES (address not in any loaded module)");

            // Dump the EEL source text (truncated to 4KB)
            if (g_eelCompileCtx.sourceText) {
                fwprintf(ef, L"\nEEL Source:\n-----------\n");
                // Write up to 4096 chars of source
                const char* src = g_eelCompileCtx.sourceText;
                int len = 0;
                while (src[len] && len < 4096) len++;
                fwrite(src, 1, len, ef);
                if (src[len]) fprintf(ef, "\n... (truncated at 4KB)");
                fprintf(ef, "\n-----------\n\n");
            }

            fclose(ef);
        }

        DLOG_ERROR("EEL crash in phase '%s' — diagnostics written to diag_eel_error.txt",
                   g_eelCompileCtx.phase);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}
extern int NumTotalPresetsLoaded;
extern std::chrono::steady_clock::time_point LastSentMDropDX12Message;

// Thread globals — defined in engine.cpp
extern volatile HANDLE g_hThread;
extern volatile bool g_bThreadAlive;
extern volatile int g_bThreadShouldQuit;
extern CRITICAL_SECTION g_cs;
extern CRITICAL_SECTION g_csPresetPending;

// Forward declaration for helper defined later in this file
// (non-static because also called from engine.cpp and engine_input.cpp)

void Engine::dumpmsg(wchar_t* s, int level) {
  DebugLogW(s, level);
}

// Returns the full path for a preset at index idx.
// If szFilename is already absolute (e.g., loaded from a saved list), uses it as-is.
// Otherwise prepends m_szPresetDir.
//----------------------------------------------------------------------
// PresetNameAt / PresetCount / PresetIsDirAt (#11)
//
// The three questions almost every caller of m_presets was actually asking.
// They answer BY VALUE, which is the whole point: the callers used to take a
// .c_str() into the vector and carry it across a SendMessage, and the render
// thread can replace the vector wholesale at any moment.
//----------------------------------------------------------------------

std::wstring Engine::PresetNameAt(int idx) const
{
    return m_presets.with([&](const PresetList& v) {
        return (idx >= 0 && idx < (int)v.size()) ? v[idx].szFilename
                                                 : std::wstring();
    });
}

size_t Engine::PresetCount() const
{
    return m_presets.with([](const PresetList& v) { return v.size(); });
}

bool Engine::PresetIsDirAt(int idx) const
{
    return m_presets.with([&](const PresetList& v) {
        return idx >= 0 && idx < (int)v.size() &&
               !v[idx].szFilename.empty() && v[idx].szFilename[0] == L'*';
    });
}

void Engine::BuildPresetPath(int idx, wchar_t* szOut, int nMax) const {
  const std::wstring nameAt = PresetNameAt(idx);
  const wchar_t* fn = nameAt.c_str();
  // Check for absolute path (drive letter or UNC)
  bool bAbsolute = (fn[0] && fn[1] == L':') || (fn[0] == L'\\' && fn[1] == L'\\');
  if (bAbsolute)
    lstrcpynW(szOut, fn, nMax);
  else
    swprintf(szOut, nMax, L"%s%s", m_szPresetDir, fn);
}

// Every display that holds its own preset in THIS process steps too (#341).
//
// "Every independent display steps with us" was implemented as
// BroadcastToChildren, which walks m_children -- actual child PROCESSES. That
// was the whole population when own-preset meant own-process. #186 phase 4 added
// the middle tier -- a display holding its own preset on an in-process surface
// -- and the nudge was never widened to it, so a display set to "Own preset"
// was simply left behind by Next and Prev.
//
// Reported as "the next command is only working for the main render when I have
// a profile loaded that has independent presets that have a profile with a named
// preset to use", and measured: SIGNAL|NEXT_PRESET left both own-preset displays
// on their preset while DISPLAY_NEXT=<N> moved them. The config in that report is
// what makes it VISIBLE rather than what causes it -- timeBetweenPresets=0 means
// never (only a negative value inherits), so a display pinned to a named startup
// preset has no clock of its own and the global Next is the only thing that
// would ever move it.
//
// The same bug, in the same function, a second time: engine_input.cpp's
// RenderCmd::NextPreset comment records the first round, when calling
// LoadRandomPreset instead of NextPreset "meant a remote pressing Next moved the
// primary and left every independent display where it was".
//
// StepDisplayOwnPreset, not a signal: it is the in-process step the addressed
// DISPLAY_NEXT=/DISPLAY_PREV= and the Displays window's buttons already share,
// so all three paths pick a preset the same way and cannot drift apart again.
// Still a nudge and not an assignment -- each display picks from its OWN
// directory at its OWN sequence position, so a wall of screens does not fall
// into lockstep.
void Engine::StepOwnPresetDisplays(bool next)
{
  if (m_bChildMode) return;
  // m_displayOutputsMutex: #27. Recursive, so the callees may take it again.
  std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);
  for (auto& out : m_displayOutputs) {
    if (!DisplayHoldsOwnPreset(out.config)) continue;
    // A READY child already had the broadcast, and stepping it here as well
    // would advance that display twice for one keypress. Not bOwnProcess:
    // a display asked for a child that has not answered its pipe yet still
    // has its in-process surface, and skipping it would swallow the step.
    if (ChildOwnsDisplayForIpc(out.config.szDeviceName)) continue;
    const wchar_t* why = nullptr;
    if (!StepDisplayOwnPreset(out.config, next, &why)) {
      // Not an error worth showing anyone: a display whose surface is not up
      // yet, or whose directory is empty, simply does not step. Logged because
      // "Next did nothing on one screen" is otherwise invisible.
      DLOG_INFO("display %ls did not step its own preset: %ls",
                out.config.szDeviceName, why ? why : L"unknown");
    }
  }
}

void Engine::PrevPreset(float fBlendTime) {
  // Every independent display steps with us. A nudge, not an assignment: each
  // child picks its OWN previous preset from its OWN directory and history, so
  // a wall of displays does not fall into lockstep (forgejo#22).
  BroadcastToChildren(L"SIGNAL|PREV_PRESET");
  StepOwnPresetDisplays(false);
  PrevPresetLocal(fBlendTime);
}

// The same step, this instance only. Addressing ONE display must not move the
// others, so DISPLAY_PREV=<N> aimed at the primary comes here rather than
// through PrevPreset. NextPreset already had this split -- LoadRandomPreset is
// its non-broadcasting half -- and PrevPreset did not.
void Engine::PrevPresetLocal(float fBlendTime) {
  if (m_RemotePresetLink) {
    PostMessageToMDropDX12Remote(WM_USER_PREV_PRESET);
    return;
  }

  // No files to step through (only dirs / empty list) — never index m_presets[-1].
  if (m_nPresets - m_nDirs <= 0)
    return;

  if (m_bSequentialPresetOrder) {
    m_nCurrentPreset--;
    if (m_nCurrentPreset < m_nDirs)
      m_nCurrentPreset = m_nPresets - 1;
    if (m_nCurrentPreset >= m_nPresets) // just in case
      m_nCurrentPreset = m_nDirs;
    if (m_nCurrentPreset < m_nDirs || m_nCurrentPreset >= m_nPresets)
      return;

    wchar_t szFile[MAX_PATH];
    BuildPresetPath(m_nCurrentPreset, szFile, MAX_PATH);

    LoadPreset(szFile, fBlendTime);
  }
  else {
    int prev = (m_presetHistoryPos - 1 + PRESET_HIST_LEN) % PRESET_HIST_LEN;
    if (m_presetHistoryPos != m_presetHistoryBackFence) {
      m_presetHistoryPos = prev;
      if (m_presetHistory[m_presetHistoryPos].empty())
        return;
      LoadPreset(m_presetHistory[m_presetHistoryPos].c_str(), fBlendTime);
    }
  }
}

void Engine::NextPreset(float fBlendTime)  // if not retracing our former steps, it will choose a random one.
{
  // See PrevPreset: each child advances to its own next-or-random preset.
  // Broadcast here rather than inside LoadRandomPreset, because that is also
  // the auto-advance timer's entry point and a child runs its own timer -- a
  // broadcast from there would make every child jump whenever the PARENT's
  // interval elapsed, which is the lockstep this feature exists to avoid.
  BroadcastToChildren(L"SIGNAL|NEXT_PRESET");
  // And the displays holding their own preset in THIS process, which no
  // broadcast can reach -- see StepOwnPresetDisplays (#341).
  StepOwnPresetDisplays(true);
  LoadRandomPreset(fBlendTime);
}

void Engine::LoadRandomPreset(float fBlendTime) {
  if (m_RemotePresetLink) {
    PostMessageToMDropDX12Remote(WM_USER_NEXT_PRESET);
    return;
  }

  // make sure file list is ok
  if (m_nPresets - m_nDirs == 0) {
    // An empty list while the scan is STILL RUNNING is not an empty directory,
    // it is a list that has not arrived yet, and the two must not be confused.
    //
    // Measured on Shane's install, 933 .milk files present and nPresetFilter=1:
    //
    //   09:34:40.440  StartupPreset: none configured, loading random
    //   09:34:40.440  ERROR: No preset files found in ...\presets1\*.milk
    //   09:34:40.564  LoadPreset: Evet + Geiss ... why=startup
    //
    // The scan published 124 ms later and a preset from that very directory
    // loaded fine. So the error was pure noise, and the built-in fallback it
    // put up was a flash of the wrong preset on every cold start. The list is
    // built on a worker (g_bThreadAlive) and startup reaches here before it
    // finishes, so this raced by construction rather than by accident.
    if (!m_bAnyPresetEverShown && g_bThreadAlive) {
      // Deferred, NOT dropped. The flag is what the pending-list swap in
      // engine.cpp looks for; without it this return leaves the screen with no
      // preset at all, which is worse than the false error it replaced.
      m_bRandomPresetDeferred = true;
      DLOG_INFO("LoadRandomPreset: preset list still being built; deferring "
                "until the scan publishes rather than declaring it empty");
      return;
    }
    // Only step in if NOTHING has ever been shown this session (first run /
    // empty dir from the very start). If a preset -- real or the built-in
    // fallback -- is already up, just silently stay on it.
    if (!m_bAnyPresetEverShown) {
      wchar_t buf[1024];
      FormatResTo(buf, IDS_ERROR_NO_PRESET_FILE_FOUND_IN_X_MILK, m_szPresetDir);
      AddError(buf, 6.0f, ERR_MISC, true);
      DebugLogA("ERROR: No preset files found in preset directory; showing the "
               "built-in fallback preset", LOG_ERROR);

      // The built-in preset (builtin_preset.h), not a black window: an empty
      // library used to render nothing at all here, indistinguishable from a
      // crash to someone who just unzipped the app. Applied straight from
      // memory via the same mechanism the Preset Editor's live-Apply uses, so
      // no disk file or preset-list entry is needed.
      ApplyPresetTextToState(kBuiltinPresetMilk, PSIDE_LIVE);
      wcsncpy_s(m_pState->m_szDesc, kBuiltinPresetDesc, _TRUNCATE);

      // Deliberately does NOT switch to UI_LOAD. Opening the preset browser
      // as a side effect of an empty directory scan puts a panel over the
      // render output that the user did not ask for and cannot easily
      // associate with a cause -- it reads as "half the screen went dark".
      //
      // It fires from ordinary operation, not just first run: a preset load
      // over IPC calls ChangePresetDir -> UpdatePresetList, and any directory
      // that momentarily scans empty -- because it is being written to, or
      // because nPresetFilter excludes everything in it -- reaches here on the
      // next auto-advance. The error notification above already says what is
      // wrong, which is enough; the user can open the browser themselves.
    }
    return;
  }

  bool bHistoryEmpty = (m_presetHistoryFwdFence == m_presetHistoryBackFence);

  // if we have history to march back forward through, do that first
  if (!m_bSequentialPresetOrder) {
    int next = (m_presetHistoryPos + 1) % PRESET_HIST_LEN;
    if (next != m_presetHistoryFwdFence && !bHistoryEmpty) {
      m_presetHistoryPos = next;
      LoadPreset(m_presetHistory[m_presetHistoryPos].c_str(), fBlendTime);
      return;
    }
  }

  // --TEMPORARY--
  // this comes in handy if you want to mass-modify a batch of presets;
  // just automatically tweak values in Import, then they immediately get exported to a .MILK in a new dir.
  /*
  for (int i=0; i<m_nPresets; i++)
  {
    char szPresetFile[512];
    lstrcpy(szPresetFile, m_szPresetDir);	// note: m_szPresetDir always ends with '\'
    lstrcat(szPresetFile, m_pPresetAddr[i]);
    //CState newstate;
    m_state2.Import(szPresetFile, GetTime());

    lstrcpy(szPresetFile, "c:\\t7\\");
    lstrcat(szPresetFile, m_pPresetAddr[i]);
    m_state2.Export(szPresetFile);
  }
  */
  // --[END]TEMPORARY--

  if (m_bSequentialPresetOrder) {
    m_nCurrentPreset++;
    if (m_nCurrentPreset < m_nDirs || m_nCurrentPreset >= m_nPresets)
      m_nCurrentPreset = m_nDirs;
  }
  else {
    // pick a random file
    // One read of the cumulative total, under the guard.
    const float fRatingTotal = m_presets.with([&](const PresetList& v) {
      return (m_nPresets > 0 && m_nPresets <= (int)v.size())
                 ? v[m_nPresets - 1].fRatingCum : 0.0f;
    });
    if (!m_bEnableRating || (fRatingTotal < 0.1f))// || (m_nRatingReadProgress < m_nPresets))
    {
      // Try up to 50 times to avoid skip/broken presets
      int nFiles = m_nPresets - m_nDirs;
      for (int attempt = 0; attempt < 50 && nFiles > 0; attempt++) {
        m_nCurrentPreset = m_nDirs + (rand() % nFiles);
        uint32_t flags = 0;
        WithAnnotation(PresetNameAt(m_nCurrentPreset).c_str(),
                       /*create=*/false,
                       [&](PresetAnnotation& a) { flags = a.flags; });
        if (!(flags & (PFLAG_SKIP | PFLAG_BROKEN)))
          break; // found a non-skipped preset
      }
    }
    else {
      float cdf_pos = (rand() % 14345) / 14345.0f * fRatingTotal;

      /*
      char buf[512];
      sprintf(buf, "max = %f, rand = %f, \tvalues: ", m_presets[m_nPresets - 1].fRatingCum, cdf_pos);
      for (int i=m_nDirs; i<m_nPresets; i++)
      {
        char buf2[32];
        sprintf(buf2, "%3.1f ", m_presets[i].fRatingCum);
        lstrcat(buf, buf2);
      }
      dumpmsg(buf);
      */

      // One hold for the whole binary search: it is O(log n) probes into
      // the cumulative distribution, and taking the guard per probe would let
      // the list change underneath a search that assumes it is monotonic.
      m_nCurrentPreset = m_presets.with([&](const PresetList& v) {
        if (m_nDirs >= (int)v.size()) return m_nDirs;
        if (cdf_pos < v[m_nDirs].fRatingCum) return m_nDirs;
        int lo = m_nDirs;
        int hi = m_nPresets;
        while (lo + 1 < hi) {
          int mid = (lo + hi) / 2;
          if (mid < (int)v.size() && v[mid].fRatingCum > cdf_pos)
            hi = mid;
          else
            lo = mid;
        }
        return hi;
      });
      // If selected preset is skip/broken, try a few more random picks
      auto flagsOfCurrent = [this]() {
        uint32_t flags = 0;
        WithAnnotation(PresetNameAt(m_nCurrentPreset).c_str(),
                       /*create=*/false,
                       [&](PresetAnnotation& a) { flags = a.flags; });
        return flags;
      };
      if (flagsOfCurrent() & (PFLAG_SKIP | PFLAG_BROKEN)) {
        int nFiles = m_nPresets - m_nDirs;
        for (int attempt = 0; attempt < 20 && nFiles > 0; attempt++) {
          m_nCurrentPreset = m_nDirs + (rand() % nFiles);
          if (!(flagsOfCurrent() & (PFLAG_SKIP | PFLAG_BROKEN)))
            break;
        }
      }
    }
  }

  // m_pPresetAddr[m_nCurrentPreset] points to the preset file to load (w/o the path);
  // first prepend the path, then load section [preset00] within that file
  wchar_t szFile[MAX_PATH] = { 0 };
  BuildPresetPath(m_nCurrentPreset, szFile, MAX_PATH);

  DLOG_INFO("LoadRandomPreset: idx=%d/%d file=%ls", m_nCurrentPreset, m_nPresets, PresetNameAt(m_nCurrentPreset).c_str());

  if (!bHistoryEmpty)
    m_presetHistoryPos = (m_presetHistoryPos + 1) % PRESET_HIST_LEN;

  LoadPreset(szFile, fBlendTime);
}

#include "config_store.h"

// ---------------------------------------------------------------------------
// Measured MilkDrop 3 PRO wipe geometry
// ---------------------------------------------------------------------------
//
// Recovered by measurement, not by guessing. MilkDrop stores the wipe per
// vertex as `mix = a*progress + c` (milkdropfs.cpp:6040), so every pixel flips
// at a progress fixed by its own wipe coordinate t = f(x,y). Sweeping a
// solid-colour .milk2 probe through 11 progress values in MD3 PRO and recording
// each pixel's crossing recovers f directly; candidate analytic forms were then
// ranked by correlation.
//
// The recovery is self-validating: with centre-fitted radial, box and diamond
// families all offered, `cercle` independently picked Euclidean distance,
// `square` picked Chebyshev and `triangle` picked L1 -- nothing in the fitter
// knew the pattern names.
//
// Two things measured NOT to matter, both previously assumed to:
//   * `blending_direction` is inert BETWEEN 0 AND +1. Matched d=0/d=1 pairs are
//     byte-identical (MAE 0.0000) even for strongly asymmetric fields where a
//     mirror would score 0.507.
//
//     Read at the time as "direction does nothing", which it is not: 0 and +1
//     are the one pair MD3 genuinely treats alike, because its only test is
//     `== -1`. The sweep that produced this line never compared against -1.
//     Direction matters for exactly four patterns -- see Milk2DirectionInverts.
//   * `random_1..5` do not move the wipe centre. `cercle` recovers
//     radial@(0.83,0.75) under no randoms and under two opposite random sets.
//     They set the band WIDTH only.

// Reports the field a .milk2 just produced, and warns when it has collapsed.
//
// A member rather than inline because it has to run at EVERY exit of the field
// builder, and it did not: the baked path returned before reaching it, so the
// three patterns that hurt most -- cisor and the two degenerate stars files --
// were the ones it could never see. Verified by its own absence: a probe over
// ten patterns got readings for seven and silence for cisor, donuts and snail,
// all baked.
//
// The MEAN matters more than the span. mix is clamped to [0,1] per vertex, so
// any healthy field reports [0.0000, 1.0000] whichever direction it was built
// at, and the span cannot tell a field from its own inverse. The mean can:
// inverting a field sends mean w to roughly 1 - w.
void Engine::CheckMilk2BlendField(const BlendPatternTarget& tgt,
                                  const char* pattern, int direction, int nVert) {
  if (nVert <= 0 || !tgt.vertinfo) return;
  const float progress = m_fMilk2FrozenProgress;
  float lo = 1.0f, hi = 0.0f;
  double sum = 0.0;
  int atP1 = 0, atP2 = 0;          // vertices pinned to one preset or the other
  for (int i = 0; i < nVert; i++) {
    float mix = tgt.vertinfo[i].a * progress + tgt.vertinfo[i].c;
    if (mix < 0.0f) mix = 0.0f;
    else if (mix > 1.0f) mix = 1.0f;
    if (mix < lo) lo = mix;
    if (mix > hi) hi = mix;
    if (mix < 0.01f) atP1++;
    else if (mix > 0.99f) atP2++;
    sum += mix;
  }
  const float mean = (float)(sum / nVert);
  const float pinned = (float)((atP1 > atP2 ? atP1 : atP2)) / (float)nVert;

  // "Only one preset is visible" -- which is NOT the same as "the field is
  // uniform", and the difference cost a false positive on sixteen files.
  //
  // The first version tested the span (hi - lo < 0.01). That is wrong in both
  // directions. It misses the near misses, which look identical on screen:
  // `arrow` spans 0.215 and still has a mean weight of 0.001, because 99% of
  // the grid is pinned at zero and one corner does all the varying; `cisor` is
  // the same shape at 95%. And it fires on a field that is uniform ON PURPOSE:
  // `zoom` takes legacy mixtype 0, which writes a=1, c=0 so that mix ==
  // progress at every vertex -- a crossfade showing BOTH presets everywhere,
  // exactly as intended. Widening the guard's coverage surfaced sixteen of
  // those immediately, reporting "0% of the grid is pinned" while calling it
  // collapsed.
  //
  // Counting PINNED vertices is the test that means what the warning says.
  // A uniform 0.71 is two presets mixed; 100% at 0.0 is one preset.
  //
  // Gated on the file asking for a PARTIAL blend, because a .milk2 frozen at
  // progress 0 or 1 shows one preset because that is what it says to do.
  // Pinned is necessary but not sufficient, because a blend that is nearly
  // FINISHED is legitimately near-pinned: at progress 0.95 almost the whole
  // grid should be showing preset 2, and saying so is noise.
  //
  // The invariant that separates the two is that a well-formed wipe's mean
  // weight tracks its progress -- the flip values are spread across the field,
  // so at progress p roughly p of the grid has crossed. A field whose flip
  // range does not overlap the file's progress at all breaks that badly:
  // `curtain` sits at mean 0.000 against a progress of 0.10-0.45. A field that
  // is merely late sits at mean 0.99 against a progress of 0.95, and is fine.
  //
  // Measured over the 298 direction=-1 files: pinned alone flagged four, of
  // which three were simply late (progress 0.85-0.95, mean within 0.14 of it).
  // The plasma family is exempt, and the exemption is measured rather than
  // assumed.
  //
  // Both tests above describe a field whose flip values are SPREAD -- pinned
  // counts how much of the grid sits at an extreme, drift assumes roughly
  // `progress` of a spread field has crossed by `progress`. A plasma field is
  // noise. Its flip values cluster wherever GenPlasma puts them, so a run of
  // them landing below the file's progress is ordinary, not a failure.
  //
  // Two files tripped this before the exemption: `Rainbow Attack NEON3`
  // (plasma, dir=+1, 96% pinned, mean 0.982 at progress 0.59) and `the sewers2
  // light` (plasma2, dir=-1, 95%, mean 0.971 at 0.53). Both were A/B'd against
  // MD3 PRO and both sit at the self-comparison noise floor -- badness 6.6 /
  // corr +0.46 and 5.2 / +0.78, against a floor of 5.1 / +0.79 measured by
  // milk2_noise_floor.py. MD3 renders them the same way. They are not defects.
  //
  // The failure this guard exists for cannot reach them anyway: #114 was a
  // FITTED line mirrored outside the range it was fitted on, and plasma takes
  // neither the fitted table nor the baked fields -- it is generated per vertex
  // from the recovered GenPlasma, which cannot leave its own range.
  const bool isPlasma = pattern && _strnicmp(pattern, "plasma", 6) == 0;
  const bool partial = (progress > 0.02f && progress < 0.98f);
  const float drift  = fabsf(mean - progress);
  if (!isPlasma && partial && pinned >= 0.95f && drift > 0.25f) {
    // NOT necessarily a defect, and the wording matters because it was read as
    // one. With the recovered generators a pinned frame is often FAITHFUL: a
    // narrow band is a sharp edge, so at a frozen progress outside its window
    // the whole frame legitimately shows one preset, and MD3 does the same.
    // Measured on MilkDrop2077.R090d (side, band 0.247, progress 0.33): MD3
    // pins 96.5% of its grid where we pin 95.8%, frame MAE 0.0033 across the
    // sweep. Thirteen corpus files trip this and every one checked matches MD3.
    //
    // It stayed silent under the fitted table only because a fitted line spans
    // nearly the whole schedule and so can never pin. Read it as "this file
    // shows one preset at its own progress", and go and compare against MD3
    // before calling it a bug.
    DLOG_WARN("milk2 wipe '%s' dir=%d progress=%.3f is one-sided: %.0f%% of the "
              "grid is pinned to one preset (mix span %.3f-%.3f, mean %.3f). "
              "Faithful if MD3 does the same at this progress -- check before "
              "treating it as a defect (issue 114, issue 128).",
              pattern ? pattern : "(none)", direction, progress,
              pinned * 100.0f, lo, hi, mean);
  }
  // Always, at Info. This is what makes the direction rule testable without
  // rendering anything and without driving MD3: load the same file at +1 and
  // at -1 and compare two numbers.
  //
  // The invariant, straight from the decompilation: for the twenty-six
  // patterns MD3 does not reverse, the two means must be IDENTICAL; for
  // cercle, triangle, corner and patches they must be complementary. That is
  // Milk2DirectionInverts stated as something a script can check.
  DLOG_INFO("milk2 wipe '%s' dir=%d progress=%.3f field mix=[%.4f,%.4f] mean=%.4f",
            pattern ? pattern : "(none)", direction, progress, lo, hi, mean);
}

// The four patterns MD3 actually reverses. Recovered from the decompilation,
// not fitted.
//
// `blending_direction` is the global DAT_00f1c55c, and its consumption set is
// grep-COMPLETE, not sampled: eleven references in MilkDroprev.c, of which two
// are declarations, three the 'U' hotkey writes (46295 / 46339 / 46383), one
// the randomiser (52952), one the save (55893), one the parse (65629) -- and
// exactly FOUR reads, all inside the wipe generator, all spelled
// `bVar10 = DAT_00f1c55c == -1;`:
//
//     MilkDroprev.c:54249 patches   :54319 corner   :54465 triangle   :54822 cercle
//
// Nothing in the render path. No preset swap, no sign on progress, no reversed
// comparison. Twenty-six of the thirty patterns never read the value at all.
//
// At each of the four the statement is the same, negating that pattern's own
// normalised coordinate before the vertex write (cercle, :54822 verbatim):
//
//     bVar10 = DAT_00f1c55c == -1;
//     *(float *)(unaff_EBP + -0x38) = *(float *)(unaff_EBP + -0x50) * 1.41421;
//     if (bVar10) { fVar13 = 1.0 - *(float *)(unaff_EBP + -0x38); }
//     else        { fVar13 = *(float *)(unaff_EBP + -0x38); }
//
// The branch-to-NAME mapping is read, not inferred: MD3PROVJ.c:56225-56294
// preserves the thirty string arguments Ghidra dropped from MilkDroprev.c, in
// the canonical `Pattern 0: zoom` .. `Pattern 29: cross2` order.
//
// Note what this does NOT change. `t -> 1-t` and `flip -> 1-flip` are the same
// operation: with p_flip = (1 - t + band/2)/(1 + band),
//
//     flip(1-t) = (t + band/2)/(1+band) = 1 - flip(t)
//
// for every t and every band. The shipped rule, measured on `cercle`, was never
// wrong in kind. It was applied to all ten analytic rows and all fifteen baked
// fields, where MD3 applies it to four patterns -- and a row fitted over half
// the progress range, mirrored, lands outside it entirely. That is why every
// `direction=-1` curtain file renders as one flat preset.
static bool Milk2DirectionInverts(const char* name) {
  return name && (_stricmp(name, "cercle")   == 0 ||   // t = sqrt(x*x+y*y)*1.41421
                  _stricmp(name, "triangle") == 0 ||   // t = |x| + |y|
                  _stricmp(name, "corner")   == 0 ||   // t = off-centre radial
                  _stricmp(name, "patches")  == 0);    // t = (sin+cos)*0.25+0.5
}

// Fills the target canvas from the measured geometry. Returns false when the
// pattern has no measured form, leaving it untouched for the caller's fallback.
//
// Deterministic by construction: a .milk2 is a permanent frozen blend, so a
// field that differed per load would be wrong. Nothing here calls FRAND.
// Seven patterns MD3 does NOT give the random band to (issue 128).
//
// Everything below is READ out of MilkDroprev.c, not fitted. The vertex pair is
// the same one milkdropfs.cpp:5517 consumes -- mix = a*progress + c -- so a
// recovered branch is transcribed directly, with no band, no flip map and no
// fitted line in between.
//
// The branch-to-name mapping is now STRUCTURAL rather than measured. The thirty
// branches each open with the same preamble, the one that picks the pattern's
// own transition time:
//
//     (((float)iVar3 / 32767.0) * A + B) * 100.0
//
// Counting those gives exactly thirty in 52900..56250, in the same order as the
// thirty string compares in MD3PROVJ.c:56225-56294 -- reversed, because Ghidra
// prints the deepest nesting first. So
//
//     name_index = 29 - branch_index
//
// Six independent anchors agree, ALL OF THEM IN THE LOWER HALF: the four
// direction-inverting patterns (patches :54249 -> 16, corner :54319 -> 17,
// triangle :54465 -> 19, cercle :54822 -> 24, and 16+13 = 17+12 = 19+10 =
// 24+5 = 29), and the two plasma branches whose assignment was previously
// settled by sweeping both ways -- plasma3 :54876 -> 25 -> name 4, plasma2
// :54980 -> 26 -> name 3, exactly what the measurement concluded.
//
// The seven below are NOT identified by that arithmetic, and should not be.
// Every anchor sits at name index 13 or lower, and the probe harness's own
// pattern list disagrees with MD3PROVJ.c's chain about where stars and stars2
// fall -- which is precisely the range these live in. So position gets them
// close and CONTENT settles them, each one twice over:
//
//   curtain          the only branch with inv_band 3.3333 = 1/0.3, and a sin
//                    hump rather than a ramp -- both independently stated in
//                    issue 128 before this reading
//   vertical         a = 6.0, inv_band 5.0 = 1/0.2, and its projection is
//   horizontal       dominated by x and by y respectively (cos(6.28) = 1.0)
//   linesvertical    a = 7.0 and sixty stripes varying along x
//   lineshorizontal  a = 7.0 and the same varying along y
//   bubbles          a = 7.0 and a product of two sinusoids -- a lattice, not
//                    a stripe, which is why no primitive ever fitted it
//   zoom             uniform, the one branch with no band at all
//
// A wipe named "vertical" draws a vertical edge, so it must vary along x. That
// is what decides the two pairs, not their order in a chain.
//
// FUN_004a5c20 is cos and FUN_004a5cc0 is sin, identified independently here
// (the vertical branch needs the x term to dominate, and cos(6.28) = 1.0 while
// sin(6.28) = -0.0032) and in ApplyMilk2BlendPattern above.
namespace {

// MD3's fixed wipe angle. Every one of these branches passes the same double,
// 0x40191eb860000000 = 6.28 -- two pi to three figures, which is why these
// wipes are axis-aligned to within a third of a percent rather than exactly.
const float kMilk2WipeAngle = 6.28f;

struct Milk2Recovered { float a, c; };

// t -> the vertex pair, in MD3's own parameterisation: c = t*invBand - invBand.
inline Milk2Recovered FromT(float t, float a, float invBand) {
  return { a, t * invBand - invBand };
}

}  // namespace

// Fills the target canvas from a branch recovered verbatim. False when it has
// no recovered generator, leaving the caller's fitted and baked paths alone.
bool Engine::ComputeMilk2RecoveredField(const BlendPatternTarget& tgt,
                                        const char* pattern, int direction) {
  if (!pattern || !tgt.vertinfo || m_nGridX <= 0 || m_nGridY <= 0)
    return false;

  enum Kind { NONE, CURTAIN, VERTICAL, HORIZONTAL, LINESV, LINESH, BUBBLES, ZOOM,
              CERCLE, SQUARE, TRIANGLE, CORNER, PATCHES, DONUTS, CLOCK, NUCLEAR,
              STARS, STARS2, CISOR, WAVE, CROSS, CROSS2, SNAIL, SNAIL2, SNAIL3,
              CHECKERBOARD, ARROW, SIDE };
  Kind kind = NONE;
  if      (_stricmp(pattern, "curtain")         == 0) kind = CURTAIN;
  else if (_stricmp(pattern, "vertical")        == 0) kind = VERTICAL;
  else if (_stricmp(pattern, "horizontal")      == 0) kind = HORIZONTAL;
  else if (_stricmp(pattern, "linesvertical")   == 0) kind = LINESV;
  else if (_stricmp(pattern, "lineshorizontal") == 0) kind = LINESH;
  else if (_stricmp(pattern, "bubbles")         == 0) kind = BUBBLES;
  else if (_stricmp(pattern, "zoom")            == 0) kind = ZOOM;
  else if (_stricmp(pattern, "cercle")          == 0) kind = CERCLE;
  else if (_stricmp(pattern, "square")          == 0) kind = SQUARE;
  else if (_stricmp(pattern, "triangle")        == 0) kind = TRIANGLE;
  else if (_stricmp(pattern, "corner")          == 0) kind = CORNER;
  else if (_stricmp(pattern, "patches")         == 0) kind = PATCHES;
  else if (_stricmp(pattern, "donuts")          == 0) kind = DONUTS;
  else if (_stricmp(pattern, "clock")           == 0) kind = CLOCK;
  else if (_stricmp(pattern, "nuclear")         == 0) kind = NUCLEAR;
  else if (_stricmp(pattern, "stars")           == 0) kind = STARS;
  else if (_stricmp(pattern, "stars2")          == 0) kind = STARS2;
  else if (_stricmp(pattern, "cisor")           == 0) kind = CISOR;
  else if (_stricmp(pattern, "wave")            == 0) kind = WAVE;
  else if (_stricmp(pattern, "cross")           == 0) kind = CROSS;
  else if (_stricmp(pattern, "cross2")          == 0) kind = CROSS2;
  else if (_stricmp(pattern, "snail")           == 0) kind = SNAIL;
  else if (_stricmp(pattern, "snail2")          == 0) kind = SNAIL2;
  else if (_stricmp(pattern, "snail3")          == 0) kind = SNAIL3;
  else if (_stricmp(pattern, "checkerboard")    == 0) kind = CHECKERBOARD;
  else if (_stricmp(pattern, "arrow")           == 0) kind = ARROW;
  else if (_stricmp(pattern, "side")            == 0) kind = SIDE;
  if (kind == NONE)
    return false;

  const float cosA = cosf(kMilk2WipeAngle);
  const float sinA = sinf(kMilk2WipeAngle);
  const float ax = tgt.aspectX, ay = tgt.aspectY;

  // Absent randoms draw the middle, the same convention ApplyMilk2BlendPattern
  // uses -- a file with no header must not render as though every draw was 0.
  const float r1 = m_bMilk2HasRandoms ? m_fMilk2Random[0] : 0.5f;
  const float r2 = m_bMilk2HasRandoms ? m_fMilk2Random[1] : 0.5f;
  const float r3 = m_bMilk2HasRandoms ? m_fMilk2Random[2] : 0.5f;
  const float r4 = m_bMilk2HasRandoms ? m_fMilk2Random[3] : 0.5f;

  // There is no coordinate correction here, and there must not be one.
  //
  // This carried `coordScale = 0.6563f * (gridX + 1)/gridX` and a long note
  // calling it "the one thing in here that is measured rather than read" --
  // MD3 walking its wipe coordinate at 2/3 of the rate its own source implies,
  // dimensionless, anchored top-left, a mesh larger than the frame.
  //
  // All of that was an artifact of the measuring rig. The probe harness ran
  // DPI-UNAWARE on a 150% display, where PrintWindow returns the TOP-LEFT
  // ~1/1.5 of a window at 1:1 rather than the whole window scaled down. MD3's
  // captures were two-thirds crops; ours come over the pipe from the app's own
  // back buffer and were whole frames. 0.6563 is 2/3 -- the crop -- and
  // "anchored at the top-left, running off the right edge and the bottom" is a
  // description of a crop written up as a property of MD3 (#167).
  //
  // Re-measured with the harness DPI-aware, on a probe rendering
  // `ret = float3(uv.x, uv.y, 0.5)` so the frame IS the coordinate field:
  //
  //     ours   u span 1.000   v span 1.000
  //     MD3    u span 0.996   v span 0.984      ratio 1.0039
  //
  // where the same probe previously read 0.992 against 0.655 = 1.514. MD3
  // covers the whole coordinate range exactly as we do.
  //
  // Two arguments kept this alive and both are worth recognising again. "We
  // only compare fractions" is true of a RESCALE and false of a CROP: a
  // feature at MD3's true centre reads at 0.5/0.667 = 0.75, which is exactly
  // what the trail probe measured. And the factor being dimensionless and
  // constant across a 1.9x range of window sizes was taken as evidence it was
  // real -- a display scale factor is precisely that.
  //
  // So u and v below are the plain mesh coordinates, and the vertical anchor
  // that went with the scale is gone too: it existed only to place a frame
  // inside a larger mesh, and there is no larger mesh.

  int nv = 0;
  for (int y = 0; y <= m_nGridY; y++) {
    // MD3's own y, NOT the flipped one the fitted table was measured in. Its
    // loops run the grid index straight through (iVar7 / gridY), so a flip here
    // would mirror every recovered field vertically.
    //
    // v runs 0..1 over the mesh and the frame IS the mesh. This used to carry
    // a top anchor, `v = (1 - s) + (y/gridY)*s`, to place the frame inside a
    // mesh believed to be 1/s larger than it. There is no larger mesh -- that
    // was the DPI-unaware capture crop, see the note above the loop -- so the
    // anchor is gone with the scale that motivated it.
    //
    // The argument that produced the anchor is worth keeping, because it was
    // sound reasoning from bad data and would be right if the premise ever
    // returned: horizontal's transposition makes its intercept and its slope
    // independent readings, and on cropped captures the two disagreed -- the
    // coordinate at the anchor matched the UNSCALED value while the slope
    // matched the scaled one, which no scale-about-the-origin can produce.
    // A crop reproduces exactly that asymmetry, because it leaves the origin
    // untouched and truncates the far edge.
    const float v = y / (float)m_nGridY;
    for (int x = 0; x <= m_nGridX; x++) {
      const float u = x / (float)m_nGridX;
      Milk2Recovered r{ 1.0f, 0.0f };

      switch (kind) {
      case CURTAIN: {
        // :53512  a = 4.33333 = (0.3 + 1)/0.3, band 0.3 hardcoded.
        //
        //     t = sin((proj + 0.5) * pi) * 0.5 + 0.5   ==   cos(proj*pi)*0.5 + 0.5
        //
        // A hump, not a ramp -- the shape no MW_BOX fit could represent at any
        // slope. The +0.5 and the pi are INSIDE the sine, which is the whole
        // difference: without them t sits near 0.5 everywhere and the wipe
        // crosses at progress 0.50 instead of somewhere in [0.115, 0.500].
        // Measured that way first, and MD3 crossed at 0.20 against our 0.50.
        const float pr = (u * ax - 0.5f) * cosA + (v * ay - 0.5f) * sinA;
        r = FromT(sinf((pr + 0.5f) * 3.1415927f) * 0.5f + 0.5f, 4.3333333f, 3.3333333f);
        break;
      }
      case VERTICAL: {
        // :53443 -- a = 6.0 = (0.2 + 1)/0.2, band 0.2 hardcoded.
        const float p = (u * ax - 0.5f) * cosA + (v * ay - 0.5f) * sinA;
        // MD3 adds 0.5 and immediately subtracts it again -- t_raw = p + 0.5,
        // then (t_raw - 0.5)/sqrt2 + 0.5 -- so the round trip cancels and only
        // the scale survives. The sqrt2 is FUN_004a5d10(2.0), the branch's only
        // square root, and it is what keeps a diagonal projection inside [0,1].
        const float t = p / 1.4142136f + 0.5f;
        r = FromT(t, 6.0f, 5.0f);
        break;
      }
      case HORIZONTAL: {
        // :53374 -- the same arithmetic as vertical, and NOT simply "the axes
        // exchanged", which is how this was first read and why it stayed the
        // worst of the seven.
        //
        // It is the ONLY one of the thirty branches whose loops are
        // TRANSPOSED. It runs its outer loop over gridX (:53376) and its inner
        // over gridY, multiplying the outer by aspectX and cos and the inner by
        // aspectY and sin -- while the vertex precompute (:42226) and the warp
        // consumer (:22417) both run y-outer/x-inner, as does every other
        // branch here. The vertex it writes is a plain linear counter either
        // way, so the field a vertex ends up holding is the one MD3 computed
        // for a DIFFERENT (x, y): the position that linear index would have in
        // a column-major walk.
        //
        // Reproducing the picture therefore means reproducing the
        // transposition, not correcting it. Undoing MD3's bug would be a
        // different renderer.
        //
        // The measurement says the same. MD3's horizontal field varies down the
        // frame with aspectX and across it at 1/(gridY + 1) of that rate, and
        // its two gradients carry OPPOSITE signs (du -0.0057, dv +0.3746)
        // where an untransposed reading gives them the same sign (ours were
        // +0.0023 and +0.4454). Opposite signs are the signature: the U term
        // survives only as the fractional part of the index arithmetic.
        // The index has to be built from the MESH position this screen vertex
        // corresponds to, not from our own loop counter: MD3's linear index
        // runs over its whole mesh, most of which is off-screen. Scale into
        // mesh coordinates first, THEN split the index -- and do not scale hu
        // and hv afterwards, because the index already is the mesh position.
        const float xm = (float)x;
        const float ym = (float)y;
        const int nvm = (int)(ym * (m_nGridX + 1) + xm + 0.5f);
        const int ix = nvm / (m_nGridY + 1);
        const int iy = nvm % (m_nGridY + 1);
        const float hu = ix / (float)m_nGridX;
        const float hv = iy / (float)m_nGridY;
        const float p = (hu * ax - 0.5f) * cosA + (hv * ay - 0.5f) * sinA;
        const float t = p / 1.4142136f + 0.5f;
        r = FromT(t, 6.0f, 5.0f);
        break;
      }
      case LINESV:
      case LINESH: {
        // :53323 linesvertical, :53268 lineshorizontal. Sixty stripes scaled by
        // random_1, and the vertex pair is written folded: a = 7.0, c = 2s - 2.
        // Decomposed that is band 0.5 with the SHARP slope, (0.5 + 3)/0.5 = 7 --
        // not band 1/6 with the ordinary one, which lands on the same 7.0 by
        // coincidence and would have been the wrong story.
        const float q = (kind == LINESV) ? (u - 0.5f) * ax : (v - 0.5f) * ay;
        const float sv = sinf(q * r1 * 60.0f);
        r = { 7.0f, sv + sv - 2.0f };
        break;
      }
      case BUBBLES: {
        // :54082 -- a product of two sinusoids ten cycles across, offset by
        // random_1, random_3 and random_4. No single primitive fits a lattice,
        // which is why this one was baked rather than fitted.
        const float sx = sinf((u - r1 * 10.0f) * ax * 10.0f + r3 * 0.5f);
        const float cy = cosf(r4 * 0.5f + (v - 0.5f) * ay * 10.0f);
        const float pr = sx * cy;
        r = { 7.0f, pr + pr - 2.0f };
        break;
      }
      case CERCLE: {
        // :54822 -- and the first of the nineteen radial-band patterns to be
        // read rather than fitted. Its band IS the formula ApplyMilk2BlendPattern
        // already hands to all of them, now confirmed from the branch:
        //
        //     band = 0.02 + 0.14*random_1 + 0.34*random_2
        //
        // The coordinate is a plain centred radius, `sqrt(x*x + y*y) * 1.41421`,
        // with the aspect on each axis before squaring. Centred at (0.5, 0.5)
        // EXACTLY -- the shipped fitted row put its centre low-right, which is
        // what least squares does when the shape is right and the coordinate
        // frame is not.
        //
        // 1.41421 is a literal in the branch, not a computed sqrt(2). It is what
        // carries the corner of the frame to t = 1, so the wipe finishes as the
        // last corner flips.
        //
        // cercle is one of the four MD3 actually reverses, and the branch shows
        // why the rule is exact rather than a good fit: `if (direction == -1)
        // t = 1 - t`, applied to the coordinate itself.
        const float band = 0.02f + 0.14f * r1 + 0.34f * r2;
        const float invBand = 1.0f / band;
        const float dx = (u - 0.5f) * ax, dy = (v - 0.5f) * ay;
        float t = sqrtf(dx * dx + dy * dy) * 1.41421f;
        if (direction == -1)
          t = 1.0f - t;
        r = FromT(t, (band + 1.0f) * invBand, invBand);
        break;
      }
      // ---- the radial-band family -------------------------------------------
      //
      // Seventeen more branches that differ from cercle, and from each other,
      // ONLY in the expression for t. They all share
      //
      //     band = 0.02 + 0.14*random_1 + 0.34*random_2
      //     a    = (band + K)/band,   c = t/band - 1/band
      //
      // which is the band ApplyMilk2BlendPattern has been feeding all of them,
      // now READ from every branch rather than assumed for any. K is 1, 2 or 3
      // per branch, written into the vertex as (band + K)*invBand.
      //
      // The mapping is name_index = 29 - branch_index, and the CONTENT confirms
      // it independently: nuclear has three lobes, stars five points, stars2
      // ten, clock exactly one sweep, snail a spiral in angle AND radius. Note
      // the probe harness's own PATTERNS list orders indices 16..22 differently
      // from MD3's dispatch chain -- the chain is the code, and the name it
      // leaves unaccounted for at 22 is `wave`, which the branch content
      // confirms: concentric rings from a corner.
      case SQUARE:
      case TRIANGLE:
      case CORNER:
      case PATCHES:
      case DONUTS:
      case CLOCK:
      case NUCLEAR:
      case STARS:
      case STARS2:
      case CISOR:
      case WAVE:
      case CROSS:
      case CROSS2:
      case SNAIL:
      case SNAIL2:
      case SNAIL3:
      case ARROW:
      case CHECKERBOARD: {
        const float band = 0.02f + 0.14f * r1 + 0.34f * r2;
        const float invBand = 1.0f / band;
        const float px = (u - 0.5f) * ax, py = (v - 0.5f) * ay;
        const float rad = sqrtf(px * px + py * py);
        const float ang = atan2f(py, px);
        // The corner-anchored branches take their origin at (u = 1, v = 0)
        // rather than the centre: `- 1.0` on x and `- 0.0` on y, verbatim.
        const float cx = (u - 1.0f) * ax, cy0 = v * ay;
        const float ex = fabsf(px), ey = fabsf(py);
        float t = 0.0f;
        int K = 1;
        bool dead = false;          // cross2 only: a vertex that never flips

        switch (kind) {
        case SQUARE:                                        // :54726
          // 1.41421 on EACH term before the max, exactly as the branch
          // writes it -- the same literal cercle uses to carry the corner
          // of the frame to t = 1.
          t = (ex * 1.41421f > ey * 1.41421f) ? ex * 1.41421f : ey * 1.41421f;
          break;
        case TRIANGLE:                                      // :54430
          t = ex + ey;
          break;
        case CORNER: {                                      // :54286
          // Off-centre AND clamped, which no other branch here is. The offsets
          // go on before the aspect, not after.
          const float ox = ((u - 0.5f) + 0.25f) * ax;
          const float oy = ((v - 0.5f) + 0.35f) * ay;
          t = sqrtf(ox * ox + oy * oy) * 1.41421f;
          if (t > 1.0f) t = 1.0f;
          if (t < 0.0f) t = 0.0f;
          break;
        }
        case PATCHES:                                       // :54213
          t = (cosf(py * 10.0f) + sinf(px * 10.0f)) * 0.25f + 0.5f;
          break;
        case DONUTS:                                        // :54363
          t = sinf(rad * 10.0f + 90.0f); K = 3;
          break;
        case CLOCK:                                         // :53882
          t = sinf((ang + ang) * 0.5f); K = 3;
          break;
        case NUCLEAR:                                       // :53813
          t = sinf((ang + ang) * 1.5f); K = 3;
          break;
        case STARS:                                         // :54018
          t = sinf(ang * 5.0f + 90.0f); K = 3;
          break;
        case STARS2:                                        // :53951
          t = sinf(ang * 10.0f + 90.0f); K = 3;
          break;
        case CISOR:                                         // :53662
          t = sinf(atan2f(cy0, cx) * 5.0f + 90.0f); K = 3;
          break;
        case WAVE:                                          // :53586
          t = sinf(sqrtf(cx * cx + cy0 * cy0) * 10.0f + 90.0f); K = 3;
          break;
        case CROSS:                                         // :53192
          t = (ex + ex < ey + ey) ? ex + ex : ey + ey;
          break;
        case CROSS2:                                        // :53089
          // The same field as cross, but a vertex past t = 0.3 is written a = 0
          // and keeps c = 0 from the precompute, so it never flips at all. Part
          // of the frame simply does not transition; that IS the pattern, and
          // it is the only branch of the thirty that writes a dead vertex.
          t = (ex + ex < ey + ey) ? ex + ex : ey + ey;
          dead = (t >= 0.3f);
          break;
        // The three snails are one spiral at three tightnesses: the radius
        // term is 10, 25 and 50 turns respectively. Reading snail2 and snail3
        // without it cost 0.088 and 0.094; snail, read with it, was 0.0094.
        case SNAIL:                                         // :54653
          t = rad * cosf(ang + rad * 10.0f - 0.2f); K = 2;
          break;
        case SNAIL2:                                        // :54580
          t = rad * cosf(ang + rad * 25.0f - 0.2f); K = 2;
          break;
        case SNAIL3:                                        // :54503
          t = rad * sinf(ang + rad * 50.0f + 0.5f); K = 2;
          break;
        case CHECKERBOARD:                                  // :54145
          t = sinf(cx * 10.0f + 90.0f) * cosf(cy0 * 10.0f + 90.0f); K = 3;
          break;
        case ARROW: {                                       // :53732
          // The only branch that reaches for atanh, and the only place in the
          // thirty where Ghidra dropped the arguments -- both calls print bare.
          // The pairing had to be settled by measurement, since the reading
          // cannot settle it. x is computed immediately before the first
          // call, which suggests atanh(x) goes inside the cosine -- and that
          // is the WRONG way round: it scores 0.1442 against this one's much
          // lower. Proximity in the decompiled text is not an argument.
          //
          // atanh runs away at +/-1 and MD3 would hand it infinities there. It
          // cannot happen with these coordinates -- |px| <= 0.5 and |py| <= 0.5
          // for any aspect -- but the clamp costs nothing and a NaN reaching
          // tgt.vertinfo would take the whole blend with it.
          const float qx = (px < -0.999f) ? -0.999f : (px > 0.999f ? 0.999f : px);
          const float qy = (py < -0.999f) ? -0.999f : (py > 0.999f ? 0.999f : py);
          t = sinf(atanhf(qx) + cosf(atanhf(qy) * 4.0f)) * 0.5f + 0.5f;
          break;
        }
        default:
          break;
        }

        if (direction == -1 && Milk2DirectionInverts(pattern))
          t = 1.0f - t;
        if (dead)
          r = Milk2Recovered{ 0.0f, 0.0f };
        else
          r = FromT(t, (band + (float)K) * invBand, invBand);
        break;
      }
      case SIDE: {
        // :55157 -- MilkDrop's mixtype 1 verbatim, the generic directional
        // wipe, and the only pattern of the thirty whose ANGLE and BAND both
        // come from the file's saved randoms rather than from constants:
        //
        //     ang  = random_1 * 6.28        <- MilkDrop's FRAND*6.28f
        //     band = 0.1 + 0.2 * random_2   <- MilkDrop's 0.1f + 0.2f*FRAND
        //
        // Everything after that is vertical's arithmetic with (cos, sin) of a
        // real angle instead of 6.28's near-degenerate pair, so a `side` wipe
        // genuinely runs at a slant while vertical and horizontal only appear
        // to be special cases of it.
        const float ang = r1 * 6.28f;
        const float band = 0.1f + 0.2f * r2;
        const float invBand = 1.0f / band;
        const float p = (u * ax - 0.5f) * cosf(ang) + (v * ay - 0.5f) * sinf(ang);
        const float t = p / 1.4142136f + 0.5f;
        r = FromT(t, (band + 1.0f) * invBand, invBand);
        break;
      }
      case ZOOM:
        // :55232 -- uniform, every vertex flipping together. Routed here rather
        // than left to fall through, so it can never pick up a fitted line or a
        // baked field by accident.
        r = { 1.0f, 0.0f };
        break;
      default:
        break;
      }

      tgt.vertinfo[nv].a = r.a;
      tgt.vertinfo[nv].c = r.c;
      nv++;
    }
  }

  // `direction` is read by CERCLE and by nothing else here: of the patterns
  // recovered so far it is the only one among the four MD3 reverses
  // (Milk2DirectionInverts), and MD3 renders the other seven identically either
  // way. It is still passed to the guard so that reports the truth about the
  // file rather than about this function.
  CheckMilk2BlendField(tgt, pattern, direction, nv);
  DLOG_INFO("milk2 wipe '%s': recovered generator, %d vertices", pattern, nv);
  return true;
}

bool Engine::ComputeMilk2BlendField(const BlendPatternTarget& tgt,
                                    const char* pattern, float bandCoord,
                                    int direction) {
  if (!tgt.vertinfo)
    return false;

  // A recovered branch wins outright over a fitted line or a baked grid.
  //
  // Not a preference -- the fitted table is an approximation OF this, and for
  // these seven it is approximating the wrong thing entirely: they do not use
  // the random band at all, so `bandCoord` above was never their band.
  if (ComputeMilk2RecoveredField(tgt, pattern, direction))
    return true;

  // NOTHING ELSE. Every one of MD3's thirty patterns has a recovered generator
  // above, so this is only reached by a pattern name MD3 itself would not
  // recognise -- a typo, or a pattern from a version we have not read.
  //
  // It used to fall through to a fitted table (a geometry family, a centre and
  // a line per pattern) and then to fifteen baked fields sampled from real
  // sweeps. Both are gone, and their removal is the point rather than tidiness:
  //
  //   * The fitted rows approximate the generators now sitting above them, and
  //     approximate them in a coordinate frame that turned out to be wrong.
  //     `cercle`, a third of the corpus, was fitted with its centre low-right;
  //     the branch is centred at (0.5, 0.5) exactly, and the offset was the
  //     fit absorbing the mesh-larger-than-the-frame error.
  //   * The baked tables were censored to the window they were swept over --
  //     several carried lo=0.050, hi=0.650 exactly -- so anything reaching them
  //     outside that window rendered a silently wrong field. That is how
  //     `cisor` came to pin 95% of its grid at zero.
  //
  // Returning false hands the caller its DETERMINISTIC legacy field, seeded
  // from the file's own name and randoms. For an unknown pattern that is the
  // honest answer: a real MilkDrop wipe rather than a confident guess at a
  // shape nobody has read.
  return false;
}

// Fills the target canvas with the wipe field a frozen .milk2 asked for.
//
// Needed in two places: when the preset loads, and again after a device
// teardown -- AllocateMyDX9Stuff reallocates the primary's array as fresh uninitialised
// memory, and nothing else recomputes it, so without this a .milk2 comes back
// as a flat 50/50 mix of both presets instead of the file's wipe pattern.
// Measured: a cercle probe at progress=0.50 went from a real field
// (p1=0.238 p2=0.174) to a uniform mix (p1=0 p2=0, mean rgb 128/127/0).
//
// NOTE: RandomizeBlendPattern() draws on FRAND for every mixtype except those
// that consume the file's saved random_1..5, so the field recomputed here can
// differ from the one the preset loaded with. That is still far better than an
// uninitialised field, and it goes away when ComputeMilk2BlendField() replaces
// this with the measured, deterministic rule.
// Stable seed for a .milk2's wipe field: FNV-1a over the pattern name and the
// file's saved random_1..5. Same file -> same field, different files -> different
// fields.
static unsigned int Milk2FieldSeed(const char* pattern, const float* randoms,
                                   bool hasRandoms) {
  unsigned int h = 2166136261u;
  for (const char* p = pattern; p && *p; ++p) {
    h ^= (unsigned char)(*p);
    h *= 16777619u;
  }
  if (hasRandoms && randoms) {
    for (int i = 0; i < 5; i++) {
      // Quantise so float noise in the last bits cannot change the seed.
      unsigned int q = (unsigned int)(randoms[i] * 100000.0f + 0.5f);
      for (int b = 0; b < 4; b++) {
        h ^= (q >> (b * 8)) & 0xFFu;
        h *= 16777619u;
      }
    }
  }
  return h ? h : 1u;
}

void Engine::ApplyMilk2BlendPattern(const BlendPatternTarget& tgt) {
  // random_1 is the transition band width, in wipe-coordinate units. Measured
  // against MD3 PRO: the band is monotonic in random_1 (frac_mixed 0.307 /
  // 0.508 / 0.641 for random_1 = 0.1 / 0.5 / 0.9) while the wipe position stays
  // put. With no randoms in the file MD3 renders its narrowest band, matching
  // about 0.18 -- the measured no-randoms frac_mixed for cercle.
  // Band width, taken from MilkDrop 3 PRO's own transition code rather than
  // fitted. It stores the vertex blend as
  //
  //     a = inv_band * (1 + band)      c = -inv_band + inv_band * t
  //
  // for a per-pattern coordinate t, so mix = 0.5 at
  //
  //     p_flip = (1 - t + band/2) / (1 + band)
  //
  // and the transition therefore spans band/(1+band) of PROGRESS -- a function
  // of band alone, independent of the pattern's geometry. band itself is
  //
  //     band = 0.02 + 0.14*random_1 + 0.34*random_2
  //
  // which is MilkDrop's own `0.02f + 0.14f*FRAND + 0.34f*FRAND` (mixtype 3,
  // radial) with the file's saved randoms substituted for the random draws.
  // MD3 reuses that one expression for nineteen of its named patterns, keeping
  // MilkDrop's plasma and directional-wipe bands for two more.
  //
  // The earlier model used random_1 alone as a width in wipe-coordinate units
  // and converted with |slope|. Both halves of that were wrong, and they partly
  // cancelled, which is why it fit as well as it did.
  float bandMD3 = 0.02f + 0.14f * 0.5f + 0.34f * 0.5f;   // randoms absent: mid draw
  if (m_bMilk2HasRandoms)
    bandMD3 = 0.02f + 0.14f * m_fMilk2Random[0] + 0.34f * m_fMilk2Random[1];
  float bandCoord = bandMD3 / (1.0f + bandMD3);

  // `plasma` is not approximated -- it is reproduced. MD3 PRO kept MilkDrop's
  // mixtype 2 verbatim (Part Q2) and, crucially, its GenPlasma calls no rand()
  // at all: each of the five FRAND displacement sites became one saved random,
  // reused at every recursion depth (MilkDroprev.c:45212). The field is
  // therefore a pure function of random_1..5, which is why MD3 renders each
  // file identically on every load -- and why a single measured grid, correct
  // band or not, cannot stand in for 58 files that each grow a different field
  // from their own four corner seeds.
  //
  // plasma2/plasma3 are the same generator with a different band, a doubled
  // normalisation and a sine post-transform (MilkDroprev.c:54876 and :54980):
  //
  //     band = 0.25 + 0.15*random_1        (vs plasma's 0.12 + 0.13*random_1)
  //     t    = sin( (c - min) * 2/(max-min) )   (vs plasma's (c-min)/(max-min))
  //
  // FUN_004a5cc0 is sin -- the directional-wipe branch identifies it as the
  // vy = sin(ang) call. The two branches differ from each other ONLY in
  // GenPlasma's starting dt (0.75 vs 0.25) and whether the min/max scan is
  // seeded from v[0].c or half of it.
  //
  // Dispatch is a chain of thirty string compares whose order Ghidra cannot
  // recover (Part Q2), so which branch carries which NAME was settled by
  // measurement instead -- both assignments were built and swept:
  //
  //   plasma2   baked 13.92 -> 8.11 (:54876) -> 7.13 (:54980)   n=40
  //   plasma3   baked 12.45 -> 8.51 (:54980) -> 8.43 (:54876)   n=43
  //
  // plasma2 prefers :54980 decisively (21 files better, 9 worse) and lands on
  // 7.13, matching plain plasma's 7.10 -- that branch is its. plasma3 cannot
  // tell the two apart (22/18, means 0.08 apart), so ITS branch is NOT
  // confirmed: it may be a third site neither of these, which would explain
  // why it stays ~1.3 above the other two patterns.
  m_nMilk2PlasmaVariant = 0;
  if (_stricmp(m_szMilk2Pattern, "plasma2") == 0)      m_nMilk2PlasmaVariant = 2;
  else if (_stricmp(m_szMilk2Pattern, "plasma3") == 0) m_nMilk2PlasmaVariant = 1;

  const bool bExactPlasma =
      m_bMilk2HasRandoms && (_stricmp(m_szMilk2Pattern, "plasma") == 0 ||
                             m_nMilk2PlasmaVariant != 0);

  if (!bExactPlasma &&
      ComputeMilk2BlendField(tgt, m_szMilk2Pattern, bandCoord, m_nMilk2Direction))
    return;

  // No measured form for this pattern (the eleven procedural ones). Fall back
  // to the legacy field rather than render a confidently wrong shape -- but
  // make it DETERMINISTIC.
  //
  // RandomizeBlendPattern() draws 65 FRAND values plus several rand() calls,
  // all from the CRT generator, so a frozen .milk2 rendered a different wipe on
  // every load. Measured reload-to-reload weight-field MAE was 0.35-0.60 for
  // `side` and `donuts`, against 0.0000 for the patterns with a measured form.
  // A permanent blend must not change between loads, and it also made tuning by
  // eye impossible -- every screenshot compared a different field.
  //
  // Seeding from the file's own pattern name and random_1..5 keeps different
  // presets looking different while making each one reproducible.
  const unsigned int seed = Milk2FieldSeed(m_szMilk2Pattern, m_fMilk2Random,
                                           m_bMilk2HasRandoms);
  // Preserve unpredictability for everything else that uses rand() afterwards.
  const unsigned int restore = (unsigned int)rand() ^ (unsigned int)GetTickCount();
  srand(seed);
  m_bMilk2DeterministicField = true;

  int savedMixType = m_nMixType;
  m_nMixType = m_nMilk2MixType;
  m_bMilk2UseSavedRandoms = m_bMilk2HasRandoms;
  // tgt, NOT the no-arg overload. This whole function takes a
  // BlendPatternTarget so a mirror can fill ITS canvas at ITS aspect -- and
  // then the procedural fallback called RandomizeBlendPattern(), which is
  // RandomizeBlendPattern(PrimaryBlendTarget()), i.e. { m_vertinfo,
  // m_fAspectX, m_fAspectY }. So for every .milk2 whose pattern has no
  // recovered analytic form, a mirror wrote its wipe across the PRIMARY's
  // field at the PRIMARY's aspect, and its own field stayed all-zero -- which
  // reads as alpha 0 everywhere, so the mirror's blend never happened at all.
  // The analytic path above (ComputeMilk2BlendField) always honoured tgt,
  // which is why this only showed on patterns that fall through to here.
  RandomizeBlendPattern(tgt);
  m_bMilk2UseSavedRandoms = false;
  m_nMixType = savedMixType;

  m_bMilk2DeterministicField = false;
  srand(restore);

  // The fallback path gets checked too -- the guard's second blind spot.
  //
  // ComputeMilk2BlendField checks at each of its own exits, but everything that
  // never reaches it was invisible: the exact-plasma bypass above (74
  // direction=-1 files in the library alone), the eleven procedural patterns,
  // and zoom on legacy mixtype 0 (another 16). A corpus sweep reported fields
  // for 208 of 298 files and silence for the other 90 -- not because they were
  // healthy, but because nothing looked.
  //
  // None of those is known to be wrong today. That is exactly why the check
  // belongs here: a guard with a thirty-percent blind spot reads as "nothing is
  // collapsed" when it means "most things were not examined".
  CheckMilk2BlendField(tgt, m_szMilk2Pattern, m_nMilk2Direction,
                       (m_nGridX + 1) * (m_nGridY + 1));
}

void Engine::RandomizeBlendPattern(const BlendPatternTarget& tgt) {
  if (!tgt.vertinfo)
    return;

  // note: we now avoid constant uniform blend b/c it's half-speed for shader blending.
  //       (both old & new shaders would have to run on every pixel...)           reenabled due to further notice
  int mixtype = 0 + (rand() % 19);
  if (m_nMixType > -1) mixtype = m_nMixType;

  if (mixtype == 0) {
    // constant, uniform blend
    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      for (int x = 0; x <= m_nGridX; x++) {
        tgt.vertinfo[nVert].a = 1;
        tgt.vertinfo[nVert].c = 0;
        nVert++;
      }
    }
  }
  else if (mixtype == 1) {
    // directional wipe
    float ang = FRAND * 6.28f;
    float vx = cosf(ang);
    float vy = sinf(ang);
    float band = 0.1f + 0.2f * FRAND; // 0.2 is good
    float inv_band = 1.0f / band;

    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      float fy;
      if (m_bScreenDependentRenderMode)
        fy = (y / (float)m_nGridY);
      else
        fy = (y / (float)m_nGridY) * tgt.aspectY;

      for (int x = 0; x <= m_nGridX; x++) {
        float fx;
        if (m_bScreenDependentRenderMode)
          fx = (x / (float)m_nGridX);
        else
          fx = (x / (float)m_nGridX) * tgt.aspectX;

        // at t==0, mix rangse from -10..0
        // at t==1, mix ranges from   1..11

        float t = (fx - 0.5f) * vx + (fy - 0.5f) * vy + 0.5f;
        t = (t - 0.5f) / sqrtf(2.0f) + 0.5f;

        tgt.vertinfo[nVert].a = inv_band * (1 + band);
        tgt.vertinfo[nVert].c = -inv_band + inv_band * t;//(x/(float)m_nGridX - 0.5f)/band;
        nVert++;
      }
    }
  }
  else if (mixtype == 2) {
    // plasma transition
    float band = 0.12f + 0.13f * FRAND;//0.02f + 0.18f*FRAND;
    if (m_bMilk2UseSavedRandoms) {
      // random_1 is a FRAND *input*, not the band itself. MD3 PRO computes
      //     band = random_1 * 0.13 + 0.12
      // (decompilation MilkDroprev.c:55079, the one branch in the binary using
      // this band), which is MilkDrop's own 0.12f + 0.13f*FRAND with the saved
      // draw substituted -- the same substitution Part P3 found for the radial
      // band. The old reading, "random_1 IS the band", predates that finding.
      // random_2..5 seed the four plasma corners.
      band = (m_nMilk2PlasmaVariant == 0)
                 ? 0.12f + 0.13f * m_fMilk2Random[0]
                 : 0.25f + 0.15f * m_fMilk2Random[0];
    }
    float inv_band = 1.0f / band;

    // first generate plasma array of height values
    if (m_bMilk2UseSavedRandoms) {
      tgt.vertinfo[0].c = m_fMilk2Random[1];
      tgt.vertinfo[m_nGridX].c = m_fMilk2Random[2];
      tgt.vertinfo[m_nGridY * (m_nGridX + 1)].c = m_fMilk2Random[3];
      tgt.vertinfo[m_nGridY * (m_nGridX + 1) + m_nGridX].c = m_fMilk2Random[4];
    } else {
      tgt.vertinfo[0].c = FRAND;
      tgt.vertinfo[m_nGridX].c = FRAND;
      tgt.vertinfo[m_nGridY * (m_nGridX + 1)].c = FRAND;
      tgt.vertinfo[m_nGridY * (m_nGridX + 1) + m_nGridX].c = FRAND;
    }
    // 0x3f400000 = 0.75f at MilkDroprev.c:54876, 0x3e800000 = 0.25f at the
    // other two sites.
    GenPlasma(tgt, 0, m_nGridX, 0, m_nGridY,
              (m_nMilk2PlasmaVariant == 1) ? 0.75f : 0.25f);

    // then find min,max so we can normalize to [0..1] range and then to the proper 'constant offset' range.
    // Variant 1 seeds the scan from HALF the first vertex, so minc can end up
    // below the true minimum and the normalisation is shifted with it.
    float minc = (m_nMilk2PlasmaVariant == 1) ? tgt.vertinfo[0].c * 0.5f
                                              : tgt.vertinfo[0].c;
    float maxc = minc;
    int x, y, nVert;

    nVert = 0;
    for (y = 0; y <= m_nGridY; y++) {
      for (x = 0; x <= m_nGridX; x++) {
        if (minc > tgt.vertinfo[nVert].c)
          minc = tgt.vertinfo[nVert].c;
        if (maxc < tgt.vertinfo[nVert].c)
          maxc = tgt.vertinfo[nVert].c;
        nVert++;
      }
    }

    // plasma normalises to [0,1]; plasma2/3 normalise to [0,2] and take the
    // sine of it, which folds the field back on itself and is what makes them
    // look like different transitions rather than a rescaled plasma.
    const bool bSinePlasma = (m_nMilk2PlasmaVariant != 0);
    float mult = (bSinePlasma ? 2.0f : 1.0f) / (maxc - minc);
    nVert = 0;
    for (y = 0; y <= m_nGridY; y++) {
      for (x = 0; x <= m_nGridX; x++) {
        float t = (tgt.vertinfo[nVert].c - minc) * mult;
        if (bSinePlasma)
          t = sinf(t);
        tgt.vertinfo[nVert].a = inv_band * (1 + band);
        tgt.vertinfo[nVert].c = -inv_band + inv_band * t;
        nVert++;
      }
    }
  }
  else if (mixtype == 3) {
    // radial blend
    float band = 0.02f + 0.14f * FRAND + 0.34f * FRAND;
    float dir = (float)((rand() % 2) * 2 - 1);      // 1=outside-in, -1=inside-out
    if (m_bMilk2UseSavedRandoms) {
      // random_1/2 are FRAND inputs, not the band itself. Using 0.46 as
      // band mixed the whole screen (pink preset2 floor + dual cameras).
      band = 0.02f + 0.14f * m_fMilk2Random[0] + 0.34f * m_fMilk2Random[1];
      // direction=-1: keep the center on PRESET1 (old). Inside-out put
      // preset2's purple floor in the middle.
      if (m_nMilk2Direction != 0)
        dir = (m_nMilk2Direction < 0) ? 1.0f : -1.0f;
    }
    float inv_band = 1.0f / band;

    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {

      float dy;
      if (m_bScreenDependentRenderMode)
        dy = (y / (float)m_nGridY - 0.5f);
      else
        dy = (y / (float)m_nGridY - 0.5f) * tgt.aspectY;

      for (int x = 0; x <= m_nGridX; x++) {
        float dx;
        if (m_bScreenDependentRenderMode)
          dx = (x / (float)m_nGridX - 0.5f);
        else
          dx = (x / (float)m_nGridX - 0.5f) * tgt.aspectX;

        float t = sqrtf(dx * dx + dy * dy) * 1.41421f;
        if (dir == -1)
          t = 1 - t;

        tgt.vertinfo[nVert].a = inv_band * (1 + band);
        tgt.vertinfo[nVert].c = -inv_band + inv_band * t;
        nVert++;
      }
    }
  }
  else if (mixtype == 4) {
    // DeepSeek - seamless clock transition
    float band = 0.08f + 0.14f * FRAND;  // optimal band width for clock transition
    float inv_band = 1.0f / band;
    float dir = (rand() % 2) ? 1.0f : -1.0f; // random direction
    float start_angle = FRAND * 6.2831853f;  // random starting angle

    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      float fy;
      if (m_bScreenDependentRenderMode)
        fy = (y / (float)m_nGridY - 0.5f);
      else
        fy = (y / (float)m_nGridY - 0.5f) * tgt.aspectY;
      for (int x = 0; x <= m_nGridX; x++) {
        float fx;
        if (m_bScreenDependentRenderMode)
          fx = (x / (float)m_nGridX - 0.5f);
        else
          fx = (x / (float)m_nGridX - 0.5f) * tgt.aspectX;

        // Calculate angle and distance from center
        float angle = atan2f(fy, fx); // range: -PI to PI
        float dist = sqrtf(fx * fx + fy * fy) * 1.41421356f; // normalized 0-1

        // Convert angle to 0-2PI range and apply direction/start
        if (angle < 0) angle += 6.2831853f;
        angle = fmodf(angle * dir + start_angle + 10.0f * 6.2831853f, 6.2831853f);

        // Calculate blend factor with seamless wrap-around
        float t = angle / 6.2831853f;
        float t_adjusted = t;

        // Handle wrap-around for smooth transition
        if (t < band) {
          t_adjusted = t + 1.0f; // treat as next cycle
        }

        // Combine with distance for better visual (optional)
        float blend = (t_adjusted - dist * 0.1f); // slight radial component

        tgt.vertinfo[nVert].a = inv_band * (1.0f + band);
        tgt.vertinfo[nVert].c = -inv_band + inv_band * blend;
        nVert++;
      }
    }
  }
  else if (mixtype == 5) {
    // DeepSeek - Spiral/Snail transition
    float band = 0.07f + 0.1f * FRAND;  // optimal band width for spiral
    int loops = 2 + (rand() % 7);       // random loops between 2-8
    float rotation_speed = FRAND * 0.5f; // optional slow rotation (0-0.5)
    bool inward_spiral = (rand() % 2) == 0; // random inward/outward direction
    if (m_bMilk2UseSavedRandoms) {
      // MD3 writes the live band/params into random_1..5 so reload matches.
      if (m_fMilk2Random[0] > 0.001f)
        band = m_fMilk2Random[0];
      loops = 2 + (int)(m_fMilk2Random[1] * 7.0f);
      if (loops < 2) loops = 2;
      if (loops > 8) loops = 8;
      rotation_speed = m_fMilk2Random[2] * 0.5f;
      inward_spiral = (m_nMilk2Direction < 0) || (m_fMilk2Random[3] < 0.5f);
    }
    float inv_band = 1.0f / band;

    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      float fy;
      if (m_bScreenDependentRenderMode)
        fy = (y / (float)m_nGridY - 0.5f);
      else
        fy = (y / (float)m_nGridY - 0.5f) * tgt.aspectY;

      for (int x = 0; x <= m_nGridX; x++) {
        float fx;
        if (m_bScreenDependentRenderMode)
          fx = (x / (float)m_nGridX - 0.5f);
        else
          fx = (x / (float)m_nGridX - 0.5f) * tgt.aspectX;

        // Calculate polar coordinates
        float angle = atan2f(fy, fx); // range: -PI to PI
        float radius = sqrtf(fx * fx + fy * fy) * 1.41421356f; // normalized 0-1

        // Convert angle to 0-2PI range
        if (angle < 0) angle += 6.2831853f;

        // Calculate spiral progression (0-1)
        float spiral_progress = fmodf(angle / (6.2831853f) + loops * radius + rotation_speed, 1.0f);

        // Reverse direction if inward spiral
        if (inward_spiral) {
          spiral_progress = 1.0f - spiral_progress;
        }

        // Apply band blending
        tgt.vertinfo[nVert].a = inv_band * (1.0f + band);
        tgt.vertinfo[nVert].c = -inv_band + inv_band * spiral_progress;
        nVert++;
      }
    }
  }
  else if (mixtype == 6) {
    // DeepSeek - Rhombus/Diamond transition
    float band = 0.07f + 0.12f * FRAND;  // slightly narrower band for sharper edges
    float inv_band = 1.0f / band;
    float angle = FRAND * 6.2831853f;     // random rotation angle (0-2π)
    float aspect = 0.8f + FRAND * 2.4f;   // aspect ratio (0.8-3.2)
    bool reverse = (rand() % 2) == 0;     // random direction

    // Precompute rotation matrix and normalization factor
    float cos_a = cosf(angle);
    float sin_a = sinf(angle);
    float norm_factor = 1.0f / (1.0f + aspect);

    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      float fy;
      if (m_bScreenDependentRenderMode)
        fy = (y / (float)m_nGridY - 0.5f);
      else
        fy = (y / (float)m_nGridY - 0.5f) * tgt.aspectY;
      for (int x = 0; x <= m_nGridX; x++) {
        float fx;
        if (m_bScreenDependentRenderMode)
          fx = (x / (float)m_nGridX - 0.5f);
        else
          fx = (x / (float)m_nGridX - 0.5f) * tgt.aspectX;

        // Rotate coordinates
        float rx = fx * cos_a - fy * sin_a;
        float ry = fx * sin_a + fy * cos_a;

        // Rhombus distance function (manhattan distance)
        float diamond = (fabsf(rx) * aspect + fabsf(ry)) * norm_factor;

        // Apply direction
        float t = reverse ? (1.0f - diamond) : diamond;

        // Apply band blending with edge clamping
        tgt.vertinfo[nVert].a = inv_band * (1.0f + band);
        tgt.vertinfo[nVert].c = -inv_band + inv_band * t;
        nVert++;
      }
    }
  }
  else if (mixtype == 7) {
    // DeepSeek - Nuclear Clock Wipe Transition
    float band = 0.05f + 0.15f * FRAND;  // band width for the transition edge
    float inv_band = 1.0f / band;
    const int exact_repeats = 3;         // exactly 3 full rotations
    bool reverse_direction = (rand() % 2) == 0;
    float glow_intensity = 0.5f + FRAND * 1.5f; // nuclear glow effect

    // Calculate center point with slight random offset
    float center_x = 0.5f + (FRAND - 0.5f) * 0.1f;
    float center_y = 0.5f + (FRAND - 0.5f) * 0.1f;

    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      float fy;
      if (m_bScreenDependentRenderMode)
        fy = (y / (float)m_nGridY - center_y);
      else
        fy = (y / (float)m_nGridY - center_y) * tgt.aspectY;
      for (int x = 0; x <= m_nGridX; x++) {
        float fx;
        if (m_bScreenDependentRenderMode)
          fx = (x / (float)m_nGridX - center_x);
        else
          fx = (x / (float)m_nGridX - center_x) * tgt.aspectX;

        // Calculate angle and distance from center
        float angle = atan2f(fy, fx); // range: -PI to PI
        float dist = sqrtf(fx * fx + fy * fy) * 1.41421356f; // normalized distance

        // Convert angle to 0-2PI range
        if (angle < 0) angle += 6.2831853f;

        // Calculate exact 3-repeat position (0-3 range)
        float clock_pos = angle / 6.2831853f * exact_repeats;

        if (reverse_direction)
          clock_pos = exact_repeats - clock_pos;

        // Keep only fractional part for seamless looping
        clock_pos = clock_pos - floorf(clock_pos);

        // Create nuclear effect by combining distance and angle
        float t = clock_pos;

        // Add distance-based falloff for glow effect
        float glow = (1.0f - dist) * glow_intensity;
        t += glow * 0.3f; // blend in some glow

        // Apply band blending
        tgt.vertinfo[nVert].a = inv_band * (1.0f + band);
        tgt.vertinfo[nVert].c = -inv_band + inv_band * t;
        nVert++;
      }
    }
  }
  else if (mixtype == 8) {
    // DeepSeek - Square/Diamond Transition
    float band = 0.08f + 0.12f * FRAND;  // transition edge width
    float inv_band = 1.0f / band;
    bool diagonal = (rand() % 2) == 0;    // true = X-shape, false = +-shape
    float center_bias = 0.3f + FRAND * 0.4f; // 0.3-0.7, controls center emphasis
    float softness = 0.1f + FRAND * 0.2f; // edge softness

    // Define our own clamp function
    auto clamp = [](float value, float min, float max) {
      return (value < min) ? min : ((value > max) ? max : value);
      };

    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      float fy;
      if (m_bScreenDependentRenderMode)
        fy = (y / (float)m_nGridY - 0.5f);
      else
        fy = (y / (float)m_nGridY - 0.5f) * tgt.aspectY;
      for (int x = 0; x <= m_nGridX; x++) {
        float fx;
        if (m_bScreenDependentRenderMode)
          fx = (x / (float)m_nGridX - 0.5f);
        else
          fx = (x / (float)m_nGridX - 0.5f) * tgt.aspectX;

        float t;
        if (diagonal) {
          // X-shaped wipe (diagonal)
          float d1 = (fx + fy) * 0.7071f; // 1/sqrt(2)
          float d2 = (fx - fy) * 0.7071f;
          t = (fabsf(d1) > fabsf(d2)) ? fabsf(d1) : fabsf(d2);
        }
        else {
          // +-shaped wipe (cardinal directions)
          t = (fabsf(fx) > fabsf(fy)) ? fabsf(fx) : fabsf(fy);
        }

        // Apply center bias for more interesting pattern
        t = powf(t, center_bias);

        // Add optional softness to edges
        t = t * (1.0f + softness) - softness * 0.5f;
        t = clamp(t, 0.0f, 1.0f);

        // Apply band blending
        tgt.vertinfo[nVert].a = inv_band * (1.0f + band);
        tgt.vertinfo[nVert].c = -inv_band + inv_band * t;
        nVert++;
      }
    }
  }
  else if (mixtype == 9) {
    // DeepSeek - Animated Checkerboard Transition
    float band = 0.05f + 0.15f * FRAND;  // transition edge sharpness
    float inv_band = 1.0f / band;
    int checker_size = 4 + (rand() % 12); // checker squares size (4-15)
    float anim_speed = 0.5f + FRAND * 2.0f; // animation speed (0.5-2.5)
    bool diagonal_anim = (rand() % 2) == 0; // diagonal or straight animation
    bool reverse = (rand() % 2) == 0; // reverse animation direction

    // Get current time for animation (using a fake time if not available)
    static float fake_time = 0.0f;
    // A frozen .milk2 must look identical on every load, so it must neither
    // read nor advance this process-wide accumulator -- otherwise the field
    // depends on how many times RandomizeBlendPattern has ever been called.
    // Measured: `checkerboard` stayed non-deterministic at native resolution
    // (reload MAE 0.024 / 0.043) after seeding rand(), because of this.
    if (!m_bMilk2DeterministicField) fake_time += 1 / GetFps();
    float time = m_bMilk2DeterministicField ? 0.0f : fake_time;

    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      float fy = y / (float)m_nGridY;
      for (int x = 0; x <= m_nGridX; x++) {
        float fx = x / (float)m_nGridX;

        // Calculate checkerboard pattern (0 or 1)
        int cx = (int)(fx * checker_size);
        int cy;
        if (m_bScreenDependentRenderMode)
          cy = (int)(fy * checker_size);
        else
          cy = (int)(fy * checker_size * tgt.aspectY);
        int checker = (cx + cy) % 2;

        // Calculate animation progress
        float anim_progress;
        if (diagonal_anim) {
          // Diagonal animation (top-left to bottom-right)
          anim_progress = (fx + fy) * 0.5f + time * anim_speed;
        }
        else {
          // Horizontal animation
          anim_progress = fx + time * anim_speed;
        }

        // Wrap around and reverse if needed
        anim_progress = fmodf(anim_progress, 2.0f);
        if (anim_progress > 1.0f) anim_progress = 2.0f - anim_progress;
        if (reverse) anim_progress = 1.0f - anim_progress;

        // Combine checker pattern with animation
        float t;
        if (checker == 0) {
          // First set of squares - delayed animation
          t = anim_progress - 0.3f;
        }
        else {
          // Second set of squares - advanced animation
          t = anim_progress + 0.3f;
        }

        // Apply band blending
        tgt.vertinfo[nVert].a = inv_band * (1.0f + band);
        tgt.vertinfo[nVert].c = -inv_band + inv_band * t;
        nVert++;
      }
    }
  }
  else if (mixtype == 10) {
    // DeepSeek - Curtain Transition
    float band = 0.05f + 0.15f * FRAND;  // transition edge width
    float inv_band = 1.0f / band;
    bool opening = (rand() % 2) == 0;    // true = opening, false = closing
    bool vertical = (rand() % 2) == 0;   // true = vertical curtains, false = horizontal
    float curtain_wrinkles = 0.5f + FRAND * 2.0f; // amount of wrinkles/folds (0.5-2.5)
    float center_gap = 0.05f + FRAND * 0.15f; // gap between curtains (0.05-0.2)
    bool reverse_motion = (rand() % 2) == 0; // reverse motion direction

    // NEW: Configure repeats/wipe patterns
    int repeats = 1 + (rand() % 4); // 1-4 repeats (1=normal curtain, 2-4=striped patterns)
    float repeat_width = 1.0f / repeats; // width of each repeat segment
    float repeat_variation = 0.3f * FRAND; // 0-0.3 variation in repeat timing
    bool alternate_direction = (rand() % 2) == 0; // alternate stripe directions

    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      float fy = (y / (float)m_nGridY);
      for (int x = 0; x <= m_nGridX; x++) {
        float fx = (x / (float)m_nGridX);

        float t;
        if (vertical) {
          // Vertical curtains (left and right)
          float pos = fx;
          float segment_pos = pos * repeats; // position within repeat segments
          int segment_idx = (int)floorf(segment_pos); // which segment we're in
          float segment_local = segment_pos - segment_idx; // 0-1 within segment

          float center_dist = fabsf(segment_local - 0.5f) - center_gap / 2;
          if (center_dist < 0) center_dist = 0;

          // Determine which curtain this pixel belongs to
          float curtain_side = (segment_local < 0.5f) ? -1.0f : 1.0f;

          // Calculate base transition value
          t = center_dist * 2.0f; // ranges 0-1 for each curtain segment

          // Add per-segment variation
          float segment_variation = sinf(segment_idx * 1.618f) * repeat_variation;
          t += segment_variation;

          // Add wrinkles/folds effect using sine wave
          float wrinkles = sinf(fy * 3.14159f * curtain_wrinkles) * 0.1f;
          t += wrinkles * (1.0f - t);

          // Adjust for opening/closing
          if (opening)
            t = 1.0f - t;

          // Adjust for curtain side and alternate directions
          if (alternate_direction && (segment_idx % 2 == 1))
            curtain_side *= -1.0f;

          if (reverse_motion)
            t = curtain_side > 0 ? t : 1.0f - t;
          else
            t = curtain_side > 0 ? 1.0f - t : t;
        }
        else {
          // Horizontal curtains (top and bottom)
          float pos = fy;
          float segment_pos = pos * repeats; // position within repeat segments
          int segment_idx = (int)floorf(segment_pos); // which segment we're in
          float segment_local = segment_pos - segment_idx; // 0-1 within segment

          float center_dist = fabsf(segment_local - 0.5f) - center_gap / 2;
          if (center_dist < 0) center_dist = 0;

          // Determine which curtain this pixel belongs to
          float curtain_side = (segment_local < 0.5f) ? -1.0f : 1.0f;

          // Calculate base transition value
          t = center_dist * 2.0f; // ranges 0-1 for each curtain segment

          // Add per-segment variation
          float segment_variation = sinf(segment_idx * 1.618f) * repeat_variation;
          t += segment_variation;

          // Add wrinkles/folds effect using sine wave
          float wrinkles = sinf(fx * 3.14159f * curtain_wrinkles) * 0.1f;
          t += wrinkles * (1.0f - t);

          // Adjust for opening/closing
          if (opening)
            t = 1.0f - t;

          // Adjust for curtain side and alternate directions
          if (alternate_direction && (segment_idx % 2 == 1))
            curtain_side *= -1.0f;

          if (reverse_motion)
            t = curtain_side > 0 ? t : 1.0f - t;
          else
            t = curtain_side > 0 ? 1.0f - t : t;
        }

        // Apply band blending
        tgt.vertinfo[nVert].a = inv_band * (1.0f + band);
        tgt.vertinfo[nVert].c = -inv_band + inv_band * t;
        nVert++;
      }
    }
  }
  else if (mixtype == 11) {
    // DeepSeek - Bubble Transition
    float band = 0.05f + 0.15f * FRAND;  // transition edge width
    float inv_band = 1.0f / band;
    int bubble_count = 10 + (rand() % 30); // number of bubbles (10-40)
    float bubble_size_min = 0.05f + FRAND * 0.1f; // min bubble size (0.05-0.15)
    float bubble_size_max = 0.15f + FRAND * 0.2f; // max bubble size (0.15-0.35)
    bool growing_bubbles = (rand() % 2) == 0; // true = bubbles grow, false = shrink

    // Generate random bubble positions and sizes
    struct Bubble {
      float x, y;     // position (0-1 range)
      float size;     // radius (0-1 range)
      float speed;    // growth/shrink speed
    };

    Bubble* bubbles = new Bubble[bubble_count];
    for (int i = 0; i < bubble_count; i++) {
      bubbles[i].x = FRAND;
      bubbles[i].y = FRAND;
      bubbles[i].size = bubble_size_min + FRAND * (bubble_size_max - bubble_size_min);
      bubbles[i].speed = 0.5f + FRAND * 1.5f; // speed multiplier (0.5-2.0)
    }

    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      float fy = (y / (float)m_nGridY);
      for (int x = 0; x <= m_nGridX; x++) {
        float fx = (x / (float)m_nGridX);

        // Find the maximum bubble influence at this pixel
        float max_influence = 0.0f;

        for (int i = 0; i < bubble_count; i++) {
          // Calculate distance to bubble center
          float dx, dy;
          if (m_bScreenDependentRenderMode) {
            dx = (fx - bubbles[i].x);
            dy = (fy - bubbles[i].y);
          }
          else {
            dx = (fx - bubbles[i].x) * tgt.aspectX;
            dy = (fy - bubbles[i].y) * tgt.aspectY;
          }
          float dist = sqrtf(dx * dx + dy * dy);

          // Calculate bubble influence (1 at center, 0 at edge)
          float influence = 1.0f - (dist / bubbles[i].size);
          if (influence < 0) influence = 0;

          // Apply smoothstep for smoother edges
          influence = influence * influence * (3.0f - 2.0f * influence);

          if (influence > max_influence)
            max_influence = influence;
        }

        // If we're shrinking bubbles, invert the influence
        float t = growing_bubbles ? max_influence : (1.0f - max_influence);

        // Apply band blending
        tgt.vertinfo[nVert].a = inv_band * (1.0f + band);
        tgt.vertinfo[nVert].c = -inv_band + inv_band * t;
        nVert++;
      }
    }
    delete[] bubbles;
  }
  else if (mixtype == 12) {
    // DeepSeek - Kaleidoscope Wipe Transition
    float band = 0.06f + 0.14f * FRAND;  // transition edge width
    float inv_band = 1.0f / band;

    // Kaleidoscope parameters
    int segments = 3 + (rand() % 9);     // 3-12 segments (triangular to dodecagonal)
    float segment_angle = 6.2831853f / segments; // angle per segment in radians
    float rotation = FRAND * 6.2831853f; // random initial rotation
    bool mirror_effect = (rand() % 2) == 0; // true = mirrored segments, false = just rotated
    float radial_factor = 0.5f + FRAND;  // 0.5-1.5 - how much radial distance affects the pattern

    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      float fy;
      if (m_bScreenDependentRenderMode)
        fy = (y / (float)m_nGridY - 0.5f);
      else
        fy = (y / (float)m_nGridY - 0.5f) * tgt.aspectY;
      for (int x = 0; x <= m_nGridX; x++) {
        float fx;
        if (m_bScreenDependentRenderMode)
          fx = (x / (float)m_nGridX - 0.5f);
        else
          fx = (x / (float)m_nGridX - 0.5f) * tgt.aspectX;

        // Calculate polar coordinates
        float angle = atan2f(fy, fx) + rotation; // range: -PI to PI plus rotation
        float radius = sqrtf(fx * fx + fy * fy) * 1.41421356f; // normalized distance

        // Wrap angle to 0-2PI range
        if (angle < 0) angle += 6.2831853f;
        if (angle >= 6.2831853f) angle -= 6.2831853f;

        // Find which segment we're in and map to first segment
        int segment = (int)(angle / segment_angle);
        float segment_offset = angle - segment * segment_angle;

        // For mirrored segments, reflect angles past the halfway point
        if (mirror_effect && segment_offset > segment_angle * 0.5f) {
          segment_offset = segment_angle - segment_offset;
        }

        // Normalize the segment angle to 0-1 range
        float normalized_angle = segment_offset / segment_angle;

        // Combine angle and radius for the pattern
        float t = (normalized_angle * 0.7f + radius * 0.3f * radial_factor);

        // Apply band blending
        tgt.vertinfo[nVert].a = inv_band * (1.0f + band);
        tgt.vertinfo[nVert].c = -inv_band + inv_band * t;
        nVert++;
      }
    }
  }
  else if (mixtype == 13) {
    // DeepSeek - Moebius Strip Transition
    float band = 0.07f + 0.13f * FRAND;  // transition edge width
    float inv_band = 1.0f / band;

    // Moebius parameters
    float twist_factor = 1.0f + FRAND * 2.0f; // 1-3 controls twist intensity
    bool reverse_twist = (rand() % 2) == 0;   // random twist direction
    float strip_width = 0.3f + FRAND * 0.4f;  // 0.3-0.7 width of the moebius strip
    float progress_offset = FRAND * 0.5f;     // 0-0.5 random phase offset

    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      float fy;
      if (m_bScreenDependentRenderMode)
        fy = (y / (float)m_nGridY - 0.5f);
      else
        fy = (y / (float)m_nGridY - 0.5f) * tgt.aspectY;
      for (int x = 0; x <= m_nGridX; x++) {
        float fx;
        if (m_bScreenDependentRenderMode)
          fx = (x / (float)m_nGridX - 0.5f);
        else
          fx = (x / (float)m_nGridX - 0.5f) * tgt.aspectX;

        // Convert to polar coordinates
        float angle = atan2f(fy, fx); // range: -PI to PI
        float radius = sqrtf(fx * fx + fy * fy) * 1.41421356f; // normalized 0-1

        // Create moebius strip effect
        float normalized_angle = (angle + 3.14159265f) / 6.2831853f; // 0-1

        // Calculate the twist - makes a half-twist as we go around the circle
        float twist_progress = (normalized_angle + progress_offset) * twist_factor;
        if (reverse_twist) twist_progress = -twist_progress;

        // Moebius strip effect combines radius with twisted angle
        float moebius_value = radius + 0.3f * sinf(twist_progress * 3.14159265f);

        // Apply strip width to create the banding effect
        float t = fmodf(moebius_value * (1.0f / strip_width), 1.0f);

        // Make the transition flow outward
        t = 1.0f - t;

        // Apply band blending
        tgt.vertinfo[nVert].a = inv_band * (1.0f + band);
        tgt.vertinfo[nVert].c = -inv_band + inv_band * t;
        nVert++;
      }
    }
  }
  else if (mixtype == 14) {
    // DeepSeek - Star Wipe Transition
    float band = 0.05f + 0.15f * FRAND;  // transition edge width
    float inv_band = 1.0f / band;
    int points = 5 + (rand() % 2);      // 5-6 points on the star
    float inner_radius = 0.3f + FRAND * 0.4f; // 0.3-0.7 inner radius
    float rotation = FRAND * 6.2831853f; // random initial rotation
    bool reverse = (rand() % 2) == 0;    // reverse direction

    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      float fy;
      if (m_bScreenDependentRenderMode)
        fy = (y / (float)m_nGridY - 0.5f);
      else
        fy = (y / (float)m_nGridY - 0.5f) * tgt.aspectY;
      for (int x = 0; x <= m_nGridX; x++) {
        float fx;
        if (m_bScreenDependentRenderMode)
          fx = (x / (float)m_nGridX - 0.5f);
        else
          fx = (x / (float)m_nGridX - 0.5f) * tgt.aspectX;

        // Convert to polar coordinates
        float angle = atan2f(fy, fx) + rotation; // range: -PI to PI plus rotation
        float radius = sqrtf(fx * fx + fy * fy) * 1.41421356f; // normalized distance

        // Wrap angle to 0-2PI range
        if (angle < 0) angle += 6.2831853f;
        if (angle >= 6.2831853f) angle -= 6.2831853f;

        // Calculate star pattern
        float segment = 6.2831853f / points;
        float point_angle = fmodf(angle, segment) / segment; // 0-1 within each segment

        // Alternate between inner and outer radius
        float star_radius;
        if (point_angle < 0.5f) {
          // First half of segment - interpolate from inner to outer radius
          star_radius = inner_radius + (1.0f - inner_radius) * point_angle * 2.0f;
        }
        else {
          // Second half of segment - interpolate from outer back to inner radius
          star_radius = 1.0f - (1.0f - inner_radius) * (point_angle - 0.5f) * 2.0f;
        }

        // Calculate how far we are from the star edge
        float t = (radius / star_radius);
        if (reverse) t = 1.0f - t;

        // Apply band blending
        tgt.vertinfo[nVert].a = inv_band * (1.0f + band);
        tgt.vertinfo[nVert].c = -inv_band + inv_band * t;
        nVert++;
      }
    }
  }
  else if (mixtype == 15) {
    // DeepSeek - Disco Floor Transition
    float band = 0.08f + 0.12f * FRAND;  // transition edge width
    float inv_band = 1.0f / band;

    // Disco floor parameters
    int tile_size = 8 + (rand() % 25);    // 8-32 pixel tile size (approximate)
    float beat_sync = 0.5f + FRAND * 1.5f; // 0.5-2.0 beat sync intensity
    bool diagonal_pattern = (rand() % 2) == 0; // alternate diagonal pattern
    bool color_cycling = (rand() % 2) == 0;   // enable color cycling effect
    float speed_factor = 0.5f + FRAND * 2.0f; // animation speed (0.5-2.5)

    // Get current time for animation (using a fake time if not available)
    static float fake_time = 0.0f;
    if (!m_bMilk2DeterministicField) fake_time += 1 / GetFps();  // see note above
    float time = (m_bMilk2DeterministicField ? 0.0f : fake_time) * speed_factor;

    // Simulate beat detection with a sine wave if real beat info isn't available
    float beat = sinf(time * 3.0f) * 0.5f + 0.5f;
    beat = powf(beat, beat_sync);

    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      float fy = y / (float)m_nGridY;
      for (int x = 0; x <= m_nGridX; x++) {
        float fx = x / (float)m_nGridX;

        // Calculate tile coordinates
        int tile_x = (int)(fx * m_nGridX / tile_size);
        int tile_y = (int)(fy * m_nGridY / tile_size);

        // Create alternating pattern
        float pattern;
        if (diagonal_pattern) {
          // Diagonal checkerboard pattern
          pattern = ((tile_x + tile_y) % 2) * 0.8f + 0.1f;
        }
        else {
          // Standard checkerboard pattern
          pattern = ((tile_x % 2) == (tile_y % 2)) * 0.8f + 0.1f;
        }

        // Add animation based on tile position and time
        float anim = sinf(time * 2.0f + tile_x * 0.3f + tile_y * 0.7f) * 0.5f + 0.5f;

        // Combine with beat detection
        float t = (pattern * 0.7f + anim * 0.3f) * beat;

        // Add color cycling effect if enabled
        if (color_cycling) {
          float hue = fmodf(time * 0.2f + tile_x * 0.1f + tile_y * 0.15f, 1.0f);
          t = fmodf(t + hue * 0.3f, 1.0f);
        }

        // Apply band blending
        tgt.vertinfo[nVert].a = inv_band * (1.0f + band);
        tgt.vertinfo[nVert].c = -inv_band + inv_band * t;
        nVert++;
      }
    }
  }
  else if (mixtype == 16) {
    // DeepSeek - Fire/Flame Transition - rising upward with random patterns
    float band = 0.08f + 0.04f * FRAND;  // flame edge thickness
    float inv_band = 1.0f / band;

    // Fire parameters
    float flame_speed = 0.7f + FRAND * 0.6f;    // speed (0.7-1.3)
    float base_height = 0.0f;                   // always start at bottom

    // Pre-compute some random flame properties
    float seed1 = FRAND * 10.0f;
    float seed2 = FRAND * 20.0f;
    float seed3 = FRAND * 30.0f;

    // Get current time for animation
    static float fire_time = 0.0f;
    if (!m_bMilk2DeterministicField) fire_time += 1 / GetFps();  // see note above
    float time = m_bMilk2DeterministicField ? 0.0f : fire_time;

    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      float fy = (y / (float)m_nGridY); // 0-1 from bottom to top
      for (int x = 0; x <= m_nGridX; x++) {
        float fx = (x / (float)m_nGridX);

        // Generate deterministic random patterns using noise functions
        float random_flame =
          sinf(fx * 15.0f + seed1 + time * 2.0f) * 0.4f +
          sinf(fx * 30.0f + seed2 + time * 3.7f) * 0.2f +
          sinf(fx * 45.0f + seed3 + time * 5.3f) * 0.1f;

        // Shape the flame (wider at bottom, narrower at top)
        float flame_shape = (1.0f - fy) * (0.3f + random_flame * 0.7f);

        // Calculate flame front position (rising from bottom)
        float flame_front = fmodf(time * flame_speed, 1.5f);

        // Flame transition value - positive when below flame front
        float t = 1.0f - (fy - flame_front + flame_shape);

        // Basic 0-1 clamping
        t = (t < 0) ? 0 : ((t > 1) ? 1 : t);

        // Apply band blending
        tgt.vertinfo[nVert].a = inv_band * (1.0f + band);
        tgt.vertinfo[nVert].c = -inv_band + inv_band * t;
        nVert++;
      }
    }
  }
  else if (mixtype == 17) {
    // DeepSeek - Drain Swirl Transition, modified by Incubo_
    float band = 0.05f + 0.15f * FRAND;  // transition edge width
    float inv_band = 1.0f / band;

    // Drain parameters
    float swirl_intensity = 2.0f + FRAND * 3.0f; // 2-5 - controls how tight the swirl is
    float drain_speed = 0.5f + FRAND * 1.5f;    // 0.5-2.0 - speed of the drain effect
    bool clockwise = (rand() % 2) == 0;         // random swirl direction
    float center_pull = 0.7f + FRAND * 0.6f;    // 0.7-1.3 - how strongly it pulls to center
    bool invert = (rand() % 2) == 0;           // random inversion

    // Get current time for animation
    static float drain_time = 0.0f;
    if (!m_bMilk2DeterministicField) drain_time += 1 / GetFps();  // see note above
    float time = (m_bMilk2DeterministicField ? 0.0f : drain_time) * drain_speed;

    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      float fy;
      if (m_bScreenDependentRenderMode)
        fy = (y / (float)m_nGridY - 0.5f);
      else
        fy = (y / (float)m_nGridY - 0.5f) * tgt.aspectY;
      for (int x = 0; x <= m_nGridX; x++) {
        float fx;
        if (m_bScreenDependentRenderMode)
          fx = (x / (float)m_nGridX - 0.5f);
        else
          fx = (x / (float)m_nGridX - 0.5f) * tgt.aspectX;

        // Calculate polar coordinates
        float radius = sqrtf(fx * fx + fy * fy) * 1.41421356f; // normalized distance
        float angle = atan2f(fy, fx); // range: -PI to PI

        // Apply swirl effect - angle changes more as you get closer to center
        float swirl_factor = (1.0f - radius) * swirl_intensity;
        if (clockwise) swirl_factor = -swirl_factor;

        // Combine with time-based animation
        float swirled_angle = angle + swirl_factor + time * 2.0f;

        // Create the drain effect - combines radial and angular motion
        float t = radius * center_pull + (1.0f - center_pull) *
          (0.5f + 0.5f * sinf(swirled_angle * 2.0f + radius * 5.0f));

        // Invert the drain if needed.
        if (invert)
          t = 1.0f - t;

        // Apply band blending
        tgt.vertinfo[nVert].a = inv_band * (1.0f + band);
        tgt.vertinfo[nVert].c = -inv_band + inv_band * t;
        nVert++;
      }
    }
  }
  else if (mixtype == 18) {
    // DeepSeek - Smooth Julia Set Fractal Transition
    float band = 0.08f + 0.12f * FRAND;  // Wider band for smoother transitions
    float inv_band = 1.0f / band;

    // Julia set parameters with constrained ranges for better blending
    float julia_real = -0.8f + FRAND * 1.6f;    // (-0.8 to 0.8)
    float julia_imag = -0.8f + FRAND * 1.6f;    // (-0.8 to 0.8)
    int max_iterations = 20 + (rand() % 20);     // 20-40 iterations (good balance)
    float zoom = 0.7f + FRAND * 1.6f;           // 0.7-2.3 zoom level
    float rotation = FRAND * 6.2831853f;         // random rotation

    // Always use smooth coloring for this version
    const bool smooth_coloring = true;

    // Additional smoothing parameters
    float edge_softness = 0.3f + FRAND * 0.5f;  // 0.3-0.8 edge softness
    float contrast = 0.7f + FRAND * 0.6f;       // 0.7-1.3 contrast adjustment

    // Precompute rotation values
    float cos_rot = cosf(rotation);
    float sin_rot = sinf(rotation);

    // Find min/max for normalization
    float min_val = FLT_MAX;
    float max_val = -FLT_MAX;
    std::vector<float> values((m_nGridY + 1) * (m_nGridX + 1));

    // First pass: compute all values and find range
    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      float fy;
      if (m_bScreenDependentRenderMode)
        fy = (y / (float)m_nGridY - 0.5f);
      else
        fy = (y / (float)m_nGridY - 0.5f) * tgt.aspectY;

      for (int x = 0; x <= m_nGridX; x++) {
        float fx;
        if (m_bScreenDependentRenderMode)
          fx = (x / (float)m_nGridX - 0.5f);
        else
          fx = (x / (float)m_nGridX - 0.5f) * tgt.aspectX;

        // Apply rotation and zoom
        float zx = (fx * cos_rot - fy * sin_rot) * zoom;
        float zy = (fx * sin_rot + fy * cos_rot) * zoom;

        // Julia set iteration
        float cx = julia_real;
        float cy = julia_imag;
        int i;
        for (i = 0; i < max_iterations; i++) {
          float tmp = zx * zx - zy * zy + cx;
          zy = 2 * zx * zy + cy;
          zx = tmp;

          if (zx * zx + zy * zy > 4.0f)
            break;
        }

        // Calculate smooth value
        float t;
        if (i < max_iterations) {
          float log_zn = logf(zx * zx + zy * zy) / 2.0f;
          float nu = logf(log_zn / logf(2.0f)) / logf(2.0f);
          t = (i + 1 - nu) / max_iterations;
        }
        else {
          t = 1.0f;  // Interior points
        }

        // Apply contrast adjustment
        t = powf(t, contrast);

        values[nVert] = t;
        if (t < min_val) min_val = t;
        if (t > max_val) max_val = t;
        nVert++;
      }
    }

    // Normalize and apply blending
    float range = max_val - min_val;
    if (range < 0.0001f) range = 1.0f; // Prevent division by zero

    nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      for (int x = 0; x <= m_nGridX; x++) {
        // Normalize value to 0-1 range
        float t = (values[nVert] - min_val) / range;

        // Apply edge softness using smoothstep function
        t = t * t * (3.0f - 2.0f * t) * (1.0f - edge_softness) + t * edge_softness;

        // Final blending calculation with smoother transition
        tgt.vertinfo[nVert].a = inv_band * (1.0f + band * 1.5f);  // Increased blend area
        tgt.vertinfo[nVert].c = -inv_band + inv_band * t * 1.1f;  // Slightly extended range

        // Ensure values stay within reasonable bounds
        tgt.vertinfo[nVert].c = max(-10.0f, min(10.0f, tgt.vertinfo[nVert].c));
        nVert++;
      }
    }
  }
  else if (mixtype == 19) {
    // Fixed vertical wipe (left-to-right) — for .milk2 "vertical" pattern
    float band = 0.15f;
    float inv_band = 1.0f / band;
    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      for (int x = 0; x <= m_nGridX; x++) {
        float fx = (x / (float)m_nGridX);
        float t = fx;
        tgt.vertinfo[nVert].a = inv_band * (1 + band);
        tgt.vertinfo[nVert].c = -inv_band + inv_band * t;
        nVert++;
      }
    }
  }
  else if (mixtype == 20) {
    // Fixed horizontal wipe (top-to-bottom) — for .milk2 "horizontal" pattern
    float band = 0.15f;
    float inv_band = 1.0f / band;
    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
      float fy = (y / (float)m_nGridY);
      for (int x = 0; x <= m_nGridX; x++) {
        float t = fy;
        tgt.vertinfo[nVert].a = inv_band * (1 + band);
        tgt.vertinfo[nVert].c = -inv_band + inv_band * t;
        nVert++;
      }
    }
  }
}

void Engine::GenPlasma(const BlendPatternTarget& tgt,
                       int x0, int x1, int y0, int y1, float dt) {
  // MilkDrop draws a fresh FRAND at each of the five displacement sites below,
  // so its plasma field is different every call. MD3 PRO's build of the same
  // function substitutes ONE saved random per site, reused at every recursion
  // depth (MilkDroprev.c:45212 -- DAT_00f1c570..580 are random_1..5). That is
  // what makes a .milk2's plasma field reproducible from the file alone.
  //
  // Site order, read off the decompilation: r1 the x0 edge, r2 the x1 edge,
  // r3 the y0 edge, r4 the y1 edge, r5 the midpoint.
  const bool sr = m_bMilk2UseSavedRandoms;
  auto disp = [&](int i) -> float {
    return sr ? (m_fMilk2Random[i] * 2.0f - 1.0f) : (FRAND * 2 - 1);
  };
  int midx = (x0 + x1) / 2;
  int midy = (y0 + y1) / 2;
  float t00 = tgt.vertinfo[y0 * (m_nGridX + 1) + x0].c;
  float t01 = tgt.vertinfo[y0 * (m_nGridX + 1) + x1].c;
  float t10 = tgt.vertinfo[y1 * (m_nGridX + 1) + x0].c;
  float t11 = tgt.vertinfo[y1 * (m_nGridX + 1) + x1].c;

  if (y1 - y0 >= 2) {
    if (x0 == 0)
      if (m_bScreenDependentRenderMode)
        tgt.vertinfo[midy * (m_nGridX + 1) + x0].c = 0.5f * (t00 + t10) + disp(0) * dt;
      else
        tgt.vertinfo[midy * (m_nGridX + 1) + x0].c = 0.5f * (t00 + t10) + disp(0) * dt * tgt.aspectY;
    if (m_bScreenDependentRenderMode)
      tgt.vertinfo[midy * (m_nGridX + 1) + x1].c = 0.5f * (t01 + t11) + disp(1) * dt;
    else
      tgt.vertinfo[midy * (m_nGridX + 1) + x1].c = 0.5f * (t01 + t11) + disp(1) * dt * tgt.aspectY;
  }
  if (x1 - x0 >= 2) {
    if (y0 == 0)
      if (m_bScreenDependentRenderMode)
        tgt.vertinfo[y0 * (m_nGridX + 1) + midx].c = 0.5f * (t00 + t01) + disp(2) * dt;
      else
        tgt.vertinfo[y0 * (m_nGridX + 1) + midx].c = 0.5f * (t00 + t01) + disp(2) * dt * tgt.aspectX;
    if (m_bScreenDependentRenderMode)
      tgt.vertinfo[y1 * (m_nGridX + 1) + midx].c = 0.5f * (t10 + t11) + disp(3) * dt;
    else
      tgt.vertinfo[y1 * (m_nGridX + 1) + midx].c = 0.5f * (t10 + t11) + disp(3) * dt * tgt.aspectX;
  }

  if (y1 - y0 >= 2 && x1 - x0 >= 2) {
    // do midpoint & recurse:
    t00 = tgt.vertinfo[midy * (m_nGridX + 1) + x0].c;
    t01 = tgt.vertinfo[midy * (m_nGridX + 1) + x1].c;
    t10 = tgt.vertinfo[y0 * (m_nGridX + 1) + midx].c;
    t11 = tgt.vertinfo[y1 * (m_nGridX + 1) + midx].c;
    tgt.vertinfo[midy * (m_nGridX + 1) + midx].c = 0.25f * (t10 + t11 + t00 + t01) + disp(4) * dt;

    GenPlasma(tgt, x0, midx, y0, midy, dt * 0.5f);
    GenPlasma(tgt, midx, x1, y0, midy, dt * 0.5f);
    GenPlasma(tgt, x0, midx, midy, y1, dt * 0.5f);
    GenPlasma(tgt, midx, x1, midy, y1, dt * 0.5f);
  }
}

void Engine::CompilePresetShadersToFile(wchar_t* sPresetFile) {
  CState* pState = new CState();
  PShaderSet pShaders;
  RemoveAngleBrackets(sPresetFile);

  DWORD ApplyFlags = STATE_ALL;
  pState->Import(sPresetFile, GetTime(), NULL, ApplyFlags);
  LoadShaders(&pShaders, pState, false, true);
  delete pState;
  pState = NULL;
}

// CurrentPresetLeaf / CurrentPresetPath - resolve m_szCurrentPresetFile (#12)
//
// The field holds a bare filename or a full path depending on how the preset
// was loaded. Both forms are legitimate; what was not is thirty call sites each
// deciding for themselves, and doing it differently -- most looked only for
// '\', so a path using '/' was handed back whole and treated as a filename.
//
// No stored discriminant, because none carries information a Windows path does
// not already: neither separator can occur in a filename, so a separator means
// path and its absence means name. The value of a single resolution point is
// that it is one behaviour rather than thirty.
const wchar_t* Engine::CurrentPresetLeaf() const {
  const wchar_t* back = wcsrchr(m_szCurrentPresetFile, L'\\');
  const wchar_t* fwd  = wcsrchr(m_szCurrentPresetFile, L'/');
  const wchar_t* sep  = (back && fwd) ? (back > fwd ? back : fwd)
                                      : (back ? back : fwd);
  return sep ? sep + 1 : m_szCurrentPresetFile;
}

void Engine::CurrentPresetPath(wchar_t* out, size_t count) const {
  if (!out || count == 0) return;
  out[0] = 0;
  if (!m_szCurrentPresetFile[0]) return;
  // Already absolute: hand it back unchanged rather than prefixing a second
  // directory onto it.
  if (wcschr(m_szCurrentPresetFile, L'\\') || wcschr(m_szCurrentPresetFile, L'/')) {
    lstrcpynW(out, m_szCurrentPresetFile, (int)count);
    return;
  }
  // `out` is a caller-supplied pointer, so the count cannot be deduced. Build
  // the whole path in a local of exactly the two sources' size and hand back a
  // bounded copy, matching the lstrcpynW above.
  wchar_t full[MAX_PATH + 512];
  FormatTo(full, L"%s%s", m_szPresetDir, m_szCurrentPresetFile);
  lstrcpynW(out, full, (int)count);
}

std::wstring Engine::CurrentPresetDir() const {
  const wchar_t* leaf = CurrentPresetLeaf();
  if (leaf == m_szCurrentPresetFile) return m_szPresetDir;   // bare name
  return std::wstring(m_szCurrentPresetFile, leaf - 1);      // drop the separator
}

void Engine::ClearPreset() {
  // Under the mirror engine mutex, and behind a GPU wait (#214).
  //
  // This rotates the shader sets, reloads them in place and rebuilds the
  // preset PSOs -- the same mutation LoadPresetTick's commit performs. That one
  // is safe because MyRenderFn runs INSIDE the MirrorEngineGuard and waits the
  // GPU first; this one is reached from the CLEARPRESET IPC verb, which arrives
  // as a PostMessage to the render window and therefore runs on the render
  // thread but OUTSIDE the guard, with mirror workers free to be mid-record.
  //
  // Both halves are needed and they answer different questions. WaitForGpu
  // covers work already SUBMITTED that names the outgoing PSOs and textures --
  // the #229 finding, measured as 2 device removals in 4 runs before it was
  // added to the commit path. The mutex covers a worker that is mid-RECORD and
  // reading m_shaders as it is rotated; no fence can help with that, because
  // nothing has been submitted yet.
  //
  // Rare and user-driven, so the cost of holding both is nothing.
  if (m_lpDX)
    m_lpDX->WaitForGpu();
  LockMirrorEngine();
  struct Unlocker {
    Engine* e;
    ~Unlocker() { e->UnlockMirrorEngine(); }
  } unlocker{ this };

  m_pState->Default(STATE_ALL);
  CopyTo(m_szCurrentPresetFile, m_pState->m_szDesc);
  RemoveAngleBrackets(m_szCurrentPresetFile);

  // Append ".milk" to m_szCurrentPresetFile
  if (wcslen(m_szCurrentPresetFile) + wcslen(L".milk") < MAX_PATH) {
    AppendTo(m_szCurrentPresetFile, L".milk");
  }

  // Hand the live set to m_OldShaders, then load the new (default, no preset)
  // shaders in place. forgejo#5: this used to Clear/null only warp and comp
  // by hand, silently leaking and then double-releasing bufferA-D whenever
  // CLEARPRESET reached a Shadertoy preset -- reachable live, via script or
  // IPC. RotatePShaderSet covers all six unconditionally.
  RotatePShaderSet(m_OldShaders, m_shaders);

  LoadShaders(&m_shaders, m_pState, false, false);
  CreateDX12PresetPSOs();
  NumTotalPresetsLoaded++;
  OnFinishedLoadingPreset();
}

void Engine::RemoveAngleBrackets(wchar_t* str) {
  if (!str) return;

  // Compacted IN PLACE. A filter that only ever removes characters cannot
  // outrun its own read cursor, so there is no temporary and no size to get
  // wrong -- which is the whole problem this replaces (issue 149).
  //
  // The previous version compacted into a wchar_t[MAX_PATH] with NO bound on
  // the write index, while all four callers pass a 512-element buffer
  // (m_szCurrentPresetFile and m_pState->m_szDesc). A preset description or
  // filename over 259 characters therefore ran off the end of a stack array.
  // Its copy back WAS bounded, so the overrun was silent -- and that bound
  // was a second defect, truncating any description over 259 characters even
  // when the loop had not overrun.
  int j = 0;
  for (int i = 0; str[i] != L'\0'; i++) {
    if (str[i] != L'<' && str[i] != L'>')
      str[j++] = str[i];
  }
  str[j] = L'\0';
}

// ---------------------------------------------------------------------------
// .milk2 double-preset support
// ---------------------------------------------------------------------------

// Maps MilkDrop3 blend-pattern names to MDropDX12 RandomizeBlendPattern() mixtype indices.
// Returns -1 (random) for any name that is not explicitly mapped.
static int Milk2PatternNameToMixtype(const char* name) {
  struct { const char* name; int type; } kMap[] = {
    {"zoom",             0},  // uniform fade
    {"side",             1},  // directional wipe
    {"plasma",           2},  // fractal plasma
    {"plasma2",          2},  // plasma variants -> plasma
    {"plasma3",          2},
    {"cercle",           3},  // radial / circle
    {"clock",            4},  // angular clock sweep
    {"snail",            5},  // spiral
    {"snail2",           5},
    {"snail3",           5},
    {"triangle",         6},  // rhombus/diamond
    {"nuclear",          7},  // nuclear clock wipe
    {"square",           8},  // square/diamond
    {"checkerboard",     9},  // animated checkerboard
    {"curtain",         10},  // curtain
    {"vertical",        19},  // fixed left-to-right directional wipe
    {"horizontal",      20},  // fixed top-to-bottom directional wipe
    {"bubbles",         11},  // bubble
    {"donuts",          11},  // donuts -> bubble (concentric circles)
    {"stars",           12},  // kaleidoscope wipe
    {"stars2",          12},
    {"cisor",           13},  // moebius strip
    {"wave",            14},  // star wipe
    {"linesvertical",   15},  // disco floor
    {"lineshorizontal", 15},
    {"patches",         16},  // fire/flame
    {"corner",          17},  // drain swirl
    {"cross",           18},  // julia fractal
    {"cross2",          18},
    {"arrow",            1},  // arrow -> directional wipe
  };
  for (auto& e : kMap)
    if (_stricmp(name, e.name) == 0) return e.type;
  return -1;
}

// MilkDrop 3.25+ embeds [SPRITEn_BEGIN]...[SPRITEn_END] in both .milk and .milk2.
// Last block per index wins. sprite=0 in the file header disables them.
void Engine::ParseEmbeddedSprites(const std::string& buf) {
  m_milk2Sprites.clear();
  m_bMilk2SpritesNeedApply = false;

  std::map<int, Milk2SpriteDesc> byIndex;
  size_t pos = 0;
  while (pos < buf.size()) {
    size_t b = buf.find("[SPRITE", pos);
    if (b == std::string::npos)
      break;
    size_t numStart = b + 7;
    if (numStart >= buf.size() || buf[numStart] < '0' || buf[numStart] > '9') {
      pos = b + 7;
      continue;
    }
    int idx = atoi(buf.c_str() + numStart);
    size_t tagEnd = buf.find("_BEGIN]", numStart);
    if (tagEnd == std::string::npos || tagEnd - numStart > 6) {
      pos = b + 7;
      continue;
    }
    char endMarker[32];
    FormatToA(endMarker, "[SPRITE%d_END]", idx);
    size_t e = buf.find(endMarker, tagEnd);
    if (e == std::string::npos) {
      pos = tagEnd + 7;
      continue;
    }
    size_t content = buf.find_first_of("\r\n", tagEnd);
    if (content == std::string::npos) {
      pos = e + 1;
      continue;
    }
    content = buf.find_first_not_of("\r\n", content);
    if (content == std::string::npos || content >= e) {
      pos = e + 1;
      continue;
    }
    std::string block = buf.substr(content, e - content);
    Milk2SpriteDesc d;
    d.nIndex = idx;
    auto getKey = [&](const char* key, std::string& dst) {
      std::string k = std::string(key) + "=";
      size_t p = 0;
      while ((p = block.find(k, p)) != std::string::npos) {
        if (p == 0 || block[p - 1] == '\n' || block[p - 1] == '\r')
          break;
        p += k.size();
      }
      if (p == std::string::npos)
        return;
      size_t start = p + k.size();
      size_t end = block.find_first_of("\r\n", start);
      dst = block.substr(start, end - start);
    };
    auto getF = [&](const char* key, float& dst) {
      std::string s;
      getKey(key, s);
      if (!s.empty())
        dst = (float)atof(s.c_str());
    };
    auto getI = [&](const char* key, int& dst) {
      std::string s;
      getKey(key, s);
      if (!s.empty())
        dst = atoi(s.c_str());
    };
    std::string name;
    getKey("SpriteName", name);
    if (name.empty()) {
      pos = e + 1;
      continue;
    }
    MultiByteToWideChar(CP_ACP, 0, name.c_str(), -1, d.szName, MAX_PATH);
    std::string ck;
    getKey("SpriteColorKey", ck);
    if (!ck.empty())
      d.nColorKey = (unsigned int)strtoul(ck.c_str(), nullptr, 0);
    getI("SpriteLayer", d.nLayer);
    getI("SpriteBlend", d.nBlend);
    getF("SpriteAlpha", d.fAlpha);
    getF("SpriteBurn", d.fBurn);
    getF("SpriteX", d.fX);
    getF("SpriteY", d.fY);
    getF("SpriteSX", d.fSX);
    getF("SpriteSY", d.fSY);
    getF("SpriteRot", d.fRot);
    getF("SpriteSpeed", d.fSpeed);
    getF("SpriteRepeatX", d.fRepeatX);
    getF("SpriteRepeatY", d.fRepeatY);
    // MD3 PRO has eleven blend modes, 0..10. Clamping to 7 here silently
    // turned Cut-off, Darken and Vivid into Invert; 25 sprite instances across
    // the preset library use those three.
    if (d.nBlend < 0) d.nBlend = 0;
    if (d.nBlend > 10) d.nBlend = 10;

    auto collectCode = [&](const char* prefix, std::string& dest) {
      dest.clear();
      for (int line = 1; line < 256; line++) {
        char key[32];
        FormatToA(key, "%s_%d", prefix, line);
        std::string s;
        getKey(key, s);
        if (s.empty())
          break;
        if (!dest.empty())
          dest += (char)LINEFEED_CONTROL_CHAR;
        dest += s;
      }
    };
    collectCode("init", d.szInit);
    collectCode("code", d.szCode);
    byIndex[idx] = d;
    pos = e + 1;
  }

  std::string sprFlag;
  {
    size_t hdrEnd = buf.find("[PRESET1_BEGIN]");
    if (hdrEnd == std::string::npos)
      hdrEnd = buf.find("[preset00]");
    std::string hdr = (hdrEnd == std::string::npos) ? std::string() : buf.substr(0, hdrEnd);
    size_t sp = hdr.find("sprite=");
    if (sp != std::string::npos)
      sprFlag = hdr.substr(sp + 7, 8);
  }
  const bool spritesOff = !sprFlag.empty() && atoi(sprFlag.c_str()) == 0;
  if (!spritesOff) {
    for (auto& kv : byIndex)
      m_milk2Sprites.push_back(kv.second);
  }
  if (!m_milk2Sprites.empty()) {
    DebugLogWFmt(LOG_INFO, L"ParseEmbeddedSprites: %d sprite(s)",
                 (int)m_milk2Sprites.size());
  }
}

bool Engine::LoadEmbeddedSpritesFromFile(const wchar_t* szPath) {
  m_milk2Sprites.clear();
  m_bMilk2SpritesNeedApply = false;
  if (!szPath || !szPath[0])
    return false;
  FILE* f = _wfopen(szPath, L"rb");
  if (!f)
    return false;
  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (fsize <= 0) {
    fclose(f);
    return false;
  }
  std::string buf((size_t)fsize, '\0');
  fread(&buf[0], 1, (size_t)fsize, f);
  fclose(f);
  ParseEmbeddedSprites(buf);
  return !m_milk2Sprites.empty();
}

// Parses a .milk2 file and writes its two preset blocks to temporary .milk files.
// On success, outTemp1/outTemp2 hold MAX_PATH paths to temp files that the caller must delete.
// Returns false on parse failure (malformed .milk2); temp files are not written.
bool Engine::ParseMilk2File(const wchar_t* szPath,
                              wchar_t* outTemp1, wchar_t* outTemp2,
                              int& outMixType, float& outProgress, int& outDirection) {
  outMixType  = -1;
  outProgress = 0.5f;
  outDirection = 1;
  m_milk2Sprites.clear();
  m_bMilk2SpritesNeedApply = false;

  // Read entire file into a string buffer.
  FILE* f = _wfopen(szPath, L"rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::string buf(fsize, '\0');
  fread(&buf[0], 1, fsize, f);
  fclose(f);

  // Parse header key=value lines before [PRESET1_BEGIN].
  {
    size_t hdrEnd = buf.find("[PRESET1_BEGIN]");
    if (hdrEnd == std::string::npos) return false;
    std::string hdr = buf.substr(0, hdrEnd);
    auto getVal = [&](const char* key) -> std::string {
      std::string k = std::string(key) + "=";
      size_t pos = hdr.find(k);
      if (pos == std::string::npos) return "";
      size_t start = pos + k.size();
      size_t end = hdr.find_first_of("\r\n", start);
      return hdr.substr(start, end - start);
    };
    std::string pat = getVal("blending_pattern");
    m_szMilk2Pattern[0] = 0;
    if (!pat.empty()) {
      outMixType = Milk2PatternNameToMixtype(pat.c_str());
      // Keep the raw name: the measured wipe geometry is per-pattern, and
      // several MD3 patterns collapse onto one legacy mixtype.
      strncpy_s(m_szMilk2Pattern, pat.c_str(), _TRUNCATE);
    }
    std::string prog = getVal("blending_progress");
    if (!prog.empty()) outProgress = (float)atof(prog.c_str());
    std::string dir = getVal("blending_direction");
    if (!dir.empty()) outDirection = atoi(dir.c_str());
    m_bMilk2HasRandoms = false;
    for (int i = 0; i < 5; i++)
      m_fMilk2Random[i] = 0.0f;
    {
      bool any = false;
      for (int i = 0; i < 5; i++) {
        char key[16];
        FormatToA(key, "random_%d", i + 1);
        std::string rv = getVal(key);
        if (!rv.empty()) {
          m_fMilk2Random[i] = (float)atof(rv.c_str());
          any = true;
        }
      }
      m_bMilk2HasRandoms = any;
    }
    m_nMilk2Direction = outDirection;
  }

  // Helper: extract text between two markers, starting from [preset00] or version header.
  auto extractPreset = [&](const char* beginMarker, const char* endMarker) -> std::string {
    size_t bPos = buf.find(beginMarker);
    if (bPos == std::string::npos) return "";
    size_t ePos = buf.find(endMarker, bPos);
    if (ePos == std::string::npos) return "";
    // The block between [PRESETn_BEGIN] and [PRESETn_END] starts with NAME= / version lines,
    // then [preset00].  Pass everything from the version/name header so Import() can
    // read MILKDROP_PRESET_VERSION and PSVERSION* before the [preset00] section.
    size_t contentStart = bPos + strlen(beginMarker);
    // Skip past the [PRESETn_BEGIN] line ending
    contentStart = buf.find_first_of("\r\n", contentStart);
    if (contentStart == std::string::npos) return "";
    contentStart = buf.find_first_not_of("\r\n", contentStart);
    if (contentStart == std::string::npos) return "";
    return buf.substr(contentStart, ePos - contentStart);
  };

  std::string p1 = extractPreset("[PRESET1_BEGIN]", "[PRESET1_END]");
  std::string p2 = extractPreset("[PRESET2_BEGIN]", "[PRESET2_END]");
  if (p1.empty() || p2.empty()) return false;

  // Write to Windows temp files.
  wchar_t tempDir[MAX_PATH];
  GetTempPathW(MAX_PATH, tempDir);

  if (GetTempFileNameW(tempDir, L"mk2", 0, outTemp1) == 0) return false;
  if (GetTempFileNameW(tempDir, L"mk2", 0, outTemp2) == 0) {
    DeleteFileW(outTemp1);
    return false;
  }

  auto writeTmp = [](const wchar_t* path, const std::string& text) -> bool {
    FILE* out = _wfopen(path, L"wb");
    if (!out) return false;
    fwrite(text.data(), 1, text.size(), out);
    fclose(out);
    return true;
  };

  if (!writeTmp(outTemp1, p1) || !writeTmp(outTemp2, p2)) {
    DeleteFileW(outTemp1);
    DeleteFileW(outTemp2);
    return false;
  }

  ParseEmbeddedSprites(buf);
  return true;
}

// Required to prevent stale data when two Import() calls use consecutively-allocated FILE*s.
} // namespace mdrop (close for global extern)
namespace mdrop {

// NOTE: LoadMilk2Preset has been replaced by async loading in LoadPreset's bIsMilk2 branch.
// ParseMilk2File is still used — it extracts temp files from the .milk2 format.

// Loads a .milk3 preset from JSON: { pass0: "hlsl...", main: "hlsl..." }.
// The text is HLSL -- any GLSL conversion already happened at import.
// Async: parses JSON on the calling thread (fast), then launches a background thread
// for shader compilation.  LoadPresetTick() picks up the result on the render thread.
// Thread cancellation for stale async loads is handled by LoadPreset before calling this.
void Engine::LoadMilk3Preset(const wchar_t* szPresetFilename, float fBlendTime) {
  // #184: see LoadPreset. The scope here covers a synchronous compile on this
  // thread; the load thread below opens its own, because the flag is
  // thread_local and does not travel to it.
  ShaderCompileScope _shaderScope(szPresetFilename);
  JsonValue root = JsonLoadFile(szPresetFilename);
  if (!root.isObject()) {
    wchar_t buf[MAX_PATH + 64];
    FormatTo(buf, L"LoadMilk3Preset: failed to parse %s", szPresetFilename);
    DebugLogW(buf, LOG_WARN);
    {
      char line[1024];
      FormatToA(line, "FAIL parse milk3: %ls\n", szPresetFilename);
      DebugLogDiagAppend(L"diag_preset_load.txt", line);
    }
    m_nLoadingPreset = 0;
    return;
  }

  int version = root[L"version"].asInt(0);
  if (version < 1) {
    DebugLogA("LoadMilk3Preset: unsupported version", LOG_WARN);
    {
      char line[256];
      FormatToA(line, "FAIL milk3 unsupported version=%d path=%ls\n", version, szPresetFilename);
      DebugLogDiagAppend(L"diag_preset_load.txt", line);
    }
    m_nLoadingPreset = 0;
    return;
  }

  // Convert wide strings to narrow for shader text storage
  auto wideToNarrow = [](const std::wstring& ws) -> std::string {
    std::string s;
    s.reserve(ws.size());
    for (wchar_t ch : ws) {
      if (ch == L'\n')
        s += (char)LINEFEED_CONTROL_CHAR;
      else if (ch < 128)
        s += (char)ch;
      else
        s += '?';
    }
    return s;
  };

  // Fill m_pNewState (NOT m_pState — that's live on the render thread).
  // LoadPresetTick will swap pointers on the render thread when compilation finishes.
  m_pNewState->Default(0xFFFFFFFF);

  // Neutralize milk warp defaults (zoom=0.9, dx=0.04) so unused mesh UVs stay
  // in-range if anything still samples them. Shadertoy does not draw the mesh,
  // but RENDER_DIAG still showed mesh_tl UVs outside [0,1] from these defaults.
  m_pNewState->m_fZoom = 1.0f;
  m_pNewState->m_fZoomExponent = 1.0f;
  m_pNewState->m_fRot = 0.0f;
  m_pNewState->m_fWarpAmount = 0.0f;
  m_pNewState->m_fXPush = 0.0f;
  m_pNewState->m_fYPush = 0.0f;
  m_pNewState->m_fStretchX = 1.0f;
  m_pNewState->m_fStretchY = 1.0f;
  m_pNewState->m_fWarpAnimSpeed = 0.0f;
  m_pNewState->m_fWarpScale = 1.0f;
  m_pNewState->m_fDecay = 1.0f;

  // Set preset description from filename
  {
    const wchar_t* p = wcsrchr(szPresetFilename, L'\\');
    if (!p) p = szPresetFilename; else p++;
    wcsncpy_s(m_pNewState->m_szDesc, p, MAX_PATH - 1);
    wchar_t* dot = wcsrchr(m_pNewState->m_szDesc, L'.');
    if (dot) *dot = L'\0';
  }

  // Extract shader text from JSON. Both key spellings are accepted; see
  // milk3_format.h for why the names changed and why nothing needs migrating.
  std::wstring imageW   = Milk3String(root, kMilk3KeyMain, kMilk3LegacyKeyMain);
  std::wstring bufferAW = Milk3String(root, Milk3PassKey(0), Milk3LegacyPassKey(0));
  std::wstring bufferBW = Milk3String(root, Milk3PassKey(1), Milk3LegacyPassKey(1));
  std::wstring bufferCW = Milk3String(root, Milk3PassKey(2), Milk3LegacyPassKey(2));
  std::wstring bufferDW = Milk3String(root, Milk3PassKey(3), Milk3LegacyPassKey(3));

  // Store Image/comp shader
  if (!imageW.empty()) {
    std::string imageA = wideToNarrow(imageW);
    strncpy_s(m_pNewState->m_szCompShadersText, MAX_SHADER_TEXT_LEN, imageA.c_str(), _TRUNCATE);
    m_pNewState->m_nCompPSVersion = MD2_PS_5_0;
  }

  // Store Buffer A shader
  if (!bufferAW.empty()) {
    std::string bufferAA = wideToNarrow(bufferAW);
    strncpy_s(m_pNewState->m_szBufferAShadersText, MAX_SHADER_TEXT_LEN, bufferAA.c_str(), _TRUNCATE);
    m_pNewState->m_nBufferAPSVersion = MD2_PS_5_0;
  }

  // Store Buffer B shader
  if (!bufferBW.empty()) {
    std::string bufferBA = wideToNarrow(bufferBW);
    strncpy_s(m_pNewState->m_szBufferBShadersText, MAX_SHADER_TEXT_LEN, bufferBA.c_str(), _TRUNCATE);
    m_pNewState->m_nBufferBPSVersion = MD2_PS_5_0;
  }

  // Store Buffer C shader
  if (!bufferCW.empty()) {
    std::string bufferCA = wideToNarrow(bufferCW);
    strncpy_s(m_pNewState->m_szBufferCShadersText, MAX_SHADER_TEXT_LEN, bufferCA.c_str(), _TRUNCATE);
    m_pNewState->m_nBufferCPSVersion = MD2_PS_5_0;
  }

  // Store Buffer D shader
  if (!bufferDW.empty()) {
    std::string bufferDA = wideToNarrow(bufferDW);
    strncpy_s(m_pNewState->m_szBufferDShadersText, MAX_SHADER_TEXT_LEN, bufferDA.c_str(), _TRUNCATE);
    m_pNewState->m_nBufferDPSVersion = MD2_PS_5_0;
  }

  // No warp shader in Shadertoy mode
  m_pNewState->m_nWarpPSVersion = 0;
  m_pNewState->m_nMaxPSVersion = MD2_PS_5_0;

  // Launch background thread for shader compilation (D3DCompile is the expensive part).
  // LoadPresetTick on the render thread will swap state + shaders when done.
  uint64_t myGeneration = ++m_nLoadGeneration;
  {
    const wchar_t* name = wcsrchr(szPresetFilename, L'\\');
    if (!name) name = wcsrchr(szPresetFilename, L'/');
    name = name ? name + 1 : szPresetFilename;
    const int imgLen = (int)strlen(m_pNewState->m_szCompShadersText);
    const int aLen = (int)strlen(m_pNewState->m_szBufferAShadersText);
    const int bLen = (int)strlen(m_pNewState->m_szBufferBShadersText);
    const int cLen = (int)strlen(m_pNewState->m_szBufferCShadersText);
    const int dLen = (int)strlen(m_pNewState->m_szBufferDShadersText);
    DebugLogWFmt(LOG_WARN,
                 L"LoadMilk3: compile start gen=%llu %s (image=%d bufferA=%d B=%d C=%d D=%d)",
                 (unsigned long long)myGeneration, name, imgLen, aLen, bLen, cLen, dLen);
    char line[512];
    FormatToA(line,
              "milk3 compile start gen=%llu %ls image=%d A=%d B=%d C=%d D=%d\n",
              (unsigned long long)myGeneration, name, imgLen, aLen, bLen, cLen, dLen);
    DebugLogDiagAppend(L"diag_preset_load.txt", line);
  }
  // Shadertoy (.milk3) presets render through RenderFrameShadertoy and never
  // run the warp or comp pass, so no override can apply to them.
  m_activeOverride.Clear();
  const std::wstring loadingName(szPresetFilename ? szPresetFilename : L"");
  m_presetLoadThread = std::thread([this, myGeneration, loadingName]() {
    // #184: the compile scope must be opened ON THIS THREAD.
    //
    // It is thread_local, and the first attempt opened it in LoadPreset's
    // body -- on the thread that STARTS the load, not the one that compiles.
    // The setting was therefore invisible here and every flag still went to
    // m_szCurrentPresetFile, which during a load still names the outgoing
    // preset. test_error_attribution.py caught that; inspection had not.
    //
    // Captured by value: m_szLoadingPreset is a single Engine buffer that a
    // newer load overwrites, and this thread can outlive the load that
    // started it -- LoadPreset detaches after 2000ms.
    // Set directly rather than with ShaderCompileScope: these lambdas
    // contain __try, and MSVC refuses an object needing unwinding in the
    // same function (C2712). Nothing to restore anyway -- the thread ends
    // here, and the flag is thread_local, so it dies with it.
    t_shaderCompilePreset = loadingName.c_str();
    // Locked (forgejo#20): unlike the sibling .milk (~2737) and .milk2
    // (~2648) load threads, this one ran with no synchronization at all. A
    // load superseded within LoadPreset's 2000ms DETACH window keeps running
    // here -- reading m_pNewState's shader-text buffers and writing
    // m_NewShaders' six PShaderInfo structs (raw COM pointers) -- at the
    // exact same time a newly-started load thread does the same into the
    // SAME shared m_NewShaders/m_pNewState (single reused Engine members,
    // never per-load). It also used to serialize the GetFast parse cursor; that reason is
    // gone (issue 13 gave every Import its own cursor), but the shared
    // shader state below still needs it. milk3 compiles take up to ~8s, so a stale thread is
    // routinely still deep inside D3DCompile when a newer load starts.
    bool ok = false, stillCurrent = false, hasComp = false, hasA = false;
    m_presetLoadMutex.lock();
    __try {
      ok = LoadShaders(&m_NewShaders, m_pNewState, false, false);
      stillCurrent = (m_nLoadGeneration.load() == myGeneration);
      hasComp = (m_NewShaders.comp.bytecodeBlob != NULL);
      hasA = (m_NewShaders.bufferA.bytecodeBlob != NULL);
      if (stillCurrent)
        m_bPresetLoadReady.store(true);
      DebugLogWFmt(LOG_WARN,
                   L"LoadMilk3: compile %s gen=%llu stillCurrent=%d comp=%d bufferA=%d (desc=%s)",
                   ok ? L"done" : L"FAILED",
                   (unsigned long long)myGeneration,
                   stillCurrent ? 1 : 0,
                   hasComp ? 1 : 0,
                   hasA ? 1 : 0,
                   m_pNewState ? m_pNewState->m_szDesc : L"?");
      char line[512];
      FormatToA(line,
                "milk3 compile %s gen=%llu stillCurrent=%d comp=%d bufferA=%d desc=%ls\n",
                ok ? "done" : "FAILED",
                (unsigned long long)myGeneration,
                stillCurrent ? 1 : 0,
                hasComp ? 1 : 0,
                hasA ? 1 : 0,
                m_pNewState ? m_pNewState->m_szDesc : L"?");
      DebugLogDiagAppend(L"diag_preset_load.txt", line);
    } __except (WriteSEHCrashDiag(GetExceptionInformation(), m_szLoadingPreset)) {
      DLOG_ERROR("LoadMilk3Preset: CRASH during async compile of %ls (code 0x%08X)",
                 m_szLoadingPreset, GetExceptionCode());
    }
    m_presetLoadMutex.unlock();
  });
}

// ---------------------------------------------------------------------------

void Engine::LoadPreset(const wchar_t* szPresetFilename, float fBlendTime) {
  // #184: shader errors compiled below belong to THIS preset, not to whatever
  // is on screen. m_szCurrentPresetFile still names the outgoing preset until
  // LoadPresetTick commits the load, so without this a failure here flagged the
  // preset that was playing before -- and a flagged preset gets skipped by the
  // selection policy.
  //
  // Held by value because the load threads below outlive this frame: LoadPreset
  // detaches after 2000ms, and m_szLoadingPreset is one buffer a newer load
  // overwrites. The scope here covers a SYNCHRONOUS compile on this thread; the
  // threads open their own, because the flag is thread_local and does not
  // travel.
  const std::wstring loadingName(szPresetFilename ? szPresetFilename : L"");
  ShaderCompileScope _shaderScope(loadingName.c_str());
  // clear old error/notification msgs...
  if (m_nFramesSinceResize > 4) {
    ClearErrors(ERR_PRESET);
    ClearErrors(ERR_NOTIFY);
  }

  // make sure preset still exists.  (might not if they are using the "back"/fwd buttons
  //  in RANDOM preset order and a file was renamed or deleted!)
  if (GetFileAttributesW(szPresetFilename) == 0xFFFFFFFF) {

    wchar_t fullPath[MAX_PATH];
    GetFullPathNameW(szPresetFilename, MAX_PATH, fullPath, NULL);
    DebugLogW(fullPath, LOG_VERBOSE);

    wchar_t buf[1024];
    FormatResTo(buf, IDS_ERROR_PRESET_NOT_FOUND_X, fullPath);
    AddError(buf, 6.0f, ERR_PRESET, true);
    m_fPresetStartTime = GetTime();
    m_fNextPresetTime = -1.0f;		// flags UpdateTime() to recompute this
    return;
  }

  if (!m_bSequentialPresetOrder) {
    // save preset in the history.  keep in mind - maybe we are searching back through it already!
    if (m_presetHistoryFwdFence == m_presetHistoryPos) {
      // we're at the forward frontier; add to history
      m_presetHistory[m_presetHistoryPos] = szPresetFilename;
      m_presetHistoryFwdFence = (m_presetHistoryFwdFence + 1) % PRESET_HIST_LEN;

      // don't let the two fences touch
      if (m_presetHistoryBackFence == m_presetHistoryFwdFence)
        m_presetHistoryBackFence = (m_presetHistoryBackFence + 1) % PRESET_HIST_LEN;
    }
    else {
      // we're retracing our steps, either forward or backward...
    }
  }

  // Cancel any pending async load before starting a new one.
  // This prevents a stale background thread from overwriting a freshly loaded preset.
  if (m_presetLoadThread.joinable()) {
    HANDLE h = (HANDLE)m_presetLoadThread.native_handle();
    // Wait longer before detaching — 100ms was abandoning most milk3 compiles mid-flight
    // when the user (or IPC) switched presets while D3DCompile was still running.
    DWORD wait = WaitForSingleObject(h, 2000);
    if (wait == WAIT_TIMEOUT) {
      const wchar_t* was = m_szLoadingPreset[0] ? m_szLoadingPreset : L"(unknown)";
      DebugLogWFmt(LOG_WARN,
                   L"Preset load: DETACH stale compile thread (was loading %s) — new load supersedes it",
                   was);
      {
        char line[768];
        FormatToA(line, "DETACH stale compile thread was=%ls\n", was);
        DebugLogDiagAppend(L"diag_preset_load.txt", line);
      }
      m_presetLoadThread.detach();
      // The detached thread may still be mid-write into m_NewShaders/m_pNewState
      // (forgejo#20) -- no raw nulling here. m_presetLoadMutex-guarded Clear()
      // below (not raw assignment: it SafeRelease()s each COM pointer first)
      // waits for the detached thread to finish before touching these fields,
      // which is what actually makes this safe.
    } else {
      m_presetLoadThread.join();
    }
    m_bPresetLoadReady = false;
    m_nLoadingPreset = 0;
  }

  // All preset types use the async background thread path.
  // Import + shader compilation run off the render thread so the current
  // preset keeps rendering without stutter. When the thread finishes,
  // LoadPresetTick() detects it and does an instant hard-cut or blended swap.

  // Detect preset type early — .json needs special routing before loading state init
  const wchar_t* lastDot = wcsrchr(szPresetFilename, L'.');   // re-pointed at the resolved path below
  bool bIsJson  = lastDot && _wcsicmp(lastDot, L".json") == 0;

  if (bIsJson) {
    // .json shader import: GLSL→HLSL conversion via ShaderImportWindow
    // Must be handled BEFORE loading state init — ImportFromFile manages its own state.
    if (!m_shaderImportWindow)
      m_shaderImportWindow = std::make_unique<ShaderImportWindow>(this);
    std::wstring result = m_shaderImportWindow->ImportFromFile(szPresetFilename);
    DLOG_INFO("LoadPreset(json): %ls", result.c_str());
    return;
  }

  // Resolve a replacement, ONCE, before anything commits to a file.
  //
  // A preset carrying MD31=/MD32= ships a stub shader and renders black here,
  // because MD3 PRO would have supplied the real one from its own cache. When a
  // rewrite exists, run that instead. See preset_replacements.h for the order.
  //
  // This has to happen before the type routing below: `lastDot` decides .milk3
  // vs .milk2 vs .milk, and a replacement may not share the original's
  // extension. Recomputing it from the resolved path is what keeps that honest.
  lstrcpynW(m_szLoadingResolved, szPresetFilename, MAX_PATH);
  {
    // The per-preset designation outranks everything, and is the ONE source
    // that does not require an MD3x key: designating a stand-in by hand is a
    // deliberate act about this preset, whatever its header says.
    std::wstring rep = PresetReplacementFor(PathFindFileNameW(szPresetFilename));
    if (!rep.empty() && GetFileAttributesW(rep.c_str()) == INVALID_FILE_ATTRIBUTES) {
      wchar_t buf[1024];
      swprintf(buf, 1024, L"%s: designated replacement missing (%s)",
               PathFindFileNameW(szPresetFilename), rep.c_str());
      AddError(buf, 6.0f, ERR_PRESET, true);
      rep.clear();
    }

    const std::wstring md3xKey =
        rep.empty() ? PresetReplacementStore::KeyForPreset(szPresetFilename)
                    : std::wstring();
    if (!rep.empty()) {
      lstrcpynW(m_szLoadingResolved, rep.c_str(), MAX_PATH);
      lastDot = wcsrchr(m_szLoadingResolved, L'.');
      DLOG_WARN("LoadPreset: %ls has a designated replacement; rendering %ls",
                szPresetFilename, rep.c_str());
    } else if (!md3xKey.empty()) {
      std::wstring missing;
      rep = PresetReplacements().Resolve(szPresetFilename, md3xKey, &missing);
      if (!rep.empty()) {
        lstrcpynW(m_szLoadingResolved, rep.c_str(), MAX_PATH);
        lastDot = wcsrchr(m_szLoadingResolved, L'.');
        DLOG_WARN("LoadPreset: %ls carries %ls; rendering %ls instead",
                  szPresetFilename, md3xKey.c_str(), rep.c_str());
      } else {
        // No replacement. The frame is about to be wrong and until now the only
        // record of why went to debug.log, where nobody saw it -- which is
        // exactly how this arrived as a bug report.
        wchar_t buf[1024];
        if (!missing.empty())
          swprintf(buf, 1024,
                   L"%s: replacement not found (%s) - rendering the original",
                   PathFindFileNameW(szPresetFilename), missing.c_str());
        else
          // VERY short on purpose. The overlay is one line in the upper-right
          // and CLIPS at the window edge rather than wrapping, so at 800px a
          // sentence beginning with the filename loses its own point -- and the
          // preset's name is already on the HUD line above this one. The key,
          // the file and the full reasoning go to the log.
          swprintf(buf, 1024,
                   L"Incomplete preset: its shader is in MilkDrop 3's cache");
        AddError(buf, 6.0f, ERR_PRESET, true);
      }
    }
  }

  // Locked (forgejo#20): a detached-but-still-running compile thread from a
  // superseded load (see DETACH above) holds this same mutex for the
  // duration of its own LoadShaders call, so this blocks until it is
  // actually done touching m_NewShaders before releasing/nulling it --
  // unlocked, this raced the same detached thread's writes into the exact
  // fields being cleared here.
  {
    std::lock_guard<std::mutex> lock(m_presetLoadMutex);
    m_NewShaders.warp.Clear();
    m_NewShaders.comp.Clear();
    m_NewShaders.bufferA.Clear();
    m_NewShaders.bufferB.Clear();
    m_NewShaders.bufferC.Clear();
    m_NewShaders.bufferD.Clear();
  }

  m_nLoadingPreset = 1;
  m_nShaderErrorsThisLoad = 0;  // see ClearPresetShaderError
  m_nTextureMissesThisLoad = 0; // per preset, so a sweep can attribute one (#307)
  m_bPresetLoadReady = false;
  m_bMilk2FrozenBlend = false;  // clear frozen blend from any previous .milk2
  m_fLoadingPresetBlendTime = fBlendTime;
  // Capture-and-reset the auto tag: only the timer path sets it, immediately
  // before calling in. Everything else (IPC, hotkeys, browser) loads untagged.
  m_bLoadingInitiatedByAuto = m_bNextLoadIsAuto;
  m_bNextLoadIsAuto = false;
  // WHY the preset changed, captured the same way and logged below. Without it
  // the log records every change and explains none, so "it changed itself
  // while locked" cannot be told apart from "something sent a next-preset" --
  // which is exactly the question a live report asked on 2026-09-07 and the
  // log could not answer. Untagged loads read as "unknown", which is itself
  // informative: it means a path nobody has labelled yet.
  const char* reason = m_pszNextLoadReason ? m_pszNextLoadReason : "unknown";
  m_pszNextLoadReason = nullptr;
  CopyTo(m_szLoadingPreset, szPresetFilename);
  m_fLoadStartTime = GetTime();
  NumTotalPresetsLoaded++;

  // Detect preset type for routing — match .milk* by finding last '.' and checking prefix
  bool bIsMilk3 = lastDot && _wcsicmp(lastDot, L".milk3") == 0;
  bool bIsMilk2 = lastDot && _wcsicmp(lastDot, L".milk2") == 0;

  {
    const wchar_t* name = wcsrchr(szPresetFilename, L'\\');
    if (!name) name = wcsrchr(szPresetFilename, L'/');
    name = name ? name + 1 : szPresetFilename;
    DebugLogWFmt(LOG_WARN,
                 L"LoadPreset: %s why=%hs milk3=%d milk2=%d blend=%.2f path=%s",
                 name, reason, bIsMilk3 ? 1 : 0, bIsMilk2 ? 1 : 0, fBlendTime,
                 szPresetFilename);
    char line[1024];
    FormatToA(line, "LoadPreset name=%ls milk3=%d milk2=%d blend=%.2f\n",
              name, bIsMilk3 ? 1 : 0, bIsMilk2 ? 1 : 0, fBlendTime);
    DebugLogDiagAppend(L"diag_preset_load.txt", line);
  }

  if (bIsMilk3) {
    // .milk3 Shadertoy preset: parse JSON on main thread (fast), compile async
    m_bLoadingShadertoyMode = true;
    LoadMilk3Preset(m_szLoadingResolved, fBlendTime);
    return;
  }

  if (bIsMilk2) {
    // .milk2 double-preset: parse on calling thread, compile async
    m_bLoadingShadertoyMode = false;
    m_bLoadingMilk2 = true;

    // Parse .milk2 structure (fast — just file I/O)
    int mixType = -1;
    float progress = 0.5f;
    int direction = 1;
    if (!ParseMilk2File(m_szLoadingResolved, m_szMilk2Temp1, m_szMilk2Temp2,
                        mixType, progress, direction)) {
      DLOG_ERROR("LoadPreset: failed to parse .milk2 %ls", szPresetFilename);
      m_nLoadingPreset = 0;
      m_bLoadingMilk2 = false;
      OnPresetLoadFailed(szPresetFilename, L"not a readable .milk2");
      return;
    }
    m_nMilk2MixType = mixType;
    // blending_direction is wipe orientation (cercle in/out), not a
    // progress flip. Inverting 0.31→0.69 made Mandala2 almost all
    // preset2 (static 3D floor) instead of preset1 currents + sprite trail.
    m_fMilk2FrozenProgress = progress;

    // Capture the split bodies IN MEMORY for the mirror-sim bundle before the
    // loader thread (which deletes the temps) can start. Mirror adoption
    // re-imports from these strings via context-private transient temps.
    {
      auto slurp = [](const wchar_t* p, std::string& out) {
        out.clear();
        FILE* tf = _wfopen(p, L"rb");
        if (!tf) return;
        fseek(tf, 0, SEEK_END);
        long sz = ftell(tf);
        fseek(tf, 0, SEEK_SET);
        if (sz > 0) {
          out.resize((size_t)sz);
          size_t got = fread(&out[0], 1, (size_t)sz, tf);
          out.resize(got);
        }
        fclose(tf);
      };
      slurp(m_szMilk2Temp1, m_pendingMilk2Body1);
      slurp(m_szMilk2Temp2, m_pendingMilk2Body2);
    }

    float loadTime = GetTime();
    uint64_t myGeneration = ++m_nLoadGeneration;
    m_presetLoadThread = std::thread([this, loadTime, myGeneration, loadingName]() {
    // #184: the compile scope must be opened ON THIS THREAD.
    //
    // It is thread_local, and the first attempt opened it in LoadPreset's
    // body -- on the thread that STARTS the load, not the one that compiles.
    // The setting was therefore invisible here and every flag still went to
    // m_szCurrentPresetFile, which during a load still names the outgoing
    // preset. test_error_attribution.py caught that; inspection had not.
    //
    // Captured by value: m_szLoadingPreset is a single Engine buffer that a
    // newer load overwrites, and this thread can outlive the load that
    // started it -- LoadPreset detaches after 2000ms.
    // Set directly rather than with ShaderCompileScope: these lambdas
    // contain __try, and MSVC refuses an object needing unwinding in the
    // same function (C2712). Nothing to restore anyway -- the thread ends
    // here, and the flag is thread_local, so it dies with it.
    t_shaderCompilePreset = loadingName.c_str();
      m_presetLoadMutex.lock();  // shared m_pNewState / m_NewShaders across load threads
      __try {
        // Import preset 1 (blend-from) into m_pMilk2OldState
        m_pMilk2OldState->Import(m_szMilk2Temp1, loadTime, nullptr, STATE_ALL);
        // Import preset 2 (blend-to) into m_pNewState
        m_pNewState->Import(m_szMilk2Temp2, loadTime, m_pMilk2OldState, STATE_ALL);

        // Compile shaders for both presets
        m_Milk2OldShaders.warp.Clear();
        m_Milk2OldShaders.comp.Clear();
        m_Milk2OldShaders.bufferA.Clear();
        m_Milk2OldShaders.bufferB.Clear();
        m_Milk2OldShaders.bufferC.Clear();
        m_Milk2OldShaders.bufferD.Clear();
        LoadShaders(&m_Milk2OldShaders, m_pMilk2OldState, false, false);
        LoadShaders(&m_NewShaders, m_pNewState, false, false);

        // MD3 menu writes ZOOM 0–100 into fVideoEchoZoom (classic range is ~1–2).
        // 100 → 2.0 (classic default echo). Leave already-classic values alone.
        auto remapMd3EchoZoom = [](CState* s) {
          if (!s) return;
          float e = s->m_fVideoEchoZoom.eval(-1);
          if (e >= 8.0f)
            s->m_fVideoEchoZoom = 1.0f + e * 0.01f;
        };
        remapMd3EchoZoom(m_pMilk2OldState);
        remapMd3EchoZoom(m_pNewState);

        // MD3 locks shader rand_preset to the .milk2 header (not a new FRAND
        // per Import). Mandala2 uses rand_preset.y as `blue` — a random 0.8
        // turns the grade pink/purple and blows the brightness.
        if (m_bMilk2HasRandoms) {
          D3DXVECTOR4 rp(m_fMilk2Random[0], m_fMilk2Random[1],
                         m_fMilk2Random[2], m_fMilk2Random[3]);
          m_pMilk2OldState->m_rand_preset = rp;
          m_pNewState->m_rand_preset = rp;
        }

        if (m_nLoadGeneration.load() == myGeneration)
          m_bPresetLoadReady.store(true);
      } __except (WriteSEHCrashDiag(GetExceptionInformation(), m_szLoadingPreset)) {
        DLOG_ERROR("LoadPreset: CRASH during async .milk2 import/compile of %ls (code 0x%08X)",
                   m_szLoadingPreset, GetExceptionCode());
      }
      m_presetLoadMutex.unlock();
      // Clean up temp files regardless of success/failure
      DeleteFileW(m_szMilk2Temp1);
      DeleteFileW(m_szMilk2Temp2);
    });
    return;
  }

  // .milk preset: async Import + compile
  m_bLoadingShadertoyMode = false;
  // MD3 3.25+ also embeds [SPRITEn] in classic .milk files (butterfly wings, etc.)
  LoadEmbeddedSpritesFromFile(m_szLoadingResolved);

  // if no preset was valid before, make sure there is no blend, because there is nothing valid to blend from.
  if (!wcscmp(m_pState->m_szDesc, INVALID_PRESET_DESC))
    m_fLoadingPresetBlendTime = 0;

  float loadTime = GetTime();
  DWORD ApplyFlags = STATE_ALL;
  ApplyFlags ^= (m_bWarpShaderLock ? STATE_WARP : 0);
  ApplyFlags ^= (m_bCompShaderLock ? STATE_COMP : 0);

  // forgejo#10: a shader lock must inherit from the state ON SCREEN right
  // now, not from whatever m_pOldState happens to hold -- which, by the time
  // this runs, is two loads back, not one (OnFinishedLoadingPreset rotates
  // m_pState into m_pOldState only once THIS load finishes). The fix cannot
  // just be "pass m_pState instead" though: Import below runs on
  // m_presetLoadThread, a background thread, and m_pState is live on the
  // RENDER thread -- reading it from there with no synchronization would
  // trade a stale-shader bug for a real data race.
  //
  // So the correct state is captured HERE, synchronously, while this is
  // still running on the render thread (LoadPreset is only ever reached via
  // RenderCmd::LoadPreset or an equivalent render-thread call), into
  // m_pOldState -- which is otherwise idle at this exact moment: its own
  // last rotation already happened, and its next one will not happen until
  // this load completes. CopySettingsFrom is the same value-only copy
  // Import's own inheritance step already uses, so m_pOldState keeps its
  // own owned resources (EEL VMs, compiled code) rather than borrowing
  // m_pState's.
  m_pOldState->CopySettingsFrom(*m_pState);

  uint64_t myGeneration = ++m_nLoadGeneration;
  m_presetLoadThread = std::thread([this, loadTime, ApplyFlags, myGeneration, loadingName]() {
    // #184: the compile scope must be opened ON THIS THREAD.
    //
    // It is thread_local, and the first attempt opened it in LoadPreset's
    // body -- on the thread that STARTS the load, not the one that compiles.
    // The setting was therefore invisible here and every flag still went to
    // m_szCurrentPresetFile, which during a load still names the outgoing
    // preset. test_error_attribution.py caught that; inspection had not.
    //
    // Captured by value: m_szLoadingPreset is a single Engine buffer that a
    // newer load overwrites, and this thread can outlive the load that
    // started it -- LoadPreset detaches after 2000ms.
    // Set directly rather than with ShaderCompileScope: these lambdas
    // contain __try, and MSVC refuses an object needing unwinding in the
    // same function (C2712). Nothing to restore anyway -- the thread ends
    // here, and the flag is thread_local, so it dies with it.
    t_shaderCompilePreset = loadingName.c_str();
    m_presetLoadMutex.lock();  // shared m_pNewState / m_NewShaders across load threads
    __try {
      // Import preset (parses .milk file, compiles NSEEL expressions)
      m_pNewState->Import(m_szLoadingResolved, loadTime, m_pOldState, ApplyFlags);
      // Decide whether a tag rule substitutes a shader for this preset. Must
      // happen before the compile below, and reads the hash Import just set.
      ResolveShaderOverrideForPreset(m_pNewState);
      ResolveAudioProfileForPreset(m_pNewState);
      // Compile both warp + comp pixel shaders (D3DCompile — the expensive part)
      LoadShaders(&m_NewShaders, m_pNewState, false, false);
      // Only signal ready if we're still the current generation
      // (a newer load may have started and detached us)
      if (m_nLoadGeneration.load() == myGeneration)
        m_bPresetLoadReady.store(true);
    } __except (WriteSEHCrashDiag(GetExceptionInformation(), m_szLoadingPreset)) {
      DLOG_ERROR("LoadPreset: CRASH during async import/compile of %ls (code 0x%08X)",
                 m_szLoadingPreset, GetExceptionCode());
    }
    m_presetLoadMutex.unlock();
  });
}

void Engine::OnFinishedLoadingPreset() {
  // note: only used this if you loaded the preset *intact* (or mostly intact)

  m_bAnyPresetEverShown = true;

  // Forget the previous preset's measured cost, then re-decide (#230).
  //
  // The decision is no longer made here on the shader text -- see
  // PresetRisksTheDevice, which reads gpuUs -- so this call's job is to bring
  // displays BACK from their own processes when a cheap preset arrives. With
  // the peak cleared there is no measurement yet, the predicate is false, and
  // any surfaces isolated for the previous preset are released. If this one is
  // also expensive, the first mirror frames say so within a frame or two and
  // the periodic re-check isolates again.
  m_nMirrorPeakGpuUs.store(0, std::memory_order_relaxed);
  // The rate balancer's verdict belongs to the preset that earned it, so a new
  // preset starts from scratch (#261). In particular a preset marked "capping
  // the main did not help" must not carry that finding onto the next one.
  ResetFrameRateBalancer();
  // Rungs 2 and 3 latch for the life of a preset -- a recovered rate is
  // evidence the rung is working, not permission to undo it -- so the preset
  // change is the ONLY thing that gives the canvas and the display tiers back.
  ResetDistressLadder();

  // Clear stale notifications from previous preset (issue #17)
  ClearErrors(ERR_NOTIFY);

  // Clamp unreasonably low gamma to avoid black-screen presets
  if (m_pState->m_fGammaAdj.eval(-1) < 0.5f)
    m_pState->m_fGammaAdj = 1.0f;

  SetMenusForPresetVersion(m_pState->m_nWarpPSVersion, m_pState->m_nCompPSVersion);
  m_nPresetsLoadedTotal++; //only increment this on COMPLETION of the load.

  // GPU Protection: warn about heavy presets
  {
    int totalInstances = 0;
    for (int i = 0; i < MAX_CUSTOM_SHAPES; i++) {
      if (m_pState->m_shape[i].enabled)
        totalInstances += m_pState->m_shape[i].instances;
    }
    if (totalInstances > 512) {
      const wchar_t* name = CurrentPresetLeaf();
      DLOG_INFO("GPU Warning: Preset has %d total shape instances (preset: %ls, res: %dx%d)",
              totalInstances, name, m_nTexSizeX, m_nTexSizeY);
    }
  }

  for (int mash = 0; mash < MASH_SLOTS; mash++)
    m_nMashPreset[mash] = m_nCurrentPreset;

  // forgejo#60: this used to hardcode "MDropDX12" here, stomping whatever
  // CreateWindowAndRun actually gave the window -- the user's WindowTitle
  // setting, the instance-numbering suffix, the harness's [TEST] tag -- the
  // first time a preset finished loading after startup. The comment's reason
  // ("IPC window handles Milkwave Remote discovery") is stale: e43dcff
  // replaced title-based discovery with Named Pipes, and nothing left in this
  // tree or in private/tools depends on the render window's title being the
  // literal string "MDropDX12". Restoring the ACTUAL title rather than
  // deleting the call outright, in case something still expects the caption
  // to be reasserted at this point.
  {
    HWND hPlugin = GetPluginWindow();
    if (hPlugin && m_szActualWindowTitle[0])
      SetWindowTextW(hPlugin, m_szActualWindowTitle);
  }

  SendPresetChangedInfoToMDropDX12Remote();

  // Notify Settings window so its preset listbox stays in sync
  if (m_settingsWindow && m_settingsWindow->IsOpen())
    PostMessage(m_settingsWindow->GetHWND(), WM_MW_PRESET_CHANGED, 0, 0);

  // Auto-refresh resource viewer if open
  if (m_hResourceWnd && IsWindow(m_hResourceWnd) && IsWindowVisible(m_hResourceWnd))
    PostMessage(m_hResourceWnd, WM_COMMAND, MAKEWPARAM(IDC_RV_REFRESH, BN_CLICKED), 0);

  // Preset name display on render (suppressed when ShowNotifications=0)
  if (m_nPresetNameAnimProfile != -1 && m_bShowNotifications) {
    // Extract preset filename without path/extension
    const wchar_t* name = CurrentPresetLeaf();
    wchar_t szName[512];
    lstrcpynW(szName, name, 512);
    wchar_t* dot = wcsrchr(szName, L'.');
    if (dot) *dot = L'\0';

    if (m_nPresetNameAnimProfile == -2 || m_nPresetNameAnimProfile >= 0) {
      // Use animation profile
      int profIdx = m_nPresetNameAnimProfile;
      if (profIdx == -2) profIdx = PickRandomAnimProfile();
      if (profIdx >= 0 && profIdx < m_nAnimProfileCount) {
        int slot = GetNextFreeSupertextIndex();
        CopyTo(m_supertexts[slot].szTextW, szName);
        m_supertexts[slot].bRedrawSuperText = true;
        m_supertexts[slot].bIsSongTitle = false;
        ApplyAnimProfileToSupertext(m_supertexts[slot], m_AnimProfiles[profIdx]);
        m_supertexts[slot].fStartTime = GetTime();
      }
    } else {
      // Simple HUD display — fixed font size, no texture scaling
      lstrcpynW(m_szPresetNameDisplay, szName, 512);
      m_fPresetNameShowUntil = GetTime() + 3.5f;
    }
  }
}
// ─── IPC via Named Pipe ────────────────────────────────────────────────────
// Outgoing messages are sent through g_pipeServer (pipe_server.h).
// The old WM_COPYDATA worker thread has been removed.

int Engine::SendMessageToMDropDX12Remote(const wchar_t* messageToSend) {
  return SendMessageToMDropDX12Remote(messageToSend, false);
}

int Engine::SendMessageToMDropDX12Remote(const wchar_t* messageToSend, bool doForce) {
  using namespace std::chrono;
  try {
    if (!messageToSend || !*messageToSend)
      return 0;

    // Throttle: skip if sent too recently (unless forced)
    auto now = steady_clock::now();
    if (!doForce && duration_cast<milliseconds>(now - LastSentMDropDX12Message).count() < 100)
      return 0;
    LastSentMDropDX12Message = now;

    extern PipeServer g_pipeServer;
    // PipeServer::Send drives the TCP broadcast itself now (forgejo#33), so
    // this must NOT also call g_tcpServer.Broadcast -- a TCP client would get
    // every one of these twice.
    g_pipeServer.Send(messageToSend);
  } catch (...) {
    // ignore
  }
  return 1;
}

void Engine::PostMessageToMDropDX12Remote(UINT msg) {
  try {
    extern PipeServer g_pipeServer;
    // Map WM_USER+N constants to SIGNAL| pipe messages
    const wchar_t* signal = nullptr;
    if (msg == WM_USER + 100) signal = L"SIGNAL|NEXT_PRESET";
    else if (msg == WM_USER + 101) signal = L"SIGNAL|PREV_PRESET";
    else if (msg == WM_USER + 102) signal = L"SIGNAL|COVER_CHANGED";
    else if (msg == WM_USER + 103) signal = L"SIGNAL|SPRITE_MODE";
    else if (msg == WM_USER + 104) signal = L"SIGNAL|MESSAGE_MODE";
    if (signal) {
      // Send() reaches TCP on its own now; broadcasting here as well would
      // duplicate it (forgejo#33).
      g_pipeServer.Send(signal);
    }
  } catch (...) {
    // ignore
  }
}

void Engine::LoadPresetTick() {
  if (m_nLoadingPreset <= 0)
    return;

  if (m_bPresetLoadReady.load()) {
    // Background thread finished — join it and apply the preset
    if (m_presetLoadThread.joinable())
      m_presetLoadThread.join();
    m_bPresetLoadReady = false;

    // Nothing below may run while a mirror list is still on the GPU (#206).
    //
    // Committing a preset releases the outgoing preset's textures and swaps
    // its shaders and PSOs. A mirror worker records against those, submits to
    // m_mirrorQueue and releases the engine lock -- so the lock says the
    // record is over while the GPU has not started it. texmgr::KillTex is the
    // sharpest edge: it calls ResetResource() on the texture AND overwrites
    // that slot's descriptor in the LIVE shader-visible heap, which is why the
    // fault arrives as a page fault at VA 0 rather than at a freed address.
    //
    // Measured, not assumed. Two independent mirrors with a preset load every
    // 1.2s died in 5 of 6 ninety-second soaks; the same churn with NO mirrors
    // passed, and the same mirrors with NO churn passed. It is the pair.
    //
    // Once per committed load, so it costs nothing per frame.
    //
    // WaitForGpu, NOT WaitForSurfaceWork (#214). The mirror wait alone was
    // half the answer: it drains the MIRROR queues and says nothing about the
    // main one, so with no mirrors configured it waited for nothing at all --
    // and then the commit below rotates the shader sets, replaces the preset
    // PSOs and releases the outgoing preset's textures while the main queue
    // still has a frame or two in flight naming them.
    //
    // Measured: churning presets at an uncapped frame rate killed the device
    // in 2 of 4 ninety-second runs with no mirrors anywhere near it, faulting
    // on the comp mesh draw of a blending frame at page fault VA 0 -- the same
    // nulled-underneath signature as #222 and #225. With this it is 0 of 8.
    //
    // WaitForGpu is a strict superset: it runs the aux waiter (which IS
    // WaitForSurfaceWork) before waiting on the main queue, so nothing the
    // original call covered is lost.
    if (m_lpDX)
      m_lpDX->WaitForGpu();
    else
      WaitForSurfaceWork();

    // An AUTO-initiated load races the freeze: the timer can start compiling,
    // testing mode (or preset lock) lands, and the finished load would still
    // apply and change the frame under whoever asked for it to hold still.
    // Discard it instead. Explicit loads are never tagged and always apply.
    if (m_bLoadingInitiatedByAuto && !AutoPresetChangesAllowed()) {
      char line[768];
      FormatToA(line, "DISCARD auto load (freeze arrived mid-compile) was=%ls\n",
                m_szLoadingPreset[0] ? m_szLoadingPreset : L"(unknown)");
      DebugLogDiagAppend(L"diag_preset_load.txt", line);
      DLOG_INFO("LoadPresetTick: discarding auto-initiated load of %ls -- "
                "auto changes were frozen while it compiled", m_szLoadingPreset);
      m_NewShaders.warp.Clear();
      m_NewShaders.comp.Clear();
      m_NewShaders.bufferA.Clear();
      m_NewShaders.bufferB.Clear();
      m_NewShaders.bufferC.Clear();
      m_NewShaders.bufferD.Clear();
      m_szLoadingPreset[0] = 0;
      m_nLoadingPreset = 0;
      m_bLoadingInitiatedByAuto = false;
      m_pendingMilk2Body1.clear();
      m_pendingMilk2Body2.clear();
      ClearErrors(ERR_NOTIFY);
      return;
    }

    // Clear the "Compiling..." notification
    ClearErrors(ERR_NOTIFY);

    // Apply the preset: swap state pointers
    CopyTo(m_szCurrentPresetFile, m_szLoadingPreset);
    // Only when they differ, so "is a replacement showing" stays one test.
    if (_wcsicmp(m_szLoadingResolved, m_szLoadingPreset) != 0)
      lstrcpynW(m_szRenderedPresetFile, m_szLoadingResolved, MAX_PATH);
    else
      m_szRenderedPresetFile[0] = 0;
    m_szLoadingPreset[0] = 0;

    // Start timing this preset for usage stats.  The path is stored whole so
    // presets.json can record where every copy of a preset lives; when the
    // preset came from the browser it is a bare name, so the preset dir is
    // prepended here rather than losing the location.
    {
      wchar_t usagePath[512];
      if (wcschr(m_szCurrentPresetFile, L'\\') || wcschr(m_szCurrentPresetFile, L'/'))
        lstrcpynW(usagePath, m_szCurrentPresetFile, 512);
      else
        FormatTo(usagePath, L"%s%s", m_szPresetDir, m_szCurrentPresetFile);
      BeginPresetUsage(usagePath);
    }

    // Defer startup preset save — only persist after 5s of render time (avoids
    // hammering INI when user cycles presets quickly)
    if (m_bEnablePresetStartupSavingOnClose) {
      lstrcpynW(m_szPendingStartupSave, m_szCurrentPresetFile, 512);
      m_fPendingStartupSaveTime = GetTime();
    }

    // A preset actually reached the screen, so the run of failures (if any) is
    // over. Reset here rather than in LoadPreset: starting a load proves
    // nothing, and clearing the count on every attempt would let an endless
    // alternation of one good and one bad file skip forever.
    m_nConsecutiveLoadFailures = 0;

    // Log which preset is now actively rendering (visible at LogLevel=2)
    // The new preset's own frame-rate ceiling, resolved once here rather than
    // on the frame path (#222).
    ResolvePresetFpsCap();
    {
      const wchar_t* name = CurrentPresetLeaf();
      float elapsed = GetTime() - m_fLoadStartTime;
      const bool hasA = (m_NewShaders.bufferA.bytecodeBlob != NULL);
      const bool hasComp = (m_NewShaders.comp.bytecodeBlob != NULL);
      DebugLogWFmt(LOG_WARN,
                   L"Preset APPLIED: %s (%.0f ms) shadertoy=%d bufferA=%d image=%d desc=%s",
                   name, elapsed * 1000.0f,
                   m_bLoadingShadertoyMode ? 1 : 0,
                   hasA ? 1 : 0, hasComp ? 1 : 0,
                   m_pNewState ? m_pNewState->m_szDesc : L"?");
      char line[768];
      FormatToA(line,
                "APPLIED %ls ms=%.0f shadertoy=%d bufferA=%d image=%d desc=%ls\n",
                name, elapsed * 1000.0f,
                m_bLoadingShadertoyMode ? 1 : 0,
                hasA ? 1 : 0, hasComp ? 1 : 0,
                m_pNewState ? m_pNewState->m_szDesc : L"?");
      DebugLogDiagAppend(L"diag_preset_load.txt", line);
    }

    // Every shader in this preset built without one compile error, so an error
    // recorded against it belongs to a build that no longer exists.  Retracting
    // it here is what stops presets.json accumulating permanent verdicts on
    // presets the shader preprocessor has since learned to fix.
    if (m_nShaderErrorsThisLoad.load() == 0)
      ClearPresetShaderError(m_szCurrentPresetFile);

    CState* temp = m_pState;
    m_pState = m_pOldState;
    m_pOldState = temp;

    // A per-preset canvasMax only takes effect when the canvas is rebuilt, and
    // loading a preset does not resize anything on its own -- so without this a
    // stored limit was silently ignored until the next window resize.
    // Enqueued rather than done inline: this runs mid-apply, and the queue
    // executes it at the top of the next frame.
    // The INCOMING preset's hash: m_pState is still the outgoing one here.
    if (EffectiveCanvasLimit(m_pNewState ? m_pNewState->m_szPresetHash : nullptr)
        != m_nCanvasLimitApplied)
      EnqueueRenderCmd(RenderCmd::ReallocCanvas);

    // The damp is resolved from the same annotation and must follow the same
    // preset change -- and from the INCOMING hash for the same reason.
    RefreshCurrentDampStrength(m_pNewState ? m_pNewState->m_szPresetHash : nullptr);


    temp = m_pState;
    m_pState = m_pNewState;
    m_pNewState = temp;

    // .milk2: swap in preset 1 as the blend-from state
    if (m_bLoadingMilk2) {
      // m_pOldState currently has the previously-rendering state (stale).
      // Swap it with m_pMilk2OldState which has preset 1 from the .milk2 file.
      temp = m_pOldState;
      m_pOldState = m_pMilk2OldState;
      m_pMilk2OldState = temp;  // recycled — will be reused on next milk2 load

      // Fix descriptions: Import() derived m_szDesc from temp file paths.
      // Override with the .milk2 filename (without path or extension).
      {
        const wchar_t* p = CurrentPresetLeaf();
        wcsncpy_s(m_pState->m_szDesc, p, MAX_PATH - 1);
        wchar_t* dot = wcsrchr(m_pState->m_szDesc, L'.');
        if (dot) *dot = L'\0';
        CopyTo(m_pOldState->m_szDesc, m_pState->m_szDesc);
      }
    }

    // Apply blend or hard-cut based on the requested blend time
    if (m_bLoadingMilk2) {
      // .milk2: frozen blend — both presets render simultaneously at fixed progress
      ApplyMilk2BlendPattern();
      m_pState->StartBlendFrom(m_pOldState, GetTime(), 1.0f); // duration doesn't matter — we freeze it
      m_pState->m_fBlendProgress = m_fMilk2FrozenProgress;
      m_bMilk2FrozenBlend = true;
    } else if (m_fLoadingPresetBlendTime >= 0.001f) {
      RandomizeBlendPattern();
      m_pState->StartBlendFrom(m_pOldState, GetTime(), m_fLoadingPresetBlendTime);
    } else {
      // Hard cut — StartBlendFrom copies needed state values (old wave mode, etc.)
      // then we immediately disable blending.
      m_pState->StartBlendFrom(m_pOldState, GetTime(), 0);
      m_pState->m_bBlending = false;
    }

    m_fPresetStartTime = GetTime();
    m_bPresetDiagLogged = false;
    m_fNextPresetTime = -1.0f;		// flags UpdateTime() to recompute this

    // Activate or deactivate Shadertoy mode based on what was loaded
    if (m_bLoadingShadertoyMode) {
      m_bShadertoyMode = true;
      m_nShadertoyStartFrame = GetFrame();
    } else {
      m_bShadertoyMode = false;
    }

    // Hand the live set to m_OldShaders (so blending still has something to
    // read from), then hand the freshly compiled set to m_shaders.
    // RotatePShaderSet covers all six members unconditionally (forgejo#5) --
    // this used to Clear m_OldShaders' six by hand but then only null out
    // m_Milk2OldShaders'/m_NewShaders' three COM pointers per member with no
    // helper tying the two counts together, which is exactly the shape that
    // stopped covering bufferA-D correctly the moment they were added
    // elsewhere in this same rotation (see ClearPreset/ApplyPresetTextToState).
    if (m_bLoadingMilk2) {
      // .milk2: use preset 1's shaders as old, preset 2's as new
      RotatePShaderSet(m_OldShaders, m_Milk2OldShaders);
    } else {
      RotatePShaderSet(m_OldShaders, m_shaders);
    }
    RotatePShaderSet(m_shaders, m_NewShaders);


    // Derive buffer/feedback flags from the newly swapped shaders.
    // These flags must ONLY change on the render thread (here) — never on the
    // background compilation thread, which would race with mid-frame rendering.
    m_bHasBufferA = (m_shaders.bufferA.bytecodeBlob != NULL);
    m_bHasBufferB = (m_shaders.bufferB.bytecodeBlob != NULL);
    m_bHasBufferC = (m_shaders.bufferC.bytecodeBlob != NULL);
    m_bHasBufferD = (m_shaders.bufferD.bytecodeBlob != NULL);
    m_bCompUsesFeedback = m_bHasBufferA;  // Buffer A always implies feedback
    m_bCompUsesImageFeedback = false;
    for (int i = 0; i < 16; i++) {
      if (m_shaders.comp.params.m_texcode[i] == TEX_FEEDBACK)
        m_bCompUsesFeedback = true;
      if (m_shaders.comp.params.m_texcode[i] == TEX_IMAGE_FEEDBACK)
        m_bCompUsesImageFeedback = true;
    }

    // The Shadertoy feedback targets are allocated HERE, not at device init,
    // and only for the buffers this preset actually declares (#344).
    //
    // They used to be ten unconditional R32G32B32A32_FLOAT targets at full
    // render resolution -- 160 bytes a pixel, about 62% of the primary's entire
    // VRAM footprint, ~1.33 GB at 2160x3840 -- created whether or not a .milk3
    // was ever loaded. Most sessions never load one.
    //
    // This runs after the flags above precisely because it reads them, and on
    // the render thread for the same reason they are only written here.
    // Idempotent, so a preset that needs the same set as the last one allocates
    // nothing.
    if (m_bShadertoyMode) {
      m_shadertoyIdleSince = 0.0;       // in use again; cancel any pending release
      EnsureShadertoyFeedbackTargets();
    } else if (ShadertoyFeedbackAllocated() && m_shadertoyIdleSince == 0.0) {
      // Start the clock rather than releasing now: stepping through a mixed
      // library would otherwise rebuild every device resource on each .milk3 ->
      // .milk transition, and the rebuild is not cheap.
      m_shadertoyIdleSince = GetTime();
    }

    // MD3 3.25+ [SPRITE*] overlays ride with .milk and .milk2. Kill the previous
    // file's sprites on every apply; launch the new ones when the file has blocks.
    if (!m_milk2Sprites.empty() && SpritesEnabled()) {
      // RECORDING, not merely non-null. The old test was
      // `m_lpDX && m_lpDX->m_commandList`, which is true from InitD3d onwards
      // and says nothing about whether the list is open -- and this function
      // is reached from CleanUpMyDX9Stuff as well as from the render loop, so
      // it runs during startup, a window resize and device recovery, all of
      // which are outside any BeginFrame/EndFrame pair.
      //
      // The sprite upload was therefore recorded into a CLOSED list and
      // dropped on the floor: the sprites did not appear, and the only trace
      // was four unattributed "cannot be called on a closed command list"
      // errors from the debug layer, on every launch (forgejo#16).
      //
      // The deferral this now reaches was already here and already correct --
      // m_bMilk2SpritesNeedApply replays it on the next frame. Nothing but the
      // question being asked was wrong.
      if (m_lpDX && m_lpDX->IsCommandListRecording())
        ApplyMilk2Sprites();
      else
        m_bMilk2SpritesNeedApply = true;
    } else {
      KillMilk2Sprites();
      m_milk2Sprites.clear();
      m_bMilk2SpritesNeedApply = false;
    }


    // Publish the mirror-sim preset bundle (classic presets only — the milk3
    // orient path renders from engine buffers and has no per-context EEL).
    if (!m_bShadertoyMode)
      PublishPresetBundle(m_bLoadingMilk2, m_fLoadingPresetBlendTime);

    m_pendingMilk2Body1.clear();
    m_pendingMilk2Body2.clear();

    // end loading mode
    m_nLoadingPreset = 0;
    m_bLoadingMilk2 = false;

    // Defer PSO creation to next frame's render pass — releasing old PSOs here
    // would destroy them while the current frame's command list still references them.
    m_bDX12PSOsDirty = true;
    OnFinishedLoadingPreset();

    return;
  }

  // Compilation still in progress — show feedback for slow compilations
  float elapsed = GetTime() - m_fLoadStartTime;
  if (elapsed > 1.0f && m_nLoadingPreset == 1) {
    // Upgrade to "compiling" state so we only show this once
    m_nLoadingPreset = 2;
    wchar_t buf[256];
    const wchar_t* name = wcsrchr(m_szLoadingPreset, L'\\');
    if (!name) name = wcsrchr(m_szLoadingPreset, L'/');
    name = name ? name + 1 : m_szLoadingPreset;
    swprintf(buf, 256, L"Compiling shader: %.80s", name);
    AddError(buf, 30.0f, ERR_NOTIFY, true);
  }

  // Check for timeout
  if (elapsed > m_fShaderCompileTimeout) {
    DebugLogWFmt(LOG_WARN, L"Preset load: TIMEOUT %.1fs loading %s", elapsed, m_szLoadingPreset);
    {
      char line[768];
      FormatToA(line, "TIMEOUT %.1fs loading=%ls\n", elapsed, m_szLoadingPreset);
      DebugLogDiagAppend(L"diag_preset_load.txt", line);
    }

    // Abandon the stuck thread
    if (m_presetLoadThread.joinable())
      m_presetLoadThread.detach();

    m_nLoadingPreset = 0;
    m_bPresetLoadReady = false;
    m_NewShaders.warp.Clear();
    m_NewShaders.comp.Clear();
    if (m_bLoadingMilk2) {
      m_Milk2OldShaders.warp.Clear();
      m_Milk2OldShaders.comp.Clear();
      m_Milk2OldShaders.bufferA.Clear();
      m_Milk2OldShaders.bufferB.Clear();
      m_bLoadingMilk2 = false;
    }

    ClearErrors(ERR_NOTIFY);
    AddError(L"Shader compile timed out \u2014 skipping preset", m_ErrorDuration, ERR_NOTIFY, true);

    // Try next preset (will start a new async load)
    NextPreset(m_fLoadingPresetBlendTime);
  }
}

bool Engine::WaitForPendingLoad(DWORD timeoutMs) {
  // Wait for the current async load to complete and apply it.
  // Used by sequential-load hotkeys (!, @, A) that need one load to finish
  // before starting the next. Returns true if the load completed, false on timeout.
  if (m_nLoadingPreset <= 0)
    return true; // nothing pending

  if (!m_presetLoadThread.joinable())
    return false;

  HANDLE h = (HANDLE)m_presetLoadThread.native_handle();
  DWORD wait = WaitForSingleObject(h, timeoutMs);
  if (wait == WAIT_TIMEOUT)
    return false; // still compiling — caller should skip subsequent loads

  // Thread finished — apply it
  m_presetLoadThread.join();
  m_bPresetLoadReady = true; // ensure LoadPresetTick sees it as ready
  LoadPresetTick();
  return true;
}

void Engine::SeekToPreset(wchar_t cStartChar) {
  if (cStartChar >= L'a' && cStartChar <= L'z')
    cStartChar -= L'a' - L'A';

  for (int i = m_nDirs; i < m_nPresets; i++) {
    wchar_t ch = PresetNameAt(i).c_str()[0];
    if (ch >= L'a' && ch <= L'z')
      ch -= L'a' - L'A';
    if (ch == cStartChar) {
      m_nPresetListCurPos = i;
      return;
    }
  }
}

void Engine::FindValidPresetDir() {
  FormatTo(m_szPresetDir, L"%spresets\\", m_szMilkdrop2Path);
  if (GetFileAttributesW(m_szPresetDir) != -1) {
    TryDescendIntoPresetSubdirHelper(m_szPresetDir);
    return;
  }
  CopyTo(m_szPresetDir, m_szMilkdrop2Path);
  if (GetFileAttributesW(m_szPresetDir) != -1)
    return;
  CopyTo(m_szPresetDir, GetPluginsDirPath());
  if (GetFileAttributesW(m_szPresetDir) != -1)
    return;
  // Keep default preset path — do NOT fall back to c:\program files or c:\
  // which would cause extremely long directory scans.
  FormatTo(m_szPresetDir, L"%spresets\\", m_szMilkdrop2Path);
}

char* NextLine(char* p) {
  // p points to the beginning of a line
  // we'll return a pointer to the first char of the next line
  // if we hit a NULL char before that, we'll return NULL.
  if (!p)
    return NULL;

  char* s = p;
  while (*s != '\r' && *s != '\n' && *s != 0)
    s++;

  while (*s == '\r' || *s == '\n')
    s++;

  if (*s == 0)
    return NULL;

  return s;
}

// Recursive directory scanner for building flat preset lists.
// Scans baseDir + relPrefix for .milk/.milk2/.milk3 files and recurses into subdirs.
// Appends results to temp_presets with relative paths (relPrefix + filename).
static void ScanDirRecursive(
    const wchar_t* baseDir,
    const wchar_t* relPrefix,    // e.g., L"" or L"subdir\\"
    int nMaxPSVersion,
    int nPresetFilter,
    PresetList& temp_presets,
    int& temp_nPresets)
{
    wchar_t szMask[MAX_PATH];
    FormatTo(szMask, L"%s%s*.*", baseDir, relPrefix);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(szMask, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (g_bThreadShouldQuit) break;

        bool bIsDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        if (bIsDir) {
            // Skip . and ..
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            // Recurse into subdirectory
            wchar_t newPrefix[MAX_PATH];
            FormatTo(newPrefix, L"%s%s\\", relPrefix, fd.cFileName);
            ScanDirRecursive(baseDir, newPrefix, nMaxPSVersion, nPresetFilter, temp_presets, temp_nPresets);
            continue;
        }

        // Check file extension
        int len = lstrlenW(fd.cFileName);
        bool bIsMilk  = (len >= 5 && _wcsicmp(fd.cFileName + len - 5, L".milk")  == 0);
        bool bIsMilk2 = (len >= 6 && _wcsicmp(fd.cFileName + len - 6, L".milk2") == 0);
        bool bIsMilk3 = (len >= 6 && _wcsicmp(fd.cFileName + len - 6, L".milk3") == 0);
        if (!bIsMilk && !bIsMilk2 && !bIsMilk3) continue;

        // Apply preset filter
        if (nPresetFilter == 1 && !bIsMilk) continue;
        if (nPresetFilter == 2 && !bIsMilk2) continue;
        if (nPresetFilter == 3 && !bIsMilk3) continue;

        // Skip file I/O for rating in recursive mode — use default rating
        float fRating = 3.0f;

        // Build relative filename: relPrefix + filename
        wchar_t szRelFilename[MAX_PATH];
        FormatTo(szRelFilename, L"%s%s", relPrefix, fd.cFileName);

        float fPrevCum = temp_nPresets > 0 ? temp_presets[temp_nPresets - 1].fRatingCum : 0;
        PresetInfo x;
        x.szFilename = szRelFilename;
        x.fRatingThis = fRating;
        x.fRatingCum = fPrevCum + fRating;
        temp_presets.push_back(x);
        temp_nPresets++;
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

// Parameters snapshotted from engine state before thread launch (no CS needed in thread)
struct ScanParams {
  bool bForce;
  bool bTryReselectCurrentPreset;
  int  nMaxPSVersion;
  int  nPresetFilter;
  bool bRecursive;
  wchar_t szPresetDir[MAX_PATH];
  wchar_t szCurrentPresetFile[512];
  wchar_t szUpdatePresetMask[MAX_PATH];  // previous mask for staleness check
  wchar_t szMilkdrop2Path[MAX_PATH];
  wchar_t szPluginsDirPath[MAX_PATH];
};

// Try fallback preset directories locally (no g_engine writes)
template <size_t N>  // deduce the destination size for FormatTo (issue 145)
static bool FindValidPresetDirLocal(wchar_t (&szPresetDir)[N], const wchar_t* szMilkdrop2Path, const wchar_t* szPluginsDirPath) {
  FormatTo(szPresetDir, L"%spresets\\", szMilkdrop2Path);
  if (GetFileAttributesW(szPresetDir) != INVALID_FILE_ATTRIBUTES)
    return true;
  CopyTo(szPresetDir, szMilkdrop2Path);
  if (GetFileAttributesW(szPresetDir) != INVALID_FILE_ATTRIBUTES)
    return true;
  CopyTo(szPresetDir, szPluginsDirPath);
  if (GetFileAttributesW(szPresetDir) != INVALID_FILE_ATTRIBUTES)
    return true;
  // Keep default preset path
  FormatTo(szPresetDir, L"%spresets\\", szMilkdrop2Path);
  return false;
}

static unsigned int WINAPI __UpdatePresetList(void* lpVoid) {
  // NOTE - this is run in a separate thread!!!
  // This thread publishes results via g_csPresetPending + atomic flags.
  // It never touches g_cs. The old preset list stays visible until the render thread swaps in the new one.

  ScanParams* params = (ScanParams*)lpVoid;
  bool bForce = params->bForce;
  bool bTryReselectCurrentPreset = params->bTryReselectCurrentPreset;
  int  nMaxPSVersion = params->nMaxPSVersion;
  int  nPresetFilter = params->nPresetFilter;
  bool bRecursive = params->bRecursive;
  wchar_t szPresetDir[MAX_PATH];
  CopyTo(szPresetDir, params->szPresetDir);
  wchar_t szCurrentPresetFile[512];
  CopyTo(szCurrentPresetFile, params->szCurrentPresetFile);
  wchar_t szMilkdrop2Path[MAX_PATH];
  CopyTo(szMilkdrop2Path, params->szMilkdrop2Path);
  wchar_t szPluginsDirPath[MAX_PATH];
  CopyTo(szPluginsDirPath, params->szPluginsDirPath);

  // Check if rescan is needed (compare mask)
  wchar_t szMask[MAX_PATH];
  FormatTo(szMask, L"%s*.*", szPresetDir);
  bool bNeedRescan = bForce || !params->szUpdatePresetMask[0] || wcscmp(szMask, params->szUpdatePresetMask);

  if (!bNeedRescan) {
    // Already up to date — nothing to do
    delete params;
    g_bThreadAlive = false;
    _endthreadex(0);
    return 0;
  }
  delete params;
  params = nullptr;

  // Update the mask on the engine (no g_cs needed — only main thread reads this)
  CopyTo(g_engine.m_szUpdatePresetMask, szMask);

  // Validate preset directory — try fallbacks locally
  for (int attempt = 0; attempt < 2 && !g_bThreadShouldQuit; attempt++) {
    if (GetFileAttributesW(szPresetDir) != INVALID_FILE_ATTRIBUTES)
      break;
    FindValidPresetDirLocal(szPresetDir, szMilkdrop2Path, szPluginsDirPath);
  }

  // Scan directory
  PresetList temp_presets;
  int temp_nDirs = 0;
  int temp_nPresets = 0;

  if (bRecursive) {
    ScanDirRecursive(szPresetDir, L"", nMaxPSVersion, nPresetFilter, temp_presets, temp_nPresets);
  } else {
    // Non-recursive: single-level scan
    FormatTo(szMask, L"%s*.*", szPresetDir);
    WIN32_FIND_DATAW fd;
    ZeroMemory(&fd, sizeof(fd));
    HANDLE h = FindFirstFileW(szMask, &fd);

    if (h == INVALID_HANDLE_VALUE) {
      // Try fallback directory
      FindValidPresetDirLocal(szPresetDir, szMilkdrop2Path, szPluginsDirPath);
      FormatTo(szMask, L"%s*.*", szPresetDir);
      h = FindFirstFileW(szMask, &fd);
    }

    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (g_bThreadShouldQuit) break;

        bool bSkip = false;
        bool bIsDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        wchar_t szFilename[512];
        CopyTo(szFilename, fd.cFileName);

        if (bIsDir) {
          if (wcscmp(fd.cFileName, L".") == 0)
            bSkip = true;
          else
            FormatTo(szFilename, L"*%s", fd.cFileName);
        } else {
          int len = lstrlenW(fd.cFileName);
          bool bIsMilk  = (len >= 5 && _wcsicmp(fd.cFileName + len - 5, L".milk")  == 0);
          bool bIsMilk2 = (len >= 6 && _wcsicmp(fd.cFileName + len - 6, L".milk2") == 0);
          bool bIsMilk3 = (len >= 6 && _wcsicmp(fd.cFileName + len - 6, L".milk3") == 0);
          if (!bIsMilk && !bIsMilk2 && !bIsMilk3)
            bSkip = true;
          if (!bSkip && nPresetFilter == 1 && !bIsMilk)  bSkip = true;
          if (!bSkip && nPresetFilter == 2 && !bIsMilk2) bSkip = true;
          if (!bSkip && nPresetFilter == 3 && !bIsMilk3) bSkip = true;
        }

        if (!bSkip) {
          PresetInfo x;
          x.szFilename = szFilename;
          x.fRatingThis = 3.0f;
          x.fRatingCum = 0;
          temp_presets.push_back(x);
          temp_nPresets++;
          if (bIsDir) temp_nDirs++;
        }
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
  }

  if (g_bThreadShouldQuit) {
    g_bThreadAlive = false;
    _endthreadex(0);
    return 0;
  }

  if (temp_nPresets == 0) {
    // Publish empty list via pending buffer
    DLOG_INFO("__UpdatePresetList: no presets found in %ls", szPresetDir);
    EnterCriticalSection(&g_csPresetPending);
    g_engine.m_pendingPresets.clear();
    g_engine.m_nPendingPresets = 0;
    g_engine.m_nPendingDirs = 0;
    g_engine.m_nPendingCurPos = 0;
    g_engine.m_bPendingListReady = true;
    g_engine.m_bPendingPresetSwap.store(true, std::memory_order_release);
    LeaveCriticalSection(&g_csPresetPending);
    g_bThreadAlive = false;
    _endthreadex(0);
    return 0;
  }

  // Sort: directories first, then alphabetical
  {
    auto sortCmp = [](const PresetInfo& a, const PresetInfo& b) {
      bool aDir = !a.szFilename.empty() && a.szFilename[0] == L'*';
      bool bDir = !b.szFilename.empty() && b.szFilename[0] == L'*';
      if (aDir != bDir) return aDir;
      return mystrcmpiW(a.szFilename.c_str(), b.szFilename.c_str()) < 0;
    };
    std::sort(temp_presets.begin(), temp_presets.begin() + temp_nPresets, sortCmp);
  }

  // Cumulative ratings
  for (int i = 0; i < temp_nPresets; i++)
    temp_presets[i].fRatingCum = (i == 0) ? temp_presets[i].fRatingThis
                                          : temp_presets[i - 1].fRatingCum + temp_presets[i].fRatingThis;

  // Reselect current preset (uses snapshotted szCurrentPresetFile — no CS needed)
  int newCurPos = 0;
  if (bTryReselectCurrentPreset && szCurrentPresetFile[0]) {
    const wchar_t* pMatch = szCurrentPresetFile;
    int dirLen = lstrlenW(szPresetDir);
    if (dirLen > 0 && _wcsnicmp(pMatch, szPresetDir, dirLen) == 0)
      pMatch += dirLen;
    else {
      const wchar_t* p2 = wcsrchr(pMatch, L'\\');
      pMatch = p2 ? (p2 + 1) : pMatch;
    }
    for (int i = temp_nDirs; i < temp_nPresets; i++) {
      if (wcscmp(pMatch, temp_presets[i].szFilename.c_str()) == 0) {
        newCurPos = i;
        break;
      }
    }
  }

  // Save filenames locally before std::move (needed for pass 2 rating reads)
  std::vector<std::wstring> filenames(temp_nPresets);
  for (int i = 0; i < temp_nPresets; i++)
    filenames[i] = temp_presets[i].szFilename;

  // Publish preset list via pending buffer (render thread picks it up next frame)
  EnterCriticalSection(&g_csPresetPending);
  g_engine.m_pendingPresets = std::move(temp_presets);
  g_engine.m_nPendingPresets = temp_nPresets;
  g_engine.m_nPendingDirs = temp_nDirs;
  g_engine.m_nPendingCurPos = newCurPos;
  g_engine.m_bPendingListReady = true;
  g_engine.m_bPendingPresetSwap.store(true, std::memory_order_release);
  LeaveCriticalSection(&g_csPresetPending);

  // Pass 2: read ratings from preset files in background
  // Uses locally-saved filenames — NO CS held during file reads
  {
    std::vector<float> ratings(temp_nPresets, 3.0f);

    for (int i = temp_nDirs; i < temp_nPresets && !g_bThreadShouldQuit; i++) {
      if (filenames[i].empty() || filenames[i][0] == L'*') continue;

      wchar_t szFullPath[MAX_PATH];
      FormatTo(szFullPath, L"%s%s", szPresetDir, filenames[i].c_str());

      FILE* f = _wfopen(szFullPath, L"r");
      if (!f) continue;

      char szLine[160];
      int bytes_to_read = sizeof(szLine) - 1;
      size_t count = fread(szLine, bytes_to_read, 1, f);
      if (count < 1) {
        fseek(f, SEEK_SET, 0);
        count = fread(szLine, 1, bytes_to_read, f);
        szLine[(int)count] = 0;
      } else {
        szLine[bytes_to_read - 1] = 0;
      }
      fclose(f);

      char* p = szLine;
      if (!strncmp(p, "MILKDROP_PRESET_VERSION", 23))
        p = NextLine(p);
      if (p && !strncmp(p, "PSVERSION", 9))
        p = NextLine(p);
      for (int z = 0; z < 10 && p; z++) {
        if (!strncmp(p, "[preset00]", 10)) {
          p = NextLine(p);
          if (p && !strncmp(p, "fRating=", 8))
            _sscanf_l(&p[8], "%f", g_use_C_locale, &ratings[i]);
          break;
        }
        p = NextLine(p);
      }
      ratings[i] = max(0.0f, min(5.0f, ratings[i]));
    }

    // Publish ratings via pending buffer
    if (!g_bThreadShouldQuit) {
      EnterCriticalSection(&g_csPresetPending);
      g_engine.m_pendingRatings = std::move(ratings);
      g_engine.m_nPendingRatingsCount = temp_nPresets;
      g_engine.m_bPendingRatingsSwap.store(true, std::memory_order_release);
      LeaveCriticalSection(&g_csPresetPending);
    }
  }

  g_bThreadAlive = false;
  _endthreadex(0);
  return 0;
}

void Engine::UpdatePresetList(bool bBackground, bool bForce, bool bTryReselectCurrentPreset) {
  // note: if dir changed, make sure bForce is true!

  // Subdir mode: 0=off, 1=on (recursive)
  m_bRecursivePresets = (m_nSubdirMode == 1);

  if (bForce) {
    if (g_bThreadAlive)
      CancelThread(500);
  }
  else {
    if (bBackground && (g_bThreadAlive || m_bPresetListReady))
      return;
    if (!bBackground && m_bPresetListReady)
      return;
  }

  assert(!g_bThreadAlive);

  // Snapshot all engine state into ScanParams (main thread, safe to read)
  ScanParams* params = new ScanParams();
  params->bForce = bForce;
  params->bTryReselectCurrentPreset = bTryReselectCurrentPreset;
  params->nMaxPSVersion = m_nMaxPSVersion;
  params->nPresetFilter = m_nPresetFilter;
  params->bRecursive = m_bRecursivePresets;
  CopyTo(params->szPresetDir, m_szPresetDir);
  CopyTo(params->szCurrentPresetFile, m_szCurrentPresetFile);
  CopyTo(params->szUpdatePresetMask, m_szUpdatePresetMask);
  CopyTo(params->szMilkdrop2Path, m_szMilkdrop2Path);
  CopyTo(params->szPluginsDirPath, GetPluginsDirPath());

  // Spawn scan thread
  g_bThreadShouldQuit = false;
  g_bThreadAlive = true;
  g_hThread = (HANDLE)_beginthreadex(NULL, 0, __UpdatePresetList, params, 0, 0);

  // Always background — scan thread publishes via pending buffer, render thread swaps.
  // Old preset list stays visible until the new one is ready.
  SetThreadPriority(g_hThread, bBackground ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_HIGHEST);
}

// Takes the list by reference rather than reaching for m_presets (#11).
//
// A sort is O(n log n) comparisons, and going through PresetNameAt for each
// one would mean that many guard acquisitions AND that many wstring copies --
// on a large library, a real cost for no benefit. The caller takes the guard
// once and this operates inside it.
//
// Nothing calls this. It recurses into itself and has no other caller in the
// tree; the scan thread sorts its own buffer before publishing. Converted
// rather than deleted because removing it is a separate decision.
void Engine::MergeSortPresets(PresetList& v, int left, int right) {
  // note: left..right range is inclusive
  int nItems = right - left + 1;

  if (nItems > 2) {
    // recurse to sort 2 halves (but don't actually recurse on a half if it only has 1 element)
    int mid = (left + right) / 2;
    /*if (mid   != left) */ MergeSortPresets(v, left, mid);
    /*if (mid+1 != right)*/ MergeSortPresets(v, mid + 1, right);

    // then merge results
    int a = left;
    int b = mid + 1;
    while (a <= mid && b <= right) {
      bool bSwap;

      // merge the sorted arrays; give preference to strings that start with a '*' character
      int nSpecial = 0;
      if (v[a].szFilename.c_str()[0] == '*') nSpecial++;
      if (v[b].szFilename.c_str()[0] == '*') nSpecial++;

      if (nSpecial == 1) {
        bSwap = (v[b].szFilename.c_str()[0] == '*');
      }
      else {
        bSwap = (mystrcmpiW(v[a].szFilename.c_str(), v[b].szFilename.c_str()) > 0);
      }

      if (bSwap) {
        PresetInfo temp = v[b];
        for (int k = b; k > a; k--)
          v[k] = v[k - 1];
        v[a] = temp;
        mid++;
        b++;
      }
      a++;
    }
  }
  else if (nItems == 2) {
    // sort 2 items; give preference to 'special' strings that start with a '*' character
    int nSpecial = 0;
    if (v[left].szFilename.c_str()[0] == '*') nSpecial++;
    if (v[right].szFilename.c_str()[0] == '*') nSpecial++;

    if (nSpecial == 1) {
      if (v[right].szFilename.c_str()[0] == '*') {
        PresetInfo temp = v[left];
        v[left] = v[right];
        v[right] = temp;
      }
    }
    else if (mystrcmpiW(v[left].szFilename.c_str(), v[right].szFilename.c_str()) > 0) {
      PresetInfo temp = v[left];
      v[left] = v[right];
      v[right] = temp;
    }
  }
}

void Engine::SavePresetAs(wchar_t* szNewFile) {
  // overwrites the file if it was already there,
  // so you should check if the file exists first & prompt user to overwrite,
  //   before calling this function

  wchar_t msg[64];

  if (!m_pState->Export(szNewFile)) {
    // error
    AddError(wasabiApiLangString(IDS_ERROR_UNABLE_TO_SAVE_THE_FILE, msg), 6.0f, ERR_PRESET, true);
  }
  else {
    // pop up confirmation
    AddNotification(wasabiApiLangString(IDS_SAVE_SUCCESSFUL, msg));

    // update m_pState->m_szDesc with the new name
    CopyTo(m_pState->m_szDesc, m_waitstring.szText);

    // refresh file listing
    UpdatePresetList(true, true);
  }
}

void Engine::DeletePresetFile(wchar_t* szDelFile) {
  // NOTE: this function additionally assumes that m_nPresetListCurPos indicates
  //		 the slot that the to-be-deleted preset occupies!

  // delete file
  if (!DeleteFileW(szDelFile)) {
    // error
    wchar_t msg[64];
    AddError(wasabiApiLangString(IDS_ERROR_UNABLE_TO_DELETE_THE_FILE, msg), 6.0f, ERR_MISC, true);
  }
  else {
    // pop up confirmation
    wchar_t buf[1024];
    FormatResTo(buf, IDS_PRESET_X_DELETED, PresetNameAt(m_nPresetListCurPos).c_str());
    AddNotification(buf);

    // refresh file listing & re-select the next file after the one deleted
    int newPos = m_nPresetListCurPos;
    UpdatePresetList(true, true);
    m_nPresetListCurPos = max(0, min(m_nPresets - 1, newPos));
  }
}

void Engine::RenamePresetFile(wchar_t* szOldFile, wchar_t* szNewFile) {
  // NOTE: this function additionally assumes that m_nPresetListCurPos indicates
  //		 the slot that the to-be-renamed preset occupies!

  wchar_t msg[64];

  if (GetFileAttributesW(szNewFile) != -1)		// check if file already exists
  {
    // error
    AddError(wasabiApiLangString(IDS_ERROR_A_FILE_ALREADY_EXISTS_WITH_THAT_FILENAME, msg), 6.0f, ERR_PRESET, true);

    // (user remains in UI_LOAD_RENAME mode to try another filename)
  }
  else {
    // rename
    if (!MoveFileW(szOldFile, szNewFile)) {
      // error
      AddError(wasabiApiLangString(IDS_ERROR_UNABLE_TO_RENAME_FILE, msg), 6.0f, ERR_MISC, true);
    }
    else {
      // pop up confirmation
      AddError(wasabiApiLangString(IDS_RENAME_SUCCESSFUL, msg), m_ErrorDuration, ERR_NOTIFY, false);

      // if this preset was the active one, update m_pState->m_szDesc with the new name
      wchar_t buf[512];
      FormatTo(buf, L"%s.milk", m_pState->m_szDesc);
      if (wcscmp(PresetNameAt(m_nPresetListCurPos).c_str(), buf) == 0) {
        CopyTo(m_pState->m_szDesc, m_waitstring.szText);
      }

      // refresh file listing & do a trick to make it re-select the renamed file
      wchar_t buf2[512];
      CopyTo(buf2, m_waitstring.szText);
      AppendTo(buf2, L".milk");
      PresetNameAt(m_nPresetListCurPos) = buf2;
      UpdatePresetList(true, true, false);

      // jump to (highlight) the new file:
      m_nPresetListCurPos = 0;
      wchar_t* p = wcsrchr(szNewFile, L'\\');
      if (p) {
        p++;
        for (int i = m_nDirs; i < m_nPresets; i++) {
          if (wcscmp(p, PresetNameAt(i).c_str()) == 0) {
            m_nPresetListCurPos = i;
            break;
          }
        }
      }
    }

    // exit waitstring mode (return to load menu)
    m_UI_mode = UI_LOAD;
    m_waitstring.bActive = false;
  }
}

/*
void Engine::UpdatePresetRatings()
{
  if (!m_bEnableRating)
    return;

    if (m_nRatingReadProgress==-1 || m_nRatingReadProgress==m_nPresets)
        return;

  int k;

    if (m_nRatingReadProgress==0 && m_nDirs>0)
    {
      for (k=0; k<m_nDirs; k++)
      {
        m_presets[m_nRatingReadProgress].fRatingCum = 0.0f;
            m_nRatingReadProgress++;
      }

        if (!m_bInstaScan)
            return;
    }

    int presets_per_frame = m_bInstaScan ? 4096 : 1;
    int k1 = m_nRatingReadProgress;
    int k2 = min(m_nRatingReadProgress + presets_per_frame, m_nPresets);
  for (k=k1; k<k2; k++)
  {
    char szFullPath[512];
    sprintf(szFullPath, "%s%s", m_szPresetDir, PresetNameAt(k).c_str());
    float f = GetPrivateProfileFloat("preset00", "fRating", 3.0f, szFullPath);
    if (f < 0) f = 0;
    if (f > 5) f = 5;

    if (k==0)
      m_presets[k].fRatingCum = f;
    else
      m_presets[k].fRatingCum = m_presets[k-1].fRatingCum + f;

        m_nRatingReadProgress++;
  }
}
*/

// Rates the running preset.
//
// Ratings live in presets.json as dated per-version observations, NOT in the
// preset file.  This used to write fRating back into the .milk -- that write
// was already unreachable (nothing called this function), and it is gone
// rather than merely unused, because the app modifying preset files is the
// behaviour being removed: read-only folders, network shares and git-tracked
// preset collections all break under it, and fRating is excluded from preset
// identity precisely because other programs write it.
void Engine::SetCurrentPresetRating(float fNewRating) {
  if (!m_bEnableRating)
    return;

  if (fNewRating < 0) fNewRating = 0;
  if (fNewRating > 5) fNewRating = 5;
  float change = (fNewRating - m_pState->m_fRating);

  SetPresetRatingMDX(m_szCurrentPresetFile, (int)(fNewRating + 0.5f));

  // update the copy of the preset in memory
  m_pState->m_fRating = fNewRating;

  // update the cumulative internal listing:
  // One hold for the whole fix-up: the running total and every entry above it
  // move together, and a reader that saw half of that would see a broken
  // cumulative distribution.
  m_presets.with([&](PresetList& v) {
    if (m_nCurrentPreset < 0 || m_nCurrentPreset >= (int)v.size()) return;
    v[m_nCurrentPreset].fRatingThis += change;
    for (int i = m_nCurrentPreset; i < m_nPresets && i < (int)v.size(); i++)
      v[i].fRatingCum += change;
  });

  /* keep in view:
    -test switching dirs w/o loading a preset, and trying to change the rating
      ->m_nCurrentPreset is out of range!
    -soln: when adjusting rating:
      1. file to modify is m_szCurrentPresetFile
      2. only update CDF if m_nCurrentPreset is not -1
    -> set m_nCurrentPreset to -1 whenever dir. changes
    -> set m_szCurrentPresetFile whenever you load a preset
  */

  // show a message
  if (!m_bShowRating) {
    // see also: DrawText() in milkdropfs.cpp
    m_fShowRatingUntilThisTime = GetTime() + 2.0f;
  }
}

// ============================================================================
// Messages tab functions
// ============================================================================


// Functions that were interleaved with other modules in engine.cpp
bool DirHasMilkFilesHelper(const wchar_t* szDir) {
  wchar_t szMask[MAX_PATH];
  WIN32_FIND_DATAW fd;
  FormatTo(szMask, L"%s*.milk", szDir);
  HANDLE h = FindFirstFileW(szMask, &fd);
  if (h != INVALID_HANDLE_VALUE) { FindClose(h); return true; }
  FormatTo(szMask, L"%s*.milk2", szDir);
  h = FindFirstFileW(szMask, &fd);
  if (h != INVALID_HANDLE_VALUE) { FindClose(h); return true; }
  FormatTo(szMask, L"%s*.milk3", szDir);
  h = FindFirstFileW(szMask, &fd);
  if (h != INVALID_HANDLE_VALUE) { FindClose(h); return true; }
  return false;
}

bool TryDescendIntoPresetSubdirHelper(wchar_t* szDir) {
  if (GetFileAttributesW(szDir) == INVALID_FILE_ATTRIBUTES)
    return false;

  if (DirHasMilkFilesHelper(szDir))
    return true;  // already has .milk files

  wchar_t szMask[MAX_PATH];
  FormatTo(szMask, L"%s*.*", szDir);
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(szMask, &fd);
  if (h == INVALID_HANDLE_VALUE) return false;

  int nChecked = 0;
  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
      wchar_t szSubDir[MAX_PATH];
      FormatTo(szSubDir, L"%s%s\\", szDir, fd.cFileName);
      if (DirHasMilkFilesHelper(szSubDir)) {
        lstrcpynW(szDir, szSubDir, MAX_PATH);
        FindClose(h);
        return true;
      }
      if (++nChecked >= 20) break;  // safety limit
    }
  } while (FindNextFileW(h, &fd));

  FindClose(h);
  return false;
}
//----------------------------------------------------------------------

void Engine::KillAllSprites() {
  // Retire rather than release (#225): this is reachable from the CLEARSPRITES
  // IPC verb and the KillSprites hotkey, neither of which can know whether a
  // submitted frame still references these textures.
  for (int x = 0; x < NUM_TEX; x++)
    if (m_texmgr.m_tex[x].pSurface)
      KillSprite(x);
}

void Engine::KillAllSupertexts() {
  for (int x = 0; x < NUM_SUPERTEXTS; x++) {
    m_supertexts[x].fStartTime = -1.0f;
    m_supertexts[x].bRedrawSuperText = false;
  }
}

bool Engine::ChangePresetDir(wchar_t* newDir, wchar_t* oldDir) {
  // change dir
  wchar_t szOldDir[512];
  wchar_t szNewDir[512];
  CopyTo(szOldDir, oldDir);
  CopyTo(szNewDir, newDir);

  int len = lstrlenW(szNewDir);
  if (len > 0 && szNewDir[len - 1] != L'\\')
    AppendTo(szNewDir, L"\\");

  CopyTo(g_engine.m_szPresetDir, szNewDir);

  bool bSuccess = true;
  if (GetFileAttributesW(g_engine.m_szPresetDir) == -1)
    bSuccess = false;
  if (bSuccess) {
    UpdatePresetList(true, true, false);

    // bSuccess = (m_nPresets > 0);
    // success
    CopyTo(g_engine.m_szPresetDir, szNewDir);

    // save new path to registry
    Config().SetString(L"Settings", L"szPresetDir", g_engine.m_szPresetDir);
  }
  else {
    // new dir. was invalid -> allow them to try again
    CopyTo(g_engine.m_szPresetDir, oldDir);

    // give them a warning
    wchar_t msg[64];
    AddError(wasabiApiLangString(IDS_INVALID_PATH, msg), m_ErrorDuration, ERR_MISC, true);
  }

  return bSuccess;
}

void Engine::SaveCurrentPresetToQuicksave(bool altDir) {
  // A LOCAL buffer, because the fallback below appends to it.
  //
  // This used to point straight at m_szCurrentPresetFile and, when that
  // was empty, at m_pState->m_szDesc -- and then `wcscat(presetFilename,
  // L".milk")` wrote through it. That appended the extension onto the LIVE
  // preset's description, in place and unbounded: quicksave an unnamed
  // preset twice and its description became "...milk.milk". Found while
  // converting this site to CurrentPresetLeaf (#12).
  wchar_t presetFilename[MAX_PATH];
  lstrcpynW(presetFilename, CurrentPresetLeaf(), MAX_PATH);

  if (presetFilename[0] == 0) {
    RemoveAngleBrackets(m_pState->m_szDesc);
    lstrcpynW(presetFilename, m_pState->m_szDesc, MAX_PATH);
    wcsncat_s(presetFilename, MAX_PATH, L".milk", _TRUNCATE);
  }

  // Get the executable's directory
  std::filesystem::path exeDir = std::filesystem::path(m_szBaseDir).parent_path();

  std::string quicksaveDir = "resources/presets/Quicksave";
  if (altDir) {
    quicksaveDir = "resources/presets/Quicksave2";
  }
  std::filesystem::path quicksavePresetPath = exeDir / quicksaveDir;
  std::filesystem::create_directories(quicksavePresetPath);

  quicksavePresetPath.append(presetFilename);
  // Convert std::filesystem::path to const wchar_t* before passing to Export
  if (!m_pState->Export(quicksavePresetPath.wstring().c_str())) {
    AddError(L"Quicksave failed", 5.0f, ERR_PRESET, true);
  }
  else {
    RemoveAngleBrackets(m_pState->m_szDesc);
    // lstrcpyW(m_pState->m_szDesc, m_szCurrentPresetFile);
    AddNotification(L"Quicksave successful");
  }
}

//----------------------------------------------------------------------

void Engine::Randomize() {
  srand((int)(GetTime() * 100));
  //m_fAnimTime		= (rand() % 51234L)*0.01f;
  m_fRandStart[0] = (rand() % 64841L) * 0.01f;
  m_fRandStart[1] = (rand() % 53751L) * 0.01f;
  m_fRandStart[2] = (rand() % 42661L) * 0.01f;
  m_fRandStart[3] = (rand() % 31571L) * 0.01f;

  //CState temp;
  //temp.Randomize(rand() % NUM_MODES);
  //m_pState->StartBlend(&temp, m_fAnimTime, m_fBlendTimeUser);
}

//----------------------------------------------------------------------

void Engine::SetMenusForPresetVersion(int WarpPSVersion, int CompPSVersion) {
  int MaxPSVersion = max(WarpPSVersion, CompPSVersion);
  wchar_t name[64];

  m_menuPreset.EnableItem(wasabiApiLangString(IDS_MENU_EDIT_WARP_SHADER, name), WarpPSVersion > 0);
  m_menuPreset.EnableItem(wasabiApiLangString(IDS_MENU_EDIT_COMPOSITE_SHADER, name), CompPSVersion > 0);
  m_menuPost.EnableItem(wasabiApiLangString(IDS_MENU_SUSTAIN_LEVEL, name), WarpPSVersion == 0);
  m_menuPost.EnableItem(wasabiApiLangString(IDS_MENU_TEXTURE_WRAP, name), WarpPSVersion == 0);
  m_menuPost.EnableItem(wasabiApiLangString(IDS_MENU_GAMMA_ADJUSTMENT, name), CompPSVersion == 0);
  m_menuPost.EnableItem(wasabiApiLangString(IDS_MENU_HUE_SHADER, name), CompPSVersion == 0);
  m_menuPost.EnableItem(wasabiApiLangString(IDS_MENU_VIDEO_ECHO_ALPHA, name), CompPSVersion == 0);
  m_menuPost.EnableItem(wasabiApiLangString(IDS_MENU_VIDEO_ECHO_ZOOM, name), CompPSVersion == 0);
  m_menuPost.EnableItem(wasabiApiLangString(IDS_MENU_VIDEO_ECHO_ORIENTATION, name), CompPSVersion == 0);
  m_menuPost.EnableItem(wasabiApiLangString(IDS_MENU_FILTER_INVERT, name), CompPSVersion == 0);
  m_menuPost.EnableItem(wasabiApiLangString(IDS_MENU_FILTER_BRIGHTEN, name), CompPSVersion == 0);
  m_menuPost.EnableItem(wasabiApiLangString(IDS_MENU_FILTER_DARKEN, name), CompPSVersion == 0);
  m_menuPost.EnableItem(wasabiApiLangString(IDS_MENU_FILTER_SOLARIZE, name), CompPSVersion == 0);
  m_menuPost.EnableItem(wasabiApiLangString(IDS_MENU_BLUR1_EDGE_DARKEN_AMOUNT, name), MaxPSVersion > 0);
  m_menuPost.EnableItem(wasabiApiLangString(IDS_MENU_BLUR1_MIN_COLOR_VALUE, name), MaxPSVersion > 0);
  m_menuPost.EnableItem(wasabiApiLangString(IDS_MENU_BLUR1_MAX_COLOR_VALUE, name), MaxPSVersion > 0);
  m_menuPost.EnableItem(wasabiApiLangString(IDS_MENU_BLUR2_MIN_COLOR_VALUE, name), MaxPSVersion > 0);
  m_menuPost.EnableItem(wasabiApiLangString(IDS_MENU_BLUR2_MAX_COLOR_VALUE, name), MaxPSVersion > 0);
  m_menuPost.EnableItem(wasabiApiLangString(IDS_MENU_BLUR3_MIN_COLOR_VALUE, name), MaxPSVersion > 0);
  m_menuPost.EnableItem(wasabiApiLangString(IDS_MENU_BLUR3_MAX_COLOR_VALUE, name), MaxPSVersion > 0);
}

void Engine::BuildMenus() {
  wchar_t buf[1024];
  wchar_t nameBuf[64];

  m_pCurMenu = &m_menuPreset;//&m_menuMain;

  m_menuPreset.Init(wasabiApiLangString(IDS_EDIT_CURRENT_PRESET, nameBuf));
  m_menuMotion.Init(wasabiApiLangString(IDS_MOTION, nameBuf));
  m_menuCustomShape.Init(wasabiApiLangString(IDS_DRAWING_CUSTOM_SHAPES, nameBuf));
  m_menuCustomWave.Init(wasabiApiLangString(IDS_DRAWING_CUSTOM_WAVES, nameBuf));
  m_menuWave.Init(wasabiApiLangString(IDS_DRAWING_SIMPLE_WAVEFORM, nameBuf));
  m_menuAugment.Init(wasabiApiLangString(IDS_DRAWING_BORDERS_MOTION_VECTORS, nameBuf));
  m_menuPost.Init(wasabiApiLangString(IDS_POST_PROCESSING_MISC, nameBuf));
  for (int i = 0; i < MAX_CUSTOM_WAVES; i++) {
    FormatResTo(buf, IDS_CUSTOM_WAVE_X, i + 1);
    m_menuWavecode[i].Init(buf);
  }
  for (int i = 0; i < MAX_CUSTOM_SHAPES; i++) {
    FormatResTo(buf, IDS_CUSTOM_SHAPE_X, i + 1);
    m_menuShapecode[i].Init(buf);
  }

  //-------------------------------------------

  // MAIN MENU / menu hierarchy

  m_menuPreset.AddChildMenu(&m_menuMotion);
  m_menuPreset.AddChildMenu(&m_menuCustomShape);
  m_menuPreset.AddChildMenu(&m_menuCustomWave);
  m_menuPreset.AddChildMenu(&m_menuWave);
  m_menuPreset.AddChildMenu(&m_menuAugment);
  m_menuPreset.AddChildMenu(&m_menuPost);

  for (int i = 0; i < MAX_CUSTOM_SHAPES; i++)
    m_menuCustomShape.AddChildMenu(&m_menuShapecode[i]);
  for (int i = 0; i < MAX_CUSTOM_WAVES; i++)
    m_menuCustomWave.AddChildMenu(&m_menuWavecode[i]);

  // NOTE: all of the eval menuitems use a CALLBACK function to register the user's changes (see last param)
  m_menuPreset.AddItem(wasabiApiLangString(IDS_MENU_EDIT_PRESET_INIT_CODE, nameBuf),
    &m_pState->m_szPerFrameInit, MENUITEMTYPE_STRING,
    wasabiApiLangString(IDS_MENU_EDIT_PRESET_INIT_CODE_TT, buf),
    256, 0, &OnUserEditedPresetInit, sizeof(m_pState->m_szPerFrameInit), 0);

  m_menuPreset.AddItem(wasabiApiLangString(IDS_MENU_EDIT_PER_FRAME_EQUATIONS, nameBuf),
    &m_pState->m_szPerFrameExpr, MENUITEMTYPE_STRING,
    wasabiApiLangString(IDS_MENU_EDIT_PER_FRAME_EQUATIONS_TT, buf),
    256, 0, &OnUserEditedPerFrame, sizeof(m_pState->m_szPerFrameExpr), 0);

  m_menuPreset.AddItem(wasabiApiLangString(IDS_MENU_EDIT_PER_VERTEX_EQUATIONS, nameBuf),
    &m_pState->m_szPerPixelExpr, MENUITEMTYPE_STRING,
    wasabiApiLangString(IDS_MENU_EDIT_PER_VERTEX_EQUATIONS_TT, buf),
    256, 0, &OnUserEditedPerPixel, sizeof(m_pState->m_szPerPixelExpr), 0);

  m_menuPreset.AddItem(wasabiApiLangString(IDS_MENU_EDIT_WARP_SHADER, nameBuf),
    &m_pState->m_szWarpShadersText, MENUITEMTYPE_STRING,
    wasabiApiLangString(IDS_MENU_EDIT_WARP_SHADER_TT, buf),
    256, 0, &OnUserEditedWarpShaders, sizeof(m_pState->m_szWarpShadersText), 0);

  m_menuPreset.AddItem(wasabiApiLangString(IDS_MENU_EDIT_COMPOSITE_SHADER, nameBuf),
    &m_pState->m_szCompShadersText, MENUITEMTYPE_STRING,
    wasabiApiLangString(IDS_MENU_EDIT_COMPOSITE_SHADER_TT, buf),
    256, 0, &OnUserEditedCompShaders, sizeof(m_pState->m_szCompShadersText), 0);

  m_menuPreset.AddItem(wasabiApiLangString(IDS_MENU_EDIT_UPGRADE_PRESET_PS_VERSION, nameBuf),
    (void*)UI_UPGRADE_PIXEL_SHADER, MENUITEMTYPE_UIMODE,
    wasabiApiLangString(IDS_MENU_EDIT_UPGRADE_PRESET_PS_VERSION_TT, buf),
    0, 0, NULL, UI_UPGRADE_PIXEL_SHADER, 0);

  m_menuPreset.AddItem(wasabiApiLangString(IDS_MENU_EDIT_DO_A_PRESET_MASH_UP, nameBuf),
    (void*)UI_MASHUP, MENUITEMTYPE_UIMODE,
    wasabiApiLangString(IDS_MENU_EDIT_DO_A_PRESET_MASH_UP_TT, buf),
    0, 0, NULL, UI_MASHUP, 0);

  //-------------------------------------------

// menu items
#define MEN_T(id) wasabiApiLangString(id, nameBuf)
#define MEN_TT(id) wasabiApiLangString(id, buf)

  m_menuWave.AddItem(MEN_T(IDS_MENU_WAVE_TYPE), &m_pState->m_nWaveMode, MENUITEMTYPE_INT, MEN_TT(IDS_MENU_WAVE_TYPE_TT), 0, NUM_WAVES - 1);
  m_menuWave.AddItem(MEN_T(IDS_MENU_SIZE), &m_pState->m_fWaveScale, MENUITEMTYPE_LOGBLENDABLE, MEN_TT(IDS_MENU_SIZE_TT));
  m_menuWave.AddItem(MEN_T(IDS_MENU_SMOOTH), &m_pState->m_fWaveSmoothing, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_SMOOTH_TT), 0.0f, 0.9f);
  m_menuWave.AddItem(MEN_T(IDS_MENU_MYSTERY_PARAMETER), &m_pState->m_fWaveParam, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_MYSTERY_PARAMETER_TT), -1.0f, 1.0f);
  m_menuWave.AddItem(MEN_T(IDS_MENU_POSITION_X), &m_pState->m_fWaveX, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_POSITION_X_TT), 0, 1);
  m_menuWave.AddItem(MEN_T(IDS_MENU_POSITION_Y), &m_pState->m_fWaveY, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_POSITION_Y_TT), 0, 1);
  m_menuWave.AddItem(MEN_T(IDS_MENU_COLOR_RED), &m_pState->m_fWaveR, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_COLOR_RED_TT), 0, 1);
  m_menuWave.AddItem(MEN_T(IDS_MENU_COLOR_GREEN), &m_pState->m_fWaveG, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_COLOR_GREEN_TT), 0, 1);
  m_menuWave.AddItem(MEN_T(IDS_MENU_COLOR_BLUE), &m_pState->m_fWaveB, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_COLOR_BLUE_TT), 0, 1);
  m_menuWave.AddItem(MEN_T(IDS_MENU_OPACITY), &m_pState->m_fWaveAlpha, MENUITEMTYPE_LOGBLENDABLE, MEN_TT(IDS_MENU_OPACITY_TT), 0.001f, 100.0f);
  m_menuWave.AddItem(MEN_T(IDS_MENU_USE_DOTS), &m_pState->m_bWaveDots, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_USE_DOTS_TT));
  m_menuWave.AddItem(MEN_T(IDS_MENU_DRAW_THICK), &m_pState->m_bWaveThick, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_DRAW_THICK_TT));
  m_menuWave.AddItem(MEN_T(IDS_MENU_MODULATE_OPACITY_BY_VOLUME), &m_pState->m_bModWaveAlphaByVolume, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_MODULATE_OPACITY_BY_VOLUME_TT));
  m_menuWave.AddItem(MEN_T(IDS_MENU_MODULATION_TRANSPARENT_VOLUME), &m_pState->m_fModWaveAlphaStart, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_MODULATION_TRANSPARENT_VOLUME_TT), 0.0f, 2.0f);
  m_menuWave.AddItem(MEN_T(IDS_MENU_MODULATION_OPAQUE_VOLUME), &m_pState->m_fModWaveAlphaEnd, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_MODULATION_OPAQUE_VOLUME_TT), 0.0f, 2.0f);
  m_menuWave.AddItem(MEN_T(IDS_MENU_ADDITIVE_DRAWING), &m_pState->m_bAdditiveWaves, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_ADDITIVE_DRAWING_TT));
  m_menuWave.AddItem(MEN_T(IDS_MENU_COLOR_BRIGHTENING), &m_pState->m_bMaximizeWaveColor, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_COLOR_BRIGHTENING_TT));

  m_menuAugment.AddItem(MEN_T(IDS_MENU_OUTER_BORDER_THICKNESS), &m_pState->m_fOuterBorderSize, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_OUTER_BORDER_THICKNESS_TT), 0, 0.5f);
  m_menuAugment.AddItem(MEN_T(IDS_MENU_COLOR_RED_OUTER), &m_pState->m_fOuterBorderR, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_COLOR_RED_OUTER_TT), 0, 1);
  m_menuAugment.AddItem(MEN_T(IDS_MENU_COLOR_GREEN_OUTER), &m_pState->m_fOuterBorderG, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_COLOR_GREEN_OUTER_TT), 0, 1);
  m_menuAugment.AddItem(MEN_T(IDS_MENU_COLOR_BLUE_OUTER), &m_pState->m_fOuterBorderB, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_COLOR_BLUE_OUTER_TT), 0, 1);
  m_menuAugment.AddItem(MEN_T(IDS_MENU_OPACITY_OUTER), &m_pState->m_fOuterBorderA, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_OPACITY_OUTER_TT), 0, 1);
  m_menuAugment.AddItem(MEN_T(IDS_MENU_INNER_BORDER_THICKNESS), &m_pState->m_fInnerBorderSize, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_INNER_BORDER_THICKNESS_TT), 0, 0.5f);
  m_menuAugment.AddItem(MEN_T(IDS_MENU_COLOR_RED_OUTER), &m_pState->m_fInnerBorderR, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_COLOR_RED_INNER_TT), 0, 1);
  m_menuAugment.AddItem(MEN_T(IDS_MENU_COLOR_GREEN_OUTER), &m_pState->m_fInnerBorderG, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_COLOR_GREEN_INNER_TT), 0, 1);
  m_menuAugment.AddItem(MEN_T(IDS_MENU_COLOR_BLUE_OUTER), &m_pState->m_fInnerBorderB, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_COLOR_BLUE_INNER_TT), 0, 1);
  m_menuAugment.AddItem(MEN_T(IDS_MENU_OPACITY_OUTER), &m_pState->m_fInnerBorderA, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_OPACITY_INNER_TT), 0, 1);
  m_menuAugment.AddItem(MEN_T(IDS_MENU_MOTION_VECTOR_OPACITY), &m_pState->m_fMvA, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_MOTION_VECTOR_OPACITY_TT), 0, 1);
  m_menuAugment.AddItem(MEN_T(IDS_MENU_NUM_MOT_VECTORS_X), &m_pState->m_fMvX, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_NUM_MOT_VECTORS_X_TT), 0, 64);
  m_menuAugment.AddItem(MEN_T(IDS_MENU_NUM_MOT_VECTORS_Y), &m_pState->m_fMvY, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_NUM_MOT_VECTORS_Y_TT), 0, 48);
  m_menuAugment.AddItem(MEN_T(IDS_MENU_OFFSET_X), &m_pState->m_fMvDX, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_OFFSET_X_TT), -1, 1);
  m_menuAugment.AddItem(MEN_T(IDS_MENU_OFFSET_Y), &m_pState->m_fMvDY, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_OFFSET_Y_TT), -1, 1);
  m_menuAugment.AddItem(MEN_T(IDS_MENU_TRAIL_LENGTH), &m_pState->m_fMvL, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_TRAIL_LENGTH_TT), 0, 5);
  m_menuAugment.AddItem(MEN_T(IDS_MENU_COLOR_RED_OUTER), &m_pState->m_fMvR, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_COLOR_RED_MOTION_VECTOR_TT), 0, 1);
  m_menuAugment.AddItem(MEN_T(IDS_MENU_COLOR_GREEN_OUTER), &m_pState->m_fMvG, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_COLOR_GREEN_MOTION_VECTOR_TT), 0, 1);
  m_menuAugment.AddItem(MEN_T(IDS_MENU_COLOR_BLUE_OUTER), &m_pState->m_fMvB, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_COLOR_BLUE_MOTION_VECTOR_TT), 0, 1);

  m_menuMotion.AddItem(MEN_T(IDS_MENU_ZOOM_AMOUNT), &m_pState->m_fZoom, MENUITEMTYPE_LOGBLENDABLE, MEN_TT(IDS_MENU_ZOOM_AMOUNT_TT));
  m_menuMotion.AddItem(MEN_T(IDS_MENU_ZOOM_EXPONENT), &m_pState->m_fZoomExponent, MENUITEMTYPE_LOGBLENDABLE, MEN_TT(IDS_MENU_ZOOM_EXPONENT_TT));
  m_menuMotion.AddItem(MEN_T(IDS_MENU_WARP_AMOUNT), &m_pState->m_fWarpAmount, MENUITEMTYPE_LOGBLENDABLE, MEN_TT(IDS_MENU_WARP_AMOUNT_TT));
  m_menuMotion.AddItem(MEN_T(IDS_MENU_WARP_SCALE), &m_pState->m_fWarpScale, MENUITEMTYPE_LOGBLENDABLE, MEN_TT(IDS_MENU_WARP_SCALE_TT));
  m_menuMotion.AddItem(MEN_T(IDS_MENU_WARP_SPEED), &m_pState->m_fWarpAnimSpeed, MENUITEMTYPE_LOGFLOAT, MEN_TT(IDS_MENU_WARP_SPEED_TT));
  m_menuMotion.AddItem(MEN_T(IDS_MENU_ROTATION_AMOUNT), &m_pState->m_fRot, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_ROTATION_AMOUNT_TT), -1.00f, 1.00f);
  m_menuMotion.AddItem(MEN_T(IDS_MENU_ROTATION_CENTER_OF_X), &m_pState->m_fRotCX, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_ROTATION_CENTER_OF_X_TT), -1.0f, 2.0f);
  m_menuMotion.AddItem(MEN_T(IDS_MENU_ROTATION_CENTER_OF_Y), &m_pState->m_fRotCY, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_ROTATION_CENTER_OF_Y_TT), -1.0f, 2.0f);
  m_menuMotion.AddItem(MEN_T(IDS_MENU_TRANSLATION_X), &m_pState->m_fXPush, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_TRANSLATION_X_TT), -1.0f, 1.0f);
  m_menuMotion.AddItem(MEN_T(IDS_MENU_TRANSLATION_Y), &m_pState->m_fYPush, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_TRANSLATION_Y_TT), -1.0f, 1.0f);
  m_menuMotion.AddItem(MEN_T(IDS_MENU_SCALING_X), &m_pState->m_fStretchX, MENUITEMTYPE_LOGBLENDABLE, MEN_TT(IDS_MENU_SCALING_X_TT));
  m_menuMotion.AddItem(MEN_T(IDS_MENU_SCALING_Y), &m_pState->m_fStretchY, MENUITEMTYPE_LOGBLENDABLE, MEN_TT(IDS_MENU_SCALING_Y_TT));

  m_menuPost.AddItem(MEN_T(IDS_MENU_SUSTAIN_LEVEL), &m_pState->m_fDecay, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_SUSTAIN_LEVEL_TT), 0.50f, 1.0f);
  m_menuPost.AddItem(MEN_T(IDS_MENU_DARKEN_CENTER), &m_pState->m_bDarkenCenter, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_DARKEN_CENTER_TT));
  m_menuPost.AddItem(MEN_T(IDS_MENU_GAMMA_ADJUSTMENT), &m_pState->m_fGammaAdj, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_GAMMA_ADJUSTMENT_TT), 1.0f, 8.0f);
  m_menuPost.AddItem(MEN_T(IDS_MENU_HUE_SHADER), &m_pState->m_fShader, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_HUE_SHADER_TT), 0.0f, 1.0f);
  m_menuPost.AddItem(MEN_T(IDS_MENU_VIDEO_ECHO_ALPHA), &m_pState->m_fVideoEchoAlpha, MENUITEMTYPE_BLENDABLE, MEN_TT(IDS_MENU_VIDEO_ECHO_ALPHA_TT), 0.0f, 1.0f);
  m_menuPost.AddItem(MEN_T(IDS_MENU_VIDEO_ECHO_ZOOM), &m_pState->m_fVideoEchoZoom, MENUITEMTYPE_LOGBLENDABLE, MEN_TT(IDS_MENU_VIDEO_ECHO_ZOOM_TT));
  m_menuPost.AddItem(MEN_T(IDS_MENU_VIDEO_ECHO_ORIENTATION), &m_pState->m_nVideoEchoOrientation, MENUITEMTYPE_INT, MEN_TT(IDS_MENU_VIDEO_ECHO_ORIENTATION_TT), 0.0f, 3.0f);
  m_menuPost.AddItem(MEN_T(IDS_MENU_TEXTURE_WRAP), &m_pState->m_bTexWrap, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_TEXTURE_WRAP_TT));
  //m_menuPost.AddItem("stereo 3D",               &m_pState->m_bRedBlueStereo,        MENUITEMTYPE_BOOL, "displays the image in stereo 3D; you need 3D glasses (with red and blue lenses) for this.");
  m_menuPost.AddItem(MEN_T(IDS_MENU_FILTER_INVERT), &m_pState->m_bInvert, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_FILTER_INVERT_TT));
  m_menuPost.AddItem(MEN_T(IDS_MENU_FILTER_BRIGHTEN), &m_pState->m_bBrighten, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_FILTER_BRIGHTEN_TT));
  m_menuPost.AddItem(MEN_T(IDS_MENU_FILTER_DARKEN), &m_pState->m_bDarken, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_FILTER_DARKEN_TT));
  m_menuPost.AddItem(MEN_T(IDS_MENU_FILTER_SOLARIZE), &m_pState->m_bSolarize, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_FILTER_SOLARIZE_TT));
  m_menuPost.AddItem(MEN_T(IDS_MENU_BLUR1_EDGE_DARKEN_AMOUNT), &m_pState->m_fBlur1EdgeDarken, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_BLUR1_EDGE_DARKEN_AMOUNT_TT), 0.0f, 1.0f);
  m_menuPost.AddItem(MEN_T(IDS_MENU_BLUR1_MIN_COLOR_VALUE), &m_pState->m_fBlur1Min, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_BLUR1_MIN_COLOR_VALUE_TT), 0.0f, 1.0f);
  m_menuPost.AddItem(MEN_T(IDS_MENU_BLUR1_MAX_COLOR_VALUE), &m_pState->m_fBlur1Max, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_BLUR1_MAX_COLOR_VALUE_TT), 0.0f, 1.0f);
  m_menuPost.AddItem(MEN_T(IDS_MENU_BLUR2_MIN_COLOR_VALUE), &m_pState->m_fBlur2Min, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_BLUR2_MIN_COLOR_VALUE_TT), 0.0f, 1.0f);
  m_menuPost.AddItem(MEN_T(IDS_MENU_BLUR2_MAX_COLOR_VALUE), &m_pState->m_fBlur2Max, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_BLUR2_MAX_COLOR_VALUE_TT), 0.0f, 1.0f);
  m_menuPost.AddItem(MEN_T(IDS_MENU_BLUR3_MIN_COLOR_VALUE), &m_pState->m_fBlur3Min, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_BLUR3_MIN_COLOR_VALUE_TT), 0.0f, 1.0f);
  m_menuPost.AddItem(MEN_T(IDS_MENU_BLUR3_MAX_COLOR_VALUE), &m_pState->m_fBlur3Max, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_BLUR3_MAX_COLOR_VALUE_TT), 0.0f, 1.0f);

  for (int i = 0; i < MAX_CUSTOM_WAVES; i++) {
    // blending: do both; fade opacities in/out (w/exagerrated weighting)
    m_menuWavecode[i].AddItem(MEN_T(IDS_MENU_ENABLED), &m_pState->m_wave[i].enabled, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_ENABLED_TT)); // bool
    m_menuWavecode[i].AddItem(MEN_T(IDS_MENU_NUMBER_OF_SAMPLES), &m_pState->m_wave[i].samples, MENUITEMTYPE_INT, MEN_TT(IDS_MENU_NUMBER_OF_SAMPLES_TT), 2, 512);        // 0-512
    m_menuWavecode[i].AddItem(MEN_T(IDS_MENU_L_R_SEPARATION), &m_pState->m_wave[i].sep, MENUITEMTYPE_INT, MEN_TT(IDS_MENU_L_R_SEPARATION_TT), 0, 256);        // 0-512
    m_menuWavecode[i].AddItem(MEN_T(IDS_MENU_SCALING), &m_pState->m_wave[i].scaling, MENUITEMTYPE_LOGFLOAT, MEN_TT(IDS_MENU_SCALING_TT));
    m_menuWavecode[i].AddItem(MEN_T(IDS_MENU_SMOOTH), &m_pState->m_wave[i].smoothing, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_SMOOTHING_TT), 0, 1);
    m_menuWavecode[i].AddItem(MEN_T(IDS_MENU_COLOR_RED), &m_pState->m_wave[i].r, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_COLOR_RED_TT), 0, 1);
    m_menuWavecode[i].AddItem(MEN_T(IDS_MENU_COLOR_GREEN), &m_pState->m_wave[i].g, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_COLOR_GREEN_TT), 0, 1);
    m_menuWavecode[i].AddItem(MEN_T(IDS_MENU_COLOR_BLUE), &m_pState->m_wave[i].b, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_COLOR_BLUE_TT), 0, 1);
    m_menuWavecode[i].AddItem(MEN_T(IDS_MENU_OPACITY), &m_pState->m_wave[i].a, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_OPACITY_WAVE_TT), 0, 1);
    m_menuWavecode[i].AddItem(MEN_T(IDS_MENU_USE_SPECTRUM), &m_pState->m_wave[i].bSpectrum, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_USE_SPECTRUM_TT));        // 0-5 [0=wave left, 1=wave center, 2=wave right; 3=spectrum left, 4=spec center, 5=spec right]
    m_menuWavecode[i].AddItem(MEN_T(IDS_MENU_USE_DOTS), &m_pState->m_wave[i].bUseDots, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_USE_DOTS_WAVE_TT)); // bool
    m_menuWavecode[i].AddItem(MEN_T(IDS_MENU_DRAW_THICK), &m_pState->m_wave[i].bDrawThick, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_DRAW_THICK_WAVE_TT)); // bool
    m_menuWavecode[i].AddItem(MEN_T(IDS_MENU_ADDITIVE_DRAWING), &m_pState->m_wave[i].bAdditive, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_ADDITIVE_DRAWING_WAVE_TT)); // bool
    m_menuWavecode[i].AddItem(MEN_T(IDS_MENU_EXPORT_TO_FILE), (void*)UI_EXPORT_WAVE, MENUITEMTYPE_UIMODE, MEN_TT(IDS_MENU_EXPORT_TO_FILE_TT), 0, 0, NULL, UI_EXPORT_WAVE, i);
    m_menuWavecode[i].AddItem(MEN_T(IDS_MENU_IMPORT_FROM_FILE), (void*)UI_IMPORT_WAVE, MENUITEMTYPE_UIMODE, MEN_TT(IDS_MENU_IMPORT_FROM_FILE_TT), 0, 0, NULL, UI_IMPORT_WAVE, i);
    m_menuWavecode[i].AddItem(MEN_T(IDS_MENU_EDIT_INIT_CODE), &m_pState->m_wave[i].m_szInit, MENUITEMTYPE_STRING, MEN_TT(IDS_MENU_EDIT_INIT_CODE_TT), 256, 0, &OnUserEditedWavecodeInit, sizeof(m_pState->m_wave[i].m_szInit), 0);
    m_menuWavecode[i].AddItem(MEN_T(IDS_MENU_EDIT_PER_FRAME_CODE), &m_pState->m_wave[i].m_szPerFrame, MENUITEMTYPE_STRING, MEN_TT(IDS_MENU_EDIT_PER_FRAME_CODE_TT), 256, 0, &OnUserEditedWavecode, sizeof(m_pState->m_wave[i].m_szPerFrame), 0);
    m_menuWavecode[i].AddItem(MEN_T(IDS_MENU_EDIT_PER_POINT_CODE), &m_pState->m_wave[i].m_szPerPoint, MENUITEMTYPE_STRING, MEN_TT(IDS_MENU_EDIT_PER_POINT_CODE_TT), 256, 0, &OnUserEditedWavecode, sizeof(m_pState->m_wave[i].m_szPerPoint), 0);
  }

  for (int i = 0; i < MAX_CUSTOM_SHAPES; i++) {
    // blending: do both; fade opacities in/out (w/exagerrated weighting)
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_ENABLED), &m_pState->m_shape[i].enabled, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_ENABLED_SHAPE_TT)); // bool
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_NUMBER_OF_INSTANCES), &m_pState->m_shape[i].instances, MENUITEMTYPE_INT, MEN_TT(IDS_MENU_NUMBER_OF_INSTANCES_TT), 1, 1024);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_NUMBER_OF_SIDES), &m_pState->m_shape[i].sides, MENUITEMTYPE_INT, MEN_TT(IDS_MENU_NUMBER_OF_SIDES_TT), 3, 100);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_DRAW_THICK), &m_pState->m_shape[i].thickOutline, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_DRAW_THICK_SHAPE_TT)); // bool
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_ADDITIVE_DRAWING), &m_pState->m_shape[i].additive, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_ADDITIVE_DRAWING_SHAPE_TT)); // bool
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_X_POSITION), &m_pState->m_shape[i].x, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_X_POSITION_TT), 0, 1);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_Y_POSITION), &m_pState->m_shape[i].y, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_Y_POSITION_TT), 0, 1);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_RADIUS), &m_pState->m_shape[i].rad, MENUITEMTYPE_LOGFLOAT, MEN_TT(IDS_MENU_RADIUS_TT));
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_ANGLE), &m_pState->m_shape[i].ang, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_ANGLE_TT), 0, 3.1415927f * 2.0f);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_TEXTURED), &m_pState->m_shape[i].textured, MENUITEMTYPE_BOOL, MEN_TT(IDS_MENU_TEXTURED_TT)); // bool
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_TEXTURE_ZOOM), &m_pState->m_shape[i].tex_zoom, MENUITEMTYPE_LOGFLOAT, MEN_TT(IDS_MENU_TEXTURE_ZOOM_TT)); // bool
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_TEXTURE_ANGLE), &m_pState->m_shape[i].tex_ang, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_TEXTURE_ANGLE_TT), 0, 3.1415927f * 2.0f); // bool
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_INNER_COLOR_RED), &m_pState->m_shape[i].r, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_INNER_COLOR_RED_TT), 0, 1);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_INNER_COLOR_GREEN), &m_pState->m_shape[i].g, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_INNER_COLOR_GREEN_TT), 0, 1);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_INNER_COLOR_BLUE), &m_pState->m_shape[i].b, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_INNER_COLOR_BLUE_TT), 0, 1);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_INNER_OPACITY), &m_pState->m_shape[i].a, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_INNER_OPACITY_TT), 0, 1);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_OUTER_COLOR_RED), &m_pState->m_shape[i].r2, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_OUTER_COLOR_RED_TT), 0, 1);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_OUTER_COLOR_GREEN), &m_pState->m_shape[i].g2, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_OUTER_COLOR_GREEN_TT), 0, 1);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_OUTER_COLOR_BLUE), &m_pState->m_shape[i].b2, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_OUTER_COLOR_BLUE_TT), 0, 1);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_OUTER_OPACITY), &m_pState->m_shape[i].a2, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_OUTER_OPACITY_TT), 0, 1);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_BORDER_COLOR_RED), &m_pState->m_shape[i].border_r, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_BORDER_COLOR_RED_TT), 0, 1);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_BORDER_COLOR_GREEN), &m_pState->m_shape[i].border_g, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_BORDER_COLOR_GREEN_TT), 0, 1);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_BORDER_COLOR_BLUE), &m_pState->m_shape[i].border_b, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_BORDER_COLOR_BLUE_TT), 0, 1);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_BORDER_OPACITY), &m_pState->m_shape[i].border_a, MENUITEMTYPE_FLOAT, MEN_TT(IDS_MENU_BORDER_OPACITY_TT), 0, 1);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_EXPORT_TO_FILE), NULL, MENUITEMTYPE_UIMODE, MEN_TT(IDS_MENU_EXPORT_TO_FILE_SHAPE_TT), 0, 0, NULL, UI_EXPORT_SHAPE, i);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_IMPORT_FROM_FILE), NULL, MENUITEMTYPE_UIMODE, MEN_TT(IDS_MENU_IMPORT_FROM_FILE_SHAPE_TT), 0, 0, NULL, UI_IMPORT_SHAPE, i);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_EDIT_INIT_CODE), &m_pState->m_shape[i].m_szInit, MENUITEMTYPE_STRING, MEN_TT(IDS_MENU_EDIT_INIT_CODE_SHAPE_TT), 256, 0, &OnUserEditedShapecodeInit, sizeof(m_pState->m_shape[i].m_szInit), 0);
    m_menuShapecode[i].AddItem(MEN_T(IDS_MENU_EDIT_PER_FRAME_INSTANCE_CODE), &m_pState->m_shape[i].m_szPerFrame, MENUITEMTYPE_STRING, MEN_TT(IDS_MENU_EDIT_PER_FRAME_INSTANCE_CODE_TT), 256, 0, &OnUserEditedShapecode, sizeof(m_pState->m_shape[i].m_szPerFrame), 0);
    //m_menuShapecode[i].AddItem("[ edit per-point code ]",&m_pState->m_shape[i].m_szPerPoint,  MENUITEMTYPE_STRING, "IN: sample [0..1]; value1 [left ch], value2 [right ch], plus all vars for per-frame code / OUT: x,y; r,g,b,a; t1-t8", 256, 0, &OnUserEditedWavecode);
  }
}



// A preset that could not be loaded must not swallow the request that asked
// for it (forgejo#103).
//
// The failure paths above clear their own flags and return, which looks tidy
// and is not: nothing reached the user, no other preset was tried, and the
// Next press that started it simply vanished. One Recycle Bin artifact in a
// library of 633 -- "$R02SA9V.milk2", which Windows named and nobody noticed --
// was enough to make the first Next of every session do nothing.
//
// Deleting that file would have fixed the symptom and nothing else. A preset
// library is user data: it collects downloads, partial writes, recycle-bin
// residue and files an editor truncated. One unusable file must not be able to
// make a keypress do nothing, silently.
//
// The shape here is App.cpp's SEH recovery, which already answers this exact
// situation when a preset crashes the RENDERER: say so, record it against the
// preset, advance, and give up after too many rather than walking a whole
// directory of bad files.
void Engine::OnPresetLoadFailed(const wchar_t* filename, const wchar_t* why) {
  // The status must stop advertising a load that has already been abandoned.
  // Leaving it set is what made this look like a wedged load rather than a
  // dropped one, and cost the original investigation its first hour.
  m_szLoadingPreset[0] = 0;

  const wchar_t* leaf = wcsrchr(filename, L'\\');
  leaf = leaf ? leaf + 1 : filename;

  wchar_t msg[512];
  swprintf(msg, 512, L"Preset failed to load (%s): %.180s", why, leaf);
  AddError(msg, 4.0f, ERR_PRESET, true);
  AutoFlagPresetError(filename, std::wstring(why), PresetErrorKind::Runtime);

  // Bounded, so a directory of unreadable files terminates. Each step recurses
  // one level (NextPreset -> LoadRandomPreset -> LoadPreset -> here), so the
  // limit is also the maximum depth; 8 is deep enough to step over a cluster
  // of bad files and shallow enough to be free.
  const int kMaxConsecutive = 8;
  if (++m_nConsecutiveLoadFailures <= kMaxConsecutive) {
    NextPreset(0.0f);   // hard cut: there is nothing to blend from
    return;
  }

  // Stop, and say why. Silently continuing to hunt would look like the app
  // ignoring the key, which is the failure this whole function exists to end.
  DLOG_ERROR("LoadPreset: %d consecutive presets failed to load — giving up",
             m_nConsecutiveLoadFailures);
  AddError(L"Several presets in a row failed to load — check the preset folder",
           8.0f, ERR_PRESET, true);
  m_nConsecutiveLoadFailures = 0;
}

} // namespace mdrop
