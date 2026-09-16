#pragma once
// child_instances.h — one MDropDX12.exe -child per display marked "own process".
//
// A child is a whole separate instance: its own preset, its own cycle timer,
// its own EEL state. That is what makes "a different preset on each screen"
// possible at all, and separate processes are the settled answer here rather
// than N copies of the engine in one process (see docs/Scripts.md, forgejo#22).
//
// The parent spawns it, pushes its configuration over the child's own named
// pipe, polls it for what it is playing, and retires it when the display stops
// being independent. A child needs no discovery: its pipe name is
// \\.\pipe\mDxChild_<pid> and the pid comes straight back from CreateProcess.
// That namespace keeps children out of everything enumerating Milkwave_ for a
// visualizer to drive -- see pipe_server.h.
//
// EVERYTHING HERE RUNS ON THE CHILD WORKER THREAD. CreateProcess plus waiting
// for a pipe to appear is seconds of blocking, and a ToolWindow touching engine
// state from its own thread is how m_szPresetDir got corrupted once already --
// so the UI only ever enqueues a ChildCommand.

#ifndef CHILD_INSTANCES_H
#define CHILD_INSTANCES_H

#include <windows.h>
#include <string>

struct ChildInstance {
    DWORD   pid = 0;
    HANDLE  hProcess = nullptr;
    wchar_t szPipeName[64] = {};
    wchar_t szDeviceName[32] = {};            // the display it is pinned to
    // 1-based slot, lowest free first, assigned at spawn and passed on the
    // command line. It names the child in its own window caption
    // (MDXChild_<N>_<parentpid>) so a stray one can still be identified now
    // that a child keeps off the taskbar. A retired slot is reused, so N says
    // which child this is rather than how many have ever been started.
    int     nIndex = 0;
    wchar_t szCurrentPreset[MAX_PATH] = {};   // last polled, for the UI
    // The rate this child is ACTUALLY rendering at, from the same poll that
    // fetches the preset above (#246).
    //
    // Nothing could answer "what is each display running at" before this.
    // DIAG_MIRRORS reports only in-process surfaces, so a display moved to its
    // own process vanished from every frame-rate measurement the app can make
    // -- which is exactly the arrangement a mirrors-vs-children comparison
    // exists to evaluate. The comparison could report the primary and nothing
    // else, so "children are 8% better on the primary" could not be told apart
    // from "the work moved somewhere unmeasured".
    //
    // -1 until the child has answered once, so "not known yet" stays distinct
    // from a genuine zero.
    float   fFps = -1.0f;
    int     nRestarts = 0;
    bool    bReady = false;                   // its pipe has answered once
    bool    bFailed = false;                  // spawn failed, or it died
    // Placed onto its monitor, once, after it started answering requests.
    // A child's pipe comes up well before its window is finished starting, and
    // its own start-up then restores the geometry saved in the shared
    // settings.ini -- overwriting anything the parent sent in between. So
    // placement waits for the first successful poll rather than racing it.
    bool    bPlaced = false;
};

enum class ChildCmd : unsigned char {
    Spawn,
    Kill,
    Reconfigure,
    // One message, delivered as-is to one child. Reconfigure is the wrong tool
    // for a settings change: it re-sends the whole configuration including the
    // startup preset and the window placement, which on a running child means
    // yanking it back to where it began. Send carries just what changed.
    Send,
};

struct ChildCommand {
    ChildCmd cmd = ChildCmd::Spawn;
    wchar_t  szDeviceName[32] = {};
    // Only Send uses this. ChildCommand is drained by copy under
    // m_childCmdMutex, which is fine -- but it is no longer trivially
    // copyable, so nothing may memcpy it.
    std::wstring sPayload;
};

#endif // CHILD_INSTANCES_H
