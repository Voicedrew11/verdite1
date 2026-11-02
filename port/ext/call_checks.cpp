#include "call_checks.h"

#include "kf/lib/types.h"

#include "kf/game/actor.h"
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
