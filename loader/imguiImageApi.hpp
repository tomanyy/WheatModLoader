#pragma once

#include <filesystem>
#include <set>

struct lua_State;

void addImguiImageApi(lua_State* state, const std::filesystem::path& modRoot,
                     const std::set<std::string>& allowed, bool enforce);
bool imguiFrameActive();
