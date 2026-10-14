#include "manifest.hpp"

#include <lua.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace modloader {
namespace {

constexpr std::size_t maxManifestBytes = 16 * 1024;
constexpr std::size_t manifestMemoryBytes = 1 << 20;
constexpr int manifestInstructions = 200000;

struct Allocator {
    std::size_t used = 0;
};

void* manifestAlloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
    auto* a = static_cast<Allocator*>(ud);
    if (nsize == 0) {
        if (ptr) a->used -= osize;
        std::free(ptr);
        return nullptr;
    }
    const std::size_t old = ptr ? osize : 0;
    if (nsize > old && (a->used - old) + nsize > manifestMemoryBytes) return nullptr;
    void* grown = std::realloc(ptr, nsize);
    if (!grown) return nsize <= old ? ptr : nullptr;
    a->used = (a->used - old) + nsize;
    return grown;
}

struct Job {
    const char* source;
    std::size_t length;
    char name[65];
    char version[33];
    char description[201];
    char author[65];
    long long apiVersion;
};

void stopHook(lua_State* L, lua_Debug*) {
    lua_pushliteral(L, "manifest took too long to evaluate");
    lua_error(L);
}

void readString(lua_State* L, const char* key, char* dest, std::size_t capacity, bool required) {
    lua_getfield(L, -1, key);
    if (lua_isnil(L, -1)) {
        if (required) luaL_error(L, "manifest is missing '%s'", key);
        dest[0] = 0;
    } else {
        if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "'%s' must be a string", key);
        std::size_t length = 0;
        const char* text = lua_tolstring(L, -1, &length);
        if (length >= capacity) luaL_error(L, "'%s' is too long", key);
        std::memcpy(dest, text, length);
        dest[length] = 0;
    }
    lua_pop(L, 1);
}

int evaluate(lua_State* L) {
    Job* job = static_cast<Job*>(lua_touserdata(L, 1));
    if (luaL_loadbufferx(L, job->source, job->length, "=manifest", "t") != LUA_OK) return lua_error(L);
    lua_newtable(L);
    lua_setupvalue(L, -2, 1);
    lua_call(L, 0, 1);
    if (!lua_istable(L, -1)) return luaL_error(L, "manifest must return a table");

    readString(L, "name", job->name, sizeof job->name, true);
    readString(L, "version", job->version, sizeof job->version, true);
    readString(L, "description", job->description, sizeof job->description, false);
    readString(L, "author", job->author, sizeof job->author, false);

    lua_getfield(L, -1, "apiVersion");
    if (!lua_isinteger(L, -1)) return luaL_error(L, "'apiVersion' must be an integer");
    job->apiVersion = static_cast<long long>(lua_tointeger(L, -1));
    return 0;
}

}  // namespace

ManifestStatus readManifest(const fs::path& file, Manifest& out, std::string& error) {
    std::error_code ec;
    if (!fs::exists(file, ec)) return ManifestStatus::missing;
    if (!fs::is_regular_file(file, ec)) {
        error = "manifest.lua is not a regular file";
        return ManifestStatus::invalid;
    }
    const auto size = fs::file_size(file, ec);
    if (ec || size > maxManifestBytes) {
        error = "manifest.lua is unreadable or larger than 16 KB";
        return ManifestStatus::invalid;
    }
    std::ifstream in(file, std::ios::binary);
    std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    Allocator allocator;
    lua_State* L = lua_newstate(manifestAlloc, &allocator);
    if (!L) {
        error = "could not create a Lua state for the manifest";
        return ManifestStatus::invalid;
    }
    Job job{};
    job.source = source.data();
    job.length = source.size();
    lua_pushcfunction(L, evaluate);
    lua_pushlightuserdata(L, &job);
    lua_sethook(L, stopHook, LUA_MASKCOUNT, manifestInstructions);
    const int rc = lua_pcall(L, 1, 0, 0);
    if (rc != LUA_OK) {
        const char* message = lua_tostring(L, -1);
        error = std::string("invalid manifest.lua: ") + (message ? message : "unknown error");
        lua_close(L);
        return ManifestStatus::invalid;
    }
    lua_close(L);

    out.name = job.name;
    out.version = job.version;
    out.description = job.description;
    out.author = job.author;
    out.apiVersion = job.apiVersion;
    return ManifestStatus::ok;
}

}  // namespace modloader
