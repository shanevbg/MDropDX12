#pragma once

#include <windows.h>
#include <string>
#include <vector>

// Looking at a tool window from outside the process (issue 154).
//
// Everything else about this app is drivable over the pipe, and its own
// windows were the blind spot. Two things kept going wrong and neither was
// visible to the harness:
//
//   - a control holding the WRONG TEXT. The Presets window's Browse buttons
//     came up reading "Ã¨E5" because a glyph escape had been mangled into the
//     system codepage. Nothing outside the process could see that.
//   - a control in the WRONG PLACE, or clipped. A button positioned past the
//     client edge looks identical to one that was never created.
//
// An external screenshot cannot answer either. PrintWindow from another
// process does not render owner-drawn buttons at all -- the pin and refresh
// glyphs in every tool window's title row come back as flat background -- so a
// capture taken from outside is blind to exactly the controls most likely to be
// wrong, and blind in a way that LOOKS like a clean result.
//
// Taken from inside, with PW_RENDERFULLCONTENT, it renders.

namespace mdrop {

// The top-level window of THIS process whose title matches, case-insensitively.
// Matches a whole title first, then a prefix, so "Preset" finds "Presets" but
// an exact "Presets" is never beaten by a longer name. NULL if nothing matches.
HWND FindToolWindowByTitle(const std::wstring& title);

// Sends one WINDOW_CTRL| line per child control, then WINDOW_CTRLS_END.
// Reports WINDOW_CTRLS=NOTFOUND if no window matches.
//
//   WINDOW_CTRL|i=|class=|id=|x=|y=|w=|h=|vis=|enab=|clip=|tw=|textfits=|u=|text=
//
// clip=1     the control's RECTANGLE extends past the client edge (issue 157)
// tw=        the width the CONTROL would need to hold its text on one line,
//            its own chrome included -- so textfits is 0 exactly when tw > w
// textfits=  0 when the text does not fit the control it is in (issue 159),
//            1 when it does, -1 where the question is not asked -- an edit or
//            a combo holds the user's data and scrolls it, which is not the
//            same kind of fact and would drown the signal
//
// Those two are different failures and neither implies the other: a button in
// the right place can still be drawn as "Ou" instead of "Out".
void SendWindowControlDump(const std::wstring& title);

// Captures the window to a PNG under capture\ and sends WINDOW_CAPTURE_PATH=.
// Reports WINDOW_CAPTURE=ERROR with a reason on failure.
void SendWindowCapture(const std::wstring& title, const std::wstring& baseDir);

// What only the engine knows about one of its windows.
//
// EnumWindows can find a window and measure it, but it cannot say what the
// window is FOR, and with several displays that is the whole question: the
// primary render window, an in-process mirror and a child's render window are
// three different things that look alike from outside. Worse, a child runs in
// its own PROCESS, so an enumeration filtered to our own pid cannot see it at
// all -- it has to be contributed here or it is simply missing.
struct KnownWindow {
  HWND         hWnd = nullptr;   // null when a child's window is not up yet
  std::wstring role;             // render | mirror | child
  int          display = 0;      // 1-based, matching GET_DISPLAY_MODE; 0 = none
  std::wstring device;           // \\.\DISPLAYn, or empty
  DWORD        pid = 0;          // non-zero only for another process's window
};

// Every top-level window this process owns, with where it is: one WINDOW| line
// per window, then WINDOWS_END (#259).
//
//   WINDOW|title=|class=|x=|y=|w=|h=|client=WxH|display=|visible=|minimized=
//         |maximized=|topmost=|dpi=
//
// Screen coordinates, because that is the frame a layout reasons in. client=
// is not redundant with w/h: the render cost, the swap chain and every capture
// are client-sized, while a layout has to reserve the FRAME, and on a tool
// window the two differ by the caption and border.
//
// Hidden windows are listed with visible=0 rather than filtered -- a child in
// hidden mode is a real window with a real rect. Zero-area windows are dropped:
// the IME plumbing Windows attaches to every process has a title and would
// otherwise be three rows of noise. A CLOSED tool window does not appear at
// all, because ToolWindow::Close destroys it rather than hiding it.
//
// `known` labels the rows: anything matched by HWND gets its role, display
// index and device name, anything else is role=tool, and any entry belonging
// to another process is appended, since the enumeration cannot reach it.
void SendWindowList(const std::vector<KnownWindow>& known);

// One window: the render window when title is empty, else a title match.
// Sends WINDOW|… or WINDOW=NOTFOUND.
void SendWindowGeometry(const std::wstring& title,
                        const std::vector<KnownWindow>& known);

// Handles GET_WINDOWS, GET_WINDOW[=], DIAG_WINDOW= and CAPTURE_WINDOW=;
// true when it took the message.
//
// hRender is passed in rather than reached for. This file is deliberately
// standalone -- format_to.h and pipe_server.h and nothing else -- so that it
// stays compilable in isolation and cheap to reason about; including engine.h
// for one handle would undo that for no gain. baseDir arrives the same way and
// for the same reason.
//
// A dispatcher of its own, consulted beside HandleDisplayModeIPC and the rest,
// rather than two more arms on LaunchMessage's else-if chain. That is not a
// preference: adding them there produced
//
//     error C1061: compiler limit: blocks nested too deeply
//
// so that chain is AT the compiler's limit and cannot take another arm. Any
// new IPC command needs a small handler like this one.
bool HandleWindowDiagIPC(const wchar_t* sMessage, const std::wstring& baseDir,
                         const std::vector<KnownWindow>& known);

}  // namespace mdrop
