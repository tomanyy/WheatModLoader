#pragma once
#include <filesystem>

struct lua_State;

namespace modloader {

void pushDataApi(lua_State* L, const std::filesystem::path& dataDir);

}  // namespace modloader
