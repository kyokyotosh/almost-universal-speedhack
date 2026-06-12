// speedhack.cpp  —  v0 payload DLL
//
// Hooks ONE timing API (GetTickCount) with proper continuity anchoring,
// so toggling/changing speed never makes the fake clock jump backward.
// This is the teachable core; the full timing family + shared-memory
// control replace the SetSpeed() export in the next step.
//
// Build as a DLL. Depends on MinHook (https://github.com/TsudaKageyu/minhook).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>

#include <MinHook.h>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
using GetTickCount_t = DWORD(WINAPI *)();
static GetTickCount_t real_GetTickCount = nullptr;

static CRITICAL_SECTION g_gtcLock;

// Continuity anchors (the heart of the Pascal port):
//   g_realAtAnchor  = real GetTickCount() when the speed last changed
//   g_fakeAtAnchor  = the fake value we were reporting at that instant
//   g_gtcMult       = multiplier in force since that anchor
static DWORD   g_realAtAnchor = 0;
static DWORD   g_fakeAtAnchor = 0;
static double  g_gtcMult      = 1.0;

// Control inputs (later fed by shared memory; for now by the SetSpeed export).
static std::atomic<double>   g_multiplier{1.0};
static std::atomic<uint32_t> g_version{0};   // bumped on every change
static uint32_t              g_seenVersion = 0;

// ---------------------------------------------------------------------------
// The hook
// ---------------------------------------------------------------------------
static DWORD WINAPI hk_GetTickCount()
{
    EnterCriticalSection(&g_gtcLock);

    const DWORD nowReal = real_GetTickCount();

    // Did the speed change since we last ran? Re-anchor using the OLD
    // multiplier so the fake clock is continuous across the change.
    const uint32_t ver = g_version.load(std::memory_order_acquire);
    if (ver != g_seenVersion) {
        const DWORD elapsedOld = nowReal - g_realAtAnchor;            // unsigned: wrap-safe
        g_fakeAtAnchor += static_cast<DWORD>(elapsedOld * g_gtcMult); // freeze fake clock here
        g_realAtAnchor  = nowReal;
        g_gtcMult       = g_multiplier.load(std::memory_order_relaxed);
        g_seenVersion   = ver;
    }

    const DWORD elapsed = nowReal - g_realAtAnchor;
    const DWORD result  = g_fakeAtAnchor + static_cast<DWORD>(elapsed * g_gtcMult);

    LeaveCriticalSection(&g_gtcLock);
    return result;
}

// ---------------------------------------------------------------------------
// Control surface (temporary — replaced by shared memory next step)
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) void SetSpeed(double multiplier)
{
    g_multiplier.store(multiplier, std::memory_order_relaxed);
    g_version.fetch_add(1, std::memory_order_release);   // triggers re-anchor in the hook
}

// ---------------------------------------------------------------------------
// Hook install — runs on a worker thread, NOT under loader lock.
// ---------------------------------------------------------------------------
static DWORD WINAPI initThread(LPVOID)
{
    if (MH_Initialize() != MH_OK)
        return 1;

    void *target = reinterpret_cast<void *>(
        GetProcAddress(GetModuleHandleW(L"kernel32"), "GetTickCount"));
    if (!target)
        return 1;

    // Seed anchors from the real clock BEFORE the hook goes live.
    InitializeCriticalSection(&g_gtcLock);
    const DWORD now = reinterpret_cast<GetTickCount_t>(target)();
    g_realAtAnchor = now;
    g_fakeAtAnchor = now;
    g_gtcMult      = 1.0;

    if (MH_CreateHook(target,
                      reinterpret_cast<void *>(&hk_GetTickCount),
                      reinterpret_cast<void **>(&real_GetTickCount)) != MH_OK)
        return 1;

    if (MH_EnableHook(target) != MH_OK)
        return 1;

    OutputDebugStringW(L"[speedhack] GetTickCount hooked.");
    return 0;
}

// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        // Do the real work off the loader lock.
        HANDLE t = CreateThread(nullptr, 0, initThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        MH_Uninitialize();
    }
    return TRUE;
}