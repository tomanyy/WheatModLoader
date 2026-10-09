// Adversarial tests: every "escape" case is a script that tries to break out or
// exhaust the host. The suite passes only if the sandbox holds.

#include "lua_sandbox.hpp"

#include <lua.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace modloader;

static int g_failed = 0, g_run = 0;
static fs::path g_tmp;

static void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

struct Harness {
    fs::path root;
    std::vector<std::string> logs;
    std::unique_ptr<ModSandbox> sb;

    Harness(const std::string& id, const std::string& main_lua,
            std::function<void(const fs::path&)> setup = {}, SandboxLimits lim = {}) {
        root = g_tmp / id / "mod";
        fs::remove_all(g_tmp / id);
        write(root / "main.lua", main_lua);
        if (setup) setup(root);
        if (lim.memory_bytes == SandboxLimits{}.memory_bytes) {
            lim.memory_bytes      = 8 * 1024 * 1024;
            lim.load_instructions = 5'000'000;
            lim.event_instructions = 200'000;
        }
        sb = std::make_unique<ModSandbox>(id, root, lim, [this](std::string_view, LogLevel, std::string_view m) {
            logs.emplace_back(m);
        });
        sb->declare_event("frame");
        sb->add_api("game", [](lua_State* L) {
            lua_pushcfunction(L, [](lua_State* S) { lua_pushstring(S, "de_dust2"); return 1; });
            lua_setfield(L, -2, "map_name");
        });
    }
    bool logged(const std::string& needle) const {
        for (const auto& l : logs)
            if (l.find(needle) != std::string::npos) return true;
        return false;
    }
    std::string all_logs() const {
        std::string s;
        for (const auto& l : logs) s += "    | " + l.substr(0, 200) + "\n";
        return s;
    }
};

static void check(const char* name, bool cond, const std::string& detail = "") {
    ++g_run;
    if (cond) {
        std::cout << "  PASS  " << name << "\n";
    } else {
        ++g_failed;
        std::cout << "  FAIL  " << name << "\n" << detail;
    }
}

// Script must load successfully (its own asserts are the test).
static void expect_ok(const char* name, const std::string& lua, std::function<void(const fs::path&)> setup = {}) {
    Harness h(std::string("t") + std::to_string(g_run), lua, std::move(setup));
    const bool ok = h.sb->load_main();
    check(name, ok, h.all_logs());
}

// Script must be stopped by the sandbox; log must mention `why`. Also reports wall time.
static void expect_stopped(const char* name, const std::string& lua, const char* why) {
    Harness h(std::string("t") + std::to_string(g_run), lua);
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = h.sb->load_main();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    check(name, !ok && h.logged(why) && ms < 5000, h.all_logs() + "    (took " + std::to_string(ms) + " ms)\n");
}

int main() {
    g_tmp = fs::temp_directory_path() / "modloader_sandbox_tests";
    fs::remove_all(g_tmp);

    std::cout << "== environment lockdown ==\n";
    expect_ok("no io/os/debug/package/dofile/loadfile/loadstring",
              "assert(io==nil and os==nil and debug==nil and package==nil)\n"
              "assert(dofile==nil and loadfile==nil and loadstring==nil and require~=nil)\n");
    expect_ok("_G is the sandbox env, not the real globals",
              "assert(_G._G == _G and _G.os == nil and getmetatable(_G) == nil)\n");
    expect_ok("string.dump removed, also via the string metatable",
              "assert(string.dump == nil and (''):len() == 0 and ('').dump == nil)\n");
    expect_ok("string metatable is locked", "assert(getmetatable('') == 'locked')\n");
    expect_ok("bytecode cannot be loaded",
              "local f, err = load('\\27Lua\\84\\0\\25\\147\\13\\10\\26\\10')\n"
              "assert(f == nil and err:find('binary'), tostring(err))\n"
              "local g = load('\\27Lua', 'x', 'b'); assert(g == nil)\n");
    expect_ok("load() chunks run in the sandbox env",
              "assert(load('return io')() == nil)\n"
              "assert(load('return os', 'c', 't', {os = 5})() == 5)\n");
    expect_ok("collectgarbage is restricted",
              "assert(type(collectgarbage('count')) == 'number')\n"
              "assert(not pcall(collectgarbage, 'stop'))\n"
              "assert(not pcall(collectgarbage, 'setpause', 1))\n");

    std::cout << "== require confinement ==\n";
    expect_ok("path traversal / absolute / separators rejected",
              "for _, n in ipairs{'..secret', '../secret', 'a/b', 'a\\\\b', '/etc/passwd', 'C:x', '.x', 'x.', '', ('a'):rep(200)} do\n"
              "  assert(not pcall(require, n), n)\n"
              "end\n",
              [](const fs::path& r) { write(r.parent_path() / "secret.lua", "return 'SECRET'"); });
    expect_ok("symlink pointing outside the mod dir rejected",
              "local ok = pcall(require, 'link'); assert(not ok)\n",
              [](const fs::path& r) {
                  write(r.parent_path() / "outside.lua", "return 'SECRET'");
                  std::error_code ec;
                  fs::create_symlink(r.parent_path() / "outside.lua", r / "link.lua", ec);
              });
    expect_ok("require works inside the mod dir (incl. dotted submodules, caching)",
              "assert(require('helper').answer == 42)\n"
              "assert(require('helper') == require('helper'))\n"
              "assert(require('sub.deep').ok)\n",
              [](const fs::path& r) {
                  write(r / "helper.lua", "return {answer = 42}");
                  write(r / "sub" / "deep.lua", "return {ok = true}");
              });
    expect_ok("circular require detected and does not poison the cache",
              "local ok, err = pcall(require, 'a'); assert(not ok and err:find('circular'), tostring(err))\n",
              [](const fs::path& r) {
                  write(r / "a.lua", "return require('b')");
                  write(r / "b.lua", "return require('a')");
              });
    expect_ok("a module's runtime error leaves no stale 'loading' marker",
              "assert(not pcall(require, 'bad'))\n"
              "local ok, err = pcall(require, 'bad'); assert(not ok and not err:find('circular'), tostring(err))\n",
              [](const fs::path& r) { write(r / "bad.lua", "error('boom')"); });

    std::cout << "== resource limits ==\n";
    expect_stopped("infinite loop", "while true do end", "budget");
    expect_stopped("pcall cannot swallow the budget error",
                   "while true do pcall(function() while true do end end) end", "budget");
    expect_stopped("coroutine runaway loop", "coroutine.wrap(function() while true do end end)()", "budget");
    expect_stopped("runaway loop inside xpcall + handler",
                   "while true do xpcall(function() while true do end end, function() end) end", "budget");
    expect_stopped("string.rep memory bomb", "local s = string.rep('x', 1e9)", "memory");
    expect_stopped("table growth memory bomb", "local t = {} for i = 1, 1e9 do t[i] = {i} end", "memory");
    expect_stopped("string concat doubling bomb", "local s = 'x' while true do s = s .. s end", "memory");
    expect_stopped("runaway recursion (stopped by the memory cap before Lua's own stack limit)", "local function f() return 1 + f() end f()", "memory");

    std::cout << "== host API ==\n";
    expect_ok("host API works and is read-only",
              "assert(game.map_name() == 'de_dust2')\n"
              "assert(not pcall(function() game.map_name = nil end))\n"
              "assert(not pcall(function() events.on = nil end))\n");
    expect_ok("undeclared events are refused",
              "assert(not pcall(events.on, 'create_move', function() end))\n"
              "assert(not pcall(events.on, 'frame', 'not a function'))\n"
              "events.on('frame', function() end)\n");
    expect_ok("handler count per event is capped",
              "for i = 1, 32 do events.on('frame', function() end) end\n"
              "assert(not pcall(events.on, 'frame', function() end))\n");

    std::cout << "== events ==\n";
    {
        Harness h("ev1",
                  "local sum = 0\n"
                  "events.on('frame', function(dt, tag) sum = sum + dt; print('sum=' .. sum .. ' tag=' .. tag) end)\n");
        h.sb->load_main();
        const bool a = h.sb->dispatch("frame", {0.5, std::string("a")});
        const bool b = h.sb->dispatch("frame", {0.5, std::string("b")});
        check("dispatch passes args to handlers", a && b && h.logged("sum=1.0 tag=b"), h.all_logs());
        check("dispatching an event with no handlers is fine", h.sb->dispatch("frame_nobody", {}));
    }
    {
        Harness h("ev2",
                  "events.on('frame', function() error('first handler fails') end)\n"
                  "events.on('frame', function() print('second ran') end)\n");
        h.sb->load_main();
        const bool r = h.sb->dispatch("frame");
        check("a failing handler does not stop the others",
              !r && h.logged("first handler fails") && h.logged("second ran") && !h.sb->is_dead(), h.all_logs());
    }
    {
        Harness h("ev3", "events.on('frame', function() while true do end end)\n");
        h.sb->load_main();
        bool alive_after_two = true;
        h.sb->dispatch("frame");
        h.sb->dispatch("frame");
        alive_after_two = !h.sb->is_dead();
        h.sb->dispatch("frame");
        check("runaway handler: struck twice (alive), disabled on third",
              alive_after_two && h.sb->is_dead() && h.logged("mod disabled"), h.all_logs());
        check("a disabled mod never runs again", !h.sb->dispatch("frame"));
    }
    {
        Harness h("ev4",
                  "events.on('frame', function() local t = {} for i = 1, 1e9 do t[i] = {i} end end)\n"
                  "events.on('frame', function() print('survivor') end)\n");
        h.sb->load_main();
        h.sb->dispatch("frame");
        check("memory-bomb handler is contained; later handlers still run; heap stays under cap",
              h.logged("survivor") && h.sb->memory_used() <= 8u * 1024 * 1024, h.all_logs());
    }

    std::cout << "== isolation between mods ==\n";
    {
        Harness a("isoA", "string.evil = 'A was here'; math.huge = 0; function tostring() return 'pwned' end\n");
        Harness b("isoB", "assert(string.evil == nil and math.huge == 1/0 and tostring(1) == '1')\n");
        const bool ra = a.sb->load_main();
        const bool rb = b.sb->load_main();
        check("mod A cannot tamper with mod B's libraries", ra && rb, a.all_logs() + b.all_logs());
    }

    std::cout << "\n" << (g_run - g_failed) << "/" << g_run << " passed\n";
    fs::remove_all(g_tmp);
    return g_failed ? 1 : 0;
}
