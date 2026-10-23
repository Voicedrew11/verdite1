#ifndef KF_PLATFORM_PRELUDE_H
#define KF_PLATFORM_PRELUDE_H

// Common system and portable interfaces.
#include <kf/audio/sound.h>
#include <kf/lib/codec.h>
#include <kf/lib/fixed_math.h>
#include <kf/lib/geometry_types.h>
#include <kf/lib/random.h>
#include <kf/lib/types.h>
#include <kf/platform/automap.h>
#include <kf/platform/files.h>
#include <kf/platform/host.h>
#include <kf/platform/input.h>
#include <kf/platform/language_runtime.h>
#include <kf/platform/module_state.h>
#include <kf/platform/saves.h>
#include <kf/renderer/lighting.h>
#include <kf/renderer/projection.h>
#include <kf/renderer/renderer.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

// Shared game-library types and operations have one identity across phases.
#include <kf/lib/audio.h>
#include <kf/lib/camera_path.h>
#include <kf/lib/debug.h>
#include <kf/lib/graphics.h>
#include <kf/lib/item_types.h>
#include <kf/lib/map_types.h>
#include <kf/lib/math.h>
#include <kf/lib/memory.h>
#include <kf/lib/render_face.h>
#include <kf/lib/render_types.h>
#include <kf/lib/resource_file.h>
#include <kf/lib/resources.h>
#include <kf/lib/tmd.h>

#endif // KF_PLATFORM_PRELUDE_H
