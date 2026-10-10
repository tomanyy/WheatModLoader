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
#include "proxyExports.inc"

namespace fs = std::filesystem;
using namespace modloader;

extern "C" {
void* gReal[WHEAT_PROXY_COUNT] = {};
}

static std::wstring gGameDir;
static std::atomic<bool> gStop{false};

static bool loadRealDll() {
    wchar_t dir[MAX_PATH];
    const UINT len = GetSystemDirectoryW(dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH - 16) return false;
    const std::wstring path = std::wstring(dir, len) + L"\\" + WHEAT_PROXY_DLL_NAME;
    HMODULE real = LoadLibraryW(path.c_str());
    if (!real) return false;
    for (unsigned i = 0; i < WHEAT_PROXY_COUNT; ++i) {
        const ProxyExport& e = kProxyExports[i];
        const char* symbol = e.name ? e.name : MAKEINTRESOURCEA(e.ordinal);
        gReal[i] = reinterpret_cast<void*>(GetProcAddress(real, symbol));
    }
    return true;
}

static bool allowedToStart() {
    wchar_t exe[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        OutputDebugStringW(L"[wheat] refused: cannot determine host executable");
        return false;
    }
    const std::wstring_view path(exe, len);
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring_view base = slash == std::wstring_view::npos ? path : path.substr(slash + 1);
    if (!equals_ascii_nocase(base, "cs2.exe")) {
        OutputDebugStringW(L"[wheat] refused: host process is not cs2.exe");
        return false;
    }
    switch (check_launch_gate(GetCommandLineW())) {
        case GateResult::Allowed:
            break;
        case GateResult::MissingInsecure:
            OutputDebugStringW(L"[wheat] refused: -insecure not on the command line");
            return false;
        case GateResult::ConflictingSecure:
            OutputDebugStringW(L"[wheat] refused: -secure present");
            return false;
    }
    gGameDir.assign(path.substr(0, slash == std::wstring_view::npos ? 0 : slash));
    return true;
}

namespace {
class FileLog {
public:
    void open(const fs::path& path) {
        out.open(path, std::ios::out | std::ios::trunc | std::ios::binary);
        start = std::chrono::steady_clock::now();
    }

    void write(std::string_view mod, LogLevel level, std::string_view msg) {
        if (!out || full) return;
        static const char* const levelNames[] = {"INFO ", "WARN ", "ERROR"};
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        char head[32];
        std::snprintf(head, sizeof head, "[%9.3f] ", secs);
        std::string line = std::string(head) + "[" + std::string(mod) + "] " + levelNames[static_cast<int>(level)] + " " + std::string(msg) + "\n";
        bytes += line.size();
        if (bytes > maxBytes) {
            full = true;
            line = "[wheat] log size limit reached; further output suppressed\n";
        }
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
        out.flush();
    }

private:
    static constexpr std::size_t maxBytes = 8u << 20;
    std::ofstream out;
    std::chrono::steady_clock::time_point start;
    std::size_t bytes = 0;
    bool full = false;
};
}  // namespace

static DWORD WINAPI workerMain(LPVOID) {
    try {
        const fs::path base = fs::path(gGameDir) / L"wheat";
        std::error_code ec;
        fs::create_directories(base / L"mods", ec);

        FileLog log;
        log.open(base / L"loader.log");
        log.write("host", LogLevel::Info, "wheat loader started (cs2.exe, -insecure confirmed)");

        ModHostConfig config;
        config.mods_dir = base / L"mods";
        config.limits.event_wall_ms = 50;
        config.log = [&log](std::string_view mod, LogLevel level, std::string_view msg) { log.write(mod, level, msg); };
        ModHost host(std::move(config));

        const std::size_t running = host.load_all();
        log.write("host", LogLevel::Info, std::to_string(running) + " mod(s) running");
        host.dispatch("mods_loaded");

        auto last = std::chrono::steady_clock::now();
        while (!gStop.load()) {
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

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (!loadRealDll()) return FALSE;
        if (allowedToStart()) {
            HANDLE thread = CreateThread(nullptr, 0, workerMain, nullptr, 0, nullptr);
            if (thread) CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        gStop.store(true);
    }
    return TRUE;
}
