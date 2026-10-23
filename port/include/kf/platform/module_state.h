#ifndef KF_PLATFORM_MODULE_STATE_H
#define KF_PLATFORM_MODULE_STATE_H

#include <type_traits>

namespace kf {
// Owners call this before their first game/opening entry, then on each re-entry.
// Keep one source of truth for authored initializers, including native pointers.
// This is an in-process object copy, not a save format or a PS1 memory image.
template <auto &Object>
void restore_initial_value()
{
    using ObjectType = std::remove_reference_t<decltype(Object)>;
    static_assert(!std::is_const_v<ObjectType> && !std::is_volatile_v<ObjectType>);
    static const auto initial = Object;
    Object = initial;
}
}

#endif // KF_PLATFORM_MODULE_STATE_H
