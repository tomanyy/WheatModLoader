#include "consoleApi.hpp"

#include <lua.hpp>

#include <algorithm>
#include <set>
#include <chrono>
#include <cstring>

namespace modloader {
namespace {

// Keep this list explicit: broad prefixes such as snd_ also include engine
// diagnostics and debug controls. Names below were checked against a CS2 dump.
constexpr std::string_view exactNames[] = {
    "fps_max", "fps_max_ui", "cl_showfps", "sensitivity", "volume", "m_rawinput", "cl_language",
    "zoom_sensitivity_ratio",

    "cl_crosshairstyle", "cl_crosshair_recoil", "cl_crosshairdot", "cl_crosshair_drawoutline", "cl_crosshair_t",
    "cl_crosshair_gap", "cl_crosshair_length", "cl_crosshair_thickness", "cl_crosshair_dynamic_spread_limit",
    "cl_crosshaircolor_r", "cl_crosshaircolor_g", "cl_crosshaircolor_b", "cl_crosshaircolor_a",
    "cl_crosshairoutline_r", "cl_crosshairoutline_g", "cl_crosshairoutline_b", "cl_crosshairoutline_a",
    "cl_crosshair_dynamic_splitdist", "cl_crosshair_dynamic_splitalpha_innermod",
    "cl_crosshair_dynamic_splitalpha_outermod", "cl_crosshair_dynamic_maxdist_splitratio",
    "cl_crosshair_friendly_warning", "cl_crosshairsize", "cl_crosshairthickness", "cl_crosshairalpha",
    "cl_crosshair_sniper_width",

    "cl_hud_color", "cl_hud_telemetry_frametime_show", "cl_hud_telemetry_frametime_poor",
    "cl_hud_telemetry_ping_show", "cl_hud_telemetry_ping_poor", "cl_hud_telemetry_net_misdelivery_show",
    "cl_hud_telemetry_net_misdelivery_poor", "cl_hud_telemetry_net_detailed",
    "cl_hud_telemetry_net_quality_graph_show", "cl_hud_telemetry_serverrecvmargin_graph_show",
    "cl_hud_radar_scale", "cl_hud_radar_background_alpha", "cl_hud_radar_map_additive",
    "cl_hud_radar_blur_background",

    "cl_radar_rotate", "cl_radar_scale", "cl_radar_scale_alternate", "cl_radar_scale_dynamic",
    "cl_radar_always_centered", "cl_radar_icon_scale_min", "cl_radar_square_with_scoreboard",
    "cl_radar_square_always", "cl_radar_show_all_players_when_spectating", "cl_radar_square_when_spectating",

    "viewmodel_presetpos", "viewmodel_fov", "viewmodel_offset_x", "viewmodel_offset_y", "viewmodel_offset_z",

    "snd_gamevolume", "snd_voipvolume", "snd_gamevoicevolume", "snd_toolvolume", "snd_musicvolume",
    "snd_menumusic_volume", "snd_menumap_volume", "snd_roundstart_volume", "snd_roundaction_volume",
    "snd_roundend_volume", "snd_mvp_volume", "snd_mapobjective_volume", "snd_tensecondwarning_volume",
    "snd_deathcamera_volume", "snd_menumusic_volume_casual", "snd_roundstart_volume_casual",
    "snd_roundaction_volume_casual", "snd_roundend_volume_casual", "snd_mvp_volume_casual",
    "snd_mapobjective_volume_casual", "snd_tensecondwarning_volume_casual", "snd_deathcamera_volume_casual",
    "snd_menumusic_volume_armsrace", "snd_roundstart_volume_armsrace", "snd_roundaction_volume_armsrace",
    "snd_roundend_volume_armsrace", "snd_mvp_volume_armsrace", "snd_mapobjective_volume_armsrace",
    "snd_tensecondwarning_volume_armsrace", "snd_deathcamera_volume_armsrace",
    "snd_menumusic_volume_deathmatch", "snd_roundstart_volume_deathmatch", "snd_roundaction_volume_deathmatch",
    "snd_roundend_volume_deathmatch", "snd_mvp_volume_deathmatch", "snd_mapobjective_volume_deathmatch",
    "snd_tensecondwarning_volume_deathmatch", "snd_deathcamera_volume_deathmatch",
    "snd_menumusic_volume_rush", "snd_roundstart_volume_rush", "snd_roundaction_volume_rush",
    "snd_roundend_volume_rush", "snd_mvp_volume_rush", "snd_mapobjective_volume_rush",
    "snd_tensecondwarning_volume_rush", "snd_deathcamera_volume_rush", "snd_music_settings_mode",
    "snd_headphone_eq", "snd_headphone_eq_active", "snd_music_mode", "snd_mute_mvp_music_live_players",
    "snd_surf_volume_slide", "snd_surf_volume_inair", "snd_surf_volume_map"};

constexpr double burstReads = 200.0;
constexpr double refillPerSecond = 2000.0;
constexpr const char* tokensKey = "modloader.consoleTokens";
constexpr const char* stampKey = "modloader.consoleStamp";
constexpr std::size_t maxTextBytes = 1024;

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool takeReadToken(lua_State* L) {
    const double now = nowSeconds();
    lua_getfield(L, LUA_REGISTRYINDEX, tokensKey);
    double tokens = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : burstReads;
    lua_pop(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, stampKey);
    const double last = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : now;
    lua_pop(L, 1);

    tokens = std::min(burstReads, tokens + (now - last) * refillPerSecond);
    const bool granted = tokens >= 1.0;
    if (granted) tokens -= 1.0;
    lua_pushnumber(L, tokens);
    lua_setfield(L, LUA_REGISTRYINDEX, tokensKey);
    lua_pushnumber(L, now);
    lua_setfield(L, LUA_REGISTRYINDEX, stampKey);
    return granted;
}

ConsoleBackend* backendOf(lua_State* L) {
    return static_cast<ConsoleBackend*>(lua_touserdata(L, lua_upvalueindex(1)));
}

int pushFailure(lua_State* L, const char* reason) {
    lua_pushnil(L);
    lua_pushstring(L, reason);
    return 2;
}

void pushValue(lua_State* L, const ConsoleValue& value) {
    switch (value.kind) {
        case ConsoleValue::Kind::boolean: lua_pushboolean(L, value.boolean); break;
        case ConsoleValue::Kind::integer: lua_pushinteger(L, static_cast<lua_Integer>(value.integer)); break;
        case ConsoleValue::Kind::number: lua_pushnumber(L, value.number); break;
        case ConsoleValue::Kind::text: {
            const std::size_t length = std::min(value.text.size(), maxTextBytes);
            lua_pushlstring(L, value.text.data(), length);
            break;
        }
    }
}

const char* checkName(lua_State* L) {
    std::size_t length = 0;
    const char* name = luaL_checklstring(L, 1, &length);
    if (std::strlen(name) != length) luaL_error(L, "invalid convar name");
    return name;
}

int readConVar(lua_State* L, bool wantDefault) {
    const char* name = checkName(L);
    if (!takeReadToken(L)) return pushFailure(L, "rate limit exceeded");

    ConsoleBackend* backend = backendOf(L);
    bool allowed = false;
    ConsoleStatus status = ConsoleStatus::unavailable;
    ConsoleValue value;
    {
        const std::string key(name);
        allowed = isReadableConVar(key);
        if (allowed && backend && backend->available()) status = backend->read(key, wantDefault, value);
    }
    if (!allowed) return pushFailure(L, "not allowed");
    switch (status) {
        case ConsoleStatus::ok:
            pushValue(L, value);
            return 1;
        case ConsoleStatus::notFound: return pushFailure(L, "not found");
        case ConsoleStatus::unsupportedType: return pushFailure(L, "unsupported type");
        default: return pushFailure(L, "console unavailable");
    }
}

int consoleGet(lua_State* L) { return readConVar(L, false); }
int consoleDefault(lua_State* L) { return readConVar(L, true); }

int consoleAllowed(lua_State* L) {
    const char* name = checkName(L);
    bool allowed = false;
    {
        const std::string key(name);
        allowed = isReadableConVar(key);
    }
    lua_pushboolean(L, allowed);
    return 1;
}

int consoleAvailable(lua_State* L) {
    ConsoleBackend* backend = backendOf(L);
    lua_pushboolean(L, backend && backend->available());
    return 1;
}

}  // namespace

bool isReadableConVar(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return std::find(std::begin(exactNames), std::end(exactNames), name) != std::end(exactNames);
}

void pushConsoleApi(lua_State* L, ConsoleBackend* backend, const std::set<std::string>& allowList) {
    const struct {
        const char* name;
        lua_CFunction function;
    } functions[] = {{"get", consoleGet}, {"default", consoleDefault}, {"allowed", consoleAllowed}, {"available", consoleAvailable}};
    for (const auto& entry : functions) {
        if (!allowList.empty() && !allowList.contains(std::string("console.") + entry.name)) continue;
        lua_pushlightuserdata(L, backend);
        lua_pushcclosure(L, entry.function, 1);
        lua_setfield(L, -2, entry.name);
    }
}

}  // namespace modloader
