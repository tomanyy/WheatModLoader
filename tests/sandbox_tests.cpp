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
            lim.load_wall_ms       = 1000;
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

static void check(const std::string& name, bool cond, const std::string& detail = "") {
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
    check(std::string(name) + " (" + std::to_string(ms) + " ms)", !ok && h.logged(why) && ms < 5000,
          h.all_logs() + "    (took " + std::to_string(ms) + " ms)\n");
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


    std::cout << "== pattern matching DoS (single C call the instruction hook cannot see) ==\n";
    expect_stopped("find: nested quantifiers",
                   "local s = ('a'):rep(40); s:find(('a*'):rep(12) .. 'b')", "pattern-matching budget");
    expect_stopped("match: 2^n optional-item explosion",
                   "local s = ('a'):rep(30); s:match(('a?'):rep(30) .. ('a'):rep(30))", "pattern-matching budget");
    expect_stopped("gmatch: catastrophic pattern in iterator",
                   "for w in (('a'):rep(40)):gmatch(('a*'):rep(12) .. 'b') do end", "pattern-matching budget");
    expect_stopped("gsub: catastrophic pattern",
                   "(('a'):rep(40)):gsub(('a*'):rep(12) .. 'b', 'x')", "pattern-matching budget");
    expect_stopped("%b on unbalanced input (quadratic scanning)",
                   "(('('):rep(30000)):find('%b()')", "pattern-matching budget");
    expect_stopped("back-reference compare blowup",
                   "(('a'):rep(6000)):find('(a*)%1b')", "pattern-matching budget");
    expect_stopped("pcall cannot swallow a pattern-budget error",
                   "local s = ('a'):rep(40) local p = ('a*'):rep(12) .. 'b'\n"
                   "while true do pcall(string.find, s, p) end", "budget");
    expect_stopped("many moderately expensive pattern calls in a loop",
                   "local s = ('a'):rep(60) local p = ('a*'):rep(3) .. 'b'\n"
                   "for i = 1, 1e7 do s:find(p) end", "budget");

    std::cout << "== slow C calls in loops (few VM instructions, lots of time) ==\n";
    expect_stopped("table.insert(t,1,x) on a big table in a loop",
                   "local t = {} for i = 1, 200000 do t[i] = i end\n"
                   "for i = 1, 1e7 do table.insert(t, 1, i); table.remove(t) end", "time budget");

    std::cout << "== patched string library still behaves like upstream ==\n";
    expect_ok("find / match / captures / positions",
              "assert(('hello world'):find('o w') == 5)\n"
              "local s, e, k, v = ('key=value'):find('(%w+)=(%w+)')\n"
              "assert(s == 1 and e == 9 and k == 'key' and v == 'value')\n"
              "assert(('abc'):find('()b()') == 2 and select(3, ('abc'):find('()b()')) == 2)\n"
              "assert(('  trim  '):match('^%s*(.-)%s*$') == 'trim')\n"
              "assert(('<a><b>'):match('<(.-)>') == 'a')\n"
              "assert(('abab'):match('(ab)%1') == 'ab')\n"
              "assert(('f(a(b)c)d'):match('%b()') == '(a(b)c)')\n"
              "assert(('THE (quick) fox'):find('%f[%a]%a+%f[%A]') == 1)\n"
              "assert(('a.b'):find('.', 1, true) == 2)\n");
    expect_ok("gsub with string, table and function replacements",
              "assert(('abc'):gsub('%w', '%0%0') == 'aabbcc')\n"
              "assert(('hello'):gsub('l', {l = 'L'}) == 'heLLo')\n"
              "assert(('hello world'):gsub('%w+', string.upper) == 'HELLO WORLD')\n"
              "local _, n = ('aaa'):gsub('a', 'b', 2); assert(n == 2)\n");
    expect_ok("gmatch iterates correctly",
              "local words = {}\n"
              "for w in ('one two  three'):gmatch('%a+') do words[#words + 1] = w end\n"
              "assert(#words == 3 and words[3] == 'three')\n"
              "local kv = {}\n"
              "for k, v in ('a=1, b=2'):gmatch('(%w+)=(%w+)') do kv[k] = v end\n"
              "assert(kv.a == '1' and kv.b == '2')\n");
    expect_ok("malformed patterns still raise ordinary errors",
              "assert(not pcall(string.find, 'a', '[a'))\n"
              "assert(not pcall(string.find, 'a', '%'))\n"
              "assert(not pcall(string.rep))\n");
    expect_ok("string.format and the rest of the library are intact",
              "assert(string.format('%5.2f|%d|%s', 3.14159, 42, 'x') == ' 3.14|42|x')\n"
              "assert(string.format('%q', 'a\\nb') == '\"a\\\\\\nb\"')\n"
              "assert(('abc'):upper() == 'ABC' and ('abc'):reverse() == 'cba' and ('abc'):byte(2) == 98)\n"
              "assert(string.pack and #string.pack('i4', 7) == 4)\n");
    expect_ok("large legitimate inputs stay within the pattern budget",
              "local big = ('x'):rep(1000000)\n"
              "assert(big:find('y') == nil)\n"
              "assert(#big:gsub('x', 'y') == 1000000)\n"
              "assert(select(2, big:gsub('x', 'x')) == 1000000)\n");

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

    {
        Harness h("ev5",
                  "events.on('frame', function() ('a'):rep(40):find(('a*'):rep(12) .. 'b') end)\n"
                  "events.on('frame', function() print('after pattern bomb') end)\n");
        h.sb->load_main();
        const bool r1 = h.sb->dispatch("frame");
        const bool survived_first = !h.sb->is_dead() && h.logged("after pattern bomb");
        h.sb->dispatch("frame");
        h.sb->dispatch("frame");
        check("pattern-bomb handler: contained, later handlers run, mod disabled after 3 strikes",
              !r1 && survived_first && h.sb->is_dead(), h.all_logs());
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
