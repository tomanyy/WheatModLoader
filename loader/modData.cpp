#include "modData.hpp"

#include <lua.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace modloader {
namespace {

constexpr std::size_t maxFileBytes = 1u << 20;
constexpr std::size_t maxTotalBytes = 4u << 20;
constexpr std::size_t maxFiles = 32;
constexpr double burstWrites = 5.0;
constexpr double refillPerSecond = 5.0;
constexpr const char* tokensKey = "modloader.dataTokens";
constexpr const char* stampKey = "modloader.dataStamp";

enum class Status { ok, notFound, tooBig, quota, ioError };

const char* statusText(Status status) {
    switch (status) {
        case Status::notFound: return "not found";
        case Status::tooBig: return "file too large (limit 1 MB)";
        case Status::quota: return "data quota exceeded (32 files, 4 MB)";
        default: return "i/o error";
    }
}

std::string toBytes(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

fs::path fromBytes(const std::string& bytes) { return fs::path(std::u8string(bytes.begin(), bytes.end())); }

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool validName(const char* name) {
    const std::size_t length = std::strlen(name);
    if (length == 0 || length > 64) return false;
    int dots = 0;
    std::size_t dotAt = 0;
    for (std::size_t i = 0; i < length; ++i) {
        const char c = name[i];
        if (c == '.') {
            ++dots;
            dotAt = i;
            continue;
        }
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    if (dots > 1) return false;
    if (dots == 1 && (dotAt == 0 || dotAt == length - 1 || length - dotAt - 1 > 8)) return false;

    static const char* const reserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7",
                                           "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    const std::string base = lower(std::string(name, dots ? dotAt : length));
    for (const char* word : reserved)
        if (base == word) return false;
    return true;
}

Status readData(const std::string& dirBytes, const char* name, std::string& out) {
    try {
        const fs::path path = fromBytes(dirBytes) / name;
        std::error_code ec;
        const fs::file_status st = fs::symlink_status(path, ec);
        if (ec || st.type() == fs::file_type::not_found) return Status::notFound;
        if (st.type() != fs::file_type::regular) return Status::ioError;
        const auto size = fs::file_size(path, ec);
        if (ec) return Status::ioError;
        if (size > maxFileBytes) return Status::tooBig;
        std::ifstream in(path, std::ios::binary);
        if (!in) return Status::ioError;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return Status::ok;
    } catch (...) {
        return Status::ioError;
    }
}

Status writeData(const std::string& dirBytes, const char* name, const char* content, std::size_t length) {
    try {
        if (length > maxFileBytes) return Status::tooBig;
        const fs::path dir = fromBytes(dirBytes);
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) return Status::ioError;

        std::size_t files = 0, total = 0, existing = 0;
        bool exists = false;
        const std::string wanted = lower(name);
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string file = it->path().filename().string();
            std::error_code e2;
            if (file.empty() || file[0] == '.' || !it->is_regular_file(e2)) continue;
            const auto size = it->file_size(e2);
            if (e2) continue;
            ++files;
            total += size;
            if (lower(file) == wanted) {
                exists = true;
                existing = size;
            }
        }
        if (!exists && files >= maxFiles) return Status::quota;
        if (total - existing + length > maxTotalBytes) return Status::quota;

        const fs::path temp = dir / (std::string(".") + name + ".tmp");
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) return Status::ioError;
            out.write(content, static_cast<std::streamsize>(length));
            out.flush();
            if (!out) {
                out.close();
                fs::remove(temp, ec);
                return Status::ioError;
            }
        }
        fs::rename(temp, dir / name, ec);
        if (ec) {
            fs::remove(temp, ec);
            return Status::ioError;
        }
        return Status::ok;
    } catch (...) {
        return Status::ioError;
    }
}

bool removeData(const std::string& dirBytes, const char* name) {
    try {
        const fs::path path = fromBytes(dirBytes) / name;
        std::error_code ec;
        const fs::file_status st = fs::symlink_status(path, ec);
        if (ec || st.type() != fs::file_type::regular) return false;
        return fs::remove(path, ec) && !ec;
    } catch (...) {
        return false;
    }
}

std::vector<std::string> listData(const std::string& dirBytes) {
    std::vector<std::string> names;
    try {
        std::error_code ec;
        for (fs::directory_iterator it(fromBytes(dirBytes), ec), end; !ec && it != end && names.size() < maxFiles; it.increment(ec)) {
            const std::string file = it->path().filename().string();
            std::error_code e2;
            if (!file.empty() && file[0] != '.' && it->is_regular_file(e2)) names.push_back(file);
        }
    } catch (...) {
    }
    std::sort(names.begin(), names.end());
    return names;
}

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool takeWriteToken(lua_State* L) {
    const double now = nowSeconds();
    lua_getfield(L, LUA_REGISTRYINDEX, tokensKey);
    double tokens = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : burstWrites;
    lua_pop(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, stampKey);
    const double last = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : now;
    lua_pop(L, 1);

    tokens = std::min(burstWrites, tokens + (now - last) * refillPerSecond);
    const bool granted = tokens >= 1.0;
    if (granted) tokens -= 1.0;
    lua_pushnumber(L, tokens);
    lua_setfield(L, LUA_REGISTRYINDEX, tokensKey);
    lua_pushnumber(L, now);
    lua_setfield(L, LUA_REGISTRYINDEX, stampKey);
    return granted;
}

const char* dirOf(lua_State* L) { return lua_tostring(L, lua_upvalueindex(1)); }

const char* checkName(lua_State* L, int index) {
    std::size_t length = 0;
    const char* name = luaL_checklstring(L, index, &length);
    if (std::strlen(name) != length || !validName(name)) luaL_error(L, "invalid data file name");
    return name;
}

int pushFailure(lua_State* L, const char* text) {
    lua_pushnil(L);
    lua_pushstring(L, text);
    return 2;
}

int dataRead(lua_State* L) {
    const char* name = checkName(L, 1);
    Status status;
    bool pushed = false;
    {
        std::string content;
        status = readData(dirOf(L), name, content);
        if (status == Status::ok) {
            lua_pushlstring(L, content.data(), content.size());
            pushed = true;
        }
    }
    if (pushed) return 1;
    if (status == Status::notFound) {
        lua_pushnil(L);
        return 1;
    }
    return pushFailure(L, statusText(status));
}

int dataWrite(lua_State* L) {
    const char* name = checkName(L, 1);
    std::size_t length = 0;
    const char* content = luaL_checklstring(L, 2, &length);
    if (!takeWriteToken(L)) return pushFailure(L, "write rate limit exceeded");
    const Status status = writeData(dirOf(L), name, content, length);
    if (status != Status::ok) return pushFailure(L, statusText(status));
    lua_pushboolean(L, 1);
    return 1;
}

int dataRemove(lua_State* L) {
    const char* name = checkName(L, 1);
    lua_pushboolean(L, removeData(dirOf(L), name));
    return 1;
}

int dataList(lua_State* L) {
    lua_newtable(L);
    const std::vector<std::string> names = listData(dirOf(L));
    lua_Integer index = 0;
    for (const std::string& name : names) {
        lua_pushlstring(L, name.data(), name.size());
        lua_rawseti(L, -2, ++index);
    }
    return 1;
}

}  // namespace

void pushDataApi(lua_State* L, const fs::path& dataDir) {
    const std::string dirBytes = toBytes(dataDir);
    const struct {
        const char* name;
        lua_CFunction function;
    } functions[] = {{"read", dataRead}, {"write", dataWrite}, {"remove", dataRemove}, {"list", dataList}};
    for (const auto& entry : functions) {
        lua_pushlstring(L, dirBytes.data(), dirBytes.size());
        lua_pushcclosure(L, entry.function, 1);
        lua_setfield(L, -2, entry.name);
    }
}

}  // namespace modloader
