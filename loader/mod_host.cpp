#include "mod_host.hpp"

#include "manifest.hpp"
#include "modData.hpp"

#include <lua.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <mutex>
#include <set>

namespace fs = std::filesystem;
using steady = std::chrono::steady_clock;

#if defined(__linux__)
void pushImguiApi(lua_State* state, const std::string& name,
                  const std::set<std::string>& allowed, bool enforce);
#endif

namespace modloader {
namespace {

constexpr const char* hostEvents[] = {"mods_loaded", "frame"};
constexpr std::size_t maxScanFiles = 256;

struct LogLimiter {
    steady::time_point window{};
    int count = 0;
    int dropped = 0;
};

struct Mod {
    std::string name;
    std::unique_ptr<LogLimiter> limiter;
    std::unique_ptr<ModSandbox> sandbox;
};

struct Watch {
    std::string name;
    fs::path dir;
    std::uint64_t loadedStamp = 0;
    std::uint64_t seenStamp = 0;
    bool everLoaded = false;
};

bool validModName(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

int wheatNow(lua_State* L) {
    lua_pushnumber(L, std::chrono::duration<double>(steady::now().time_since_epoch()).count());
    return 1;
}

std::uint64_t computeStamp(const fs::path& dir) {
    std::uint64_t sum = 0;
    std::size_t files = 0;
    try {
        std::error_code ec;
        fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
        while (!ec && it != end) {
            const fs::directory_entry& entry = *it;
            std::error_code e2;
            const bool isLink = entry.is_symlink(e2);
            if (entry.is_directory(e2)) {
                if (isLink || entry.path().filename() == "data") it.disable_recursion_pending();
            } else if (!isLink && entry.is_regular_file(e2)) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (ext == ".lua") {
                    if (++files > maxScanFiles) break;
                    const auto mtime = static_cast<std::uint64_t>(entry.last_write_time(e2).time_since_epoch().count());
                    const std::uint64_t size = entry.file_size(e2);
                    const std::string rel = entry.path().lexically_relative(dir).generic_string();
                    sum += std::hash<std::string>{}(rel) * 31u + mtime * 1000003u + size;
                }
            }
            it.increment(ec);
        }
    } catch (const std::exception&) {
        return 0;
    }
    return sum ^ (static_cast<std::uint64_t>(files) << 56);
}

}  // namespace

struct ModHost::Impl {
    ModHostConfig cfg;
    std::mutex mutex;
    std::vector<Mod> mods;
    std::vector<Watch> watches;
    std::set<std::string> reportedSkips;

    void hostLog(LogLevel level, std::string_view msg) const {
        if (cfg.log) cfg.log("host", level, msg);
    }

    void roll(const std::string& mod, LogLimiter& limiter, steady::time_point now, bool force = false) const {
        if (!force && now - limiter.window < std::chrono::seconds(1)) return;
        if (limiter.dropped > 0 && cfg.log)
            cfg.log(mod, LogLevel::Warn, "(" + std::to_string(limiter.dropped) + " log lines dropped by rate limit)");
        limiter.window = now;
        limiter.count = 0;
        limiter.dropped = 0;
    }

    void logFromMod(const std::string& mod, LogLimiter& limiter, LogLevel level, std::string_view msg) const {
        roll(mod, limiter, steady::now());
        if (limiter.count < cfg.max_log_lines_per_sec) {
            ++limiter.count;
            if (cfg.log) cfg.log(mod, level, msg);
        } else {
            ++limiter.dropped;
        }
    }

    void skipOnce(const std::string& key, LogLevel level, const std::string& message) {
        if (reportedSkips.insert(key).second) hostLog(level, message);
    }

    std::vector<std::pair<std::string, fs::path>> scanFolders() {
        std::vector<std::pair<std::string, fs::path>> found;
        std::error_code ec;
        for (fs::directory_iterator it(cfg.mods_dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            const bool isLink = it->is_symlink(e2);
            std::string name;
            try {
                name = it->path().filename().string();
            } catch (const std::exception&) {
                skipOnce("?unrepresentable", LogLevel::Warn, "skipping a folder whose name cannot be represented");
                continue;
            }
            if (isLink) {
                skipOnce(name, LogLevel::Warn, "skipping '" + name + "': symlinked mod folders are not allowed");
                continue;
            }
            if (!it->is_directory(e2)) continue;
            if (!validModName(name)) {
                skipOnce(name, LogLevel::Warn, "skipping '" + name + "': mod names may only contain letters, digits, '_' and '-' (max 64)");
                continue;
            }
            found.emplace_back(std::move(name), it->path());
        }
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        return found;
    }

    std::unique_ptr<Mod> buildMod(const std::string& name, const fs::path& dir) {
        std::error_code ec;
        if (!fs::is_regular_file(dir / "main.lua", ec)) {
            skipOnce(name + "/main", LogLevel::Info, "skipping '" + name + "': no main.lua");
            return nullptr;
        }

        Manifest manifest;
        std::string error;
        switch (readManifest(dir / "manifest.json", manifest, error)) {
            case ManifestStatus::missing:
                hostLog(LogLevel::Warn, "mod '" + name + "' has no manifest.json; using defaults");
                manifest.name = name;
                manifest.version = "?";
                manifest.apiVersion = kApiVersion;
                break;
            case ManifestStatus::invalid:
                hostLog(LogLevel::Error, "mod '" + name + "': " + error);
                return nullptr;
            case ManifestStatus::ok:
                if (manifest.apiVersion < 1 || manifest.apiVersion > kApiVersion) {
                    hostLog(LogLevel::Error, "mod '" + name + "' needs API version " + std::to_string(manifest.apiVersion) +
                                                 " but this loader provides " + std::to_string(kApiVersion));
                    return nullptr;
                }
                break;
        }

        auto mod = std::make_unique<Mod>();
        mod->name = name;
        mod->limiter = std::make_unique<LogLimiter>();
        LogLimiter* limiter = mod->limiter.get();
        Impl* self = this;
        try {
            mod->sandbox = std::make_unique<ModSandbox>(
                name, dir, cfg.limits, [self, limiter](std::string_view who, LogLevel level, std::string_view msg) {
                    self->logFromMod(std::string(who), *limiter, level, msg);
                });
        } catch (const std::exception& e) {
            hostLog(LogLevel::Error, "could not create sandbox for '" + name + "': " + e.what());
            return nullptr;
        }
        for (const char* event : hostEvents) mod->sandbox->declare_event(event);
        mod->sandbox->add_api("wheat", [name](lua_State* L) {
            lua_pushinteger(L, kApiVersion);
            lua_setfield(L, -2, "api_version");
            lua_pushstring(L, name.c_str());
            lua_setfield(L, -2, "mod_name");
            lua_pushcfunction(L, wheatNow);
            lua_setfield(L, -2, "now");
        });
        ConsoleBackend* consoleBackend = cfg.console.get();
        const auto apiAllowed = [&](const std::string& name) {
            return !cfg.enforce_api_allow_list || cfg.api_allow_list.contains(name);
        };
        if (!cfg.enforce_api_allow_list || cfg.api_allow_list.contains("console.get") ||
            cfg.api_allow_list.contains("console.default") || cfg.api_allow_list.contains("console.allowed") ||
            cfg.api_allow_list.contains("console.available")) {
            const auto allow = cfg.enforce_api_allow_list ? cfg.api_allow_list : std::set<std::string>{};
            mod->sandbox->add_api("console", [consoleBackend, allow](lua_State* L) { pushConsoleApi(L, consoleBackend, allow); });
        }
        const fs::path dataDir = dir / "data";
        if (apiAllowed("data")) mod->sandbox->add_api("data", [dataDir](lua_State* L) { pushDataApi(L, dataDir); });
#if defined(__linux__)
        const std::set<std::string> imguiApis{
            "imgui.begin", "imgui.text", "imgui.button", "imgui.finish", "imgui.separator", "imgui.same_line"
        };
        if (std::any_of(imguiApis.begin(), imguiApis.end(), apiAllowed)) {
            const auto allowed = cfg.api_allow_list;
            const bool enforce = cfg.enforce_api_allow_list;
            const std::string displayName = manifest.name;
            mod->sandbox->add_api("imgui", [displayName, allowed, enforce](lua_State* L) {
                pushImguiApi(L, displayName, allowed, enforce);
            });
        }
#endif

        if (!mod->sandbox->load_main()) {
            hostLog(LogLevel::Error, "mod '" + name + "' failed to load");
            return nullptr;
        }
        std::string label = "loaded mod '" + manifest.name + "' (internal id '" + name + "') v" + manifest.version;
        if (!manifest.author.empty()) label += " by " + manifest.author;
        hostLog(LogLevel::Info, label);
        return mod;
    }

    std::vector<Mod>::iterator findMod(const std::string& name) {
        return std::find_if(mods.begin(), mods.end(), [&](const Mod& m) { return m.name == name; });
    }

    std::size_t refreshFolders(bool initial) {
        std::size_t changes = 0;
        const auto folders = scanFolders();

        for (auto it = watches.begin(); it != watches.end();) {
            const bool present = std::any_of(folders.begin(), folders.end(), [&](const auto& f) { return f.first == it->name; });
            if (present) {
                ++it;
                continue;
            }
            std::unique_ptr<Mod> removed;
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto mod = findMod(it->name);
                if (mod != mods.end()) {
                    removed = std::make_unique<Mod>(std::move(*mod));
                    mods.erase(mod);
                }
            }
            if (removed) {
                hostLog(LogLevel::Info, "unloaded mod '" + it->name + "' (folder removed)");
                ++changes;
            }
            it = watches.erase(it);
        }

        for (const auto& [name, dir] : folders) {
            auto found = std::find_if(watches.begin(), watches.end(), [&](const Watch& w) { return w.name == name; });
            if (found == watches.end()) {
                watches.push_back(Watch{name, dir});
                found = watches.end() - 1;
            }
            Watch& watch = *found;

            const std::uint64_t stamp = computeStamp(dir);
            if (watch.everLoaded && stamp == watch.loadedStamp) continue;
            if (!initial && stamp != watch.seenStamp) {
                watch.seenStamp = stamp;
                continue;
            }
            watch.everLoaded = true;
            watch.loadedStamp = stamp;
            watch.seenStamp = stamp;

            const bool reloading = findMod(name) != mods.end();
            if (!reloading && mods.size() >= cfg.max_mods) {
                skipOnce(name + "/cap", LogLevel::Warn, "mod limit reached (" + std::to_string(cfg.max_mods) + "); not loading '" + name + "'");
                continue;
            }
            if (reloading) hostLog(LogLevel::Info, "reloading mod '" + name + "'");

            std::unique_ptr<Mod> fresh = buildMod(name, dir);
            if (!fresh) {
                if (reloading) hostLog(LogLevel::Warn, "reload of '" + name + "' failed; keeping the previous version");
                continue;
            }
            std::unique_ptr<Mod> old;
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto mod = findMod(name);
                if (mod != mods.end()) {
                    old = std::make_unique<Mod>(std::move(*mod));
                    *mod = std::move(*fresh);
                } else {
                    mods.push_back(std::move(*fresh));
                }
            }
            ++changes;
        }
        return changes;
    }
};

ModHost::ModHost(ModHostConfig cfg) : impl_(std::make_unique<Impl>()) { impl_->cfg = std::move(cfg); }

ModHost::~ModHost() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto now = steady::now();
    for (auto& mod : impl_->mods) impl_->roll(mod.name, *mod.limiter, now, true);
}

std::size_t ModHost::load_all() {
    std::error_code ec;
    if (!fs::is_directory(impl_->cfg.mods_dir, ec)) {
        impl_->hostLog(LogLevel::Warn, "mods directory not found: " + impl_->cfg.mods_dir.string());
        return 0;
    }
    impl_->refreshFolders(true);
    return running();
}

std::size_t ModHost::refresh() { return impl_->refreshFolders(false); }

std::size_t ModHost::frame(double dt, std::int64_t number) {
    Impl& im = *impl_;
    std::unique_lock<std::mutex> lock(im.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return 0;
    const std::vector<EventArg> args{EventArg{dt}, EventArg{number}};
    const auto now = steady::now();
    std::size_t failed = 0;
    for (auto& mod : im.mods) {
        if (mod.sandbox->is_dead()) continue;
        failed += mod.sandbox->runTimers();
        if (!mod.sandbox->dispatch("frame", args)) ++failed;
        im.roll(mod.name, *mod.limiter, now);
    }
    return failed;
}

std::size_t ModHost::dispatch(const std::string& event, const std::vector<EventArg>& args) {
    Impl& im = *impl_;
    std::unique_lock<std::mutex> lock(im.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return 0;
    const auto now = steady::now();
    std::size_t failed = 0;
    for (auto& mod : im.mods) {
        if (mod.sandbox->is_dead()) continue;
        if (!mod.sandbox->dispatch(event, args)) ++failed;
        im.roll(mod.name, *mod.limiter, now);
    }
    return failed;
}

std::size_t ModHost::running() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return static_cast<std::size_t>(std::count_if(impl_->mods.begin(), impl_->mods.end(), [](const Mod& m) { return !m.sandbox->is_dead(); }));
}

std::vector<std::string> ModHost::mod_names() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<std::string> names;
    for (const auto& mod : impl_->mods) names.push_back(mod.name);
    return names;
}

}  // namespace modloader
