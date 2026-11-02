#ifndef KF_EXT_CALL_CHECKS_H
#define KF_EXT_CALL_CHECKS_H
// The run-time checks port/ext/call_layer.toml names (verdite::RawArgumentCheck): what a function's
// argument must be now, which a range cannot say. A new check is a new function here.
#include <cstdint>
#include <string_view>

// Around each game function the call layer runs (RawCallLayer::call_began and call_ended).
void kf_call_began();
void kf_call_ended();
// A mod's call into the game is running. A frame must not be presented then: it would run mods'
// callbacks inside the call, so tools/call_audit.py holds back what reaches one.
bool kf_call_running();

// A slot of the actor pool that holds an actor.
std::string_view kf_call_actor_in_use(std::int64_t slot);
// A slot of the map object pool that holds an object: a free one's id is no definition's index.
std::string_view kf_call_map_object_in_use(std::int64_t slot);

#endif // KF_EXT_CALL_CHECKS_H
