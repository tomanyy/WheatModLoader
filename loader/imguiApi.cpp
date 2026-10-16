#include "lua_sandbox.hpp"

#include <imgui.h>
#include <lua.hpp>
#include "imguiImageApi.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <set>
#include <string>

namespace {

std::atomic<bool> frameActive{false};
thread_local int windowDepth = 0;

int beginWindow(lua_State* state) {
    if (lua_gettop(state) > 0 && lua_type(state, 1) != LUA_TSTRING)
        return luaL_error(state, "imgui.begin expects an optional string title");
    if (!frameActive.load(std::memory_order_relaxed) || !ImGui::GetCurrentContext()) {
        lua_pushboolean(state, false);
        return 1;
    }
    const char* title = lua_gettop(state) > 0 ? lua_tostring(state, 1) : lua_tostring(state, lua_upvalueindex(1));
    std::size_t length = title ? std::strlen(title) : 0;
    if (length > 128) length = 128;
    char safeTitle[129];
    if (length) std::memcpy(safeTitle, title, length);
    safeTitle[length] = '\0';
    const bool visible = ImGui::Begin(length ? safeTitle : "Wheat mod");
    ++windowDepth;
    lua_pushboolean(state, visible);
    return 1;
}

int text(lua_State* state) {
    if (lua_type(state, 1) != LUA_TSTRING) return luaL_error(state, "imgui.text expects a string");
    if (!frameActive.load(std::memory_order_relaxed) || !ImGui::GetCurrentContext()) return 0;
    std::size_t size = 0;
    const char* value = luaL_checklstring(state, 1, &size);
    size = std::min<std::size_t>(size, 2048);
    ImGui::TextUnformatted(value, value + size);
    return 0;
}

int button(lua_State* state) {
    if (lua_type(state, 1) != LUA_TSTRING) return luaL_error(state, "imgui.button expects a string");
    if (!frameActive.load(std::memory_order_relaxed) || !ImGui::GetCurrentContext()) {
        lua_pushboolean(state, false);
        return 1;
    }
    std::size_t size = 0;
    const char* value = luaL_checklstring(state, 1, &size);
    size = std::min<std::size_t>(size, 128);
    char label[129];
    std::memcpy(label, value, size);
    label[size] = '\0';
    lua_pushboolean(state, ImGui::Button(label));
    return 1;
}

int endWindow(lua_State*) {
    if (frameActive.load(std::memory_order_relaxed) && ImGui::GetCurrentContext() && windowDepth > 0) {
        ImGui::End();
        --windowDepth;
    }
    return 0;
}

int separator(lua_State*) {
    if (frameActive.load(std::memory_order_relaxed) && ImGui::GetCurrentContext()) ImGui::Separator();
    return 0;
}

int sameLine(lua_State*) {
    if (frameActive.load(std::memory_order_relaxed) && ImGui::GetCurrentContext()) ImGui::SameLine();
    return 0;
}

}

void setImguiFrameActive(bool active) {
    if (!active && frameActive.load(std::memory_order_relaxed) && ImGui::GetCurrentContext()) {
        while (windowDepth > 0) {
            ImGui::End();
            --windowDepth;
        }
    }
    frameActive.store(active, std::memory_order_relaxed);
}

bool imguiFrameActive() {
    return frameActive.load(std::memory_order_relaxed);
}

void pushImguiApi(lua_State* state, const std::string& displayName, const std::filesystem::path& modRoot,
                  const std::set<std::string>& allowed, bool enforce) {
    const auto add = [&](const char* api, const char* field, lua_CFunction function, bool modNameUpvalue = false) {
        if (enforce && !allowed.contains(api)) return;
        if (modNameUpvalue) lua_pushlstring(state, displayName.data(), std::min<std::size_t>(displayName.size(), 128));
        lua_pushcclosure(state, function, modNameUpvalue ? 1 : 0);
        lua_setfield(state, -2, field);
    };
    add("imgui.begin", "begin", beginWindow, true);
    add("imgui.text", "text", text);
    add("imgui.button", "button", button);
    add("imgui.finish", "finish", endWindow);
    add("imgui.separator", "separator", separator);
    add("imgui.same_line", "same_line", sameLine);
    addImguiImageApi(state, modRoot, allowed, enforce);
}
