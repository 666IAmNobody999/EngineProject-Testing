/*
  Pikafish, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

  Pikafish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Pikafish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "winperf.h"

#if !defined(_WIN32)

namespace Stockfish::WinPerf {

void apply_process_tuning() {}
void acquire_power_plan() {}
void release_power_plan() {}
bool run_power_watcher(int, char*[]) { return false; }

}  // namespace Stockfish::WinPerf

#else

    #if !defined(NOMINMAX)
        #define NOMINMAX
    #endif
    #if !defined(WIN32_LEAN_AND_MEAN)
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>

    #include <atomic>
    #include <cstdint>
    #include <cstdlib>
    #include <cstring>
    #include <fstream>
    #include <string>
    #include <vector>

namespace Stockfish::WinPerf {

namespace {

// All system calls below are resolved at run time, so the engine still starts on
// Windows versions (or SDKs) that lack one of them, and no extra import library
// is needed for any toolchain. A failing call is silently ignored.
template<typename F>
F load(const wchar_t* dll, const char* name) {
    HMODULE mod = GetModuleHandleW(dll);
    if (!mod)
        mod = LoadLibraryW(dll);
    F       fn   = nullptr;
    FARPROC proc = mod ? GetProcAddress(mod, name) : nullptr;
    if (proc)
        std::memcpy(&fn, &proc, sizeof(fn));
    return fn;
}

// ---------------------------------------------------------------------------
// Process tuning: I/O priority, memory priority, dynamic priority boost
// ---------------------------------------------------------------------------

using NtSetInformationProcessFn = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG);
using SetProcessInformationFn   = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);

constexpr ULONG ProcessIoPriority     = 33;  // PROCESSINFOCLASS::ProcessIoPriority
constexpr ULONG IoPriorityNormal      = 2;
constexpr ULONG IoPriorityHigh        = 3;
constexpr int   ProcessMemoryPriority = 0;  // PROCESS_INFORMATION_CLASS::ProcessMemoryPriority
constexpr ULONG MemoryPriorityNormal  = 5;  // MEMORY_PRIORITY_NORMAL

bool enable_privilege(const wchar_t* name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount           = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)
           && AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr)
           && GetLastError() == ERROR_SUCCESS;  // ERROR_NOT_ALL_ASSIGNED: not held
    CloseHandle(token);
    return ok;
}

// ---------------------------------------------------------------------------
// Power plan
// ---------------------------------------------------------------------------

using PowerGetActiveSchemeFn  = DWORD(WINAPI*)(HKEY, GUID**);
using PowerSetActiveSchemeFn  = DWORD(WINAPI*)(HKEY, const GUID*);
using PowerEnumerateFn        = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, int, ULONG, UCHAR*, DWORD*);
using PowerReadFriendlyNameFn = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, UCHAR*, DWORD*);
using PowerDuplicateSchemeFn  = DWORD(WINAPI*)(HKEY, const GUID*, GUID**);

constexpr int  AccessScheme  = 16;  // POWER_DATA_ACCESSOR::ACCESS_SCHEME
constexpr GUID UltimateGuid  = {0xe9a42b02, 0xd5df, 0x448d, {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};
constexpr char WatchSwitch[] = "--pikafish-power-watch";

struct PowerApi {
    PowerGetActiveSchemeFn  getActive = load<PowerGetActiveSchemeFn>(L"powrprof.dll", "PowerGetActiveScheme");
    PowerSetActiveSchemeFn  setActive = load<PowerSetActiveSchemeFn>(L"powrprof.dll", "PowerSetActiveScheme");
    PowerEnumerateFn        enumerate = load<PowerEnumerateFn>(L"powrprof.dll", "PowerEnumerate");
    PowerReadFriendlyNameFn readName  = load<PowerReadFriendlyNameFn>(L"powrprof.dll", "PowerReadFriendlyName");
    PowerDuplicateSchemeFn  duplicate = load<PowerDuplicateSchemeFn>(L"powrprof.dll", "PowerDuplicateScheme");

    bool ok() const { return getActive && setActive && enumerate; }
};

PowerApi& power() {
    static PowerApi api;
    return api;
}

bool same(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

bool active_scheme(GUID& out) {
    GUID* p = nullptr;
    if (power().getActive(nullptr, &p) != ERROR_SUCCESS || !p)
        return false;
    out = *p;
    LocalFree(p);
    return true;
}

std::wstring friendly_name(const GUID& g) {
    if (!power().readName)
        return {};
    DWORD size = 0;
    DWORD rc   = power().readName(nullptr, &g, nullptr, nullptr, nullptr, &size);
    if ((rc != ERROR_SUCCESS && rc != ERROR_MORE_DATA) || size < sizeof(wchar_t))
        return {};
    std::vector<UCHAR> buf(size + sizeof(wchar_t), 0);
    if (power().readName(nullptr, &g, nullptr, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return {};
    return std::wstring(reinterpret_cast<const wchar_t*>(buf.data()));
}

std::wstring data_dir() {
    wchar_t buf[MAX_PATH];
    DWORD   n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        n = GetTempPathW(MAX_PATH, buf);
        if (n == 0 || n >= MAX_PATH)
            return {};
    }
    std::wstring dir(buf);
    if (!dir.empty() && dir.back() != L'\\')
        dir += L'\\';
    dir += L"Pikafish";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L'\\';
}

// GUID of the Ultimate Performance copy created by the engine on an earlier run
// (kept so that a hidden template is duplicated only once per machine).
std::wstring created_scheme_file() { return data_dir() + L"ultimate_scheme.guid"; }
std::wstring state_file() { return data_dir() + L"powerplan.state"; }

bool scheme_exists(const GUID& g) {
    for (ULONG i = 0;; ++i)
    {
        GUID  s{};
        DWORD size = sizeof(s);
        if (power().enumerate(nullptr, nullptr, nullptr, AccessScheme, i, reinterpret_cast<UCHAR*>(&s), &size)
            != ERROR_SUCCESS)
            return false;
        if (same(s, g))
            return true;
    }
}

// Finds the "Ultimate Performance" plan, creating it from the built-in template
// when Windows keeps it hidden (the default on most non-workstation editions).
bool find_ultimate(GUID& out) {
    // 1. The template itself is visible (Pro for Workstations, or user-unhidden).
    if (scheme_exists(UltimateGuid))
    {
        out = UltimateGuid;
        return true;
    }

    // 2. A copy this engine created before.
    std::wstring createdFile = created_scheme_file();
    {
        std::ifstream f(createdFile.c_str(), std::ios::binary);
        GUID          g{};
        if (f.read(reinterpret_cast<char*>(&g), sizeof(g)) && scheme_exists(g))
        {
            out = g;
            return true;
        }
    }

    // 3. A copy the user created (powercfg -duplicatescheme), found by name.
    std::vector<std::wstring> names = {L"Ultimate Performance",
                                       L"\u7D42\u6975\u6548\u80FD",  // 終極效能
                                       L"\u5353\u8D8A\u6027\u80FD",  // 卓越性能
                                       L"\u7A76\u6975\u306E\u30D1\u30D5\u30A9\u30FC\u30DE\u30F3\u30B9"};
    std::wstring templateName = friendly_name(UltimateGuid);
    if (!templateName.empty())
        names.push_back(templateName);

    for (ULONG i = 0;; ++i)
    {
        GUID  s{};
        DWORD size = sizeof(s);
        if (power().enumerate(nullptr, nullptr, nullptr, AccessScheme, i, reinterpret_cast<UCHAR*>(&s), &size)
            != ERROR_SUCCESS)
            break;
        std::wstring name = friendly_name(s);
        for (const auto& n : names)
            if (name == n)
            {
                out = s;
                return true;
            }
    }

    // 4. Create it once from the hidden template.
    GUID* dup = nullptr;
    if (!power().duplicate || power().duplicate(nullptr, &UltimateGuid, &dup) != ERROR_SUCCESS || !dup)
        return false;
    out = *dup;
    LocalFree(dup);
    std::ofstream f(createdFile.c_str(), std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(&out), sizeof(out));
    return true;
}

// Shared state of all running engine instances (several engines may run at the
// same time, e.g. in an engine match): the plan to restore and the processes
// still using the Ultimate Performance plan. Guarded by a named mutex.
struct Instance {
    DWORD    pid;
    FILETIME created;
};

struct State {
    GUID                  original{};
    GUID                  ultimate{};
    std::uint32_t         switched = 0;  // the engine changed the plan and must restore it
    std::vector<Instance> instances;
};

constexpr std::uint32_t StateMagic = 0x57504B50;  // "PKPW"

bool read_state(State& s) {
    std::ifstream f(state_file().c_str(), std::ios::binary);
    std::uint32_t magic = 0, n = 0;
    if (!f.read(reinterpret_cast<char*>(&magic), sizeof(magic)) || magic != StateMagic)
        return false;
    f.read(reinterpret_cast<char*>(&s.original), sizeof(s.original));
    f.read(reinterpret_cast<char*>(&s.ultimate), sizeof(s.ultimate));
    f.read(reinterpret_cast<char*>(&s.switched), sizeof(s.switched));
    f.read(reinterpret_cast<char*>(&n), sizeof(n));
    if (!f || n > 4096)
        return false;
    s.instances.resize(n);
    f.read(reinterpret_cast<char*>(s.instances.data()), std::streamsize(n * sizeof(Instance)));
    return bool(f);
}

void write_state(const State& s) {
    std::ofstream f(state_file().c_str(), std::ios::binary | std::ios::trunc);
    std::uint32_t n = std::uint32_t(s.instances.size());
    f.write(reinterpret_cast<const char*>(&StateMagic), sizeof(StateMagic));
    f.write(reinterpret_cast<const char*>(&s.original), sizeof(s.original));
    f.write(reinterpret_cast<const char*>(&s.ultimate), sizeof(s.ultimate));
    f.write(reinterpret_cast<const char*>(&s.switched), sizeof(s.switched));
    f.write(reinterpret_cast<const char*>(&n), sizeof(n));
    f.write(reinterpret_cast<const char*>(s.instances.data()), std::streamsize(n * sizeof(Instance)));
}

bool same_time(const FILETIME& a, const FILETIME& b) {
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}

bool alive(const Instance& in) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, in.pid);
    if (!h)
        return GetLastError() == ERROR_ACCESS_DENIED;  // exists, but not ours to inspect
    FILETIME c, e, k, u;
    bool     ok = WaitForSingleObject(h, 0) == WAIT_TIMEOUT && GetProcessTimes(h, &c, &e, &k, &u)
          && same_time(c, in.created);
    CloseHandle(h);
    return ok;
}

void prune(State& s) {
    std::vector<Instance> live;
    for (const auto& in : s.instances)
        if (alive(in))
            live.push_back(in);
    s.instances.swap(live);
}

class StateLock {
   public:
    StateLock() {
        mutex = CreateMutexW(nullptr, FALSE, L"Local\\Pikafish.PowerPlan");
        if (mutex)
        {
            DWORD rc = WaitForSingleObject(mutex, 10000);
            locked   = rc == WAIT_OBJECT_0 || rc == WAIT_ABANDONED;
        }
    }
    ~StateLock() {
        if (locked)
            ReleaseMutex(mutex);
        if (mutex)
            CloseHandle(mutex);
    }
    explicit operator bool() const { return locked; }

   private:
    HANDLE mutex  = nullptr;
    bool   locked = false;
};

// Removes `pid` from the shared state; the last instance restores the plan.
void release_instance(DWORD pid) {
    if (!power().ok())
        return;
    StateLock lock;
    if (!lock)
        return;
    State s;
    if (!read_state(s))
        return;

    std::vector<Instance> rest;
    for (const auto& in : s.instances)
        if (in.pid != pid)
            rest.push_back(in);
    s.instances.swap(rest);
    prune(s);

    if (!s.instances.empty())
    {
        write_state(s);
        return;
    }

    // Restore only if the plan is still the one the engine selected, so a plan
    // the user picked by hand while the engine was running is kept.
    GUID cur{};
    if (s.switched && active_scheme(cur) && same(cur, s.ultimate))
        power().setActive(nullptr, &s.original);
    DeleteFileW(state_file().c_str());
}

void spawn_watcher(DWORD pid) {
    std::vector<wchar_t> exe(32768);
    DWORD                n = GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size()));
    if (n == 0 || n >= exe.size())
        return;

    std::wstring cmd = L"\"" + std::wstring(exe.data(), n) + L"\" ";
    for (const char* c = WatchSwitch; *c; ++c)
        cmd += wchar_t(*c);
    cmd += L" " + std::to_wstring(pid);

    // No inherited handles: the watcher must not keep the GUI's pipes open.
    // Break away from the GUI's job object when allowed, so that killing the
    // engine's job does not take the watcher with it.
    DWORD flags = CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS;
    for (DWORD extra : {DWORD(CREATE_BREAKAWAY_FROM_JOB), DWORD(0)})
    {
        STARTUPINFOW        si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        std::vector<wchar_t> line(cmd.begin(), cmd.end());
        line.push_back(L'\0');
        if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, flags | extra, nullptr, nullptr,
                           &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return;
        }
    }
}

std::atomic<bool> powerAcquired{false};

BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT)
        release_power_plan();
    return FALSE;  // let the default handling continue
}

}  // namespace

void apply_process_tuning() {
    HANDLE proc = GetCurrentProcess();

    // 1. CPU priority: Above Normal.
    if (GetPriorityClass(proc) != ABOVE_NORMAL_PRIORITY_CLASS)
        SetPriorityClass(proc, ABOVE_NORMAL_PRIORITY_CLASS);

    // 2. Dynamic thread priority boost on (FALSE = boost not disabled).
    BOOL boostDisabled = FALSE;
    if (GetProcessPriorityBoost(proc, &boostDisabled) && boostDisabled)
        SetProcessPriorityBoost(proc, FALSE);

    // 3. I/O priority: High. Windows only grants it with
    //    SeIncreaseBasePriorityPrivilege (an elevated administrator); otherwise
    //    fall back to Normal so that a GUI-lowered priority is still undone.
    static auto  ntSet   = load<NtSetInformationProcessFn>(L"ntdll.dll", "NtSetInformationProcess");
    static ULONG ioLevel = IoPriorityHigh;
    static bool  ioTried = false;
    if (ntSet && ioLevel)
    {
        ULONG io = ioLevel;
        LONG  st = ntSet(proc, ProcessIoPriority, &io, sizeof(io));
        if (st < 0 && !ioTried)
        {
            if (enable_privilege(L"SeIncreaseBasePriorityPrivilege"))
                st = ntSet(proc, ProcessIoPriority, &io, sizeof(io));
            if (st < 0)
            {
                io      = ioLevel = IoPriorityNormal;
                st      = ntSet(proc, ProcessIoPriority, &io, sizeof(io));
                ioLevel = st < 0 ? 0 : IoPriorityNormal;
            }
        }
        ioTried = true;
    }

    // 4. Memory priority: Normal (Windows 8 and later).
    static auto setInfo = load<SetProcessInformationFn>(L"kernel32.dll", "SetProcessInformation");
    if (setInfo)
    {
        ULONG mem = MemoryPriorityNormal;  // MEMORY_PRIORITY_INFORMATION
        setInfo(proc, ProcessMemoryPriority, &mem, sizeof(mem));
    }
}

void acquire_power_plan() {
    if (powerAcquired.load() || !power().ok())
        return;

    Instance self{GetCurrentProcessId(), {}};
    FILETIME e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &self.created, &e, &k, &u))
        return;

    {
        StateLock lock;
        if (!lock)
            return;

        State s;
        bool  hadState = read_state(s);
        prune(s);

        if (s.instances.empty())
        {
            GUID cur{}, ult{};
            if (!active_scheme(cur) || !find_ultimate(ult))
                return;

            // Every engine was killed before it could restore (and its watcher
            // was killed as well): the plan saved then is still the one to restore.
            bool stale = hadState && s.switched && same(s.ultimate, ult) && same(cur, ult);
            if (!stale)
            {
                s          = State{};
                s.original = cur;
                s.ultimate = ult;
                s.switched = !same(cur, ult) && power().setActive(nullptr, &ult) == ERROR_SUCCESS;
            }
        }

        s.instances.push_back(self);
        write_state(s);
        powerAcquired = true;
    }

    std::atexit(release_power_plan);
    SetConsoleCtrlHandler(console_handler, TRUE);
    spawn_watcher(self.pid);
}

void release_power_plan() {
    if (powerAcquired.exchange(false))
        release_instance(GetCurrentProcessId());
}

bool run_power_watcher(int argc, char* argv[]) {
    if (argc < 3 || std::strcmp(argv[1], WatchSwitch) != 0)
        return false;

    DWORD pid = DWORD(std::strtoul(argv[2], nullptr, 10));
    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid))
    {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
    release_instance(pid);
    return true;
}

}  // namespace Stockfish::WinPerf

#endif  // _WIN32
