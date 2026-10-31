#pragma once
// A session on a floor's mesh (signaling.h, floor_mesh_url), for the board. Internal: the public
// header has no mesh, and a session from here behaves as a room's, except that its peers are
// named by link frames as well as in welcome.

#include "verdite_online.h"

#include <cstdint>

// NULL when a name is malformed. Close it with vo_close.
vo_session *vo_open_mesh(const char *service_url, const char *game, uint32_t area,
                         const char *build);
