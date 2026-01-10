#include <Windows.h>

namespace {

using GetSystemTimePreciseAsFileTimeFn = VOID(WINAPI*)(LPFILETIME);

static INIT_ONCE                        g_init_once = INIT_ONCE_STATIC_INIT;
static GetSystemTimePreciseAsFileTimeFn g_resolved  = nullptr;

BOOL WINAPI InitOnceResolvePreciseTime(PINIT_ONCE, PVOID, PVOID*) {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) {
        return TRUE;
    }

    g_resolved =
        reinterpret_cast<GetSystemTimePreciseAsFileTimeFn>(GetProcAddress(kernel32, "GetSystemTimePreciseAsFileTime"));
    return TRUE;
}

VOID WINAPI CompatGetSystemTimePreciseAsFileTime(LPFILETIME file_time) {
    InitOnceExecuteOnce(&g_init_once, InitOnceResolvePreciseTime, nullptr, nullptr);

    if (g_resolved) {
        g_resolved(file_time);
        return;
    }

    // Windows 7 fallback (Win8+ API missing): use the coarse system time API.
    GetSystemTimeAsFileTime(file_time);
}

} // namespace

// MSVC generates calls through the import address table symbol `__imp_*` when a
// WinAPI function is declared as `__declspec(dllimport)`. Windows 7 doesn't
// export `GetSystemTimePreciseAsFileTime`, so importing it causes the DLL to
// fail to load. Defining the `__imp_` symbol locally prevents a hard import and
// lets us provide a runtime-resolved implementation.
extern "C" {
__declspec(selectany) GetSystemTimePreciseAsFileTimeFn __imp_GetSystemTimePreciseAsFileTime =
    &CompatGetSystemTimePreciseAsFileTime;
}

#if defined(_M_IX86)
// 32-bit builds use stdcall name decoration for the import pointer symbol.
// Alias `__imp__GetSystemTimePreciseAsFileTime@4` to our undecorated symbol.
#pragma comment(linker, "/alternatename:__imp__GetSystemTimePreciseAsFileTime@4=__imp_GetSystemTimePreciseAsFileTime")
#endif
