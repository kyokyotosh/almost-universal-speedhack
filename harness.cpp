// harness.cpp  —  proves the DLL works WITHOUT the injector.
//
// LoadLibrary loads speedhack.dll into THIS process, so the GetTickCount
// calls below get hooked. Watch the delta between prints: it starts at
// ~500ms (1x), then accelerates after SetSpeed(4.0) — and crucially the
// printed value keeps climbing smoothly, never jumps backward. That smooth
// transition is the continuity anchoring doing its job.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

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

    Sleep(200);  // give the DLL's init thread a moment to install the hook

    DWORD prev = GetTickCount();
    for (int i = 0; i < 20; ++i) {
        const DWORD now = GetTickCount();
        printf("tick=%-10lu  delta=%lu\n", now, now - prev);
        prev = now;

        if (i == 5) {
            printf("--- SetSpeed(4.0) ---\n");
            SetSpeed(4.0);
        }
        Sleep(500);  // REAL half-second every iteration
    }

    FreeLibrary(dll);
    return 0;
}