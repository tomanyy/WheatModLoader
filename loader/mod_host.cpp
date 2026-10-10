#include "mod_host.hpp"

#include <lua.hpp>

#include <algorithm>
#include <chrono>
#include <exception>

namespace fs = std::filesystem;
using steady = std::chrono::steady_clock;

namespace modloader {

namespace {

constexpr const char* kHostEvents[] = {"mods_loaded", "tick"};

struct LogLimiter {
    steady::time_point window{};
    int count   = 0;
    int dropped = 0;
};

struct Mod {
    std::string name;
    std::unique_ptr<LogLimiter> limiter;  // heap: the sandbox's log callback keeps a pointer to it
    std::unique_ptr<ModSandbox> sandbox;
};

bool valid_mod_name(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (char c : n) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// wheat.now(): monotonic seconds. No wall-clock time, nothing that identifies the machine.
int wheat_now(lua_State* L) {
    lua_pushnumber(L, std::chrono::duration<double>(steady::now().time_since_epoch()).count());
    return 1;
}

}  // namespace

struct ModHost::Impl {
    ModHostConfig cfg;
    std::vector<Mod> mods;

    void host_log(LogLevel lv, std::string_view msg) const {
        if (cfg.log) cfg.log("host", lv, msg);
    }

    // Emits a "N dropped" notice if the limiter's window has rolled over.
    void roll(const std::string& mod, LogLimiter& lim, steady::time_point now, bool force = false) const {
        if (!force && now - lim.window < std::chrono::seconds(1)) return;
        if (lim.dropped > 0 && cfg.log)
            cfg.log(mod, LogLevel::Warn, "(" + std::to_string(lim.dropped) + " log lines dropped by rate limit)");
        lim.window  = now;
        lim.count   = 0;
        lim.dropped = 0;
    }

    void log_from_mod(const std::string& mod, LogLimiter& lim, LogLevel lv, std::string_view msg) const {
        roll(mod, lim, steady::now());
        if (lim.count < cfg.max_log_lines_per_sec) {
            ++lim.count;
            if (cfg.log) cfg.log(mod, lv, msg);
        } else {
            ++lim.dropped;
        }
    }
};

ModHost::ModHost(ModHostConfig cfg) : impl_(std::make_unique<Impl>()) { impl_->cfg = std::move(cfg); }

ModHost::~ModHost() {
    const auto now = steady::now();
    for (auto& m : impl_->mods) impl_->roll(m.name, *m.limiter, now, /*force=*/true);
    // sandboxes are destroyed here; their __gc finalizers run under a budget (see ModSandbox::~ModSandbox)
}

std::size_t ModHost::load_all() {
    Impl& im = *impl_;
    std::error_code ec;
    if (!fs::is_directory(im.cfg.mods_dir, ec)) {
        im.host_log(LogLevel::Warn, "mods directory not found: " + im.cfg.mods_dir.string());
        return 0;
    }

    std::vector<std::pair<std::string, fs::path>> found;  // sorted by name => deterministic load order
    for (fs::directory_iterator it(im.cfg.mods_dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const bool is_link = it->is_symlink(e2);
        std::string name;
        try {
            name = it->path().filename().string();
        } catch (const std::exception&) {
            im.host_log(LogLevel::Warn, "skipping a folder whose name cannot be represented");
            continue;
        }
        if (is_link) {
            im.host_log(LogLevel::Warn, "skipping '" + name + "': symlinked mod folders are not allowed");
            continue;
        }
        if (!it->is_directory(e2)) continue;
        if (!valid_mod_name(name)) {
            im.host_log(LogLevel::Warn, "skipping '" + name + "': mod names may only contain letters, digits, '_' and '-' (max 64)");
            continue;
        }
        found.emplace_back(std::move(name), it->path());
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    for (auto& [name, dir] : found) {
        if (im.mods.size() >= im.cfg.max_mods) {
            im.host_log(LogLevel::Warn, "mod limit reached (" + std::to_string(im.cfg.max_mods) + "); not loading '" + name + "' or later mods");
            break;
        }
        std::error_code e3;
        if (!fs::is_regular_file(dir / "main.lua", e3)) {
            im.host_log(LogLevel::Info, "skipping '" + name + "': no main.lua");
            continue;
        }

        Mod mod;
        mod.name    = name;
        mod.limiter = std::make_unique<LogLimiter>();
        LogLimiter* lim = mod.limiter.get();
        Impl* self      = &im;
        try {
            mod.sandbox = std::make_unique<ModSandbox>(
                name, dir, im.cfg.limits,
                [self, lim](std::string_view who, LogLevel lv, std::string_view msg) {
                    self->log_from_mod(std::string(who), *lim, lv, msg);
                });
        } catch (const std::exception& e) {
            im.host_log(LogLevel::Error, "could not create sandbox for '" + name + "': " + e.what());
            continue;
        }
        for (const char* ev : kHostEvents) mod.sandbox->declare_event(ev);
        mod.sandbox->add_api("wheat", [name](lua_State* L) {
            lua_pushinteger(L, kApiVersion);
            lua_setfield(L, -2, "api_version");
            lua_pushstring(L, name.c_str());
            lua_setfield(L, -2, "mod_name");
            lua_pushcfunction(L, wheat_now);
            lua_setfield(L, -2, "now");
        });

        if (!mod.sandbox->load_main()) {
            im.host_log(LogLevel::Error, "mod '" + name + "' failed to load and was dropped");
            continue;  // sandbox (and its Lua state) is destroyed here
        }
        im.host_log(LogLevel::Info, "loaded mod '" + name + "'");
        im.mods.push_back(std::move(mod));
    }
    return running();
}

std::size_t ModHost::dispatch(const std::string& event, const std::vector<EventArg>& args) {
    Impl& im = *impl_;
    std::size_t failed = 0;
    const auto now = steady::now();
    for (auto& m : im.mods) {
        if (m.sandbox->is_dead()) continue;
        if (!m.sandbox->dispatch(event, args)) ++failed;
        im.roll(m.name, *m.limiter, now);  // flush "dropped" notices even if the mod went quiet
    }
    return failed;
}

std::size_t ModHost::running() const {
    return static_cast<std::size_t>(std::count_if(impl_->mods.begin(), impl_->mods.end(),
                                                  [](const Mod& m) { return !m.sandbox->is_dead(); }));
}

std::vector<std::string> ModHost::mod_names() const {
    std::vector<std::string> v;
    for (const auto& m : impl_->mods) v.push_back(m.name);
    return v;
}

}  // namespace modloader
