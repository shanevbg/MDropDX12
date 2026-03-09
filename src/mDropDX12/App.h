#pragma once

void PrecompilePresetShaders(std::wstring& wLine, std::wofstream& compiledList, int& compiledShaders);

// Named pipe IPC — extern state for settings UI
#include "pipe_server.h"
extern PipeServer g_pipeServer;
extern WCHAR g_szLastIPCMessage[2048];
extern WCHAR g_szLastIPCTime[16];
extern std::atomic<int> g_lastIPCMessageSeq;
