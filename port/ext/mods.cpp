// Lua mods with live reload. A mod is a folder under <user dir>/mods holding
// mod.json (id, name, version, author, description, entry) and Lua sources.
// Each mod runs in its own lua_State; saving any file in its folder reloads it,
// and a failing mod is shut down and reported without touching the game.
#include "internal.hpp"
#include <cJSON.h>
#include <imgui.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <type_traits>
#include <kf/platform/prelude.h>
#include <kf/game/player.h>
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace kf::ext {
namespace fs = std::filesystem;
namespace {
struct Mod {
    fs::path dir;
    std::string id, name, version, author, description, entry = "main.lua";
    bool enabled = false;
    lua_State *L = nullptr;
    std::string error;
    fs::file_time_type stamp{};
    std::size_t file_count = 0;
    bool config_dirty = false;
    bool has_settings = false;
};

std::vector<std::unique_ptr<Mod>> mods;
Mod *active;                     // the mod whose callback is running
bool in_imgui_frame;             // ui.* is only valid while drawing
Uint64 last_scan;
std::uint64_t frame_counter;
int input_context;
bool keys_now[SDL_SCANCODE_COUNT], keys_before[SDL_SCANCODE_COUNT];
constexpr const char *context_names[] = {"opening", "gameplay", "menu", "scripted"};

// ---------- small helpers ----------
std::string read_text(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

void mod_log(const Mod &mod, const std::string &text) {
    std::printf("[%s] %s\n", mod.id.c_str(), text.c_str());
}

Mod *mod_of(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "kf.mod");
    auto *mod = static_cast<Mod *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return mod;
}

// ---------- config <-> JSON ----------
cJSON *lua_to_json(lua_State *L, int index, int depth) {
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
    case LUA_TBOOLEAN: return cJSON_CreateBool(lua_toboolean(L, index));
    case LUA_TNUMBER: return cJSON_CreateNumber(lua_tonumber(L, index));
    case LUA_TSTRING: return cJSON_CreateString(lua_tostring(L, index));
    case LUA_TTABLE: {
        if (depth > 8)
            return nullptr;
        cJSON *object = cJSON_CreateObject();
        lua_pushnil(L);
        while (lua_next(L, index)) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                if (cJSON *value = lua_to_json(L, -1, depth + 1))
                    cJSON_AddItemToObject(object, lua_tostring(L, -2), value);
            }
            lua_pop(L, 1);
        }
        return object;
    }
    default: return nullptr;
    }
}

void json_to_lua(lua_State *L, const cJSON *item) {
    if (cJSON_IsBool(item))
        lua_pushboolean(L, cJSON_IsTrue(item));
    else if (cJSON_IsNumber(item)) {
        if (item->valuedouble == static_cast<double>(static_cast<lua_Integer>(item->valuedouble)))
            lua_pushinteger(L, static_cast<lua_Integer>(item->valuedouble));
        else
            lua_pushnumber(L, item->valuedouble);
    } else if (cJSON_IsString(item))
        lua_pushstring(L, item->valuestring);
    else if (cJSON_IsObject(item)) {
        lua_newtable(L);
        for (const cJSON *child = item->child; child; child = child->next) {
            json_to_lua(L, child);
            lua_setfield(L, -2, child->string);
        }
    } else
        lua_pushnil(L);
}

void save_config(Mod &mod) {
    if (!mod.L)
        return;
    lua_getglobal(mod.L, "config");
    if (lua_istable(mod.L, -1)) {
        cJSON *entry = settings_mod_entry(mod.id);
        cJSON *value = lua_to_json(mod.L, -1, 0);
        if (cJSON_GetObjectItemCaseSensitive(entry, "config"))
            cJSON_ReplaceItemInObjectCaseSensitive(entry, "config", value);
        else
            cJSON_AddItemToObject(entry, "config", value);
        settings_mark_dirty();
    }
    lua_pop(mod.L, 1);
    mod.config_dirty = false;
}

// ---------- kf.player ----------
struct PlayerField {
    const char *name;
    lua_Integer (*get)();
    void (*set)(lua_Integer);
};
#define KF_FIELD(label, expression) \
    {label, [] { return static_cast<lua_Integer>(expression); }, \
     [](lua_Integer value) { expression = static_cast<std::remove_reference_t<decltype(expression)>>(value); }}
const PlayerField player_fields[] = {
    KF_FIELD("hp", player_state.vitals.current_hp),
    KF_FIELD("max_hp", player_state.vitals.maximum_hp),
    KF_FIELD("mp", player_state.vitals.current_mp),
    KF_FIELD("max_mp", player_state.vitals.maximum_mp),
    KF_FIELD("gold", player_state.gold),
    KF_FIELD("experience", player_state.experience),
    KF_FIELD("next_level_experience", player_state.next_level_experience),
    KF_FIELD("level", player_state.progress_state.level),
    KF_FIELD("physical_power", player_state.physical_power),
    KF_FIELD("magic", player_state.magic),
    KF_FIELD("base_physical_power", player_state.base_physical_power),
    KF_FIELD("base_magic", player_state.base_magic),
    KF_FIELD("physical_power_training", player_state.physical_power_training),
    KF_FIELD("magic_training", player_state.magic_training),
    KF_FIELD("magic_charge", player_state.magic_charge),
    KF_FIELD("status_flags", player_state.status_effect_flags),
    KF_FIELD("cutting_attack", player_state.cutting_attack),
    KF_FIELD("striking_attack", player_state.striking_attack),
    KF_FIELD("piercing_attack", player_state.piercing_attack),
    KF_FIELD("holy_attack", player_state.holy_attack),
    KF_FIELD("fire_attack", player_state.fire_attack),
    KF_FIELD("cutting_defense", player_state.cutting_defense),
    KF_FIELD("striking_defense", player_state.striking_defense),
    KF_FIELD("piercing_defense", player_state.piercing_defense),
    KF_FIELD("poison_resistance", player_state.poison_resistance),
    KF_FIELD("magic_defense", player_state.magic_defense),
    KF_FIELD("fire_defense", player_state.fire_defense),
    KF_FIELD("curse_timer", player_state.curse_timer),
    KF_FIELD("darkness_timer", player_state.darkness_timer),
    KF_FIELD("poison_timer", player_state.poison_timer),
    KF_FIELD("slowed_timer", player_state.slowed_timer),
};
#undef KF_FIELD

const PlayerField *find_field(const char *name) {
    for (const auto &field : player_fields)
        if (std::strcmp(field.name, name) == 0)
            return &field;
    return nullptr;
}

int player_index(lua_State *L) {
    const char *name = luaL_checkstring(L, 2);
    if (const auto *field = find_field(name)) {
        lua_pushinteger(L, field->get());
        return 1;
    }
    if (std::strcmp(name, "floor") == 0) {
        lua_pushinteger(L, kf_enum_encode<lua_Integer>(player_state.progress_state.current_floor));
        return 1;
    }
    return luaL_error(L, "kf.player has no field '%s' (see kf.player_fields())", name);
}

int player_newindex(lua_State *L) {
    const char *name = luaL_checkstring(L, 2);
    const auto *field = find_field(name);
    if (!field)
        return luaL_error(L, "kf.player.%s cannot be written", name);
    field->set(luaL_checkinteger(L, 3));
    return 0;
}

// ---------- kf.* ----------
int kf_log(lua_State *L) {
    std::string text;
    const int count = lua_gettop(L);
    for (int i = 1; i <= count; ++i) {
        if (i > 1)
            text += '\t';
        text += luaL_tolstring(L, i, nullptr);
        lua_pop(L, 1);
    }
    mod_log(*mod_of(L), text);
    return 0;
}
int kf_frame(lua_State *L) { lua_pushinteger(L, static_cast<lua_Integer>(frame_counter)); return 1; }
int kf_time(lua_State *L) { lua_pushnumber(L, static_cast<double>(SDL_GetTicks()) / 1000.0); return 1; }
int kf_context(lua_State *L) {
    lua_pushstring(L, context_names[std::clamp(input_context, 0, 3)]);
    return 1;
}
SDL_Scancode check_key(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    const SDL_Scancode code = SDL_GetScancodeFromName(name);
    if (code == SDL_SCANCODE_UNKNOWN)
        luaL_error(L, "unknown key '%s' (use SDL key names such as \"F5\", \"G\", \"Left Shift\")", name);
    return code;
}
int kf_key_down(lua_State *L) {
    const auto code = check_key(L);
    lua_pushboolean(L, !overlay_captures_input() && keys_now[code]);
    return 1;
}
int kf_key_pressed(lua_State *L) {
    const auto code = check_key(L);
    lua_pushboolean(L, !overlay_captures_input() && keys_now[code] && !keys_before[code]);
    return 1;
}
int kf_player_fields(lua_State *L) {
    lua_newtable(L);
    int i = 1;
    for (const auto &field : player_fields) {
        lua_pushstring(L, field.name);
        lua_rawseti(L, -2, i++);
    }
    lua_pushstring(L, "floor");
    lua_rawseti(L, -2, i);
    return 1;
}
int kf_view(lua_State *L) {
    const auto view = game_view();
    lua_pushnumber(L, view.x); lua_pushnumber(L, view.y);
    lua_pushnumber(L, view.w); lua_pushnumber(L, view.h);
    return 4;
}
int kf_view_original(lua_State *L) {
    const auto view = original_view();
    lua_pushnumber(L, view.x); lua_pushnumber(L, view.y);
    lua_pushnumber(L, view.w); lua_pushnumber(L, view.h);
    return 4;
}
int kf_save_config(lua_State *L) {
    save_config(*mod_of(L));
    return 0;
}

// ---------- ui.* (ImGui) ----------
void require_frame(lua_State *L) {
    if (!in_imgui_frame)
        luaL_error(L, "ui.* functions only work inside on_draw or on_settings");
}
ImU32 check_color(lua_State *L, int first, ImU32 fallback) {
    if (lua_isnoneornil(L, first))
        return fallback;
    const auto channel = [&](int i, double otherwise) {
        return static_cast<float>(luaL_optnumber(L, first + i, otherwise));
    };
    return ImGui::GetColorU32(ImVec4(channel(0, 1), channel(1, 1), channel(2, 1), channel(3, 1)));
}
void mark_changed(lua_State *L, bool changed) {
    if (changed)
        mod_of(L)->config_dirty = true;
}
int ui_text(lua_State *L) { require_frame(L); ImGui::TextUnformatted(luaL_checkstring(L, 1)); return 0; }
int ui_text_colored(lua_State *L) {
    require_frame(L);
    const ImVec4 color(static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)),
                       static_cast<float>(luaL_checknumber(L, 3)), 1.0f);
    ImGui::TextColored(color, "%s", luaL_checkstring(L, 4));
    return 0;
}
int ui_button(lua_State *L) { require_frame(L); lua_pushboolean(L, ImGui::Button(luaL_checkstring(L, 1))); return 1; }
int ui_checkbox(lua_State *L) {
    require_frame(L);
    bool value = lua_toboolean(L, 2);
    mark_changed(L, ImGui::Checkbox(luaL_checkstring(L, 1), &value));
    lua_pushboolean(L, value);
    return 1;
}
int ui_slider_int(lua_State *L) {
    require_frame(L);
    int value = static_cast<int>(luaL_checkinteger(L, 2));
    mark_changed(L, slider_input(luaL_checkstring(L, 1), &value, static_cast<int>(luaL_checkinteger(L, 3)),
                                 static_cast<int>(luaL_checkinteger(L, 4))));
    lua_pushinteger(L, value);
    return 1;
}
int ui_slider_float(lua_State *L) {
    require_frame(L);
    float value = static_cast<float>(luaL_checknumber(L, 2));
    mark_changed(L, slider_input(luaL_checkstring(L, 1), &value, static_cast<float>(luaL_checknumber(L, 3)),
                                 static_cast<float>(luaL_checknumber(L, 4)), luaL_optstring(L, 5, "%.2f")));
    lua_pushnumber(L, value);
    return 1;
}
int ui_combo(lua_State *L) {
    require_frame(L);
    const char *label = luaL_checkstring(L, 1);
    int index = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<std::string> items;
    for (lua_Integer i = 1; lua_rawgeti(L, 3, i) == LUA_TSTRING; ++i) {
        items.emplace_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    const char *preview = index >= 0 && index < static_cast<int>(items.size()) ? items[index].c_str() : "";
    if (ImGui::BeginCombo(label, preview)) {
        for (int i = 0; i < static_cast<int>(items.size()); ++i)
            if (ImGui::Selectable(items[i].c_str(), i == index)) {
                index = i;
                mark_changed(L, true);
            }
        ImGui::EndCombo();
    }
    lua_pushinteger(L, index + 1);
    return 1;
}
int ui_separator(lua_State *L) { require_frame(L); ImGui::Separator(); return 0; }
int ui_same_line(lua_State *L) { require_frame(L); ImGui::SameLine(); return 0; }
int ui_spacing(lua_State *L) { require_frame(L); ImGui::Spacing(); return 0; }
int ui_tooltip(lua_State *L) { require_frame(L); ImGui::SetItemTooltip("%s", luaL_checkstring(L, 1)); return 0; }
int ui_begin_window(lua_State *L) {
    require_frame(L);
    const bool no_input = lua_toboolean(L, 2);
    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;
    if (no_input && !overlay_open())
        flags |= ImGuiWindowFlags_NoInputs;
    lua_pushboolean(L, ImGui::Begin(luaL_checkstring(L, 1), nullptr, flags));
    return 1;
}
int ui_end_window(lua_State *L) { require_frame(L); ImGui::End(); return 0; }
int ui_draw_text(lua_State *L) {
    require_frame(L);
    const ImVec2 at(static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)));
    const char *text = luaL_checkstring(L, 3);
    const ImU32 color = check_color(L, 4, IM_COL32_WHITE);
    // An explicit pixel size keeps HUD text independent of the interface scale.
    const float size = static_cast<float>(luaL_optnumber(L, 8, hud_font_size));
    ImFont *font = ImGui::GetFont();
    ImDrawList *list = ImGui::GetForegroundDrawList();
    list->AddText(font, size, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 200), text);
    list->AddText(font, size, at, color, text);
    return 0;
}
int ui_draw_rect(lua_State *L) {
    require_frame(L);
    const float x = static_cast<float>(luaL_checknumber(L, 1)), y = static_cast<float>(luaL_checknumber(L, 2));
    const float w = static_cast<float>(luaL_checknumber(L, 3)), h = static_cast<float>(luaL_checknumber(L, 4));
    const ImU32 color = check_color(L, 5, IM_COL32_WHITE);
    const bool filled = lua_isnoneornil(L, 9) || lua_toboolean(L, 9);
    ImDrawList *list = ImGui::GetForegroundDrawList();
    if (filled)
        list->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), color);
    else
        list->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), color);
    return 0;
}
// Maps the PS1's 320x240 picture coordinates onto the window (the original area
// sits centred in a widescreen picture).
int ui_game_to_screen(lua_State *L) {
    const auto view = original_view();
    lua_pushnumber(L, view.x + luaL_checknumber(L, 1) * view.w / 320.0);
    lua_pushnumber(L, view.y + luaL_checknumber(L, 2) * view.h / 240.0);
    return 2;
}

void register_api(lua_State *L, Mod &mod) {
    lua_pushlightuserdata(L, &mod);
    lua_setfield(L, LUA_REGISTRYINDEX, "kf.mod");

    const luaL_Reg kf_functions[] = {
        {"log", kf_log}, {"frame", kf_frame}, {"time", kf_time}, {"context", kf_context},
        {"key_down", kf_key_down}, {"key_pressed", kf_key_pressed}, {"player_fields", kf_player_fields},
        {"view", kf_view}, {"view_original", kf_view_original}, {"save_config", kf_save_config}, {nullptr, nullptr}};
    luaL_newlib(L, kf_functions);
    lua_pushstring(L, "0.1");
    lua_setfield(L, -2, "api_version");
    lua_newtable(L);
    lua_newtable(L);
    lua_pushcfunction(L, player_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, player_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    lua_setfield(L, -2, "player");
    lua_setglobal(L, "kf");

    const luaL_Reg ui_functions[] = {
        {"text", ui_text}, {"text_colored", ui_text_colored}, {"button", ui_button}, {"checkbox", ui_checkbox},
        {"slider_int", ui_slider_int}, {"slider_float", ui_slider_float}, {"combo", ui_combo},
        {"separator", ui_separator}, {"same_line", ui_same_line}, {"spacing", ui_spacing}, {"tooltip", ui_tooltip},
        {"begin_window", ui_begin_window}, {"end_window", ui_end_window}, {"draw_text", ui_draw_text},
        {"draw_rect", ui_draw_rect}, {"game_to_screen", ui_game_to_screen}, {nullptr, nullptr}};
    luaL_newlib(L, ui_functions);
    lua_setglobal(L, "ui");

    // require() finds files in the mod's own folder.
    lua_getglobal(L, "package");
    const auto dir = mod.dir.generic_string();
    lua_pushstring(L, (dir + "/?.lua;" + dir + "/?/init.lua").c_str());
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);

    const cJSON *entry = settings_mod_entry(mod.id);
    const cJSON *config = cJSON_GetObjectItemCaseSensitive(entry, "config");
    if (cJSON_IsObject(config))
        json_to_lua(L, config);
    else
        lua_newtable(L);
    lua_setglobal(L, "config");
}

// ---------- lifecycle ----------
int traceback(lua_State *L) {
    luaL_traceback(L, L, lua_tostring(L, 1), 1);
    return 1;
}

void unload(Mod &mod, bool call_unload = true);

void fail(Mod &mod, const std::string &message) {
    mod.error = message;
    mod_log(mod, "error: " + message);
    unload(mod, false);
}

// Calls a global function if the mod defines it. Returns false if the mod failed.
bool call(Mod &mod, const char *name) {
    if (!mod.L)
        return false;
    lua_State *L = mod.L;
    lua_pushcfunction(L, traceback);
    if (lua_getglobal(L, name) != LUA_TFUNCTION) {
        lua_pop(L, 2);
        return true;
    }
    Mod *previous = active;
    active = &mod;
    const int status = lua_pcall(L, 0, 0, -2);
    active = previous;
    if (status != LUA_OK) {
        std::string message = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
        lua_pop(L, 2);
        fail(mod, std::string(name) + ": " + message);
        return false;
    }
    lua_pop(L, 1);
    return true;
}

bool defines(Mod &mod, const char *name) {
    if (!mod.L)
        return false;
    const bool result = lua_getglobal(mod.L, name) == LUA_TFUNCTION;
    lua_pop(mod.L, 1);
    return result;
}

void unload(Mod &mod, bool call_unload) {
    if (!mod.L)
        return;
    if (call_unload)
        call(mod, "on_unload");
    if (mod.L && mod.config_dirty)
        save_config(mod);
    if (mod.L)
        lua_close(mod.L);
    mod.L = nullptr;
    mod.has_settings = false;
}

void load(Mod &mod) {
    unload(mod);
    mod.error.clear();
    const Uint64 started = SDL_GetTicksNS();
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    register_api(L, mod);
    mod.L = L;
    lua_pushcfunction(L, traceback);
    const auto entry = (mod.dir / mod.entry).string();
    int status = luaL_loadfile(L, entry.c_str());
    if (status == LUA_OK) {
        active = &mod;
        status = lua_pcall(L, 0, 0, -2);
        active = nullptr;
    }
    if (status != LUA_OK) {
        std::string message = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
        fail(mod, message);
        return;
    }
    lua_pop(L, 1);
    if (!call(mod, "on_load"))
        return;
    mod.has_settings = defines(mod, "on_settings");
    char elapsed[32];
    std::snprintf(elapsed, sizeof elapsed, "%.1f", static_cast<double>(SDL_GetTicksNS() - started) / 1e6);
    mod_log(mod, "loaded " + mod.name + " " + mod.version + " (" + elapsed + " ms)");
}

void read_manifest(Mod &mod) {
    const auto folder = mod.dir.filename().string();
    mod.id = folder;
    mod.name = folder;
    mod.version = mod.author = mod.description = "";
    mod.entry = "main.lua";
    const auto manifest = mod.dir / "mod.json";
    if (!fs::exists(manifest))
        return;
    cJSON *json = cJSON_Parse(read_text(manifest).c_str());
    if (!json) {
        mod.error = "mod.json is not valid JSON";
        return;
    }
    const auto text = [&](const char *key, std::string &target) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
        if (cJSON_IsString(item) && *item->valuestring)
            target = item->valuestring;
    };
    text("id", mod.id);
    mod.name = mod.id;
    text("name", mod.name);
    text("version", mod.version);
    text("author", mod.author);
    text("description", mod.description);
    text("entry", mod.entry);
    cJSON_Delete(json);
}

// Newest write time over mod.json and every .lua file, plus how many there are,
// so adding or deleting a file also counts as a change.
void fingerprint(const fs::path &dir, fs::file_time_type &stamp, std::size_t &count) {
    stamp = {};
    count = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error))
            continue;
        const auto extension = it->path().extension().string();
        if (extension != ".lua" && it->path().filename() != "mod.json")
            continue;
        stamp = std::max(stamp, it->last_write_time(error));
        ++count;
    }
}

void set_enabled(Mod &mod, bool enabled) {
    mod.enabled = enabled;
    cJSON *entry = settings_mod_entry(mod.id);
    if (cJSON_GetObjectItemCaseSensitive(entry, "enabled"))
        cJSON_ReplaceItemInObjectCaseSensitive(entry, "enabled", cJSON_CreateBool(enabled));
    else
        cJSON_AddBoolToObject(entry, "enabled", enabled);
    settings_mark_dirty();
    if (enabled)
        load(mod);
    else {
        unload(mod);
        mod.error.clear();
        mod_log(mod, "disabled");
    }
}

void scan() {
    std::error_code error;
    fs::create_directories(mods_dir(), error);
    std::vector<fs::path> found;
    for (fs::directory_iterator it(mods_dir(), error), end; !error && it != end; it.increment(error))
        if (it->is_directory(error) && (fs::exists(it->path() / "mod.json") || fs::exists(it->path() / "main.lua")))
            found.push_back(it->path());
    // Removed folders.
    for (auto it = mods.begin(); it != mods.end();) {
        if (std::find(found.begin(), found.end(), (*it)->dir) == found.end()) {
            unload(**it);
            std::printf("[mods] %s removed\n", (*it)->id.c_str());
            it = mods.erase(it);
        } else
            ++it;
    }
    for (const auto &dir : found) {
        auto existing = std::find_if(mods.begin(), mods.end(), [&](const auto &mod) { return mod->dir == dir; });
        fs::file_time_type stamp;
        std::size_t count;
        fingerprint(dir, stamp, count);
        if (existing == mods.end()) {
            auto mod = std::make_unique<Mod>();
            mod->dir = dir;
            read_manifest(*mod);
            mod->stamp = stamp;
            mod->file_count = count;
            const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(settings_mod_entry(mod->id), "enabled");
            mod->enabled = cJSON_IsTrue(enabled);
            std::printf("[mods] found %s%s\n", mod->id.c_str(), mod->enabled ? " (enabled)" : "");
            if (mod->enabled)
                load(*mod);
            mods.push_back(std::move(mod));
        } else if ((*existing)->stamp != stamp || (*existing)->file_count != count) {
            Mod &mod = **existing;
            mod.stamp = stamp;
            mod.file_count = count;
            read_manifest(mod);
            mod_log(mod, "changed on disk");
            if (mod.enabled)
                load(mod);
        }
    }
    std::sort(mods.begin(), mods.end(), [](const auto &a, const auto &b) { return a->name < b->name; });
}
}

void mods_start() {
    // First launch: copy the example mods shipped beside the executable. They
    // stay disabled until switched on in the Mods window.
    std::error_code error;
    if (!std::filesystem::exists(mods_dir(), error)) {
        const char *base = SDL_GetBasePath();
        const auto bundled = std::filesystem::path(reinterpret_cast<const char8_t *>(base ? base : "")) / "mods";
        if (base && std::filesystem::is_directory(bundled, error)) {
            std::filesystem::copy(bundled, mods_dir(), std::filesystem::copy_options::recursive, error);
            std::printf("[mods] copied the example mods to %s (all disabled)\n", mods_dir().string().c_str());
        }
    }
    scan();
    last_scan = SDL_GetTicks();
}

void mods_shutdown() {
    for (auto &mod : mods)
        unload(*mod);
    mods.clear();
}

void mods_poll() {
    if (SDL_GetTicks() - last_scan < 500)
        return;
    last_scan = SDL_GetTicks();
    scan();
}

void mods_frame(int context) {
    ++frame_counter;
    input_context = context;
    int count = 0;
    const bool *state = SDL_GetKeyboardState(&count);
    std::copy_n(keys_now, SDL_SCANCODE_COUNT, keys_before);
    for (int i = 0; i < SDL_SCANCODE_COUNT; ++i)
        keys_now[i] = i < count && state[i];
    for (auto &mod : mods)
        if (mod->L)
            call(*mod, "on_frame");
}

void mods_draw_hud() {
    // Mod windows use the base font size whatever the interface scale is.
    ImGui::PushFont(nullptr, hud_font_size / ImGui::GetStyle().FontScaleMain);
    in_imgui_frame = true;
    for (auto &mod : mods) {
        if (!mod->L)
            continue;
        ImGui::PushID(mod->id.c_str());
        call(*mod, "on_draw");
        ImGui::PopID();
    }
    in_imgui_frame = false;
    ImGui::PopFont();
}

int mods_loaded_count() {
    return static_cast<int>(std::count_if(mods.begin(), mods.end(), [](const auto &mod) { return mod->L != nullptr; }));
}

void mods_draw_panel() {
    const auto folder = mods_dir().string();
    ImGui::TextDisabled("Folder: %s", folder.c_str());
    if (ImGui::Button("Open mods folder"))
        SDL_OpenURL(("file:///" + mods_dir().generic_string()).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Reload all"))
        for (auto &mod : mods)
            if (mod->enabled)
                load(*mod);
    ImGui::SameLine();
    ImGui::TextDisabled("(saving a mod's files reloads it automatically)");
    ImGui::Separator();
    if (mods.empty())
        ImGui::TextWrapped("No mods yet. Create a folder in the mods folder with a mod.json and main.lua.");
    in_imgui_frame = true;
    for (auto &mod_pointer : mods) {
        Mod &mod = *mod_pointer;
        ImGui::PushID(mod.id.c_str());
        bool enabled = mod.enabled;
        if (ImGui::Checkbox("##enabled", &enabled))
            set_enabled(mod, enabled);
        ImGui::SameLine();
        ImGui::Text("%s", mod.name.c_str());
        if (!mod.version.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("v%s", mod.version.c_str());
        }
        if (!mod.author.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("by %s", mod.author.c_str());
        }
        ImGui::SameLine();
        if (!mod.error.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "error");
        else if (mod.L)
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "running");
        ImGui::Indent();
        if (!mod.description.empty())
            ImGui::TextWrapped("%s", mod.description.c_str());
        if (!mod.error.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.45f, 1.0f), "%s", mod.error.c_str());
        if (mod.L && mod.has_settings && ImGui::TreeNode("Settings")) {
            call(mod, "on_settings");
            if (mod.L && mod.config_dirty)
                save_config(mod);
            ImGui::TreePop();
        }
        ImGui::Unindent();
        ImGui::Separator();
        ImGui::PopID();
    }
    in_imgui_frame = false;
}
}
