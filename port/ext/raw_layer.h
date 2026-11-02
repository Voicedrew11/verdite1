#ifndef KF_EXT_RAW_LAYER_H
#define KF_EXT_RAW_LAYER_H
// The raw layer of the modding API (docs/RAW_LAYER_PLAN.md): every named global and struct field of
// the decomp, read and written by path. tools/raw_layer.py writes raw_layer.cpp from the decomp's
// headers and port/ext/raw_layer.toml; nothing here is written by hand except this interface.
#include "verdite/raw.h"
#include "verdite/raw_call.h"

#include <string_view>

// The definitions file's text for the layer: what the layer is and what it does not promise.
inline constexpr std::string_view kf_raw_doc =
    R"(The raw layer: every named global and struct field of the decomp, read and written by path, checked against its type, range and bounds. The names are the decomp's and follow its renames: nothing here is stable. Raw writes and kf.rules can touch the same tables (monster definitions, level growth, prices, equipment records); kf.rules lays its values again at the next map load, over a raw write. A write the game later overwrites, or one that leaves other state out of step, is the mod's to manage.)";

// The layer at kf.raw, built from the generated tables in raw_layer.cpp.
const verdite::RawLayer &kf_raw_layer();

inline constexpr std::string_view kf_call_doc =
    R"(The call layer: the game's functions, called by name, every argument checked first. The names are the decomp's and follow its renames: nothing here is stable. A call that changes state may run only directly in on_between_updates. A call that leaves the game's other state out of step is the mod's to manage.)";

// The layer at kf.call (docs/CALL_LAYER_PLAN.md), from the same generated source.
const verdite::RawCallLayer &kf_call_layer();

#endif // KF_EXT_RAW_LAYER_H
