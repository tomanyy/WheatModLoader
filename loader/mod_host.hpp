#pragma once
// ModHost: finds mods on disk, runs each in its own ModSandbox, fans events out to them.
// Platform independent. NOT thread-safe: call every method from one thread.
//
// Mod layout:   <mods_dir>/<mod_name>/main.lua      (+ any files main.lua require()s)
// A mod name is 1-64 chars of [A-Za-z0-9_-]. Symlinked mod folders are skipped.

#include "lua_sandbox.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace modloader {

inline constexpr int kApiVersion = 1;

struct ModHostConfig {
    std::filesystem::path mods_dir;
    SandboxLimits limits{};
    LogFn log;                        // receives host messages (mod == "host") and mod output
    std::size_t max_mods = 64;
    int max_log_lines_per_sec = 50;   // per mod; excess lines are dropped and counted
};

class ModHost {
public:
    explicit ModHost(ModHostConfig cfg);
    ~ModHost();
    ModHost(const ModHost&)            = delete;
    ModHost& operator=(const ModHost&) = delete;

    // Discovers and loads every mod. A mod that fails to load is logged and dropped; the rest still run.
    // Returns the number of mods now running.
    std::size_t load_all();

    // Sends an event to every running mod. Returns how many mods had a handler fail.
    // Events available to mods: "mods_loaded", "frame" (see kHostEvents in mod_host.cpp).
    std::size_t dispatch(const std::string& event, const std::vector<EventArg>& args = {});

    std::size_t running() const;                 // mods that are loaded and not disabled
    std::vector<std::string> mod_names() const;  // all loaded mods, in load order

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace modloader
