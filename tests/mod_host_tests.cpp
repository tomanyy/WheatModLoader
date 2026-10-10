#include "mod_host.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace modloader;

static int g_run = 0, g_failed = 0;
static fs::path g_tmp;

static void check(const std::string& name, bool cond, const std::string& detail = "") {
    ++g_run;
    if (cond) {
        std::cout << "  PASS  " << name << "\n";
    } else {
        ++g_failed;
        std::cout << "  FAIL  " << name << "\n" << detail;
    }
}

static void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

struct Rig {
    fs::path mods;
    std::vector<std::string> logs;  // "mod|level|message"
    std::unique_ptr<ModHost> host;

    explicit Rig(const std::string& id, int log_limit = 50, std::size_t max_mods = 64) {
        mods = g_tmp / id / "mods";
        fs::remove_all(g_tmp / id);
        fs::create_directories(mods);
        cfg.mods_dir = mods;
        cfg.max_mods = max_mods;
        cfg.max_log_lines_per_sec = log_limit;
        cfg.limits.load_instructions  = 5'000'000;
        cfg.limits.event_instructions = 200'000;
        cfg.limits.load_wall_ms       = 1000;
        cfg.limits.memory_bytes       = 8u << 20;
        cfg.log = [this](std::string_view mod, LogLevel lv, std::string_view m) {
            logs.push_back(std::string(mod) + "|" + std::to_string(static_cast<int>(lv)) + "|" + std::string(m));
        };
    }
    void start() { host = std::make_unique<ModHost>(cfg); }
    bool logged(const std::string& needle) const {
        for (const auto& l : logs)
            if (l.find(needle) != std::string::npos) return true;
        return false;
    }
    int count(const std::string& needle) const {
        int n = 0;
        for (const auto& l : logs)
            if (l.find(needle) != std::string::npos) ++n;
        return n;
    }
    std::string dump() const {
        std::string s;
        for (const auto& l : logs) s += "    | " + l.substr(0, 160) + "\n";
        return s;
    }
    ModHostConfig cfg;
};

int main() {
    g_tmp = fs::temp_directory_path() / "modhost_tests";
    fs::remove_all(g_tmp);

    std::cout << "== discovery ==\n";
    {
        Rig r("disc");
        write(r.mods / "zeta" / "main.lua", "print('zeta up')");
        write(r.mods / "alpha" / "main.lua", "print('alpha up')");
        write(r.mods / "no_main" / "readme.txt", "x");
        write(r.mods / "bad name!" / "main.lua", "print('should never run')");
        write(r.mods / "loose_file.lua", "print('should never run')");
        std::error_code ec;
        write(r.mods.parent_path() / "elsewhere" / "main.lua", "print('should never run')");
        fs::create_directory_symlink(r.mods.parent_path() / "elsewhere", r.mods / "linked", ec);
        r.start();
        const auto n = r.host->load_all();
        const auto names = r.host->mod_names();
        check("only valid mod folders load, in sorted order",
              n == 2 && names.size() == 2 && names[0] == "alpha" && names[1] == "zeta", r.dump());
        check("invalid names, loose files and mod-less folders are skipped with a reason",
              r.logged("bad name!") && r.logged("no main.lua") && !r.logged("should never run"), r.dump());
        if (!ec) check("symlinked mod folder is refused", r.logged("symlink"), r.dump());
    }
    {
        Rig r("nodir");
        fs::remove_all(r.mods);
        r.start();
        check("missing mods directory is handled", r.host->load_all() == 0 && r.logged("not found"), r.dump());
    }
    {
        Rig r("cap", 50, 3);
        for (int i = 0; i < 6; ++i) write(r.mods / ("m" + std::to_string(i)) / "main.lua", "");
        r.start();
        check("mod count is capped", r.host->load_all() == 3 && r.logged("mod limit reached"), r.dump());
    }

    std::cout << "== failure isolation ==\n";
    {
        Rig r("iso");
        write(r.mods / "a_good" / "main.lua", "events.on('tick', function() print('good tick') end)");
        write(r.mods / "b_syntax" / "main.lua", "this is not lua !!!");
        write(r.mods / "c_loop" / "main.lua", "while true do end");
        write(r.mods / "d_bomb" / "main.lua", "local s = string.rep('x', 1e9)");
        write(r.mods / "e_error" / "main.lua", "error('mod e exploded')");
        write(r.mods / "f_good" / "main.lua", "events.on('tick', function() print('good tick') end)");
        r.start();
        const auto n = r.host->load_all();
        check("broken mods are dropped, the good ones still run", n == 2 && r.logged("failed to load and was dropped"), r.dump());
        check("each failure is explained in the log",
              r.logged("budget exceeded") && r.logged("not enough memory") && r.logged("mod e exploded"), r.dump());
        r.logs.clear();
        r.host->dispatch("tick", {0.1});
        check("good mods receive events", r.count("good tick") == 2, r.dump());
    }
    {
        Rig r("strikes");
        write(r.mods / "a_runaway" / "main.lua", "events.on('tick', function() while true do end end)");
        write(r.mods / "b_fine" / "main.lua", "events.on('tick', function() print('b ok') end)");
        r.start();
        r.host->load_all();
        std::size_t failed_total = 0;
        for (int i = 0; i < 5; ++i) failed_total += r.host->dispatch("tick", {0.1});
        check("a runaway mod is disabled after repeated strikes; neighbours keep running",
              r.host->running() == 1 && r.count("b ok") == 5 && failed_total == 3 && r.logged("mod disabled"), r.dump());
    }

    std::cout << "== wheat API ==\n";
    {
        Rig r("api");
        write(r.mods / "probe" / "main.lua",
              "assert(wheat.api_version == 1)\n"
              "assert(wheat.mod_name == 'probe')\n"
              "local t = wheat.now(); assert(type(t) == 'number' and t > 0 and wheat.now() >= t)\n"
              "assert(not pcall(function() wheat.api_version = 99 end))\n"
              "assert(not pcall(events.on, 'create_move', function() end))\n"
              "assert(io == nil and os == nil and debug == nil and package == nil)\n"
              "events.on('mods_loaded', function() print('mods_loaded seen') end)\n");
        r.start();
        const bool ok = r.host->load_all() == 1;
        r.host->dispatch("mods_loaded");
        check("wheat API is present, read-only, and events are allowlisted", ok && r.logged("mods_loaded seen"), r.dump());
    }
    {
        Rig r("mods_iso");
        write(r.mods / "a" / "main.lua", "string.pwned = true; wheat_leak = 'from a'");
        write(r.mods / "b" / "main.lua", "assert(string.pwned == nil and wheat_leak == nil)");
        r.start();
        check("mods cannot see or modify each other's state", r.host->load_all() == 2, r.dump());
    }

    std::cout << "== log rate limiting ==\n";
    {
        Rig r("spam", 20);
        write(r.mods / "spammer" / "main.lua", "for i = 1, 10000 do print('spam ' .. i) end");
        r.start();
        const auto n = r.host->load_all();
        const int lines = r.count("spam ");
        check("a log flood is cut to the per-second limit", n == 1 && lines >= 1 && lines <= 21, "lines=" + std::to_string(lines) + "\n" + r.dump());
        r.host.reset();  // destructor flushes the drop notice
        check("the number of dropped lines is reported", r.logged("log lines dropped by rate limit"), r.dump());
    }

    std::cout << "\n" << (g_run - g_failed) << "/" << g_run << " passed\n";
    fs::remove_all(g_tmp);
    return g_failed ? 1 : 0;
}
