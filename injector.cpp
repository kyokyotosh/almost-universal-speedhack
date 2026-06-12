#include "injector.h"

#include <memory>

namespace {

// --- RAII: a Win32 HANDLE that closes itself ------------------------------
struct HandleDeleter {
    void operator()(HANDLE h) const {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
};
// HANDLE is void*, so unique_ptr<void, Deleter> models it exactly. "Empty"
// is nullptr — and OpenProcess / CreateRemoteThread / VirtualAllocEx all
// return NULL (not INVALID_HANDLE_VALUE) on failure, so the nullptr check
// in unique_ptr is the right one.
using UniqueHandle = std::unique_ptr<void, HandleDeleter>;

// --- RAII: a block of memory inside ANOTHER process -----------------------
// Carries the process handle + address so it can VirtualFreeEx itself when
// it goes out of scope — no manual cleanup at any return path.
class RemoteMemory {
public:
    RemoteMemory(HANDLE proc, SIZE_T size)
        : proc_(proc),
          addr_(VirtualAllocEx(proc, nullptr, size,
                               MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {}

    ~RemoteMemory() {
        if (addr_)
            VirtualFreeEx(proc_, addr_, 0, MEM_RELEASE);
    }

    RemoteMemory(const RemoteMemory&)            = delete;
    RemoteMemory& operator=(const RemoteMemory&) = delete;

    void* get() const { return addr_; }
    explicit operator bool() const { return addr_ != nullptr; }

private:
    HANDLE proc_;
    void*  addr_;
};

// --- small helper: "<step> failed (err N)" --------------------------------
InjectResult fail(const wchar_t* step, DWORD err)
{
    wchar_t buf[160];
    swprintf(buf, 160, L"%ls failed (err %lu)", step, err);
    return { false, buf };
}

} // namespace

// ---------------------------------------------------------------------------
InjectResult Injector::inject(DWORD pid, const std::wstring& dllPath)
{
    // 1. Open the target with exactly the rights we need.
    UniqueHandle proc{ OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION  | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid) };
    if (!proc)
        return fail(L"OpenProcess", GetLastError());

    // 2. Find LoadLibraryW. kernel32 loads at the same base in every process
    //    of the same bitness, so this address is valid in the target too.
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLib = GetProcAddress(k32, "LoadLibraryW");
    if (!loadLib)
        return fail(L"GetProcAddress(LoadLibraryW)", GetLastError());

    // 3. Allocate space in the target and write the DLL path into it.
    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    RemoteMemory mem{ proc.get(), bytes };
    if (!mem)
        return fail(L"VirtualAllocEx", GetLastError());

    if (!WriteProcessMemory(proc.get(), mem.get(),
                            dllPath.c_str(), bytes, nullptr))
        return fail(L"WriteProcessMemory", GetLastError());

    // 4. Run LoadLibraryW(path) on a new thread in the target.
    UniqueHandle thread{ CreateRemoteThread(
        proc.get(), nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLib),
        mem.get(), 0, nullptr) };
    if (!thread)
        return fail(L"CreateRemoteThread", GetLastError());

    // 5. Wait for the load to finish (cap it so a wedged target can't hang us).
    if (WaitForSingleObject(thread.get(), 10000) != WAIT_OBJECT_0)
        return fail(L"remote thread timed out", 0);

    // The thread's exit code is LoadLibraryW's return TRUNCATED to 32 bits.
    // Zero means the load failed; non-zero is success. (The real confirmation
    // is the control channel connecting afterwards — a truncated-to-zero
    // HMODULE is astronomically unlikely but this is why we don't trust the
    // exact value, only zero/non-zero.)
    DWORD exitCode = 0;
    GetExitCodeThread(thread.get(), &exitCode);
    if (exitCode == 0)
        return fail(L"LoadLibraryW returned NULL in target (wrong bitness or bad path?)", 0);

    return { true, L"Injected." };
    // proc, thread, and the remote DLL-path allocation all release here,
    // in reverse order, automatically.
}

// ---------------------------------------------------------------------------
bool Injector::enableDebugPrivilege()
{
    UniqueHandle token;
    {
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(),
                              TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &raw))
            return false;
        token.reset(raw);
    }

    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid))
        return false;

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount           = 1;
    tp.Privileges[0].Luid       = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(token.get(), FALSE, &tp, sizeof(tp), nullptr, nullptr))
        return false;

    // AdjustTokenPrivileges "succeeds" even if it couldn't assign the
    // privilege — the real status is in GetLastError.
    return GetLastError() == ERROR_SUCCESS;
}
