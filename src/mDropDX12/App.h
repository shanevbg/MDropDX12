#pragma once

void PrecompilePresetShaders(std::wstring& wLine, std::wofstream& compiledList, int& compiledShaders);

// IPC window thread — extern state for settings UI
extern std::atomic<bool> g_bIPCRunning;
extern WCHAR g_szIPCWindowTitle[256];
extern WCHAR g_szLastIPCMessage[2048];
extern WCHAR g_szLastIPCTime[16];
extern std::atomic<int> g_lastIPCMessageSeq;
void StartIPCThread(HINSTANCE hInst);
void StopIPCThread();
