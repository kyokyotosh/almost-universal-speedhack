// harness.cpp  —  proves ALL four hooks at once, without an injector.
//
// LoadLibrary loads speedhack.dll into this process, hooking every timing API.
// Each column below should advance ~500 per real half-second before the call,
// then ~2000 after SetSpeed(4.0). Watch that they all move together and that
// none jumps backward at the transition.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <mmsystem.h>

using SetSpeed_t = void(*)(double);

int main()
{
    HMODULE dll = LoadLibraryW(L"speedhack.dll");
    if (!dll) {
        printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }

    auto SetSpeed = reinterpret_cast<SetSpeed_t>(GetProcAddress(dll, "SetSpeed"));
    if (!SetSpeed) {
        printf("SetSpeed export not found\n");
        return 1;
    }

    Sleep(200);  // let the DLL's init thread install the hooks

    LARGE_INTEGER freq, qpcPrev;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&qpcPrev);
    DWORD     gtcPrev   = GetTickCount();
    ULONGLONG gtc64Prev = GetTickCount64();
    DWORD     tgtPrev   = timeGetTime();

    printf("%-12s %-12s %-12s %-12s\n", "GTC", "GTC64", "timeGetTime", "QPC(ms)");

    for (int i = 0; i < 18; ++i) {
        const DWORD     gtc   = GetTickCount();
        const ULONGLONG gtc64 = GetTickCount64();
        const DWORD     tgt   = timeGetTime();
        LARGE_INTEGER   qpc;  QueryPerformanceCounter(&qpc);

        const double qpcMs =
            double(qpc.QuadPart - qpcPrev.QuadPart) * 1000.0 / double(freq.QuadPart);

        printf("%-12lu %-12llu %-12lu %-12.0f\n",
               gtc - gtcPrev, gtc64 - gtc64Prev, tgt - tgtPrev, qpcMs);

        gtcPrev = gtc; gtc64Prev = gtc64; tgtPrev = tgt; qpcPrev = qpc;

        if (i == 5) {
            printf("--- SetSpeed(4.0) ---\n");
            SetSpeed(4.0);
        }
        Sleep(500);  // a REAL half-second every iteration
    }

    FreeLibrary(dll);
    return 0;
}