#include "mod_host.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace modloader;

namespace {
std::atomic<bool> stopping{false};
std::thread worker;

bool processIsCs2() {
    std::error_code ec;
    const std::string name = fs::read_symlink("/proc/self/exe", ec).filename().string();
    return !ec && (name == "cs2" || name == "cs2_linux64");
}

bool hasInsecureArgument() {
    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    std::string argument;
    while (std::getline(cmdline, argument, '\0')) {
        if (argument == "-secure") return false;
        if (argument == "-insecure") return true;
    }
    return false;
}

void runHost() {
    try {
        if (!processIsCs2()) return;
        if (!hasInsecureArgument()) return;
        std::error_code ec;
        const fs::path gameDir = fs::read_symlink("/proc/self/exe", ec).parent_path();
        if (ec) return;
        const fs::path wheatDir = gameDir / "wheat";
        fs::create_directories(wheatDir / "mods", ec);

        std::ofstream logFile(wheatDir / "loader.log", std::ios::out | std::ios::trunc);
        const auto started = std::chrono::steady_clock::now();
        LogFn logger = [&logFile, started](std::string_view mod, LogLevel level, std::string_view message) {
            if (!logFile) return;
            static constexpr const char* labels[] = {"INFO", "WARN", "ERROR"};
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            logFile << '[' << elapsed << "] [" << mod << "] " << labels[static_cast<int>(level)] << ' ' << message << '\n';
            logFile.flush();
        };
        logger("host", LogLevel::Info, "Wheat Linux preload started (cs2, -insecure confirmed)");

        ModHostConfig config;
        config.mods_dir = wheatDir / "mods";
        config.limits.event_wall_ms = 20;
        config.limits.event_instructions = 200'000;
        config.log = logger;
        const fs::path allowFile = wheatDir / "apiAllowList.txt";
        if (fs::exists(allowFile, ec)) {
            config.enforce_api_allow_list = true;
            std::ifstream allowInput(allowFile);
            std::string api;
            while (std::getline(allowInput, api)) {
                if (!api.empty() && api.size() <= 64 &&
                    api.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._") == std::string::npos)
                    config.api_allow_list.insert(api);
            }
        }

        ModHost host(std::move(config));
        const std::size_t loaded = host.load_all();
        logger("host", LogLevel::Info, std::to_string(loaded) + " mod(s) running");
        host.dispatch("mods_loaded");

        auto previous = std::chrono::steady_clock::now();
        std::uint64_t frame = 0;
        unsigned refresh = 0;
        while (!stopping.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - previous).count();
            previous = now;
            host.frame(dt, static_cast<std::int64_t>(++frame));
            if (++refresh == 10) {
                host.refresh();
                refresh = 0;
            }
        }
        logger("host", LogLevel::Info, "Wheat Linux preload stopping");
    } catch (...) {
    }
}

__attribute__((constructor)) void wheatPreload() {
    stopping.store(false, std::memory_order_relaxed);
    worker = std::thread(runHost);
}

__attribute__((destructor)) void wheatUnload() {
    stopping.store(true, std::memory_order_relaxed);
    if (worker.joinable()) worker.join();
}
}  // namespace
