#include "coop_death_play.h"

#include "coop_floor_play.h"
#include "internal.hpp"

#include <kf/game/actor.h>
#include <kf/game/collision.h>
#include <kf/game/game.h>
#include <kf/game/player.h>
#include <kf/game/graphics.h>
#include <kf/lib/floor.h>
#include <kf/lib/map.h>
#include <kf/lib/map_data.h>
#include <kf/platform/coop.h>
#include <kf/platform/input.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <numbers>
#include <span>
#include <vector>

namespace kf::ext {
namespace {

constexpr std::uint32_t calm_ms = 4000;      // without a hit taken or dealt, before a down player comes back beside them
constexpr std::uint32_t down_least_ms = 3000; // down at least this long, so whose eyes is said before coming back
constexpr s32 step_most = 699;               // PLAYER_MAX_STEP_RISE: a cell to come back on is no further up or down
constexpr float cell_units = KF_MAP_TILE_SIZE;

struct Death {
    bool on = false;
    CoopPlayerId self = 0;
    bool down = false;
    std::uint32_t down_updates = 0;
    CoopPlayerId eyes = 0;
    CoopFight fight{0};
};
Death death;

std::uint32_t updates_in(std::uint32_t ms) {
    const auto update_ms = static_cast<std::uint32_t>(world_update_vblanks) * 1000 / 60;
    return (ms + update_ms - 1) / std::max<std::uint32_t>(update_ms, 1);
}

int loaded_floor() {
    return kf_enum_encode<int>(player_state.progress_state.current_floor);
}

s32 cell_centre(int cell) {
    return cell * KF_MAP_TILE_SIZE + KF_MAP_TILE_CENTER;
}

// The other players in the room as their poses have them, in the roster's order, with each pose.
struct Seen {
    std::vector<CoopPartyMember> room;
    std::map<CoopPlayerId, OnlinePose> poses;
};

Seen others() {
    const auto poses = online_room_poses();
    Seen seen;
    for (const CoopRoster::Player &player : coop_floors_room()) {
        if (player.id == death.self)
            continue;
        CoopPartyMember member{player.id};
        if (const auto pose = poses.find(player.peer); pose != poses.end()) {
            member = {player.id, pose->second.floor, pose->second.x, pose->second.z, pose->second.down,
                pose->second.fighting};
            seen.poses[player.id] = pose->second;
        }
        seen.room.push_back(member);
    }
    return seen;
}

s16 angle_of(float radians) {
    constexpr float turn = 2.0f * std::numbers::pi_v<float>;
    const auto angle = static_cast<s32>(std::lround(radians / turn * (KF_ANGLE_WRAP_MASK + 1)));
    return static_cast<s16>(angle & KF_ANGLE_WRAP_MASK);
}

// The view at another player's eyes; this player stands nowhere, so the grid does not count it.
void look_from(const OnlinePose &pose) {
    player_state.camera_position = {static_cast<s32>(pose.x * cell_units), pose.foot_height - KF_PLAYER_CAMERA_HEIGHT,
        static_cast<s32>(pose.z * cell_units)};
    player_state.camera_rotation = {pose.pitch, angle_of(pose.yaw), 0};
    player_state.foot_height = pose.foot_height;
    player_state.motion_state.map_cell.x = static_cast<u16>(player_state.camera_position.vx / KF_MAP_TILE_SIZE);
    player_state.motion_state.map_cell.z = static_cast<u16>(player_state.camera_position.vz / KF_MAP_TILE_SIZE);
}

// To the floor the eyes are on, as a warp unloads and loads one, so this machine counts as on it.
void follow_to_floor(const OnlinePose &pose) {
    map_unload_floor();
    const auto floor = kf_enum_decode<KfFloorId>(static_cast<u8>(pose.floor));
    player_state.progress_state.current_floor = floor;
    player_state.map_variant = floor == KF_FLOOR_5 ? KF_FLOOR5_ENTRY_VARIANT : KF_MAP_VARIANT_DEFAULT;
    map_load_floor();
    player_state.camera_position.vx = cell_centre(static_cast<int>(pose.x));
    player_state.camera_position.vz = cell_centre(static_cast<int>(pose.z));
    player_sync_position_to_map();
    const auto &cell = player_state.motion_state.map_cell;
    collision_adjust_cell_occupancy(cell.x, cell.z, -1);
    player_state.allow_near_actor_spawn = KF_ACTOR_NEAR_SPAWN_FORBIDDEN;
    std::printf("[coop] down: following onto floor %d\n", pose.floor);
}

// A cell a player could stand on now, about as high as `feet`, that is not a warp or a pit.
bool free_to_stand(CoopCell cell, s32 feet) {
    if (cell.x < 0 || cell.z < 0 || cell.x >= KF_MAP_COLUMNS || cell.z >= KF_MAP_ROWS)
        return false;
    const KfMapAttribute attribute = map_attribute_at_cell(cell.x, cell.z);
    if (attribute == KF_MAP_ATTRIBUTE_WARP || attribute == KF_MAP_ATTRIBUTE_BOTTOMLESS_PIT
        || attribute == KF_MAP_ATTRIBUTE_NONE || std::abs(map_base_floor_height(cell.x, cell.z) - feet) > step_most)
        return false;
    const auto hit = collision_query_world(cell_centre(cell.x), KF_COLLISION_IGNORE_HEIGHT, cell_centre(cell.z),
        KF_COLLISION_PLAYER_RADIUS, KF_COLLISION_PLAYER_HEIGHT, KF_COLLISION_SKIP_PLAYER);
    return hit.kind == KfCollisionKind::None;
}

// Revived beside the eyes as with a Grass Fruit, faded in as the game's revival is.
void come_back_beside(const OnlinePose &pose) {
    const CoopCell at{static_cast<int>(pose.x), static_cast<int>(pose.z)};
    const CoopCell cell = coop_cell_beside(at, [&](CoopCell beside) { return free_to_stand(beside, pose.foot_height); });
    player_state.camera_position.vx = cell_centre(cell.x);
    player_state.camera_position.vz = cell_centre(cell.z);
    player_state.camera_rotation = {0, angle_of(pose.yaw), 0};
    player_state.view_bob_offset = 0;
    player_sync_position_to_map();
    player_state.vitals.current_hp = player_state.vitals.maximum_hp;
    player_state.vitals.current_mp = player_state.vitals.maximum_mp;
    player_state.status_effect_flags = KF_PLAYER_STATUS_NONE;
    player_state.progress_state.highest_floor = std::max(player_state.progress_state.highest_floor,
        player_state.progress_state.current_floor);
    player_state.update_state = KF_PLAYER_UPDATE_RECOVERY_FADE;
    player_state.death_visual_blend = 0;
    game_graphics_runtime.hud_brightness = 0;
    player_state.view_rotation_offset = {};
    player_state.previous_map_cell = player_state.motion_state.map_cell;
    player_state.camera_position.vy = player_state.foot_height - KF_PLAYER_CAMERA_HEIGHT;
    death.down = false;
    death.eyes = 0;
    death.fight = CoopFight(updates_in(calm_ms));
    std::printf("[coop] back beside another player at (%d, %d)\n", cell.x, cell.z);
}

// Whether an enemy goes after this player now: awake and alive, pursuing or attacking, and its
// target this player. The floor's owner knows its targets; a machine drawing the owner's actors
// takes the nearest player up on the floor, as the owner chooses.
bool chasing(KfActorAction action) {
    constexpr std::array chasing_actions = {KF_ACTOR_ACTION_PURSUE, KF_ACTOR_ACTION_MELEE_ATTACK,
        KF_ACTOR_ACTION_JUMP_ATTACK, KF_ACTOR_ACTION_SPECIAL_ATTACK, KF_ACTOR_ACTION_EFFECT0, KF_ACTOR_ACTION_EFFECT1,
        KF_ACTOR_ACTION_EFFECT2, KF_ACTOR_ACTION_MULTI_HIT_ATTACK};
    return std::ranges::find(chasing_actions, action) != chasing_actions.end();
}

bool nearest_is_this_player(const KfActor &actor, std::span<const KfOtherPlayer> others) {
    const auto squared = [&](const VECTOR &at) {
        const std::int64_t dx = actor.position.vx - at.vx, dz = actor.position.vz - at.vz;
        return dx * dx + dz * dz;
    };
    const std::int64_t mine = squared(player_state.camera_position);
    return std::ranges::none_of(others, [&](const KfOtherPlayer &other) { return squared(other.camera_position) < mine; });
}

bool chased() {
    const bool runs_floor = coop_floors_run_here();
    const auto others = player_others_on_floor();
    for (std::size_t i = 0; i < actor_state.actors.size(); ++i) {
        const KfActor &actor = actor_state.actors[i];
        if (actor.slot_state == KF_ACTOR_SLOT_FREE || actor.lifecycle != KF_ACTOR_LIFECYCLE_ACTIVE || actor.health == 0
            || !chasing(actor.action))
            continue;
        if (runs_floor ? actor_target_players[i] == KF_ACTOR_TARGET_THIS_PLAYER : nearest_is_this_player(actor, others))
            return true;
    }
    return false;
}

} // namespace

void coop_death_start(CoopPlayerId self) {
    death = Death{};
    death.on = true;
    death.self = self;
    death.fight = CoopFight(updates_in(calm_ms));
}

void coop_death_stop() {
    death = Death{};
}

void coop_death_went_down() {
    if (!death.on || death.down)
        return;
    death.down = true;
    death.down_updates = 0;
    death.eyes = 0;
    const auto &cell = player_state.motion_state.map_cell;
    collision_adjust_cell_occupancy(cell.x, cell.z, -1);
    player_death_apply_visual_fade(&player_death_saved_color_matrix, 0);
    player_state.update_state = KF_PLAYER_UPDATE_NORMAL;
    player_state.vitals.current_hp = 0;
    player_state.status_effect_flags = KF_PLAYER_STATUS_NONE;
    player_state.weapon_attack_phase = KF_WEAPON_ATTACK_INACTIVE;
    player_state.view_rotation_offset = {};
    player_state.view_bob_offset = 0;
    std::printf("[coop] down\n");
}

void coop_death_down_update(bool next) {
    if (!death.down)
        return;
    ++death.down_updates;
    const Seen seen = others();
    const float x = static_cast<float>(player_state.camera_position.vx) / cell_units;
    const float z = static_cast<float>(player_state.camera_position.vz) / cell_units;
    death.eyes = next ? coop_eyes_next(seen.room, death.eyes) : coop_eyes_choose(seen.room, death.eyes, loaded_floor(), x, z);
    const auto pose = seen.poses.find(death.eyes);
    if (pose == seen.poses.end())
        return;   // no one up: the host sends the party back to its last save
    if (pose->second.floor != loaded_floor()) {
        follow_to_floor(pose->second);
        return;
    }
    look_from(pose->second);
    // Back after the host's wait even beside a fight: an enemy can chase forever.
    const bool calm = !pose->second.fighting || death.down_updates >= updates_in(static_cast<std::uint32_t>(coop_revive_wait_s()) * 1000);
    if (calm && death.down_updates >= updates_in(down_least_ms))
        come_back_beside(pose->second);
}

bool coop_death_down() {
    return death.down;
}

void coop_death_fought() {
    death.fight.hit();
}

void coop_death_world_update() {
    if (death.on && !death.down)
        death.fight.update(chased());
}

bool coop_death_fighting() {
    return death.on && !death.down && death.fight.fighting();
}

std::uint64_t coop_death_eyes_peer() {
    if (!death.down || death.eyes == 0)
        return 0;
    for (const CoopRoster::Player &player : coop_floors_room())
        if (player.id == death.eyes)
            return player.peer;
    return 0;
}

std::string coop_death_eyes_name() {
    return death.down && death.eyes != 0 ? coop_player_name_of(death.eyes) : std::string();
}

bool coop_death_party_down(std::span<const CoopPlayerId> not_back) {
    if (!death.on)
        return false;
    std::vector<CoopPartyMember> room = others().room;
    for (CoopPartyMember &member : room)
        if (std::ranges::find(not_back, member.id) != not_back.end())
            member.down = false;
    return coop_party_down(death.down, room);
}

std::string coop_death_json() {
    return std::string("\"down\":") + (death.down ? "true" : "false") + ",\"eyes\":\"" + coop_death_eyes_name() +
           "\",\"fighting\":" + (coop_death_fighting() ? "true" : "false");
}

}
