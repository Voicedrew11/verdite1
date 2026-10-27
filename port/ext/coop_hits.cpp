#include "coop_hits.h"

#include "coop_floor_play.h"
#include "internal.hpp"

#include <kf/game/actor.h>
#include <kf/game/effect.h>
#include <kf/game/player.h>
#include <kf/platform/coop.h>

#include <cstdio>
#include <utility>
#include <variant>
#include <vector>

namespace kf::ext {
namespace {

struct Hits {
    std::vector<std::pair<CoopPlayerId, CoopMessage>> inbox;
    int swings_felt = 0;
    int hits_felt = 0;
};
Hits hits;

bool on_floor(int floor) {
    return floor == coop_floors_floor() && floor == kf_enum_encode<int>(player_state.progress_state.current_floor);
}

bool runs_floor(int floor) {
    const CoopPlayerId self = coop_floors_self();
    return self != 0 && on_floor(floor) && coop_floors_runner() == self;
}

// The awake actor in a slot, if it still holds the definition the other machine saw there.
KfActor *actor_seen(std::uint8_t slot, std::uint8_t definition) {
    KfActor &actor = actor_state.actors[slot];
    if (actor.lifecycle != KF_ACTOR_LIFECYCLE_ACTIVE || actor.definition_id != definition)
        return nullptr;
    return &actor;
}

void tell_everyone(const CoopMessage &message) {
    if (const auto bytes = coop_message_encode(message))
        online_broadcast_control(*bytes);
}

// On the owner: applied as the game applies its own player's hit, without training this player,
// and a kill by the hitter's own hit pays out as one of this player's would.
void apply(const CoopActorHit &hit) {
    KfActor *actor = runs_floor(hit.floor) ? actor_seen(hit.slot, hit.definition) : nullptr;
    if (!actor)
        return;
    const u16 health = actor->health;
    actor_apply_damage(hit.slot, hit.base_power, hit.damage[0], hit.damage[1], hit.damage[2], hit.damage[3],
        hit.damage[4], hit.scale, KF_EFFECT_TYPE_NONE);
    if (!hit.credited || health == 0 || actor->health != 0)
        return;
    const u16 experience = actor_state.definitions.entries[hit.definition].experience_reward;
    player_add_experience(static_cast<s16>(experience));
    coop_actor_killed(experience);
}

void apply(const CoopActorAim &aim) {
    if (KfActor *actor = runs_floor(aim.floor) ? actor_seen(aim.slot, aim.definition) : nullptr)
        actor_state.player_target = actor;
}

// On the victim: the owner's swing tested again against this player where it stands here and the
// actor as it is drawn here, so no one feels a swing they dodged on their own screen.
void apply(const CoopSwing &swing) {
    const KfActor *actor = on_floor(swing.floor) ? actor_seen(swing.slot, swing.definition) : nullptr;
    if (!actor)
        return;
    const KfActorDefinition &definition = actor_state.definitions.entries[swing.definition];
    const VECTOR target = std::exchange(actor_state.player_position, player_state.camera_position);
    const bool reaches = actor_swing_reaches_player(actor, &definition, swing.minimum_distance,
        swing.maximum_distance, swing.angle_offset, swing.angle_tolerance);
    actor_state.player_position = target;
    if (!reaches)
        return;
    ++hits.swings_felt;
    player_apply_damage(definition.attack_components[KF_COMBAT_COMPONENT_CUTTING],
        definition.attack_components[KF_COMBAT_COMPONENT_STRIKING],
        definition.attack_components[KF_COMBAT_COMPONENT_PIERCING], kf_enum_decode<KfPlayerStatusFlags>(swing.status),
        0, 0, KF_FIXED12_ONE, KF_PLAYER_DAMAGE_MULTIPLIER_ONE);
}

void apply(const CoopPlayerHit &hit) {
    if (!on_floor(hit.floor))
        return;
    ++hits.hits_felt;
    player_apply_damage(hit.cutting, hit.striking, hit.piercing, kf_enum_decode<KfPlayerStatusFlags>(hit.status),
            hit.magic, hit.fire, hit.scale_q12, hit.multiplier_tenths);
}

void apply(const CoopKill &kill) {
    if (on_floor(kill.floor))
        player_add_experience(static_cast<s16>(kill.experience));
}

// On a floor another machine runs, this player's aim (a spell's target) goes to it; here no actor
// runs to take it.
void send_aim() {
    KfActor *target = std::exchange(actor_state.player_target, nullptr);
    const CoopPlayerId runner = coop_floors_runner();
    if (!target || runner == 0 || runner == coop_floors_self())
        return;
    const auto slot = static_cast<std::uint8_t>(target - actor_state.actors.data());
    coop_floors_send(runner, CoopActorAim{coop_floors_floor(), slot, target->definition_id});
}

} // namespace

bool coop_hits_message(const CoopMessage &message) {
    return std::holds_alternative<CoopActorHit>(message) || std::holds_alternative<CoopActorAim>(message)
        || std::holds_alternative<CoopKill>(message) || std::holds_alternative<CoopSwing>(message)
        || std::holds_alternative<CoopPlayerHit>(message);
}

void coop_hits_receive(CoopPlayerId from, const CoopMessage &message) {
    if (from != 0 && coop_floors_self() != 0)
        hits.inbox.emplace_back(from, message);
}

void coop_hits_world_update() {
    if (coop_floors_self() == 0)
        return;
    const auto inbox = std::exchange(hits.inbox, {});
    for (const auto &[from, message] : inbox) {
        if (const auto *hit = std::get_if<CoopActorHit>(&message))
            apply(*hit);
        else if (const auto *aim = std::get_if<CoopActorAim>(&message))
            apply(*aim);
        else if (const auto *kill = std::get_if<CoopKill>(&message))
            apply(*kill);
        else if (const auto *swing = std::get_if<CoopSwing>(&message))
            apply(*swing);
        else if (const auto *player_hit = std::get_if<CoopPlayerHit>(&message))
            apply(*player_hit);
    }
    if (coop_floors_runner() != coop_floors_self())
        send_aim();
}

void coop_hits_stop() {
    hits = Hits{};
}

std::string coop_hits_json() {
    return "\"swingsFelt\":" + std::to_string(hits.swings_felt) + ",\"hitsFelt\":" + std::to_string(hits.hits_felt);
}

}

namespace kf {

void coop_hit_actor_elsewhere(const CoopActorHitArgs &hit) {
    using namespace ext;
    const CoopPlayerId runner = coop_floors_runner();
    if (runner == 0 || runner == coop_floors_self() || hit.actor >= coop_actor_slots)
        return;
    coop_floors_send(runner, CoopActorHit{coop_floors_floor(), static_cast<std::uint8_t>(hit.actor), hit.definition,
        hit.base_power, hit.damage, hit.scale, hit.credited});
}

void coop_swing_at_other(u32 player, const CoopSwingArgs &swing) {
    using namespace ext;
    const CoopPlayerId victim = coop_floors_player_online(player);
    if (victim == 0 || swing.actor >= coop_actor_slots)
        return;
    coop_floors_send(victim, CoopSwing{coop_floors_floor(), static_cast<std::uint8_t>(swing.actor), swing.definition,
        swing.minimum_distance, swing.maximum_distance, swing.angle_offset, swing.angle_tolerance, swing.status});
}

void coop_hit_other_player(u32 player, const CoopPlayerHitArgs &hit) {
    using namespace ext;
    const CoopPlayerId victim = coop_floors_player_online(player);
    if (victim == 0)
        return;
    coop_floors_send(victim, CoopPlayerHit{coop_floors_floor(), hit.cutting, hit.striking, hit.piercing, hit.status,
        hit.magic, hit.fire, hit.scale_q12, hit.multiplier_tenths});
}

void coop_actor_killed(u16 experience) {
    using namespace ext;
    if (coop_floors_self() != 0)
        tell_everyone(CoopKill{coop_floors_floor(), experience});
}

}
