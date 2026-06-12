// speedhack.cpp  —  full timing-API hook set
//
// Hooks GetTickCount, GetTickCount64, timeGetTime, and QueryPerformanceCounter,
// each with continuity anchoring so changing speed never makes the fake clock
// jump. SetSpeed() is still the control surface for now; the next step swaps it
// for a shared-memory section the Qt app writes to.
//
// Build as a DLL. Depends on MinHook.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>

#include <MinHook.h>

// ---------------------------------------------------------------------------
// Shared control inputs (later: shared memory; for now: SetSpeed export)
// ---------------------------------------------------------------------------
static std::atomic<double>   g_multiplier{1.0};
static std::atomic<uint32_t> g_version{0};   // bumped on every change

// ---------------------------------------------------------------------------
// One anchor per timing source. T is the source's native width:
//   uint32_t for GetTickCount / timeGetTime (wrap-safe in 32-bit)
//   uint64_t for GetTickCount64 / QueryPerformanceCounter (never wraps)
// ---------------------------------------------------------------------------
template <typename T>
struct Anchor {
    CRITICAL_SECTION lock;
    T        realAtAnchor = 0;   // real reading when speed last changed
    T        fakeAtAnchor = 0;   // fake reading we reported at that instant
    double   mult         = 1.0; // multiplier in force since the anchor
    uint32_t seenVersion  = 0;

    void init(T now) {
        InitializeCriticalSection(&lock);
        realAtAnchor = now;
        fakeAtAnchor = now;
        mult         = 1.0;
        seenVersion  = g_version.load(std::memory_order_acquire);
    }
};

// On a speed change we freeze the fake
// clock at its current value (computed with the OLD multiplier), then carry on
// from there — so the output is continuous across the change.
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

static Anchor<uint32_t> g_gtc;     // GetTickCount
static Anchor<uint64_t> g_gtc64;   // GetTickCount64
static Anchor<uint32_t> g_tgt;     // timeGetTime
static Anchor<uint64_t> g_qpc;     // QueryPerformanceCounter

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

// The QPC wrinkle: the result goes through a caller-supplied pointer. We read the real
// counter into our own local, compute the scaled value, then write it once to
// the caller's buffer — and guard against a null pointer. Return value is
// passed through untouched.
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
// Control surface (temporary — replaced by shared memory next step)
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) void SetSpeed(double multiplier)
{
    g_multiplier.store(multiplier, std::memory_order_relaxed);
    g_version.fetch_add(1, std::memory_order_release);
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
    if (!t) return true;   // absent on very old Windows — not fatal, just skip
    if (MH_CreateHook(t, reinterpret_cast<void *>(&hk_GetTickCount64),
                      reinterpret_cast<void **>(&real_GetTickCount64)) != MH_OK)
        return false;
    g_gtc64.init(real_GetTickCount64());
    return true;
}

static bool createAndSeed_TGT()
{
    HMODULE winmm = LoadLibraryW(L"winmm.dll");
    if (!winmm) return true;   // no winmm — skip timeGetTime
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

    // Create + seed each hook (trampolines populated, hooks still disabled).
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
    }
    return TRUE;
}