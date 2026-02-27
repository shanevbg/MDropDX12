#pragma once

void PrecompilePresetShaders(std::wstring& wLine, std::wofstream& compiledList, int& compiledShaders);

// IPC window thread — extern state for settings UI
extern std::atomic<bool> g_bIPCRunning;
extern WCHAR g_szIPCWindowTitle[256];
void StartIPCThread(HINSTANCE hInst);
void StopIPCThread();
