#include "call_checks.h"

#include "kf/lib/types.h"

#include "coop_party.h"
#include "kf/game/actor.h"
#include "kf/game/effect.h"
#include "kf/game/player.h"
#include "kf/lib/floor.h"
#include "kf/lib/map.h"

#include <cstddef>

namespace {
int calls_running = 0;
} // namespace

void kf_call_began() {
    ++calls_running;
}
void kf_call_ended() {
    --calls_running;
}
bool kf_call_running() {
    return calls_running > 0;
}

std::string_view kf_call_actor_in_use(std::int64_t slot) {
    return actor_state.actors[static_cast<std::size_t>(slot)].slot_state != KF_ACTOR_SLOT_FREE
               ? std::string_view{}
               : "that actor's slot is free";
}

std::string_view kf_call_map_object_in_use(std::int64_t slot) {
    return map_object_state.objects[static_cast<std::size_t>(slot)].object_id != KF_OBJECT_NONE
               ? std::string_view{}
               : "that map object's slot is free";
}

std::string_view kf_call_effect_in_use(std::int64_t slot) {
    return effect_state.records[static_cast<std::size_t>(slot)].type != KF_EFFECT_SLOT_FREE
               ? std::string_view{}
               : "that effect's slot is free";
}

std::string_view kf_call_actor_profile_exists(std::int64_t code) {
    return (code & kf_enum_encode<std::int64_t>(KF_ACTOR_EFFECT_KIND_MASK)) < KF_ACTOR_ACTION_PROFILE_COUNT
               ? std::string_view{}
               : "that effect code's kind has no action profile";
}

std::string_view kf_call_weapon_or_none(std::int64_t item) {
    return kf::ext::coop_item_fits(0, static_cast<std::uint8_t>(item)) ? std::string_view{}
                                                              : "that item is not a weapon";
}

namespace {
constexpr std::string_view no_such_variant = "that floor has no such variant on the disc";

bool variant_exists(std::int64_t floor, std::int64_t variant) {
    if (floor == kf_enum_encode<std::int64_t>(KF_FLOOR_5))
        return variant >= kf_enum_encode<std::int64_t>(KF_FLOOR5_ENTRY_VARIANT) &&
               variant <= kf_enum_encode<std::int64_t>(KF_FLOOR5_ALTERNATE_MUSIC_VARIANT);
    return variant == kf_enum_encode<std::int64_t>(KF_MAP_VARIANT_DEFAULT);
}
} // namespace

std::string_view kf_call_floor_and_variant_exist(std::span<const verdite::RawValue> arguments) {
    const auto floor = std::get<std::int64_t>(arguments[0]);
    if (floor < kf_enum_encode<std::int64_t>(KF_FLOOR_1) || floor > kf_enum_encode<std::int64_t>(KF_FLOOR_5))
        return "that is not a floor";
    return variant_exists(floor, std::get<std::int64_t>(arguments[1])) ? std::string_view{} : no_such_variant;
}

std::string_view kf_call_variant_exists_here(std::span<const verdite::RawValue> arguments) {
    // Any floor but 5 has no variant at all to load, even its default.
    const auto floor = kf_enum_encode<std::int64_t>(player_state.progress_state.current_floor);
    if (floor != kf_enum_encode<std::int64_t>(KF_FLOOR_5))
        return "only floor 5 has variants to warp between";
    return variant_exists(floor, std::get<std::int64_t>(arguments[0])) ? std::string_view{} : no_such_variant;
}

KfBoundActor::KfBoundActor(KfActor *actor) : previous_(actor_state.current) {
    actor_bind_current(actor);
}
KfBoundActor::~KfBoundActor() {
    actor_bind_current(previous_);
}

KfBoundEffect::KfBoundEffect(KfEffectRecord *effect)
    : previous_(effect_state.current_record), previous_magic_(effect_state.current_magic) {
    effect_pool_set_current(effect);
}
KfBoundEffect::~KfBoundEffect() {
    effect_state.current_record = previous_;
    effect_state.current_magic = previous_magic_;
}
