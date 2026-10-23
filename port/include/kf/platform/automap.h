#ifndef KF_PLATFORM_AUTOMAP_H
#define KF_PLATFORM_AUTOMAP_H
// PC extension: automap. The game publishes a snapshot of the current floor
// once per update; the host tracks which cells the player has seen (per floor,
// saved beside each save slot) and the overlay draws the minimap and full map.
#include <kf/lib/types.h>
#include <kf/platform/saves.h>

namespace kf {
inline constexpr int automap_columns = 100;
inline constexpr int automap_rows = 100;
inline constexpr int automap_floor_count = 5;

enum class AutomapCell : u8 {
    Void,       // nothing authored here
    Wall,       // solid
    Floor,
    // Diagonal half cells, named by their walkable half in cell-local x/z (0..1).
    HalfXGeZ, HalfSumLe, HalfZGeX, HalfSumGe,
    Door,       // a closed door's gap
    DoorOpen,   // an open door's gap
    Chasm,      // walkable, but only by a fatal fall (a pit or trench)
    HarpBridge, // a bridge the Harp raises, lowered into its chasm
    HarpBridgeRaised
};

enum class AutomapHazard : u8 { None, Pit, Poison, Bottomless };

enum class AutomapMarkerKind : u8 {
    // Standard: shown once their cell has been seen.
    Door, LockedDoor, Warp, SavePoint, Fountain, Npc,
    // Cheats: shown only when enabled in the settings.
    Item, Chest, Enemy,
    // Hover only: the middle of a Harp bridge.
    HarpBridge
};

// Warp labels: 1..5 lead to that floor.
inline constexpr u8 automap_warp_teleport = 10;   // same-floor teleport
inline constexpr u8 automap_warp_exit = 11;       // the way out (ending)

inline constexpr u8 automap_no_item = 0xff;
inline constexpr u8 automap_unlocked = 0xff;

struct AutomapMarker {
    AutomapMarkerKind kind;
    // Warp: destination (1..5 or the constants above). Fountain: 1 when full.
    // Npc: the character id (their dialogue folder, TALK/Cxx).
    // LockedDoor and Chest: the lock's link id (automap_unlocked when open); a
    // link below 128 that names an item is the key that opens it.
    u8 label;
    u8 items[4];  // Item: the item id. Chest: its contents (automap_no_item when empty).
    float x, z;   // in cells; cell (x, z) spans [x, x+1)
};

inline constexpr int automap_marker_capacity = 512;

struct AutomapSnapshot {
    u32 serial;            // increments on every publish
    int floor;             // 1..5; 0 when no floor is loaded
    bool owns_map;         // the Watchman's or Sorcerer's map is in the inventory
    float player_x, player_z;
    float player_yaw;      // radians; 0 faces +z (north), increasing turns towards -x
    AutomapCell cells[automap_rows * automap_columns];   // [z * columns + x]
    AutomapHazard hazards[automap_rows * automap_columns];
    int marker_count;
    AutomapMarker markers[automap_marker_capacity];
};

// Host-owned. The game fills it, then calls automap_published().
AutomapSnapshot &automap_snapshot();
void automap_published();
// Seen cells. A new game or a full restart forgets them.
void automap_reset();
bool automap_seen(int floor, int x, int z);
// Saved and restored with the game's own save slots and the quick save.
void automap_save(SaveSlot slot);
void automap_load(SaveSlot slot);
}

#endif // KF_PLATFORM_AUTOMAP_H
