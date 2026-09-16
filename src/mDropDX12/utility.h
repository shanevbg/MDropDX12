/*
  LICENSE
  -------
Copyright 2005-2013 Nullsoft, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

  * Neither the name of Nullsoft nor the names of its contributors may be used to
    endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef MDROP_UTILITY_H
#define MDROP_UTILITY_H 1

#include <windows.h>
#include <crtdefs.h>
#include "d3dx9compat.h"  // replaces <d3dx9.h>; provides D3DX types for DX9→DX12 migration

#define SafeRelease(x) { if (x) {x->Release(); x=NULL;} }
#define SafeDelete(x) { if (x) {delete x; x=NULL;} }
#define IsNullGuid(lpGUID) ( ((int*)lpGUID)[0]==0 && ((int*)lpGUID)[1]==0 && ((int*)lpGUID)[2]==0 && ((int*)lpGUID)[3]==0 )
#define DlgItemIsChecked(hDlg, nIDDlgItem) ((SendDlgItemMessage(hDlg, nIDDlgItem, BM_GETCHECK, (WPARAM) 0, (LPARAM) 0) == BST_CHECKED) ? true : false)
#define CosineInterp(x) (0.5f - 0.5f*cosf((x) * 3.1415926535898f))
#define InvCosineInterp(x) (acosf(1.0f - 2.0f*(x))/3.1415926535898f)
float   PowCosineInterp(float x, float pow);
float   AdjustRateToFPS(float per_frame_decay_rate_at_fps1, float fps1, float actual_fps);

// Settings I/O lives in config_store.h -- mdrop::Config() / mdrop::ConfigFile().
// The helpers that used to sit here took their arguments in the opposite order
// from the Win32 functions they wrapped, which is exactly the kind of thing a
// single typed facade exists to stop.

extern  _locale_t g_use_C_locale;
extern	char keyMappings[8];

void    RemoveExtension(wchar_t* str);
void    RemoveSingleAmpersands(wchar_t* str);
void    TextToGuid(char* str, GUID* pGUID);
void    GuidToText(GUID* pGUID, char* str, int nStrLen);
// When this EXE was built, read from the exe itself rather than from
// __DATE__/__TIME__.
//
// Those two expand when a TRANSLATION UNIT is compiled, not when the binary is
// linked, so every file that uses them reports its own last compile and an
// incremental build leaves most of them stale. Measured: VERSION said
// "Sep 12 13:43:10" from ipc_help.cpp while the exe had been linked on Sep 14
// at 04:34:27, and the About tab -- built from a different file again -- showed
// a third value. A build time that is really "when this one source file last
// compiled" is worse than none, because it is read as the former.
//
// The file's last-write time is set once per link and is the same answer no
// matter who asks. Format matches what __DATE__ " " __TIME__ produced
// ("Mmm dd yyyy HH:MM:SS") so nothing reading these strings has to change.
//
// Computed once and cached; returns a static buffer.
const wchar_t* BuildTimestampW();

void    MissingDirectX(HWND hwnd);
bool    CheckForMMX();
bool    CheckForSSE();
void    GetDesktopFolder(char* szDesktopFolder); // should be MAX_PATH len.

#include <shlobj.h>
#include <list>
#include <string>

BOOL    DoExplorerMenu(HWND hwnd, const wchar_t* pszPath, POINT point);
BOOL    DoExplorerMenu(HWND hwnd, LPITEMIDLIST pidl, POINT point);
UINT    GetItemCount(LPITEMIDLIST pidl);
LPITEMIDLIST GetNextItem(LPITEMIDLIST pidl);
LPITEMIDLIST DuplicateItem(LPMALLOC pMalloc, LPITEMIDLIST pidl);
void    FindDesktopWindows(HWND* desktop_progman, HWND* desktopview_wnd, HWND* listview_wnd);
void    ExecutePidl(LPITEMIDLIST pidl, char* szPathAndFile, char* szWorkingDirectory, HWND hWnd);
int     GetDesktopIconSize();

// handy functions for populating Combo Boxes:
inline void AddItem(HWND ctrl, const wchar_t* text, DWORD itemdata) {
  LRESULT nPos = SendMessageW(ctrl, CB_ADDSTRING, 0, (LPARAM)text);
  SendMessage(ctrl, CB_SETITEMDATA, nPos, itemdata);
}
inline void SelectItemByPos(HWND ctrl, int pos) {
  SendMessage(ctrl, CB_SETCURSEL, pos, 0);
}
int SelectItemByValue(HWND ctrl, DWORD value);
bool ReadCBValue(HWND hwnd, DWORD ctrl_id, int* pRetValue);

void* GetTextResource(UINT id, int no_fallback);

intptr_t myOpenURL(HWND hwnd, wchar_t* loc);

// Debug log — writes timestamped messages to debug.log in the base directory.
// DebugLogInit rotates debug.log → debug.prev.log (keeps only current + last run).
// Log levels: 0=Off, 1=Error, 2=Warn, 3=Info (default), 4=Verbose
#define LOG_ERROR   1
#define LOG_WARN    2
#define LOG_INFO    3
#define LOG_VERBOSE 4

// Log output destinations (bitmask)
#define LOG_OUTPUT_FILE  1
#define LOG_OUTPUT_ODS   2  // OutputDebugString
#define LOG_OUTPUT_BOTH  3  // default

extern int g_debugLogLevel;
extern int g_debugLogOutput;

// subdir: optional per-instance directory under log\, so concurrent
// instances sharing a folder do not write over one another's debug.log.
void DebugLogInit(const wchar_t* baseDir, const wchar_t* subdir = nullptr);
// Deletes log\child-<pid>\ directories nothing has written to for 24h.
// Called by DebugLogInit on the instance that owns log\; separate so the
// native suite can drive it against a scratch directory. Returns the count.
int  DebugLogPruneStaleChildLogs(const wchar_t* logRoot);
void DebugLogSetLevel(int level);
void DebugLogSetOutput(int output);
void DebugLogW(const wchar_t* msg, int level = LOG_INFO);
void DebugLogA(const char* msg, int level); // default (= LOG_INFO) is in d3dx9compat.h (included above)
void DebugLogWFmt(const wchar_t* fmt, ...);
void DebugLogWFmt(int level, const wchar_t* fmt, ...);
void DebugLogAFmt(const char* fmt, ...);
void DebugLogAFmt(int level, const char* fmt, ...);

// VRAM checkpoints (#344): what LOCAL VRAM reads RIGHT NOW, and the delta since
// the previous mark. m_vramUsageBytes is resampled about every two seconds,
// which is far too coarse to sit between two allocations and say which one
// cost what -- and "which call allocated this" was unanswerable without it.
//
// Off by default and cheap when off: QueryVideoMemoryInfo is a real call, so a
// mark in a per-frame path would be a cost of its own. Turn it on for a run
// with DIAG_VRAMMARK=1 and read log/debug.log.
void VramMarkSetEnabled(bool on);
bool VramMarkEnabled();
void VramMark(const char* tag);
#define VMARK(tag) do { if (VramMarkEnabled()) VramMark(tag); } while(0)

// Diagnostic file helpers — all files written to log/ subdirectory
const wchar_t* DebugLogDiagPath(const wchar_t* diagName, wchar_t* buf, size_t bufLen);
void  DebugLogDiagWrite(const wchar_t* diagName, const char* content);    // truncate + write
void  DebugLogDiagAppend(const wchar_t* diagName, const char* content);   // append
void  DebugLogDiagTruncate(const wchar_t* diagName);                      // clear file
FILE* DebugLogDiagOpen(const wchar_t* diagName, const wchar_t* mode);     // open for streaming
void  DebugLogClearAll();                                                  // delete all log files
const wchar_t* DebugLogGetDir();                                              // get log/ directory path

// Level-gated logging macros — skip ALL formatting when level is suppressed
#define DLOG_ERROR(fmt, ...)   do { if (g_debugLogLevel >= LOG_ERROR)   DebugLogAFmt(LOG_ERROR, fmt, __VA_ARGS__); } while(0)
#define DLOG_WARN(fmt, ...)    do { if (g_debugLogLevel >= LOG_WARN)    DebugLogAFmt(LOG_WARN, fmt, __VA_ARGS__); } while(0)
#define DLOG_INFO(fmt, ...)    do { if (g_debugLogLevel >= LOG_INFO)    DebugLogAFmt(LOG_INFO, fmt, __VA_ARGS__); } while(0)
#define DLOG_VERBOSE(fmt, ...) do { if (g_debugLogLevel >= LOG_VERBOSE) DebugLogAFmt(LOG_VERBOSE, fmt, __VA_ARGS__); } while(0)

#define DLOGW_ERROR(fmt, ...)   do { if (g_debugLogLevel >= LOG_ERROR)   DebugLogWFmt(LOG_ERROR, fmt, __VA_ARGS__); } while(0)
#define DLOGW_WARN(fmt, ...)    do { if (g_debugLogLevel >= LOG_WARN)    DebugLogWFmt(LOG_WARN, fmt, __VA_ARGS__); } while(0)
#define DLOGW_INFO(fmt, ...)    do { if (g_debugLogLevel >= LOG_INFO)    DebugLogWFmt(LOG_INFO, fmt, __VA_ARGS__); } while(0)
#define DLOGW_VERBOSE(fmt, ...) do { if (g_debugLogLevel >= LOG_VERBOSE) DebugLogWFmt(LOG_VERBOSE, fmt, __VA_ARGS__); } while(0)

// True when diagnostic files (diag_*.txt) should be written
#define DLOG_DIAG_ENABLED() (g_debugLogLevel >= LOG_VERBOSE)

// UTF-8 <-> Wide string conversion helpers
std::wstring UTF8ToWide(const char* utf8);
std::wstring UTF8ToWide(const std::string& utf8);
std::string  WideToUTF8(const wchar_t* wide);
std::string  WideToUTF8(const std::wstring& wide);

#endif