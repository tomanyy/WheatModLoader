#pragma once
// Per-mod Lua 5.4 sandbox.
//
// Design: ONE lua_State PER MOD. That gives us, for free:
//   * a separate memory budget per mod (custom allocator)
//   * a separate instruction budget per mod (count hook)
//   * no shared string metatable / library tables between mods
//   * a mod that dies (OOM, runaway loop) takes only itself down
//
// Inside a state, a mod only ever sees a hand-built whitelist environment.
// The real globals table is never reachable from Lua.
//
// SECURITY MODEL: the sandbox boundary is "the lua_State + the whitelist env".
// Everything the HOST registers via add_api() is attack surface: treat every
// argument coming from Lua as hostile and validate it.

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
    std::size_t memory_bytes        = 16 * 1024 * 1024;  // hard cap on Lua heap
    std::uint64_t load_instructions = 20'000'000;        // budget for main.lua
    std::uint64_t event_instructions = 500'000;          // budget PER event handler call
    std::uint64_t pattern_steps     = 5'000'000;         // per-call cap on matcher work (also drawn from the instruction budget)
    int load_wall_ms                = 2000;              // wall-clock cap for main.lua (catches slow C calls in loops)
    int event_wall_ms               = 50;                // wall-clock cap PER event handler call
    int max_strikes                 = 3;                 // budget/memory violations before the mod is disabled
};

// NOTE: construct strings explicitly (std::string{"x"}). A bare "x" literal
// converts to bool in a std::variant and is NOT what you want.
using EventArg = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string>;

// Called with an empty table on top of the Lua stack. Push your functions into it
// (lua_pushcfunction + lua_setfield). Must not throw, and must not call lua_error
// while holding C++ objects with destructors (Lua built as C uses longjmp).
using ApiBuilder = std::function<void(lua_State*)>;

class ModSandbox {
public:
    // `root` is the mod's folder. main.lua is loaded from it; require() is confined to it.
    ModSandbox(std::string mod_name, std::filesystem::path root, SandboxLimits limits, LogFn log);
    ~ModSandbox();

    ModSandbox(const ModSandbox&)            = delete;
    ModSandbox& operator=(const ModSandbox&) = delete;

    // Host configuration. Must be called BEFORE load_main().
    void add_api(std::string name, ApiBuilder build);  // exposed to the mod as a read-only global table
    void declare_event(std::string name);              // only declared events can be subscribed to

    // Runs <root>/main.lua under the load budget. Returns false (and disables the mod) on failure.
    bool load_main();

    // Calls every handler the mod registered for `event`, each under its own budget.
    // Returns true only if all handlers completed without error.
    bool dispatch(const std::string& event, const std::vector<EventArg>& args = {});

    // Runs every timer that has come due (timers.after / timers.every). Returns how many callbacks failed.
    std::size_t runTimers();

    std::size_t memory_used() const;
    bool is_dead() const;

private:
    std::unique_ptr<detail::SandboxImpl> impl_;
};

}  // namespace modloader
