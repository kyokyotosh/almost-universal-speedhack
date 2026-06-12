// speedhack_ipc.h  —  shared contract between the DLL and any controller.
//
// Both ends include this so the struct layout is guaranteed identical.
// The DLL is the "server" (creates the section); the harness and the Qt app
// are "writers" (open it and push a new multiplier).

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cwchar>

namespace shipc {

constexpr uint32_t kMagic = 0x53504844;   // 'SPHD' — sanity check on attach

#pragma pack(push, 8)
struct Control {
    uint32_t     magic;        // must equal kMagic
    volatile LONG version;     // writer bumps this on every change
    double       multiplier;   // desired speed
};
#pragma pack(pop)
static_assert(sizeof(Control) == 16, "Control layout changed — both ends must match");

// Per-process section name so the controller can target a specific game.
// Local\ namespace = current session, no special privileges needed.
inline void makeName(DWORD pid, wchar_t* out, size_t cch)
{
    swprintf(out, cch, L"Local\\Speedhack_%lu", pid);
}

// ---------------------------------------------------------------------------
// Writer side — used by the harness AND the Qt app. Identical code both places.
// ---------------------------------------------------------------------------
class ControlWriter {
public:
    ~ControlWriter() { close(); }

    // Attach to the section the DLL created for `pid`. The DLL builds it on a
    // worker thread just after injection, so we retry briefly to cover the gap.
    bool open(DWORD pid, int retryMs = 2000)
    {
        wchar_t name[64];
        makeName(pid, name, 64);

        constexpr int step = 25;
        for (int waited = 0; ; waited += step) {
            map_ = OpenFileMappingW(FILE_MAP_WRITE, FALSE, name);
            if (map_) break;
            if (waited >= retryMs) return false;
            Sleep(step);
        }

        ctl_ = static_cast<Control*>(
            MapViewOfFile(map_, FILE_MAP_WRITE, 0, 0, sizeof(Control)));
        if (!ctl_) { close(); return false; }

        if (ctl_->magic != kMagic) { close(); return false; }   // wrong/partial section
        return true;
    }

    // Write multiplier, THEN bump version — order matters across processes.
    void setSpeed(double m)
    {
        if (!ctl_) return;
        ctl_->multiplier = m;
        MemoryBarrier();                       // multiplier visible before version
        InterlockedIncrement(&ctl_->version);  // signals the DLL's watcher
    }

    bool valid() const { return ctl_ != nullptr; }

    void close()
    {
        if (ctl_) { UnmapViewOfFile(ctl_); ctl_ = nullptr; }
        if (map_) { CloseHandle(map_);     map_ = nullptr; }
    }

private:
    HANDLE   map_ = nullptr;
    Control* ctl_ = nullptr;
};

} // namespace shipc
