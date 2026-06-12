// speedhack.cpp  —  full timing-API hook set, driven by shared memory.
//
// Hooks GetTickCount, GetTickCount64, timeGetTime, and QueryPerformanceCounter,
// each with continuity anchoring. Speed is now controlled through a named
// shared-memory section (see speedhack_ipc.h) instead of an export: a watcher
// thread polls the section and pushes changes into the hook state.
//
// Build as a DLL. Depends on MinHook.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>

#include <MinHook.h>
#include "speedhack_ipc.h"

// ---------------------------------------------------------------------------
// Hook-internal control state (fed by the watcher below)
// ---------------------------------------------------------------------------
static std::atomic<double>   g_multiplier{1.0};
static std::atomic<uint32_t> g_version{0};   // bumped to trigger per-source re-anchor

// ---------------------------------------------------------------------------
// One anchor per timing source. T = the source's native width.
// ---------------------------------------------------------------------------
template <typename T>
struct Anchor {
    CRITICAL_SECTION lock;
    T        realAtAnchor = 0;
    T        fakeAtAnchor = 0;
    double   mult         = 1.0;
    uint32_t seenVersion  = 0;

    void init(T now) {
        InitializeCriticalSection(&lock);
        realAtAnchor = now;
        fakeAtAnchor = now;
        mult         = 1.0;
        seenVersion  = g_version.load(std::memory_order_acquire);
    }
};

template <typename T>
static T computeFake(Anchor<T>& a, T nowReal)
{
    EnterCriticalSection(&a.lock);

    const uint32_t ver = g_version.load(std::memory_order_acquire);
    if (ver != a.seenVersion) {
        const T elapsedOld = nowReal - a.realAtAnchor;                  // wrap-safe for uint32_t
        a.fakeAtAnchor += static_cast<T>(elapsedOld * a.mult);
        a.realAtAnchor  = nowReal;
        a.mult          = g_multiplier.load(std::memory_order_relaxed);
        a.seenVersion   = ver;
    }

    const T elapsed = nowReal - a.realAtAnchor;
    const T result  = a.fakeAtAnchor + static_cast<T>(elapsed * a.mult);

    LeaveCriticalSection(&a.lock);
    return result;
}

// ---------------------------------------------------------------------------
// Real function pointers + anchors
// ---------------------------------------------------------------------------
using GetTickCount_t   = DWORD(WINAPI *)();
using GetTickCount64_t = ULONGLONG(WINAPI *)();
using timeGetTime_t    = DWORD(WINAPI *)();
using QPC_t            = BOOL(WINAPI *)(LARGE_INTEGER *);

static GetTickCount_t   real_GetTickCount   = nullptr;
static GetTickCount64_t real_GetTickCount64 = nullptr;
static timeGetTime_t    real_timeGetTime    = nullptr;
static QPC_t            real_QPC            = nullptr;

static Anchor<uint32_t> g_gtc;
static Anchor<uint64_t> g_gtc64;
static Anchor<uint32_t> g_tgt;
static Anchor<uint64_t> g_qpc;

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
static DWORD WINAPI hk_GetTickCount()
{
    return static_cast<DWORD>(computeFake<uint32_t>(g_gtc, real_GetTickCount()));
}

static ULONGLONG WINAPI hk_GetTickCount64()
{
    return static_cast<ULONGLONG>(
        computeFake<uint64_t>(g_gtc64, real_GetTickCount64()));
}

static DWORD WINAPI hk_timeGetTime()
{
    return static_cast<DWORD>(computeFake<uint32_t>(g_tgt, real_timeGetTime()));
}

static BOOL WINAPI hk_QueryPerformanceCounter(LARGE_INTEGER *lpCount)
{
    LARGE_INTEGER real;
    const BOOL ok = real_QPC(&real);
    if (!ok)
        return ok;

    const uint64_t fake =
        computeFake<uint64_t>(g_qpc, static_cast<uint64_t>(real.QuadPart));

    if (lpCount)
        lpCount->QuadPart = static_cast<LONGLONG>(fake);

    return ok;
}

// ---------------------------------------------------------------------------
// Shared-memory control channel (server side)
// ---------------------------------------------------------------------------
static HANDLE          g_map = nullptr;
static shipc::Control* g_ctl = nullptr;

static bool createControl()
{
    wchar_t name[64];
    shipc::makeName(GetCurrentProcessId(), name, 64);

    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                               0, sizeof(shipc::Control), name);
    if (!g_map)
        return false;

    g_ctl = static_cast<shipc::Control*>(
        MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(shipc::Control)));
    if (!g_ctl)
        return false;

    // Initialize only if we created it fresh (re-injection would re-attach).
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        g_ctl->multiplier = 1.0;
        g_ctl->version    = 0;
        MemoryBarrier();
        g_ctl->magic      = shipc::kMagic;   // publish magic last
    }
    return true;
}

// Polls the section ~30ms and translates an IPC version bump into a hook
// re-anchor. 30ms latency on a speed change is imperceptible and keeps the
// hot path (the hooks themselves) free of any shared-memory access.
static DWORD WINAPI controlWatcher(LPVOID)
{
    LONG lastSeen = g_ctl->version;
    for (;;) {
        const LONG v = g_ctl->version;
        MemoryBarrier();
        if (v != lastSeen) {
            const double m = g_ctl->multiplier;
            lastSeen = v;
            g_multiplier.store(m, std::memory_order_relaxed);
            g_version.fetch_add(1, std::memory_order_release);
        }
        Sleep(30);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Install — on a worker thread, off the loader lock.
// ---------------------------------------------------------------------------
static bool createAndSeed_GTC()
{
    void *t = reinterpret_cast<void *>(
        GetProcAddress(GetModuleHandleW(L"kernel32"), "GetTickCount"));
    if (!t) return false;
    if (MH_CreateHook(t, reinterpret_cast<void *>(&hk_GetTickCount),
                      reinterpret_cast<void **>(&real_GetTickCount)) != MH_OK)
        return false;
    g_gtc.init(real_GetTickCount());
    return true;
}

static bool createAndSeed_GTC64()
{
    void *t = reinterpret_cast<void *>(
        GetProcAddress(GetModuleHandleW(L"kernel32"), "GetTickCount64"));
    if (!t) return true;   // absent on very old Windows — skip, not fatal
    if (MH_CreateHook(t, reinterpret_cast<void *>(&hk_GetTickCount64),
                      reinterpret_cast<void **>(&real_GetTickCount64)) != MH_OK)
        return false;
    g_gtc64.init(real_GetTickCount64());
    return true;
}

static bool createAndSeed_TGT()
{
    HMODULE winmm = LoadLibraryW(L"winmm.dll");
    if (!winmm) return true;
    void *t = reinterpret_cast<void *>(GetProcAddress(winmm, "timeGetTime"));
    if (!t) return true;
    if (MH_CreateHook(t, reinterpret_cast<void *>(&hk_timeGetTime),
                      reinterpret_cast<void **>(&real_timeGetTime)) != MH_OK)
        return false;
    g_tgt.init(real_timeGetTime());
    return true;
}

static bool createAndSeed_QPC()
{
    void *t = reinterpret_cast<void *>(
        GetProcAddress(GetModuleHandleW(L"kernel32"), "QueryPerformanceCounter"));
    if (!t) return false;
    if (MH_CreateHook(t, reinterpret_cast<void *>(&hk_QueryPerformanceCounter),
                      reinterpret_cast<void **>(&real_QPC)) != MH_OK)
        return false;
    LARGE_INTEGER now;
    real_QPC(&now);
    g_qpc.init(static_cast<uint64_t>(now.QuadPart));
    return true;
}

static DWORD WINAPI initThread(LPVOID)
{
    if (MH_Initialize() != MH_OK)
        return 1;

    bool ok = true;
    ok &= createAndSeed_GTC();
    ok &= createAndSeed_GTC64();
    ok &= createAndSeed_TGT();
    ok &= createAndSeed_QPC();

    if (!ok) {
        OutputDebugStringW(L"[speedhack] hook setup failed.");
        return 1;
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        OutputDebugStringW(L"[speedhack] MH_EnableHook failed.");
        return 1;
    }
    OutputDebugStringW(L"[speedhack] timing hooks installed.");

    if (createControl()) {
        HANDLE w = CreateThread(nullptr, 0, controlWatcher, nullptr, 0, nullptr);
        if (w) CloseHandle(w);
        OutputDebugStringW(L"[speedhack] control channel ready.");
    } else {
        OutputDebugStringW(L"[speedhack] control channel failed (stuck at 1x).");
    }
    return 0;
}

// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HANDLE t = CreateThread(nullptr, 0, initThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        if (g_ctl) UnmapViewOfFile(g_ctl);
        if (g_map) CloseHandle(g_map);
    }
    return TRUE;
}