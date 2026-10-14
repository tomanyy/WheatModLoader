#pragma once
#include "consoleApi.hpp"
#include "lua_sandbox.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace modloader {

inline constexpr int kApiVersion = 1;

struct ModHostConfig {
    std::filesystem::path mods_dir;
    SandboxLimits limits{};
    LogFn log;
    std::size_t max_mods = 64;
    int max_log_lines_per_sec = 50;
    std::shared_ptr<ConsoleBackend> console;
    std::set<std::string> api_allow_list;
    bool enforce_api_allow_list = false;
};

class ModHost {
public:
    explicit ModHost(ModHostConfig cfg);
    ~ModHost();
    ModHost(const ModHost&)            = delete;
    ModHost& operator=(const ModHost&) = delete;

    std::size_t load_all();

    // Called from the render thread. Never blocks: if the host is busy reloading, the call is skipped.
    std::size_t frame(double dt, std::int64_t number);
    std::size_t dispatch(const std::string& event, const std::vector<EventArg>& args = {});

    // Called from the loader thread about once a second: picks up new, changed and removed mods.
    std::size_t refresh();

    std::size_t running() const;
    std::vector<std::string> mod_names() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace modloader
