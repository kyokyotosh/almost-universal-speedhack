// harness.cpp  —  proves the hooks AND the real shared-memory control path.
//
// LoadLibrary loads speedhack.dll into this process (so every timing API is
// hooked AND the DLL creates a control section for our PID). We then open that
// section as a ControlWriter — exactly what the Qt app does — and push 4.0.
// No SetSpeed export anymore: this is the production control path.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>   // timeGetTime
#include <cstdio>

#include "speedhack_ipc.h"

int main()
{
    HMODULE dll = LoadLibraryW(L"speedhack.dll");
    if (!dll) {
        printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }

    // Open the control section the DLL created for THIS process.
    shipc::ControlWriter ctl;
    if (!ctl.open(GetCurrentProcessId())) {
        printf("control channel open failed\n");
        return 1;
    }

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
            printf("--- ctl.setSpeed(4.0) via shared memory ---\n");
            ctl.setSpeed(4.0);
        }
        Sleep(500);  // a REAL half-second every iteration
    }

    FreeLibrary(dll);
    return 0;
}