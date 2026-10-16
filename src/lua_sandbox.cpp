#include "lua_sandbox.hpp"

#include <lua.hpp>

static_assert(LUA_VERSION_NUM == 504, "lua_sandbox targets Lua 5.4 exactly (Lua 5.5 changed the C API)");

#include "wheat_pattern_hooks.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <type_traits>

namespace fs = std::filesystem;

// LONGJMP NOTE
// If Lua is compiled as C, lua_error() longjmps and skips C++ destructors.
// Every C function below that can raise a Lua error therefore keeps C++ objects
// inside helper functions that RETURN before any lua_error()/luaL_error() runs.

namespace modloader::detail {

constexpr int kHookStride                    = 128;        // VM instructions between hook calls (deadline check granularity)
constexpr std::size_t kMaxSourceBytes        = 1 << 20;    // 1 MiB per script file
constexpr std::size_t kMaxHandlersPerEvent   = 32;
constexpr std::size_t kMaxLogLine            = 4096;
constexpr int kStrikeDecayDispatches         = 1000;
constexpr std::size_t kHostHeadroom          = 512 * 1024;
constexpr int kMaxTimers                     = 64;
constexpr double kMinRepeatSeconds           = 0.05;
constexpr double kMaxTimerSeconds            = 86400.0;
constexpr const char* kTimersKey             = "modloader.timers";
constexpr const char* kLoadedKey             = "modloader.loaded";
constexpr const char* kHandlersKey           = "modloader.handlers";
constexpr const char* kEnvKey                = "modloader.env";
static char kLoadingSentinel;  // address used as a "module is mid-load" marker

struct SandboxImpl {
    std::string name;
    fs::path root;
    SandboxLimits limits;
    LogFn log;
    std::vector<std::pair<std::string, ApiBuilder>> apis;
    std::vector<std::string> events;

    lua_State* L = nullptr;
    lua_State* mainThread = nullptr;
    int mainThreadRef = LUA_NOREF;
    double mainThreadDue = 0.0;
    std::size_t mem_used = 0;

    // instruction budget for the call currently in progress
    std::uint64_t budget = 0;
    std::uint64_t used = 0;          // VM instructions
    lua_Unsigned pattern_used = 0;   // pattern-matcher steps (same pool as instructions)
    bool tripped = false;
    const char* trip_reason = nullptr;
    std::chrono::steady_clock::time_point deadline{};

    int strikes = 0;
    int cleanDispatches = 0;
    bool inCall = false;
    lua_Integer nextTimerId = 0;
    int timerCount = 0;
    bool dead = false;
    bool env_built = false;

    void emit(LogLevel lvl, std::string_view msg) const {
        if (log) log(name, lvl, msg);
    }
    bool has_event(const char* n) const {
        for (const auto& e : events)
            if (e == n) return true;
        return false;
    }
    int guarded_pcall(int nargs, int nres, std::uint64_t instruction_budget, int wall_ms);
    int guarded_resume(lua_State* thread, int nargs, int& nresults,
                       std::uint64_t instruction_budget, int wall_ms);
    void strike(int rc);
    void build_env();
};

static SandboxImpl* impl_of(lua_State* L) {
    // Every thread (coroutine) copies the main thread's extra space at creation,
    // so this works from inside coroutines too.
    return *static_cast<SandboxImpl**>(lua_getextraspace(L));
}

// ---------------------------------------------------------------- memory cap

static void* limited_alloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
    auto* im = static_cast<SandboxImpl*>(ud);
    if (nsize == 0) {
        if (ptr) im->mem_used -= osize;
        std::free(ptr);
        return nullptr;
    }
    const std::size_t old = ptr ? osize : 0;  // when ptr==NULL, osize is a type tag, not a size
    const std::size_t cap = im->limits.memory_bytes + (im->inCall ? 0 : kHostHeadroom);
    if (nsize > old && (im->mem_used - old) + nsize > cap) return nullptr;
    void* n = std::realloc(ptr, nsize);
    if (!n) return (nsize <= old) ? ptr : nullptr;  // shrinking must never fail
    im->mem_used = (im->mem_used - old) + nsize;
    return n;
}

// ---------------------------------------------------------- instruction cap

static void budget_hook(lua_State* L, lua_Debug*) {
    SandboxImpl* im = impl_of(L);
    if (!im->tripped) {
        im->used += static_cast<std::uint64_t>(lua_gethookcount(L));
        if (im->used + im->pattern_used > im->budget) {
            im->tripped = true;
            im->trip_reason = "sandbox: instruction budget exceeded";
        } else if (std::chrono::steady_clock::now() > im->deadline) {
            // Catches loops of slow C calls (table.insert(t,1,x) on a big table, etc.) that burn
            // lots of time for very few VM instructions.
            im->tripped = true;
            im->trip_reason = "sandbox: time budget exceeded";
        } else {
            return;
        }
    }
    // Once tripped, fire on EVERY instruction of this thread. A script that wraps the
    // runaway loop in pcall() can catch one error, but not the next instruction's.
    lua_sethook(L, budget_hook, LUA_MASKCOUNT, 1);
    lua_pushstring(L, im->trip_reason);
    lua_error(L);
}

// Called by the patched string library when one pattern call exceeds its step limit.
extern "C" lua_Unsigned* wheat_pattern_counter(lua_State* L) { return &impl_of(L)->pattern_used; }

extern "C" lua_Unsigned wheat_pattern_allowance(lua_State* L) {
    const SandboxImpl* im = impl_of(L);
    const std::uint64_t spent = im->used + im->pattern_used;
    const std::uint64_t left  = im->budget > spent ? im->budget - spent : 0;
    return static_cast<lua_Unsigned>(std::min<std::uint64_t>(left, im->limits.pattern_steps));
}

extern "C" void wheat_pattern_exhausted(lua_State* L) {
    SandboxImpl* im = impl_of(L);
    im->tripped = true;
    im->trip_reason = "sandbox: pattern-matching budget exceeded";
    lua_sethook(L, budget_hook, LUA_MASKCOUNT, 1);
    lua_pushstring(L, im->trip_reason);
    lua_error(L);
}

static int msg_handler(lua_State* L) {
    lua_sethook(L, nullptr, 0, 0);  // building the traceback is not charged to the mod
    // Deliberately NOT luaL_tolstring: that would run mod-controlled __tostring unmetered.
    const char* msg = lua_tostring(L, 1);
    if (msg)
        luaL_traceback(L, L, msg, 1);
    else
        lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    return 1;
}

int SandboxImpl::guarded_pcall(int nargs, int nres, std::uint64_t instruction_budget, int wall_ms) {
    const int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, msg_handler);
    lua_insert(L, base);
    used = 0;
    pattern_used = 0;
    budget = instruction_budget;
    tripped = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wall_ms);
    lua_sethook(L, budget_hook, LUA_MASKCOUNT, kHookStride);
    inCall = true;
    const int rc = lua_pcall(L, nargs, nres, base);
    inCall = false;
    lua_sethook(L, nullptr, 0, 0);
    lua_remove(L, base);
    return rc;
}

int SandboxImpl::guarded_resume(lua_State* thread, int nargs, int& nresults,
                                std::uint64_t instruction_budget, int wall_ms) {
    used = 0;
    pattern_used = 0;
    budget = instruction_budget;
    tripped = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wall_ms);
    lua_sethook(thread, budget_hook, LUA_MASKCOUNT, kHookStride);
    inCall = true;
    const int rc = lua_resume(thread, L, nargs, &nresults);
    inCall = false;
    lua_sethook(thread, nullptr, 0, 0);
    return rc;
}

void SandboxImpl::strike(int rc) {
    if (!tripped && rc != LUA_ERRMEM) return;
    if (++strikes >= limits.max_strikes && !dead) {
        dead = true;
        emit(LogLevel::Error, "mod disabled: repeated resource-limit violations");
    }
}

// ------------------------------------------------------------ file helpers

// Resolves rel inside root (following symlinks) and reads it. Never raises Lua errors.
static bool read_source(const fs::path& root, const fs::path& rel, std::string& out, const char*& err) {
    std::error_code ec;
    const fs::path rootc = fs::weakly_canonical(root, ec);
    if (ec) { err = "mod directory unavailable"; return false; }
    const fs::path file = fs::weakly_canonical(rootc / rel, ec);
    if (ec) { err = "bad path"; return false; }
    const fs::path r = file.lexically_relative(rootc);
    if (r.empty() || *r.begin() == "..") { err = "path escapes mod directory"; return false; }
    if (!fs::is_regular_file(file, ec)) { err = "file not found"; return false; }
    const auto size = fs::file_size(file, ec);
    if (ec || size > kMaxSourceBytes) { err = "file too large"; return false; }
    std::ifstream f(file, std::ios::binary);
    if (!f) { err = "cannot open file"; return false; }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// Pushes the compiled chunk (with the sandbox env as its _ENV) and returns true,
// or pushes an error string and returns false. Text chunks only: "t" refuses bytecode.
static bool push_chunk(lua_State* L, SandboxImpl* im, const fs::path& rel, const std::string& chunkname, int env_idx) {
    std::string src;
    const char* err = nullptr;
    if (!read_source(im->root, rel, src, err)) {
        lua_pushstring(L, err);
        return false;
    }
    if (luaL_loadbufferx(L, src.data(), src.size(), chunkname.c_str(), "t") != LUA_OK) return false;
    lua_pushvalue(L, env_idx);
    lua_setupvalue(L, -2, 1);  // upvalue 1 of a main chunk is _ENV
    return true;
}

// ------------------------------------------------------- sandbox functions

static int sb_print(lua_State* L) {
    const int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++) {
        luaL_tolstring(L, i, nullptr);
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    std::size_t len = 0;
    const char* s = lua_tolstring(L, -1, &len);
    impl_of(L)->emit(LogLevel::Info, std::string_view(s, std::min(len, kMaxLogLine)));
    return 0;
}

// load(chunk [, chunkname [, mode [, env]]]) -- string chunks only, text only.
static int sb_load(lua_State* L) {
    std::size_t len = 0;
    const char* s = lua_tolstring(L, 1, &len);
    if (!s) return luaL_error(L, "load: only string chunks are supported");
    const char* name = luaL_optstring(L, 2, "=(load)");
    // `mode` (arg 3) is ignored on purpose: binary chunks are never allowed.
    if (luaL_loadbufferx(L, s, len, name, "t") != LUA_OK) {
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;  // nil, message
    }
    if (!lua_isnone(L, 4))
        lua_pushvalue(L, 4);
    else
        lua_pushvalue(L, lua_upvalueindex(1));
    lua_setupvalue(L, -2, 1);
    return 1;
}

static bool valid_module_name(const char* s) {
    if (!*s || std::strlen(s) > 128) return false;
    bool prev_dot = true;  // also rejects a leading '.'
    for (; *s; ++s) {
        const char c = *s;
        if (c == '.') {
            if (prev_dot) return false;  // no "..", no leading dot
            prev_dot = true;
            continue;
        }
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;  // no '/', '\\', ':', spaces, NULs...
        prev_dot = false;
    }
    return !prev_dot;
}

static bool push_module_chunk(lua_State* L, SandboxImpl* im, const char* name, int env_idx) {
    std::string rel = name;
    std::replace(rel.begin(), rel.end(), '.', '/');
    rel += ".lua";
    return push_chunk(L, im, rel, "@" + rel, env_idx);
}

static int sb_require(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (!valid_module_name(name)) return luaL_error(L, "invalid module name");

    lua_getfield(L, LUA_REGISTRYINDEX, kLoadedKey);  // idx 2
    lua_getfield(L, 2, name);                        // idx 3
    if (lua_touserdata(L, 3) == &kLoadingSentinel) return luaL_error(L, "circular require of '%s'", name);
    if (!lua_isnil(L, 3)) return 1;
    lua_pop(L, 1);

    if (!push_module_chunk(L, impl_of(L), name, lua_upvalueindex(1)))
        return luaL_error(L, "module '%s' failed to load: %s", name, lua_tostring(L, -1));

    lua_pushlightuserdata(L, &kLoadingSentinel);
    lua_setfield(L, 2, name);
    lua_pushstring(L, name);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {  // clear the marker, then rethrow
        lua_pushnil(L);
        lua_setfield(L, 2, name);
        return lua_error(L);
    }
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, 2, name);
    return 1;
}

// Only "count" and "collect": nothing that can retune the collector.
static int sb_collectgarbage(lua_State* L) {
    static const char* const opts[] = {"count", "collect", nullptr};
    if (luaL_checkoption(L, 1, "collect", opts) == 0) {
        lua_pushnumber(L, lua_gc(L, LUA_GCCOUNT) + lua_gc(L, LUA_GCCOUNTB) / 1024.0);
    } else {
        lua_gc(L, LUA_GCCOLLECT);
        lua_pushinteger(L, 0);
    }
    return 1;
}

static int sb_events_on(lua_State* L) {
    const char* ev = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!impl_of(L)->has_event(ev)) return luaL_error(L, "unknown event '%s'", ev);

    lua_getfield(L, LUA_REGISTRYINDEX, kHandlersKey);
    lua_getfield(L, -1, ev);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, ev);
    }
    const lua_Unsigned n = lua_rawlen(L, -1);
    if (n >= kMaxHandlersPerEvent) return luaL_error(L, "too many handlers for '%s'", ev);
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, static_cast<lua_Integer>(n) + 1);
    return 0;
}

static double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static int timerCreate(lua_State* L, bool repeating) {
    const lua_Number delay = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!(delay >= (repeating ? kMinRepeatSeconds : 0.0)) || delay > kMaxTimerSeconds)
        return luaL_error(L, "invalid timer delay");
    SandboxImpl* im = impl_of(L);
    if (im->timerCount >= kMaxTimers) return luaL_error(L, "too many timers");

    lua_getfield(L, LUA_REGISTRYINDEX, kTimersKey);
    const lua_Integer id = ++im->nextTimerId;
    lua_createtable(L, 0, 3);
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, "fn");
    lua_pushnumber(L, nowSeconds() + delay);
    lua_setfield(L, -2, "due");
    if (repeating) {
        lua_pushnumber(L, delay);
        lua_setfield(L, -2, "interval");
    }
    lua_rawseti(L, -2, id);
    ++im->timerCount;
    lua_pushinteger(L, id);
    return 1;
}

static int sb_timers_after(lua_State* L) { return timerCreate(L, false); }
static int sb_timers_every(lua_State* L) { return timerCreate(L, true); }

static int sb_wait(lua_State* L) {
    const lua_Number delay = luaL_checknumber(L, 1);
    if (!(delay >= 0.0) || delay > kMaxTimerSeconds)
        return luaL_error(L, "wait delay must be between 0 and 86400 seconds");
    if (!lua_isyieldable(L))
        return luaL_error(L, "wait() can only be used in the top-level main.lua coroutine");
    lua_pushnumber(L, delay);
    return lua_yield(L, 1);
}

static int sb_timers_cancel(lua_State* L) {
    const lua_Integer id = luaL_checkinteger(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, kTimersKey);
    lua_rawgeti(L, -1, id);
    const bool existed = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (existed) {
        lua_pushnil(L);
        lua_rawseti(L, -2, id);
        --impl_of(L)->timerCount;
    }
    lua_pushboolean(L, existed);
    return 1;
}

static int ro_newindex(lua_State* L) { return luaL_error(L, "attempt to modify a read-only table"); }

// Replaces the table on top of the stack with a read-only proxy.
// This is a guard against accidents, NOT a security boundary (rawset bypasses it).
// The boundary is the separate lua_State per mod.
static void make_readonly(lua_State* L) {
    lua_newtable(L);  // proxy
    lua_newtable(L);  // metatable
    lua_pushvalue(L, -3);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, ro_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    lua_remove(L, -2);
}

// --------------------------------------------------------------- env setup

static int open_libs(lua_State* L) {
    // Deliberately NOT luaL_openlibs: io, os, package and debug are never opened.
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);      lua_pop(L, 1);
    // Patched string library: identical to upstream except pattern matching is step-limited.
    luaL_requiref(L, LUA_STRLIBNAME, wheat_luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);    lua_pop(L, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1); lua_pop(L, 1);

    // string.dump would let a mod produce bytecode. It is also reachable as ("").dump
    // through the shared string metatable, so remove it from the real table.
    lua_getglobal(L, "string");
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);

    // Lock the string metatable so getmetatable("") returns a dummy and the real
    // metatable can't be inspected or replaced.
    lua_pushliteral(L, "");
    lua_getmetatable(L, -1);
    lua_pushliteral(L, "locked");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 2);

    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kLoadedKey);
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kHandlersKey);
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kTimersKey);
    return 0;
}

static int build_env_trampoline(lua_State* L) {
    impl_of(L)->build_env();
    return 0;
}

void SandboxImpl::build_env() {
    static const char* const base_whitelist[] = {
        "assert", "error", "getmetatable", "ipairs", "next", "pairs", "pcall", "rawequal", "rawget",
        "rawlen", "rawset", "select", "setmetatable", "tonumber", "tostring", "type", "xpcall", nullptr};
    static const char* const libs[] = {"string", "table", "math", "utf8", "coroutine", nullptr};

    lua_newtable(L);
    const int env = lua_gettop(L);

    for (const char* const* n = base_whitelist; *n; ++n) {
        lua_getglobal(L, *n);
        lua_setfield(L, env, *n);
    }
    for (const char* const* n = libs; *n; ++n) {
        lua_getglobal(L, *n);
        lua_setfield(L, env, *n);
    }
    lua_pushvalue(L, env);
    lua_setfield(L, env, "_G");
    lua_pushliteral(L, "Lua 5.4");
    lua_setfield(L, env, "_VERSION");

    lua_pushvalue(L, env);
    lua_pushcclosure(L, sb_load, 1);
    lua_setfield(L, env, "load");
    lua_pushvalue(L, env);
    lua_pushcclosure(L, sb_require, 1);
    lua_setfield(L, env, "require");
    lua_pushcfunction(L, sb_print);
    lua_setfield(L, env, "print");
    lua_pushcfunction(L, sb_collectgarbage);
    lua_setfield(L, env, "collectgarbage");
    lua_pushcfunction(L, sb_wait);
    lua_setfield(L, env, "wait");

    lua_newtable(L);
    lua_pushcfunction(L, sb_events_on);
    lua_setfield(L, -2, "on");
    make_readonly(L);
    lua_setfield(L, env, "events");

    lua_newtable(L);
    lua_pushcfunction(L, sb_timers_after);
    lua_setfield(L, -2, "after");
    lua_pushcfunction(L, sb_timers_every);
    lua_setfield(L, -2, "every");
    lua_pushcfunction(L, sb_timers_cancel);
    lua_setfield(L, -2, "cancel");
    make_readonly(L);
    lua_setfield(L, env, "timers");

    for (const auto& [api_name, builder] : apis) {
        lua_newtable(L);
        const int tbl = lua_gettop(L);
        builder(L);
        lua_settop(L, tbl);  // tolerate a builder that leaves junk on the stack
        make_readonly(L);
        lua_setfield(L, env, api_name.c_str());
    }

    // Defense in depth: strip the dangerous loaders from the REAL globals too.
    for (const char* n : {"dofile", "loadfile", "load", "print", "collectgarbage"}) {
        lua_pushnil(L);
        lua_setglobal(L, n);
    }

    lua_setfield(L, LUA_REGISTRYINDEX, kEnvKey);  // pops env
}

static void push_arg(lua_State* L, const EventArg& a) {
    std::visit(
        [L](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) lua_pushnil(L);
            else if constexpr (std::is_same_v<T, bool>) lua_pushboolean(L, v);
            else if constexpr (std::is_same_v<T, std::int64_t>) lua_pushinteger(L, static_cast<lua_Integer>(v));
            else if constexpr (std::is_same_v<T, double>) lua_pushnumber(L, v);
            else lua_pushlstring(L, v.data(), v.size());
        },
        a);
}

// Stack on entry: 1 = handler, 2 = lightuserdata -> const std::vector<EventArg>*.
// Running inside lua_pcall means an out-of-memory while pushing arguments is caught, not fatal.
static int invokeHandler(lua_State* L) {
    const auto* args = static_cast<const std::vector<EventArg>*>(lua_touserdata(L, 2));
    lua_settop(L, 1);
    luaL_checkstack(L, static_cast<int>(args->size()) + 4, "too many event arguments");
    for (const auto& a : *args) push_arg(L, a);
    lua_call(L, static_cast<int>(args->size()), 0);
    return 0;
}

}  // namespace modloader::detail

// ===================================================================== public

namespace modloader {
using namespace detail;

ModSandbox::ModSandbox(std::string mod_name, fs::path root, SandboxLimits limits, LogFn log)
    : impl_(std::make_unique<SandboxImpl>()) {
    impl_->name   = std::move(mod_name);
    impl_->root   = std::move(root);
    impl_->limits = limits;
    impl_->log    = std::move(log);

    impl_->L = lua_newstate(limited_alloc, impl_.get());
    if (!impl_->L) throw std::runtime_error("sandbox: could not create Lua state");
    *static_cast<SandboxImpl**>(lua_getextraspace(impl_->L)) = impl_.get();

    lua_pushcfunction(impl_->L, open_libs);
    if (lua_pcall(impl_->L, 0, 0, 0) != LUA_OK) {
        lua_close(impl_->L);
        impl_->L = nullptr;
        throw std::runtime_error("sandbox: could not initialise Lua libraries");
    }
}

ModSandbox::~ModSandbox() {
    if (!impl_ || !impl_->L) return;
    // lua_close runs every pending __gc finalizer, which is mod code. Meter it.
    impl_->tripped = false;
    impl_->used    = 0;
    impl_->pattern_used = 0;
    impl_->budget  = impl_->limits.event_instructions;
    impl_->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(impl_->limits.event_wall_ms);
    lua_sethook(impl_->L, budget_hook, LUA_MASKCOUNT, kHookStride);
    lua_close(impl_->L);
}

void ModSandbox::add_api(std::string name, ApiBuilder build) {
    impl_->apis.emplace_back(std::move(name), std::move(build));
}

void ModSandbox::declare_event(std::string name) { impl_->events.push_back(std::move(name)); }

bool ModSandbox::load_main() {
    auto& im = *impl_;
    lua_State* L = im.L;
    if (im.dead) return false;

    if (!im.env_built) {
        lua_pushcfunction(L, build_env_trampoline);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            im.emit(LogLevel::Error, std::string("could not build environment: ") + lua_tostring(L, -1));
            lua_pop(L, 1);
            im.dead = true;
            return false;
        }
        im.env_built = true;
    }

    const int top = lua_gettop(L);
    lua_getfield(L, LUA_REGISTRYINDEX, kEnvKey);
    const int env_idx = lua_gettop(L);

    bool ok = push_chunk(L, &im, "main.lua", "@main.lua", env_idx);
    if (ok) {
        im.mainThread = lua_newthread(L);
        im.mainThreadRef = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_pushvalue(L, top + 2);
        lua_xmove(L, im.mainThread, 1);
        int nresults = 0;
        const int rc = im.guarded_resume(im.mainThread, 0, nresults,
                                         im.limits.load_instructions, im.limits.load_wall_ms);
        if (rc == LUA_YIELD) {
            double delay = 0.05;
            if (nresults > 0 && lua_isnumber(im.mainThread, -nresults))
                delay = std::clamp(lua_tonumber(im.mainThread, -nresults), 0.0, kMaxTimerSeconds);
            lua_pop(im.mainThread, nresults);
            im.mainThreadDue = nowSeconds() + delay;
        } else if (rc == LUA_OK) {
            lua_settop(im.mainThread, 0);
            luaL_unref(L, LUA_REGISTRYINDEX, im.mainThreadRef);
            im.mainThreadRef = LUA_NOREF;
            im.mainThread = nullptr;
        } else {
            const char* message = lua_tostring(im.mainThread, -1);
            im.emit(LogLevel::Error, message ? message : "unknown error in main.lua");
            luaL_unref(L, LUA_REGISTRYINDEX, im.mainThreadRef);
            im.mainThreadRef = LUA_NOREF;
            im.mainThread = nullptr;
            ok = false;
        }
    } else {
        im.emit(LogLevel::Error, std::string("cannot load main.lua: ") + lua_tostring(L, -1));
    }
    lua_settop(L, top);
    if (!ok) im.dead = true;
    return ok;
}

bool ModSandbox::dispatch(const std::string& event, const std::vector<EventArg>& args) {
    auto& im = *impl_;
    lua_State* L = im.L;
    if (im.dead || !im.env_built) return false;

    const int top = lua_gettop(L);
    if (!lua_checkstack(L, 8)) return false;

    lua_getfield(L, LUA_REGISTRYINDEX, kHandlersKey);
    lua_getfield(L, -1, event.c_str());
    if (!lua_istable(L, -1)) {
        lua_settop(L, top);
        return true;
    }
    const int handlers = lua_gettop(L);
    const lua_Unsigned n = lua_rawlen(L, handlers);  // fixed up front: handlers added mid-dispatch wait

    bool all_ok = true;
    for (lua_Unsigned i = 1; i <= n && !im.dead; ++i) {
        lua_pushcfunction(L, invokeHandler);
        lua_rawgeti(L, handlers, static_cast<lua_Integer>(i));
        lua_pushlightuserdata(L, const_cast<std::vector<EventArg>*>(&args));
        const int rc = im.guarded_pcall(2, 0, im.limits.event_instructions, im.limits.event_wall_ms);
        if (rc != LUA_OK) {
            all_ok = false;
            im.emit(LogLevel::Error, lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error");
            lua_pop(L, 1);
            im.strike(rc);
        }
    }
    lua_settop(L, top);
    if (!all_ok) {
        im.cleanDispatches = 0;
    } else if (im.strikes > 0 && ++im.cleanDispatches >= kStrikeDecayDispatches) {
        im.strikes = 0;
        im.cleanDispatches = 0;
    }
    return all_ok;
}

std::size_t ModSandbox::runTimers() {
    auto& im = *impl_;
    lua_State* L = im.L;
    if (im.dead || !im.env_built || !lua_checkstack(L, 8)) return 0;

    const double now = nowSeconds();
    if (im.timerCount == 0 && (!im.mainThread || im.mainThreadDue > now)) return 0;
    const int top = lua_gettop(L);
    lua_getfield(L, LUA_REGISTRYINDEX, kTimersKey);
    const int timers = lua_gettop(L);

    lua_Integer dueIds[kMaxTimers];
    int dueCount = 0;
    lua_pushnil(L);
    while (lua_next(L, timers) != 0) {
        lua_getfield(L, -1, "due");
        const double due = lua_tonumber(L, -1);
        lua_pop(L, 2);
        if (due <= now && dueCount < kMaxTimers) dueIds[dueCount++] = lua_tointeger(L, -1);
    }

    std::size_t failed = 0;
    for (int i = 0; i < dueCount && !im.dead; ++i) {
        lua_rawgeti(L, timers, dueIds[i]);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        lua_getfield(L, -1, "interval");
        const bool repeating = lua_isnumber(L, -1) != 0;
        const double interval = repeating ? lua_tonumber(L, -1) : 0.0;
        lua_pop(L, 1);
        lua_getfield(L, -1, "fn");
        if (repeating) {
            lua_pushnumber(L, now + interval);
            lua_setfield(L, -3, "due");
        } else {
            lua_pushnil(L);
            lua_rawseti(L, timers, dueIds[i]);
            --im.timerCount;
        }
        lua_remove(L, -2);

        const int rc = im.guarded_pcall(0, 0, im.limits.event_instructions, im.limits.event_wall_ms);
        if (rc != LUA_OK) {
            ++failed;
            im.emit(LogLevel::Error, lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error");
            lua_pop(L, 1);
            im.strike(rc);
        }
    }
    lua_settop(L, top);

    if (im.mainThread && im.mainThreadDue <= nowSeconds() && !im.dead) {
        int nresults = 0;
        const int rc = im.guarded_resume(im.mainThread, 0, nresults,
                                         im.limits.event_instructions, im.limits.event_wall_ms);
        if (rc == LUA_YIELD) {
            double delay = 0.05;
            if (nresults > 0 && lua_isnumber(im.mainThread, -nresults))
                delay = std::clamp(lua_tonumber(im.mainThread, -nresults), 0.0, kMaxTimerSeconds);
            lua_pop(im.mainThread, nresults);
            im.mainThreadDue = nowSeconds() + delay;
        } else if (rc == LUA_OK) {
            lua_settop(im.mainThread, 0);
            luaL_unref(L, LUA_REGISTRYINDEX, im.mainThreadRef);
            im.mainThreadRef = LUA_NOREF;
            im.mainThread = nullptr;
        } else {
            ++failed;
            const char* message = lua_tostring(im.mainThread, -1);
            im.emit(LogLevel::Error, message ? message : "unknown error in main.lua coroutine");
            im.strike(rc);
            luaL_unref(L, LUA_REGISTRYINDEX, im.mainThreadRef);
            im.mainThreadRef = LUA_NOREF;
            im.mainThread = nullptr;
            im.dead = true;
        }
    }
    return failed;
}

std::size_t ModSandbox::memory_used() const { return impl_->mem_used; }
bool ModSandbox::is_dead() const { return impl_->dead; }

}  // namespace modloader
