#pragma once
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
    LogFn log;
    std::size_t max_mods = 64;
    int max_log_lines_per_sec = 50;
};

class ModHost {
public:
    explicit ModHost(ModHostConfig cfg);
    ~ModHost();
    ModHost(const ModHost&)            = delete;
    ModHost& operator=(const ModHost&) = delete;
    std::size_t load_all();
    std::size_t dispatch(const std::string& event, const std::vector<EventArg>& args = {});

    std::size_t running() const;
    std::vector<std::string> mod_names() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace modloader
