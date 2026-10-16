#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

#include "engineConsole.hpp"
#include "frameHook.hpp"
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
static std::mutex gHostMutex;
static ModHost* gHost = nullptr;

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
        std::lock_guard<std::mutex> lock(mutex);
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
    std::mutex mutex;
    std::ofstream out;
    std::chrono::steady_clock::time_point start;
    std::size_t bytes = 0;
    bool full = false;
};
}  // namespace

static void dispatchFrame(double dt, std::uint64_t number) {
    std::unique_lock<std::mutex> lock(gHostMutex, std::try_to_lock);
    if (!lock.owns_lock() || !gHost) return;
    gHost->frame(dt, static_cast<std::int64_t>(number));
}

static bool waitForModule(const wchar_t* name, int timeoutMs) {
    for (int waited = 0; waited < timeoutMs && !gStop.load(); waited += 250) {
        if (GetModuleHandleW(name)) return true;
        Sleep(250);
    }
    return false;
}

struct HostPublisher {
    explicit HostPublisher(ModHost* host) {
        std::lock_guard<std::mutex> lock(gHostMutex);
        gHost = host;
    }
    ~HostPublisher() {
        std::lock_guard<std::mutex> lock(gHostMutex);
        gHost = nullptr;
    }
};

static bool initializeConsole(EngineConsole& console, const fs::path& base, FileLog& log) {
    const bool enabled = console.initialize(base);
    log.write("host", enabled ? LogLevel::Info : LogLevel::Warn,
              enabled ? "console API enabled (read-only); details in consoleReport.txt" : "console API off; see consoleReport.txt");
    return enabled;
}

static DWORD WINAPI workerMain(LPVOID) {
    try {
        const fs::path base = fs::path(gGameDir) / L"wheat";
        std::error_code ec;
        fs::create_directories(base / L"mods", ec);

        FileLog log;
        log.open(base / L"loader.log");
        log.write("host", LogLevel::Info, "wheat loader started (cs2.exe, -insecure confirmed)");

        auto engineConsole = std::make_shared<EngineConsole>();

        ModHostConfig config;
        config.console = engineConsole;
        config.mods_dir = base / L"mods";
        const fs::path allowFile = base / L"apiAllowList.txt";
        if (fs::exists(allowFile)) {
            config.enforce_api_allow_list = true;
            std::ifstream allowInput(allowFile);
            std::string apiName;
            while (std::getline(allowInput, apiName)) {
                if (!apiName.empty() && apiName.back() == '\r') apiName.pop_back();
                if (!apiName.empty() && apiName.size() <= 64 && apiName.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._") == std::string::npos)
                    config.api_allow_list.insert(apiName);
            }
        }
        const bool consoleApiEnabled = config.enforce_api_allow_list &&
            (config.api_allow_list.contains("console.get") || config.api_allow_list.contains("console.default") ||
             config.api_allow_list.contains("console.allowed") || config.api_allow_list.contains("console.available"));
        config.limits.event_wall_ms = 20;
        config.limits.event_instructions = 200'000;
        config.log = [&log](std::string_view mod, LogLevel level, std::string_view msg) { log.write(mod, level, msg); };
        ModHost host(std::move(config));
        const auto initializeConsoleIfAllowed = [&] {
            if (!consoleApiEnabled) {
                log.write("host", LogLevel::Info, "console API blocked by installation API Allow List");
                return;
            }
            for (unsigned attempt = 0; attempt < 180 && !gStop.load(); ++attempt) {
                if (initializeConsole(*engineConsole, base, log)) return;
                if (attempt % 10 == 9)
                    log.write("host", LogLevel::Info, "console API is waiting for CS2 convars to register; retrying calibration");
                Sleep(1000);
            }
            log.write("host", LogLevel::Warn, "console API stayed unavailable after 180 seconds; loading mods without it");
        };

        HostPublisher publisher(&host);
        bool modsLoaded = false;
        const auto loadMods = [&] {
            if (modsLoaded) return;
            log.write("host", LogLevel::Info, "waiting 5 seconds for CS2 startup settings before starting mods");
            for (int waited = 0; waited < 5000 && !gStop.load(); waited += 100) Sleep(100);
            if (gStop.load()) return;
            std::lock_guard<std::mutex> hostLock(gHostMutex);
            const std::size_t running = host.load_all();
            log.write("host", LogLevel::Info, std::to_string(running) + " mod(s) running");
            host.dispatch("mods_loaded");
            modsLoaded = true;
        };

        bool hooked = false;
        if (waitForModule(L"rendersystemdx11.dll", 180000)) {
            Sleep(3000);
            std::string error;
            hooked = frameHook::install(dispatchFrame, error);
            if (hooked) {
                log.write("host", LogLevel::Info, error.empty() ? "frame hook installed (IDXGISwapChain::Present)" : "frame hook installed with warning: " + error);
            } else {
                log.write("host", LogLevel::Error, "frame hook failed: " + error + "; falling back to a 10 Hz timer");
            }
        } else if (!gStop.load()) {
            log.write("host", LogLevel::Warn, "rendersystemdx11.dll never loaded; falling back to a 10 Hz timer");
        }

        if (hooked) {
            for (int waited = 0; waited < 60000 && !gStop.load() && frameHook::framesSeen() == 0; waited += 250) Sleep(250);
            log.write("host", frameHook::framesSeen() ? LogLevel::Info : LogLevel::Warn,
                      frameHook::framesSeen() ? "first frame seen" : "no frames seen 60 s after installing the hook");
            initializeConsoleIfAllowed();
            loadMods();
            for (unsigned beat = 1; !gStop.load(); ++beat) {
                Sleep(250);
                if (beat % 4 == 0) host.refresh();
            }
        } else {
            initializeConsoleIfAllowed();
            loadMods();
            auto last = std::chrono::steady_clock::now();
            std::uint64_t number = 0;
            while (!gStop.load()) {
                Sleep(100);
                if (number % 10 == 9) host.refresh();
                const auto now = std::chrono::steady_clock::now();
                const double dt = std::chrono::duration<double>(now - last).count();
                last = now;
                dispatchFrame(dt, ++number);
            }
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
        frameHook::stop();
    }
    return TRUE;
}
