#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>

struct InjectResult {
    bool         ok = false;
    std::wstring message;
};

class Injector {
public:
    // LoadLibraryW(dllPath) inside the target process.
    // The injector and the target MUST be the same bitness (x64<->x64).
    static InjectResult inject(DWORD pid, const std::wstring& dllPath);

    // Grant this process SeDebugPrivilege so OpenProcess can reach more
    // targets. Call once at startup. Returns false if it couldn't be enabled
    // (often fine for same-user, non-elevated targets).
    static bool enableDebugPrivilege();
};
