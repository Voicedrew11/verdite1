#include <kf/platform/agent.h>
#include <kf/platform/coop.h>
#include <kf/ext/ext.h>
#include <kf/platform/host.h>
#include <kf/platform/fuzz.h>
#include <kf/platform/input.h>
#include <kf/platform/menu_pointer.h>
#include <kf/platform/online_players.h>
#include <kf/platform/title.h>
#include <kf/game/actor.h>
#include <kf/game/effect.h>
#include <kf/game/magic.h>
#include <kf/lib/map_types.h>
#include <kf/game/player.h>
#include <kf/lib/item.h>
#include <kf/lib/map.h>
#include <kf/lib/map_data.h>
#include <kf/lib/codec.h>
#include <verdite/agent.h>
#include <verdite/crash.h>
#include <verdite/menu_pointer.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

namespace kf {
namespace {
constexpr int default_port = 27901;   // Verdite2 27900, Verdite3 27903
constexpr std::uint64_t ns_per_ms = 1000000;
constexpr long default_hold_ms = 150, max_hold_ms = 600000;

struct Press { u32 buttons = 0; std::uint64_t until_ns = 0; };
Press press;
bool quit_requested;
std::optional<std::array<int, 2>> move_request; // a cell, applied between updates
std::optional<std::array<long, 2>> give_request; // an item (255: gold) and a count, applied between updates
std::optional<int> warp_request;                // a floor, applied between updates
std::optional<int> hurt_request;                // hp lost between updates, as poison takes it
std::optional<std::size_t> take_request;        // an object slot emptied between updates, as a pickup does
std::optional<std::size_t> hit_request;         // an actor slot struck between updates, as the sword does
bool cast_request = false;                      // a Fire Ball launched between updates, as the magic button does

struct ButtonName { const char *name; Button button; };
constexpr std::array<ButtonName, 14> button_names = {{
    {"up", Button::Up}, {"down", Button::Down}, {"left", Button::Left}, {"right", Button::Right},
    {"strafe_left", Button::StrafeLeft}, {"strafe_right", Button::StrafeRight},
    {"look_up", Button::LookUp}, {"look_down", Button::LookDown},
    {"attack", Button::Attack}, {"magic", Button::Magic}, {"confirm", Button::Confirm},
    {"back", Button::Back}, {"start", Button::Start}, {"select", Button::Select}}};

std::string snapshot() {
    const InputContext context = host_input_context();
    const auto &vitals = player_state.vitals;
    char text[512];
    // The player block is filled by a new game or a load; the opening leaves it stale.
    const bool in_game = context != InputContext::Opening && vitals.maximum_hp != 0;
    int n = std::snprintf(text, sizeof text, "{\"context\":\"%s\",\"inGame\":%s,\"clockMs\":%llu,\"frames\":%llu",
        input_context_name(context), in_game ? "true" : "false",
        static_cast<unsigned long long>(host_clock_ns() / ns_per_ms),
        static_cast<unsigned long long>(host_frames_presented()));
    if (in_game) {
        const auto &position = player_state.camera_position;
        const auto &rotation = player_state.camera_rotation;
        n += std::snprintf(text + n, sizeof text - n,
            ",\"hp\":%u,\"maxHp\":%u,\"mp\":%u,\"maxMp\":%u,\"area\":%d,\"level\":%u,\"gold\":%u,"
            "\"experience\":%d,\"pos\":[%d,%d,%d],\"pitch\":%d,\"yaw\":%d",
            vitals.current_hp, vitals.maximum_hp, vitals.current_mp, vitals.maximum_mp,
            static_cast<int>(player_state.progress_state.current_floor), player_state.progress_state.level,
            player_state.gold, player_state.experience, position.vx, position.vy, position.vz, rotation.vx, rotation.vy);
    }
    return std::string(text, n) + "}";
}

std::string do_state(verdite::AgentArgs) { return verdite::json_ok("state", ",\"state\":" + snapshot()); }

std::string do_press(verdite::AgentArgs args) {
    if (args.empty())
        return verdite::json_error("press <button>[+<button>...] [holdMs=150]");
    u32 buttons = 0;
    std::string names = args[0];
    for (std::size_t at = 0; at <= names.size();) {
        const std::size_t end = std::min(names.find('+', at), names.size());
        const std::string name = names.substr(at, end - at);
        bool found = false;
        for (const auto &entry : button_names)
            if (name == entry.name) {
                buttons |= button_mask(entry.button);
                found = true;
            }
        if (!found)
            return verdite::json_error("no such button '" + name + "'; try help");
        at = end + 1;
    }
    long hold = default_hold_ms;
    if (args.size() > 1) {
        char *end = nullptr;
        hold = std::strtol(args[1].c_str(), &end, 10);
        if (*end || hold < 1 || hold > max_hold_ms)
            return verdite::json_error("holdMs must be 1..600000");
    }
    press = {buttons, host_clock_ns() + static_cast<std::uint64_t>(hold) * ns_per_ms};
    verdite::crash_event("agent: press " + names + " " + std::to_string(hold) + " ms");
    return verdite::json_ok("press", ",\"button\":" + verdite::json_quote(names) + ",\"holdMs\":" + std::to_string(hold));
}

std::string do_release(verdite::AgentArgs) {
    press = {};
    return verdite::json_ok("release");
}

std::string do_move(verdite::AgentArgs args) {
    if (args.size() != 2)
        return verdite::json_error("move <cell_x> <cell_z>");
    if (coop_player_down())
        return verdite::json_error("the player is down");
    char *end_x = nullptr, *end_z = nullptr;
    const long x = std::strtol(args[0].c_str(), &end_x, 10), z = std::strtol(args[1].c_str(), &end_z, 10);
    if (*end_x || *end_z || x < 0 || z < 0 || x >= KF_MAP_COLUMNS || z >= KF_MAP_ROWS)
        return verdite::json_error("cells are 0..99");
    move_request = std::array{static_cast<int>(x), static_cast<int>(z)};
    return verdite::json_ok("move");
}

std::string do_warp(verdite::AgentArgs args) {
    if (args.size() != 1)
        return verdite::json_error("warp <floor 1..5>");
    if (coop_player_down())
        return verdite::json_error("the player is down");
    char *end = nullptr;
    const long floor = std::strtol(args[0].c_str(), &end, 10);
    if (*end || floor < 1 || floor > 5)
        return verdite::json_error("warp <floor 1..5>");
    warp_request = static_cast<int>(floor);
    return verdite::json_ok("warp");
}

// An object slot of this floor: its id (255 for none), and `take` empties it at the next update.
std::string do_object(verdite::AgentArgs args) {
    if (args.empty() || args.size() > 2 || (args.size() == 2 && args[1] != "take"))
        return verdite::json_error("object <slot 0..189> [take]");
    char *end = nullptr;
    const long index = std::strtol(args[0].c_str(), &end, 10);
    if (*end || index < 0 || index >= KF_MAP_OBJECT_CAPACITY)
        return verdite::json_error("object <slot 0..189> [take]");
    if (args.size() == 2)
        take_request = static_cast<std::size_t>(index);
    const auto id = kf_enum_encode<u8>(map_object_state.objects[static_cast<std::size_t>(index)].object_id);
    return verdite::json_ok("object", ",\"id\":" + std::to_string(id));
}

// Strikes an actor slot at the next update as the player's sword does, at full charge; or aims a
// spell at it, as the cone's target is chosen.
std::string do_hit(verdite::AgentArgs args) {
    if (args.empty() || args.size() > 2 || (args.size() == 2 && args[1] != "aim"))
        return verdite::json_error("hit <slot 0..127> [aim]");
    char *end = nullptr;
    const long slot = std::strtol(args[0].c_str(), &end, 10);
    if (*end || slot < 0 || slot >= KF_ACTOR_CAPACITY)
        return verdite::json_error("hit <slot 0..127> [aim]");
    if (args.size() == 2)
        actor_state.player_target = &actor_state.actors[static_cast<std::size_t>(slot)];
    else
        hit_request = static_cast<std::size_t>(slot);
    return verdite::json_ok("hit");
}

std::string do_give(verdite::AgentArgs args) {
    if (args.size() != 2)
        return verdite::json_error("give <item> <count>");
    char *end_item = nullptr, *end_count = nullptr;
    const long item = std::strtol(args[0].c_str(), &end_item, 10), count = std::strtol(args[1].c_str(), &end_count, 10);
    if (*end_item || *end_count || (item != 255 && !kf_item_id_valid(static_cast<s32>(item))) || count < -100000 || count > 100000)
        return verdite::json_error("items are 0..79 or a mod's, or 255 for gold; counts -100000..100000");
    give_request = std::array{item, count};
    return verdite::json_ok("give");
}

std::string do_hurt(verdite::AgentArgs args) {
    if (args.size() != 1)
        return verdite::json_error("hurt <hp>");
    if (coop_player_down())
        return verdite::json_error("the player is down");
    char *end = nullptr;
    const long hp = std::strtol(args[0].c_str(), &end, 10);
    if (*end || hp < 1 || hp > KF_PLAYER_VITAL_MAX)
        return verdite::json_error("hurt <hp 1..9999>");
    hurt_request = static_cast<int>(hp);
    return verdite::json_ok("hurt");
}

std::string do_coop(verdite::AgentArgs) {
    return verdite::json_ok("coop", "," + ext::coop_json());
}

// Launches a Fire Ball at the next update as the magic button does, without its cost.
std::string do_cast(verdite::AgentArgs args) {
    if (!args.empty())
        return verdite::json_error("cast");
    if (coop_player_down())
        return verdite::json_error("the player is down");
    cast_request = true;
    return verdite::json_ok("cast");
}

// The effects in flight on this floor: this machine's (with what launched them), those it draws
// from the floor's owner, and the other players' spells it draws.
std::string do_effects(verdite::AgentArgs) {
    std::string effects;
    auto add = [&](std::size_t slot, const KfEffectRecord &effect, const char *from) {
        char text[96];
        std::snprintf(text, sizeof text, "%s{\"slot\":%zu,\"kind\":%u,\"from\":\"%s\"}", effects.empty() ? "" : ",",
            slot, kf_enum_encode<u8>(effect.kind), from);
        effects += text;
    };
    for (std::size_t i = 0; i < KF_EFFECT_CAPACITY; ++i) {
        if (effect_state.records[i].type != KF_EFFECT_SLOT_FREE)
            add(i, effect_state.records[i], effect_sources[i] == KfEffectSource::Actor ? "actor"
                    : effect_sources[i] == KfEffectSource::Player                    ? "player"
                                                                                     : "other");
        if (effect_shown_records[i].type != KF_EFFECT_SLOT_FREE)
            add(i, effect_shown_records[i], "owner");
        for (const auto &shown : effect_player_shown_records)
            if (shown[i].type != KF_EFFECT_SLOT_FREE)
                add(i, shown[i], "another player");
    }
    return verdite::json_ok("effects", ",\"effects\":[" + effects + "]");
}

// The awake actors on this floor: slot, definition, action, position and the player each goes
// after (0 this one, else the online id).
std::string do_actors(verdite::AgentArgs) {
    std::string actors;
    for (std::size_t i = 0; i < actor_state.actors.size(); ++i) {
        const auto &actor = actor_state.actors[i];
        if (actor.slot_state == KF_ACTOR_SLOT_FREE || actor.lifecycle != KF_ACTOR_LIFECYCLE_ACTIVE)
            continue;
        char text[160];
        std::snprintf(text, sizeof text, "%s{\"slot\":%zu,\"definition\":%u,\"action\":%d,\"hp\":%d,\"pos\":[%d,%d,%d],\"target\":%u}",
            actors.empty() ? "" : ",", i, actor.definition_id, static_cast<int>(actor.action), static_cast<int>(actor.health),
            actor.position.vx, actor.position.vy, actor.position.vz, actor_target_players[i]);
        actors += text;
    }
    return verdite::json_ok("actors", ",\"actors\":[" + actors + "]");
}

// The other players in the room on this floor, as drawn: online id, position in cells, feet
// height, attack phase and stride.
std::string do_players(verdite::AgentArgs) {
    std::string players;
    for (const auto &player : online_players_on_floor(kf_enum_encode<s32>(player_state.progress_state.current_floor))) {
        char text[192];
        std::snprintf(text, sizeof text, "%s{\"id\":%u,\"pos\":[%.3f,%.3f],\"footHeight\":%d,\"attackPhase\":%d,\"stride\":%.2f}",
            players.empty() ? "" : ",", player.id, static_cast<double>(player.x), static_cast<double>(player.z),
            player.foot_height, static_cast<int>(player.attack_phase), static_cast<double>(player.stride));
        players += text;
    }
    return verdite::json_ok("players", ",\"players\":[" + players + "]");
}

// The menu frame on screen: each row's widget (0 window, 1 list, 2 choice), box, and
// whether it is the cursor's and of the widget taking input.
std::string do_menu(verdite::AgentArgs) {
    std::string rows;
    for (const auto &row : host_menu_pointer().shown_rows()) {
        char text[160];
        std::snprintf(text, sizeof text, "%s{\"widget\":%u,\"row\":%d,\"box\":[%g,%g,%g,%g],\"cursor\":%s,\"input\":%s}",
            rows.empty() ? "" : ",", row.widget, row.row, row.box.x, row.box.y, row.box.width, row.box.height,
            row.cursor ? "true" : "false", row.input ? "true" : "false");
        rows += text;
    }
    return verdite::json_ok("menu", ",\"rows\":[" + rows + "]");
}

// The title menu as English text: whether it is up, the page's heading, code and note, and its rows.
// "title type <text>" types on the join or name page, as the keyboard does.
std::string do_title(verdite::AgentArgs args) {
    if (args.size() == 2 && args[0] == "type") {
        for (const char key : args[1])
            if (!title_type(key))
                return verdite::json_error("the join or name page is not up, or it does not take that key");
    } else if (!args.empty()) {
        return verdite::json_error("title [type <text>]");
    }
    const TitleText text = title_text(Language::English);
    std::string lines;
    for (const std::string &line : text.lines)
        lines += (lines.empty() ? "" : ",") + verdite::json_quote(line);
    std::string rows;
    for (const TitleRowText &row : text.rows)
        rows += (rows.empty() ? "" : ",") + std::string("{\"text\":") + verdite::json_quote(row.text) +
                ",\"value\":" + verdite::json_quote(row.value) + ",\"enabled\":" + (row.enabled ? "true" : "false") +
                ",\"selected\":" + (row.selected ? "true" : "false") + "}";
    return verdite::json_ok("title", std::string(",\"open\":") + (title_is_open() ? "true" : "false") +
        ",\"heading\":" + verdite::json_quote(text.heading) + ",\"code\":" + verdite::json_quote(text.code) +
        ",\"lines\":[" + lines + "],\"note\":" + verdite::json_quote(text.note) + ",\"rows\":[" + rows + "]");
}

// A position in the original 320 x 240 pixels, from "<x> <y>".
std::optional<std::array<float, 2>> menu_position(verdite::AgentArgs args) {
    if (args.size() != 2)
        return std::nullopt;
    char *end_x = nullptr, *end_y = nullptr;
    const float x = std::strtof(args[0].c_str(), &end_x);
    const float y = std::strtof(args[1].c_str(), &end_y);
    if (*end_x || *end_y)
        return std::nullopt;
    return std::array{x, y};
}

std::string do_point(verdite::AgentArgs args) {
    if (host_input_context() != InputContext::Menu)
        return verdite::json_error("no menu is up");
    const auto position = menu_position(args);
    if (!position)
        return verdite::json_error("point <x> <y>");
    host_menu_pointer().moved((*position)[0], (*position)[1]);
    return verdite::json_ok("point");
}

std::string do_click(verdite::AgentArgs args) {
    if (host_input_context() != InputContext::Menu)
        return verdite::json_error("no menu is up");
    if (args.size() == 1 && args[0] == "right") {
        host_menu_pointer().back();
        verdite::crash_event("agent: click right");
        return verdite::json_ok("click");
    }
    const auto position = menu_position(args);
    if (!position)
        return verdite::json_error("click <x> <y> | click right");
    host_menu_pointer().pressed((*position)[0], (*position)[1]);
    host_menu_pointer().released((*position)[0], (*position)[1]);
    verdite::crash_event("agent: click " + args[0] + " " + args[1]);
    return verdite::json_ok("click");
}

std::string do_options(verdite::AgentArgs) { return verdite::json_ok("options", ",\"options\":" + ext::options_json()); }

std::string do_setup(verdite::AgentArgs args) {
    std::string line;
    for (const std::string &arg : args)
        line += (line.empty() ? "" : " ") + arg;
    if (const std::string why = ext::setup_command(line); !why.empty())
        return verdite::json_error(why);
    return verdite::json_ok("setup", ",\"setup\":" + ext::setup_json());
}

std::string do_option(verdite::AgentArgs args) {
    if (args.empty() || args.size() > 2)
        return verdite::json_error("option <id> [value]");
    const std::string why = ext::option_change(args[0], args.size() == 2 ? args[1] : std::string());
    return why.empty() ? verdite::json_ok("option") : verdite::json_error(args[0] + ": " + why);
}

std::string do_quit(verdite::AgentArgs) {
    quit_requested = true;
    return verdite::json_ok("quit");
}
}

void agent_start() {
    verdite::AgentConfig config;
    config.tag = "KF1";
    config.port = verdite::agent_port_from(std::getenv("KF_SHELL"), default_port);
    config.beacon = verdite::agent_switch_from(std::getenv("KF_AGENT"));
    config.snapshot = snapshot;
    std::string buttons;
    for (const auto &entry : button_names)
        buttons += (buttons.empty() ? "" : " ") + std::string(entry.name);
    verdite::agent_start(config, {
        {"state", "state - the player and phase as JSON", do_state},
        {"press", "press <button>[+<button>...] [holdMs=150] - hold game buttons for game-clock ms, replacing the last press: " + buttons, do_press},
        {"release", "release - let go of the pressed buttons", do_release},
        {"move", "move <cell_x> <cell_z> - put the player on a cell of this floor at the next update", do_move},
        {"actors", "actors - the awake actors on this floor and the player each goes after", do_actors},
        {"effects", "effects - the effects in flight on this floor: this machine's (from an actor, this player or other), the owner's it draws and the other players' spells", do_effects},
        {"cast", "cast - launch a Fire Ball at the next update as the magic button does, without its cost", do_cast},
        {"players", "players - the other players in the room on this floor, as drawn", do_players},
        {"warp", "warp <floor> - change floor as a warp does, onto the first open cell, at the next update", do_warp},
        {"object", "object <slot> [take] - an object slot's id on this floor (255 none); take empties it as a pickup does", do_object},
        {"hit", "hit <slot> [aim] - strike an actor at the next update as the sword does at full charge, or aim a spell at it", do_hit},
        {"give", "give <item> <count> - change the pack (item 255: the purse) at the next update, as a pickup or use would", do_give},
        {"hurt", "hurt <hp> - lose hp at the next update as poison takes it, dying at 0", do_hurt},
        {"coop", "coop - the coop session: role, whether its world is in play, the pack, purse and what each player wears", do_coop},
        {"menu", "menu - the rows of the menu on screen, in game pixels", do_menu},
        {"title", "title [type <digits>] - the title menu's page as English text; type a room code on its join page", do_title},
        {"point", "point <x> <y> - move the mouse over a menu (game pixels)", do_point},
        {"click", "click <x> <y> | click right - click a menu with the mouse", do_click},
        {"options", "options - the settings this platform shows, with their values, as JSON", do_options},
        {"option", "option <id> [value] - change a setting (true/false, a number, a choice's value), or press an action", do_option},
        {"setup", "setup [start | preset <id> | choose <group> <n|keep> | tier <id> | option <id> <value> | next | back | finish | skip] - drive the first-run setup; its view as JSON", do_setup},
        {"quit", "quit - exit the game", do_quit},
    });
}

void agent_between_updates() {
    if (give_request) {
        const auto [item, count] = *give_request;
        give_request.reset();
        if (item == 255) {
            player_state.gold = static_cast<u32>(std::clamp<long long>(static_cast<long long>(player_state.gold) + count, 0, UINT32_MAX));
        } else {
            u8 &stock = item_stock[kf_enum_encode<u8>(KF_ITEM_STOCK_PLAYER)][item];
            stock = static_cast<u8>(std::clamp<long>(stock + count, 0, KF_ITEM_STACK_CAPACITY));
        }
    }
    if (hit_request) {
        actor_apply_damage(static_cast<u16>(*hit_request), player_state.physical_power, player_state.cutting_attack,
            player_state.striking_attack, player_state.piercing_attack, player_state.holy_attack,
            player_state.fire_attack, KF_ACTOR_DAMAGE_SCALE_ONE, KF_ACTOR_DAMAGE_CREDIT_PLAYER);
        hit_request.reset();
    }
    if (cast_request) {
        cast_request = false;
        const KfEffectKind selected = player_state.selected_magic_id;
        player_state.selected_magic_id = KfEffectKind::KF_MAGIC_FIRE_BALL;
        effect_spawn_source = KfEffectSource::Player;
        magic_cast();
        effect_spawn_source = KfEffectSource::Other;
        player_state.selected_magic_id = selected;
    }
    if (hurt_request) {
        if (!coop_player_down() && player_state.update_state != KF_PLAYER_UPDATE_DYING)
            player_adjust_hp(-*hurt_request);
        hurt_request.reset();
    }
    if (take_request) {
        map_object_state.objects[*take_request].object_id = KF_OBJECT_NONE;
        take_request.reset();
    }
    if (warp_request) {
        const auto floor = kf_enum_decode<KfFloorId>(static_cast<u8>(*warp_request));
        warp_request.reset();
        player_warp_change_floor(floor, floor == KF_FLOOR_5 ? KF_FLOOR5_ENTRY_VARIANT : KF_MAP_VARIANT_DEFAULT);
        if (const auto cell = first_open_cell())
            player_move_to_cell((*cell)[0], (*cell)[1]);
    }
    if (!move_request)
        return;
    const auto [x, z] = *move_request;
    move_request.reset();
    player_move_to_cell(x, z);
}

void agent_poll() {
    verdite::agent_poll();
    if (quit_requested) {
        // Let the client thread send the reply first.
        SDL_Delay(100);
        host_shutdown();
        std::exit(0);
    }
}

u32 agent_buttons() {
    return press.buttons && host_clock_ns() < press.until_ns ? press.buttons : 0;
}
}
