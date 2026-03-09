// NSEEL host stubs — provides mutex callbacks required by ns-eel2.
// These are called during VM allocation, variable registration, and RAM operations.

#include <windows.h>

static CRITICAL_SECTION g_eelMutex;
static bool g_eelMutexInit = false;

extern "C" {

void NSEEL_HOSTSTUB_EnterMutex()
{
    if (!g_eelMutexInit) {
        InitializeCriticalSection(&g_eelMutex);
        g_eelMutexInit = true;
    }
    EnterCriticalSection(&g_eelMutex);
}

void NSEEL_HOSTSTUB_LeaveMutex()
{
    LeaveCriticalSection(&g_eelMutex);
}

} // extern "C"
