#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

struct lua_State;
namespace modloader {
namespace detail { struct SandboxImpl; }
enum class LogLevel { Info, Warn, Error };
using LogFn = std::function<void(std::string_view mod, LogLevel level, std::string_view msg)>;

struct SandboxLimits {
    std::size_t memory_bytes        = 16 * 1024 * 1024;
    std::uint64_t load_instructions = 20'000'000;
    std::uint64_t event_instructions = 500'000;
    std::uint64_t pattern_steps     = 5'000'000;
    int load_wall_ms                = 2000;
    int event_wall_ms               = 50;
    int max_strikes                 = 3;
};

using EventArg = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string>;
using ApiBuilder = std::function<void(lua_State*)>;

class ModSandbox {
public:
    ModSandbox(std::string mod_name, std::filesystem::path root, SandboxLimits limits, LogFn log);
    ~ModSandbox();

    ModSandbox(const ModSandbox&)            = delete;
    ModSandbox& operator=(const ModSandbox&) = delete;

    void add_api(std::string name, ApiBuilder build);
    void declare_event(std::string name);

    bool load_main();
    bool dispatch(const std::string& event, const std::vector<EventArg>& args = {});

    std::size_t memory_used() const;
    bool is_dead() const;

private:
    std::unique_ptr<detail::SandboxImpl> impl_;
};

}  // namespace modloader
