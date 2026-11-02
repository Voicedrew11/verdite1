#ifndef KF_EXT_CALL_CHECKS_H
#define KF_EXT_CALL_CHECKS_H
// The run-time checks port/ext/call_layer.toml names (verdite::RawArgumentCheck): what a function's
// argument must be now, which a range cannot say. A new check is a new function here.
#include <cstdint>
#include <span>
#include <string_view>

#include "verdite/raw.h"

struct KfActor;
struct KfEffectRecord;
struct KfMagicRecord;

// Around each game function the call layer runs but those listed as running frames
// (RawCallLayer::call_began and call_ended).
void kf_call_began();
void kf_call_ended();
// Such a call is running. A frame must not be presented then: it would run mods' callbacks inside
// the call, and tools/call_audit.py is meant to have held back or listed what reaches one.
bool kf_call_running();

// A slot of the actor pool that holds an actor.
std::string_view kf_call_actor_in_use(std::int64_t slot);
// A slot of the map object pool that holds an object: a free one's id is no definition's index.
std::string_view kf_call_map_object_in_use(std::int64_t slot);
// A slot of the effect pool that holds an effect.
std::string_view kf_call_effect_in_use(std::int64_t slot);
// An effect code whose kind has an action profile.
std::string_view kf_call_actor_profile_exists(std::int64_t code);
// A weapon, or KF_OBJECT_NONE: a weapon's id names its file.
std::string_view kf_call_weapon_or_none(std::int64_t item);

// (floor, variant): only floor 5 has variants, and it has no default one, so any other pair names
// files the disc does not have.
std::string_view kf_call_floor_and_variant_exist(std::span<const verdite::RawValue> arguments);
// (variant, cell_x, cell_z): the same for the floor the player is on.
std::string_view kf_call_variant_exists_here(std::span<const verdite::RawValue> arguments);

// The game's current actor, or current effect, around one call: what a function that takes no actor
// acts on. What was current before comes back with it.
class KfBoundActor {
public:
    explicit KfBoundActor(KfActor *actor);
    ~KfBoundActor();
    KfBoundActor(const KfBoundActor &) = delete;
    KfBoundActor &operator=(const KfBoundActor &) = delete;

private:
    KfActor *previous_;
};

class KfBoundEffect {
public:
    explicit KfBoundEffect(KfEffectRecord *effect);
    ~KfBoundEffect();
    KfBoundEffect(const KfBoundEffect &) = delete;
    KfBoundEffect &operator=(const KfBoundEffect &) = delete;

private:
    KfEffectRecord *previous_;
    KfMagicRecord *previous_magic_;
};

#endif // KF_EXT_CALL_CHECKS_H
