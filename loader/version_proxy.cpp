// version.dll proxy = the Wheat mod loader's entry point inside CS2 (Windows build, run under Proton).
//
// How it gets loaded: the launch wrapper (launch/wheat-launch.sh) sets WINEDLLOVERRIDES="version=n,b"
// ONLY when the game is started with -insecure. Wine then prefers this file (placed next to cs2.exe)
// over its own version.dll. On a normal launch the override is absent and this file is never loaded.
//
// Defense in depth: even if it IS loaded (override set by hand, wrapper bypassed), it does nothing but
// forward calls unless ALL of these hold:
//     1. the host process is cs2.exe
//     2. the process command line carries an unquoted -insecure and no -secure
// Anything else => pure pass-through, no thread, no sandbox, no files touched.
//
// Build: MinGW-w64 only (the export thunks are GNU assembler).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "insecure_gate.hpp"
#include "mod_host.hpp"

namespace fs = std::filesystem;
using namespace modloader;

// ------------------------------------------------------------------ export forwarding
// Every export is a one-instruction thunk: jmp *g_real[i]. The real function is looked up in the
// system's own version.dll (loaded by full path, so we never resolve to ourselves).

extern "C" {
void* g_real[17] = {};
}

#define WHEAT_THUNK(idx, name) \
    asm(".text\n.p2align 4\n.globl " #name "\n" #name ":\n\tjmp *g_real+" #idx "*8(%rip)\n");

WHEAT_THUNK(0, GetFileVersionInfoA)
WHEAT_THUNK(1, GetFileVersionInfoByHandle)
WHEAT_THUNK(2, GetFileVersionInfoExA)
WHEAT_THUNK(3, GetFileVersionInfoExW)
WHEAT_THUNK(4, GetFileVersionInfoSizeA)
WHEAT_THUNK(5, GetFileVersionInfoSizeExA)
WHEAT_THUNK(6, GetFileVersionInfoSizeExW)
WHEAT_THUNK(7, GetFileVersionInfoSizeW)
WHEAT_THUNK(8, GetFileVersionInfoW)
WHEAT_THUNK(9, VerFindFileA)
WHEAT_THUNK(10, VerFindFileW)
WHEAT_THUNK(11, VerInstallFileA)
WHEAT_THUNK(12, VerInstallFileW)
WHEAT_THUNK(13, VerLanguageNameA)
WHEAT_THUNK(14, VerLanguageNameW)
WHEAT_THUNK(15, VerQueryValueA)
WHEAT_THUNK(16, VerQueryValueW)

static const char* const kExportNames[17] = {
    "GetFileVersionInfoA",        "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
    "GetFileVersionInfoExW",      "GetFileVersionInfoSizeA",    "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW",  "GetFileVersionInfoSizeW",    "GetFileVersionInfoW",
    "VerFindFileA",               "VerFindFileW",               "VerInstallFileA",
    "VerInstallFileW",            "VerLanguageNameA",           "VerLanguageNameW",
    "VerQueryValueA",             "VerQueryValueW"};

static bool load_real_version() {
    wchar_t dir[MAX_PATH];
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) return false;
    const std::wstring path = std::wstring(dir, n) + L"\\version.dll";
    HMODULE real = LoadLibraryW(path.c_str());
    if (!real) return false;
    for (int i = 0; i < 17; ++i) g_real[i] = reinterpret_cast<void*>(GetProcAddress(real, kExportNames[i]));
    return true;
}

// ------------------------------------------------------------------ gate
static std::wstring g_game_dir;
static std::atomic<bool> g_stop{false};

static void dbg(const wchar_t* msg) { OutputDebugStringW(msg); }

// Returns true (and fills g_game_dir) only for cs2.exe launched with -insecure.
static bool allowed_to_start() {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        dbg(L"[wheat] refused: cannot determine host executable");
        return false;
    }
    std::wstring_view path(exe, n);
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring_view base = (slash == std::wstring_view::npos) ? path : path.substr(slash + 1);
    if (!equals_ascii_nocase(base, "cs2.exe")) {
        dbg(L"[wheat] refused: host process is not cs2.exe");
        return false;
    }
    switch (check_launch_gate(GetCommandLineW())) {
        case GateResult::Allowed: break;
        case GateResult::MissingInsecure: dbg(L"[wheat] refused: -insecure not on the command line"); return false;
        case GateResult::ConflictingSecure: dbg(L"[wheat] refused: -secure present"); return false;
    }
    g_game_dir.assign(path.substr(0, slash == std::wstring_view::npos ? 0 : slash));
    return true;
}

// ------------------------------------------------------------------ log file
namespace {
class FileLog {
public:
    void open(const fs::path& p) {
        out_.open(p, std::ios::out | std::ios::trunc | std::ios::binary);
        t0_ = std::chrono::steady_clock::now();
    }
    void write(std::string_view mod, LogLevel lv, std::string_view msg) {
        if (!out_ || full_) return;
        static const char* const kLevel[] = {"INFO ", "WARN ", "ERROR"};
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
        char head[64];
        std::snprintf(head, sizeof head, "[%9.3f] ", t);
        std::string line = std::string(head) + "[" + std::string(mod) + "] " + kLevel[static_cast<int>(lv)] + " " + std::string(msg) + "\n";
        bytes_ += line.size();
        if (bytes_ > kMaxBytes) {
            full_ = true;
            line = "[wheat] log size limit reached; further output suppressed\n";
        }
        out_.write(line.data(), static_cast<std::streamsize>(line.size()));
        out_.flush();
    }

private:
    static constexpr std::size_t kMaxBytes = 8u << 20;
    std::ofstream out_;
    std::chrono::steady_clock::time_point t0_;
    std::size_t bytes_ = 0;
    bool full_ = false;
};
}  // namespace

// ------------------------------------------------------------------ worker
// All Lua runs on THIS thread only (the sandbox is single-threaded by design).
// "tick" is a 10 Hz placeholder driven by a timer. Once the engine frame hook exists it will fire from
// there instead; the event name and arguments stay the same.
static DWORD WINAPI worker(LPVOID) {
    try {
        const fs::path base = fs::path(g_game_dir) / L"wheat";
        std::error_code ec;
        fs::create_directories(base / L"mods", ec);

        FileLog log;
        log.open(base / L"loader.log");
        log.write("host", LogLevel::Info, "wheat loader started (cs2.exe, -insecure confirmed)");

        ModHostConfig cfg;
        cfg.mods_dir            = base / L"mods";
        cfg.limits.event_wall_ms = 50;
        cfg.log                 = [&log](std::string_view mod, LogLevel lv, std::string_view msg) { log.write(mod, lv, msg); };
        ModHost host(std::move(cfg));

        const std::size_t n = host.load_all();
        log.write("host", LogLevel::Info, std::to_string(n) + " mod(s) running");
        host.dispatch("mods_loaded");

        auto last = std::chrono::steady_clock::now();
        while (!g_stop.load()) {
            Sleep(100);
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - last).count();
            last = now;
            host.dispatch("tick", {EventArg{dt}});
        }
        log.write("host", LogLevel::Info, "wheat loader stopping");
    } catch (const std::exception& e) {
        OutputDebugStringA((std::string("[wheat] loader thread failed: ") + e.what()).c_str());
    } catch (...) {
        OutputDebugStringA("[wheat] loader thread failed: unknown exception");
    }
    return 0;
}

// ------------------------------------------------------------------ entry point
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        if (!load_real_version()) return FALSE;  // cannot forward => failing loudly beats crashing later
        if (allowed_to_start()) {
            HANDLE t = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        g_stop.store(true);
    }
    return TRUE;
}
