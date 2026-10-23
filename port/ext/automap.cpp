// Automap: a north-up minimap in a corner and a full floor map on M, drawn from
// the snapshot the game publishes each update (kf/platform/automap.h).
#include "internal.hpp"
#include <imgui.h>
#include <kf/platform/automap.h>
#include <kf/platform/input.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace kf::ext {
namespace {
bool full_map_open;
// Full map view: zoom 1 fits the explored floor; panning moves the centre (cells).
float map_zoom = 1.0f;
float map_centre_x, map_centre_z;
bool map_centre_set;

// Item names as the English translation patch shows them in the menus (read
// from the game's own name rows), with apostrophes the patch leaves out.
// Used only for the map's hover labels.
const char *const item_names[] = {
    "Short Sword", "Battle Axe", "Knight Sword", "Colichemarde", "Morningstar", "Bastard Sword",
    "Crescent Axe", "Triple Fang", "Flame Sword", "Shadow Blade", "Dragon Sword", "Moonlight Sword",
    nullptr, "Iron Mask", "Knight Helm", "Great Helm", "Blood Mask", "Mist Crown", nullptr,
    "Breast Plate", "Knight Plate", "Full Plate", "Fire Mail", "Skull Armor", "Forest Armor", nullptr,
    "Small Shield", "Knight Shield", "Tower Shield", "Black Dragon", "Wind Guard", nullptr,
    "Gauntlets", "Knight Gloves", "Stone Hands", "Iron Boots", "Knight Boots", "Leg Guards", "Feather Boots",
    "Gold Coin", "Blood Stone", "Moon Stone", "Verdite", "Medicinal Herb", "Antidote Herb", "Recovery Medicine",
    "Dragon Tree Leaf", "Dragon Tree Fruit", "Light Ring", "Moon Charm", "Wind Blade Bracelet",
    "Twin Dragon Ring", "Gold Cross", "Deceased's Key", "Light Family Key", "Keeper's Map",
    "Dragon Cup", "Phantom Rod", "Green Dragon Rod", "Truth Mirror", "Dungeon Key",
    "Dragon Crest", "Harp", "Water Seal Stone", "Earth Seal Stone", nullptr, nullptr, "Light Tower Seal",
    "Fire Seal Stone", "Wind Seal Stone", "Light Seal Stone", nullptr, nullptr, "Magician's Map",
    "Magician's Key", "Magic Hand", "Sword Fan Dragon", nullptr, nullptr, nullptr};
static_assert(sizeof item_names / sizeof item_names[0] == 80, "one name per item id");

std::string item_name(u8 id) {
    if (id < sizeof item_names / sizeof item_names[0] && item_names[id])
        return item_names[id];
    return "Item " + std::to_string(id);
}

// What opens a lock: a key (the link names the key's item id) or something else.
std::string lock_hint(u8 link) {
    constexpr u8 reusable_first = 128;
    if (link == automap_unlocked)
        return {};
    if (link < reusable_first && link < sizeof item_names / sizeof item_names[0] && item_names[link]
            && std::string(item_names[link]).find("Key") != std::string::npos)
        return "Needs the " + item_name(link);
    if (link >= reusable_first)
        return "Opened by a switch";
    return "Opened by an event or mechanism elsewhere";
}

constexpr int gameplay_context = static_cast<int>(InputContext::Gameplay);
constexpr int scripted_context = static_cast<int>(InputContext::Scripted);

bool walkable(AutomapCell cell) {
    return cell != AutomapCell::Void && cell != AutomapCell::Wall;
}

bool is_half(AutomapCell cell) {
    return cell >= AutomapCell::HalfXGeZ && cell <= AutomapCell::HalfSumGe;
}

struct MapView {
    ImDrawList *draw;
    float origin_x, origin_y;   // screen position of the map point (centre_x, centre_z)
    float centre_x, centre_z;   // in cells
    float scale;                // pixels per cell
    float alpha;
    ImVec2 at(float x, float z) const {
        // North (+z) is up.
        return ImVec2(origin_x + (x - centre_x) * scale, origin_y - (z - centre_z) * scale);
    }
    ImU32 colour(int r, int g, int b, float a = 1.0f) const {
        return IM_COL32(r, g, b, static_cast<int>(255 * a * alpha));
    }
};

bool cell_shown(const AutomapSnapshot &map, int x, int z) {
    if (x < 0 || z < 0 || x >= automap_columns || z >= automap_rows)
        return false;
    return !settings().map_fog || map.owns_map || automap_seen(map.floor, x, z);
}

AutomapCell cell_at(const AutomapSnapshot &map, int x, int z) {
    if (x < 0 || z < 0 || x >= automap_columns || z >= automap_rows)
        return AutomapCell::Void;
    return map.cells[z * automap_columns + x];
}

void draw_cells(const MapView &view, const AutomapSnapshot &map, int min_x, int min_z, int max_x, int max_z) {
    const ImU32 floor_colour = view.colour(78, 70, 56, 0.9f);
    const ImU32 wall_colour = view.colour(215, 200, 160);
    const ImU32 door_colour = view.colour(190, 120, 50);
    const float wall_width = std::max(1.0f, view.scale * 0.14f);
    for (int z = std::max(min_z, 0); z <= std::min(max_z, automap_rows - 1); ++z) {
        for (int x = std::max(min_x, 0); x <= std::min(max_x, automap_columns - 1); ++x) {
            const auto cell = cell_at(map, x, z);
            if (!walkable(cell) || !cell_shown(map, x, z))
                continue;
            // Corners: a = (x, z), b = (x+1, z), c = (x+1, z+1), d = (x, z+1).
            const ImVec2 a = view.at(x, z), b = view.at(x + 1, z), c = view.at(x + 1, z + 1), d = view.at(x, z + 1);
            ImU32 fill = floor_colour;
            switch (map.hazards[z * automap_columns + x]) {
            case AutomapHazard::Pit: fill = view.colour(120, 60, 40, 0.9f); break;
            case AutomapHazard::Poison: fill = view.colour(60, 110, 50, 0.9f); break;
            case AutomapHazard::Bottomless: fill = view.colour(20, 16, 14, 0.95f); break;
            case AutomapHazard::None: break;
            }
            switch (cell) {
            case AutomapCell::HalfXGeZ:
                view.draw->AddTriangleFilled(a, b, c, fill);
                view.draw->AddLine(a, c, wall_colour, wall_width);
                break;
            case AutomapCell::HalfSumLe:
                view.draw->AddTriangleFilled(a, b, d, fill);
                view.draw->AddLine(b, d, wall_colour, wall_width);
                break;
            case AutomapCell::HalfZGeX:
                view.draw->AddTriangleFilled(a, c, d, fill);
                view.draw->AddLine(a, c, wall_colour, wall_width);
                break;
            case AutomapCell::HalfSumGe:
                view.draw->AddTriangleFilled(b, c, d, fill);
                view.draw->AddLine(b, d, wall_colour, wall_width);
                break;
            case AutomapCell::Door:
                view.draw->AddRectFilled(d, b, door_colour);
                break;
            case AutomapCell::DoorOpen:
                view.draw->AddRectFilled(d, b, fill);
                view.draw->AddRect(d, b, door_colour, 0.0f, 0, std::max(1.0f, view.scale * 0.1f));
                break;
            case AutomapCell::Chasm:
            case AutomapCell::HarpBridge:
                // Grey with diagonal hatching that joins into stripes across cells.
                view.draw->AddRectFilled(d, b, view.colour(46, 46, 50, 0.92f));
                if (view.scale >= 5.0f)
                    view.draw->AddLine(a, c, view.colour(78, 78, 84, 0.9f), std::max(1.0f, view.scale * 0.08f));
                if (cell == AutomapCell::HarpBridge) {
                    // Lowered: an outline where the bridge will rise.
                    const float inset = view.scale * 0.12f;
                    view.draw->AddRect(ImVec2(d.x + inset, d.y + inset), ImVec2(b.x - inset, b.y - inset),
                                       view.colour(70, 170, 190, 0.85f), 0.0f, 0, std::max(1.0f, view.scale * 0.1f));
                }
                break;
            case AutomapCell::HarpBridgeRaised:
                view.draw->AddRectFilled(d, b, view.colour(60, 140, 160, 0.95f));
                break;
            default:
                view.draw->AddRectFilled(d, b, fill);
                break;
            }
            // Walls: a side facing a solid neighbour. Half cells only own the two
            // sides their walkable triangle touches.
            const bool south = cell != AutomapCell::HalfZGeX && cell != AutomapCell::HalfSumGe;
            const bool north = cell != AutomapCell::HalfXGeZ && cell != AutomapCell::HalfSumLe;
            const bool west = cell != AutomapCell::HalfXGeZ && cell != AutomapCell::HalfSumGe;
            const bool east = cell != AutomapCell::HalfZGeX && cell != AutomapCell::HalfSumLe;
            if (south && !walkable(cell_at(map, x, z - 1)))
                view.draw->AddLine(a, b, wall_colour, wall_width);
            if (north && !walkable(cell_at(map, x, z + 1)))
                view.draw->AddLine(d, c, wall_colour, wall_width);
            if (west && !walkable(cell_at(map, x - 1, z)))
                view.draw->AddLine(a, d, wall_colour, wall_width);
            if (east && !walkable(cell_at(map, x + 1, z)))
                view.draw->AddLine(b, c, wall_colour, wall_width);
        }
    }
}

// Visible (or, for plain doors, hoverable: their cells are drawn instead of a symbol).
bool marker_known(const AutomapSnapshot &map, const AutomapMarker &marker) {
    const Settings &s = settings();
    switch (marker.kind) {
    case AutomapMarkerKind::Item: return s.map_items;
    case AutomapMarkerKind::Chest: return s.map_chests;
    case AutomapMarkerKind::Enemy: return s.map_enemies;
    default: return cell_shown(map, static_cast<int>(marker.x), static_cast<int>(marker.z));
    }
}

bool marker_shown(const AutomapSnapshot &map, const AutomapMarker &marker) {
    return marker.kind != AutomapMarkerKind::Door && marker.kind != AutomapMarkerKind::HarpBridge
        && marker_known(map, marker);
}

// People, by character id, as their own dialogue names them (TALK/Cxx images).
// Those who never give a name stay "Person".
// Names and roles from the Mirror of Truth profiles (KF/PRSN/PERnn.TIM, nn =
// character id), in the English translation's spelling.
const char *person_name(u8 character) {
    switch (character) {
    case 1: return "Wilfred Light (merchant)";
    case 2: return "Gill Baccas (merchant)";
    case 3: return "Juedds Kross (priest)";
    case 4: return "Basque Crize (soldier)";
    case 5: return "Watts Virgil (soldier)";
    case 6: return "Erik Blair (soldier)";
    case 7: return "Heinz Ruhart (minstrel)";
    case 8: return "Rally Kelvim (sage)";
    case 9: return "Nicholas Oarman (grave digger)";
    case 10: return "Forest Dragon";
    case 11: return "Miria (the dragon's fairy)";
    case 12: return "Randalf VIII (former king)";
    case 13: return "Ozzy Ranvalgve (graveyard caretaker)";
    case 14:
    case 15:
    case 16: return "Evil Dark Magician";
    case 17: return "Reinhardt III (former king)";
    default: return "Person";
    }
}

std::string marker_description(const AutomapMarker &marker) {
    const bool hints = settings().map_key_hints;
    switch (marker.kind) {
    case AutomapMarkerKind::Door:
        return "Door";
    case AutomapMarkerKind::LockedDoor:
        return hints ? "Locked door\n" + lock_hint(marker.label) : "Locked door";
    case AutomapMarkerKind::Warp:
        if (marker.label == automap_warp_teleport)
            return "Teleporter (same floor)";
        if (marker.label == automap_warp_exit)
            return "The way out";
        return "Warp to floor " + std::to_string(marker.label);
    case AutomapMarkerKind::SavePoint:
        return "Save point";
    case AutomapMarkerKind::Fountain:
        return marker.label ? "Fountain" : "Fountain (dry)";
    case AutomapMarkerKind::Npc:
        return person_name(marker.label);
    case AutomapMarkerKind::Item: {
        const std::string effect = item_description(marker.items[0]);
        return effect.empty() ? item_name(marker.items[0]) : item_name(marker.items[0]) + "\n" + effect;
    }
    case AutomapMarkerKind::Chest: {
        std::string text = marker.label != automap_unlocked ? "Locked chest" : "Chest";
        for (u8 item : marker.items)
            if (item != automap_no_item)
                text += "\n  " + item_name(item);
        if (hints && marker.label != automap_unlocked)
            text += "\n" + lock_hint(marker.label);
        return text;
    }
    case AutomapMarkerKind::Enemy:
        return marker.items[0] != automap_no_item ? "Enemy\nDrops: " + item_name(marker.items[0]) : "Enemy";
    case AutomapMarkerKind::HarpBridge:
        return "Harp bridge\nPlay the Harp nearby to raise it for a short time";
    }
    return {};
}

// A label beside the cursor for the marker under it, drawn above the map.
void draw_hover(const MapView &view, const AutomapSnapshot &map, ImVec2 mouse, float text_size) {
    const float reach = std::max(8.0f, view.scale * 0.6f);
    const AutomapMarker *best = nullptr;
    float best_distance = reach * reach;
    for (int i = 0; i < map.marker_count; ++i) {
        const auto &marker = map.markers[i];
        if (!marker_known(map, marker))
            continue;
        const ImVec2 p = view.at(marker.x, marker.z);
        const float dx = p.x - mouse.x, dy = p.y - mouse.y;
        if (dx * dx + dy * dy < best_distance) {
            best_distance = dx * dx + dy * dy;
            best = &marker;
        }
    }
    if (!best)
        return;
    const std::string text = marker_description(*best);
    ImFont *font = ImGui::GetFont();
    // Item descriptions wrap at about 32 characters.
    const float wrap = text_size * 16.0f;
    const ImVec2 extent = font->CalcTextSizeA(text_size, FLT_MAX, wrap, text.c_str());
    const float pad = text_size * 0.4f;
    ImVec2 at(mouse.x + text_size, mouse.y + text_size * 0.6f);
    const ViewRect game = game_view();
    if (at.x + extent.x + pad > game.x + game.w)
        at.x = mouse.x - text_size - extent.x - pad;
    if (at.y + extent.y + pad > game.y + game.h)
        at.y = mouse.y - extent.y - pad * 2;
    view.draw->AddRectFilled(ImVec2(at.x - pad, at.y - pad), ImVec2(at.x + extent.x + pad, at.y + extent.y + pad),
                             IM_COL32(20, 17, 12, 235), pad);
    view.draw->AddRect(ImVec2(at.x - pad, at.y - pad), ImVec2(at.x + extent.x + pad, at.y + extent.y + pad),
                       IM_COL32(200, 180, 130, 255), pad);
    view.draw->AddText(font, text_size, at, IM_COL32(240, 230, 210, 255), text.c_str(), nullptr, wrap);
}

void draw_label(const MapView &view, ImVec2 centre, float size, const char *text, ImU32 colour) {
    ImFont *font = ImGui::GetFont();
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    view.draw->AddText(font, size, ImVec2(centre.x - extent.x * 0.5f, centre.y - extent.y * 0.5f), colour, text);
}

void draw_markers(const MapView &view, const AutomapSnapshot &map, bool labels) {
    // The full map keeps markers legible even when a cell is only a few pixels.
    const float r = std::max(labels ? 5.0f : 2.5f, view.scale * 0.42f);
    const ImU32 outline = view.colour(0, 0, 0, 0.8f);
    for (int i = 0; i < map.marker_count; ++i) {
        const auto &marker = map.markers[i];
        if (!marker_shown(map, marker))
            continue;
        const ImVec2 p = view.at(marker.x, marker.z);
        switch (marker.kind) {
        case AutomapMarkerKind::LockedDoor:
            view.draw->AddRectFilled(ImVec2(p.x - r * 0.6f, p.y - r * 0.5f), ImVec2(p.x + r * 0.6f, p.y + r * 0.6f), view.colour(210, 60, 50));
            view.draw->AddCircle(ImVec2(p.x, p.y - r * 0.5f), r * 0.4f, view.colour(210, 60, 50), 12, std::max(1.0f, r * 0.2f));
            break;
        case AutomapMarkerKind::Warp: {
            view.draw->AddCircleFilled(p, r * 1.1f, view.colour(150, 90, 220));
            view.draw->AddCircle(p, r * 1.1f, outline, 16, 1.0f);
            if (labels || r >= 4.0f) {
                char text[4];
                if (marker.label == automap_warp_teleport)
                    std::snprintf(text, sizeof text, "T");
                else if (marker.label == automap_warp_exit)
                    std::snprintf(text, sizeof text, "X");
                else
                    std::snprintf(text, sizeof text, "%d", marker.label);
                draw_label(view, p, r * 1.7f, text, view.colour(255, 255, 255));
            }
            break;
        }
        case AutomapMarkerKind::SavePoint: {
            const ImVec2 top(p.x, p.y - r), right(p.x + r, p.y), bottom(p.x, p.y + r), left(p.x - r, p.y);
            view.draw->AddQuadFilled(top, right, bottom, left, view.colour(80, 220, 230));
            view.draw->AddQuad(top, right, bottom, left, outline, 1.0f);
            break;
        }
        case AutomapMarkerKind::Fountain:
            if (marker.label)
                view.draw->AddCircleFilled(p, r, view.colour(70, 140, 255));
            view.draw->AddCircle(p, r, view.colour(70, 140, 255), 16, std::max(1.0f, r * 0.3f));
            break;
        case AutomapMarkerKind::Npc:
            view.draw->AddCircleFilled(p, r * 0.8f, view.colour(120, 230, 120));
            view.draw->AddCircle(p, r * 0.8f, outline, 12, 1.0f);
            break;
        case AutomapMarkerKind::Item:
            view.draw->AddCircleFilled(p, r * 0.55f, view.colour(250, 220, 90));
            break;
        case AutomapMarkerKind::Chest:
            view.draw->AddRectFilled(ImVec2(p.x - r * 0.75f, p.y - r * 0.55f), ImVec2(p.x + r * 0.75f, p.y + r * 0.55f), view.colour(230, 170, 60));
            view.draw->AddRect(ImVec2(p.x - r * 0.75f, p.y - r * 0.55f), ImVec2(p.x + r * 0.75f, p.y + r * 0.55f), outline, 0.0f, 0, 1.0f);
            break;
        case AutomapMarkerKind::Enemy:
            view.draw->AddCircleFilled(p, r * 0.7f, view.colour(235, 50, 50));
            if (marker.items[0] != automap_no_item) {
                // A gold ring: this enemy drops an item when it dies.
                const float ring = std::max(1.5f, r * 0.3f);
                view.draw->AddCircle(p, r * 0.7f + ring * 0.5f, view.colour(255, 205, 60), 16, ring);
            } else {
                view.draw->AddCircle(p, r * 0.7f, outline, 12, 1.0f);
            }
            break;
        case AutomapMarkerKind::Door:
        case AutomapMarkerKind::HarpBridge:
            break;
        }
    }
}

void draw_player(const MapView &view, const AutomapSnapshot &map) {
    const ImVec2 p = view.at(map.player_x, map.player_z);
    // Facing (-sin yaw, +cos yaw) in map x/z; screen y points south.
    const float fx = -std::sin(map.player_yaw), fz = std::cos(map.player_yaw);
    const ImVec2 forward(fx, -fz), side(fz, fx);
    const float size = std::max(5.0f, view.scale * 0.9f);
    const ImVec2 tip(p.x + forward.x * size, p.y + forward.y * size);
    const ImVec2 back(p.x - forward.x * size * 0.45f, p.y - forward.y * size * 0.45f);
    const ImVec2 left(p.x - forward.x * size * 0.7f + side.x * size * 0.65f, p.y - forward.y * size * 0.7f + side.y * size * 0.65f);
    const ImVec2 right(p.x - forward.x * size * 0.7f - side.x * size * 0.65f, p.y - forward.y * size * 0.7f - side.y * size * 0.65f);
    view.draw->AddQuadFilled(tip, left, back, right, view.colour(255, 250, 220));
    view.draw->AddQuad(tip, left, back, right, view.colour(0, 0, 0), 1.5f);
}

void draw_minimap(const AutomapSnapshot &map) {
    const Settings &s = settings();
    const ViewRect game = game_view();
    const float size = game.h * static_cast<float>(s.minimap_size) / 100.0f;
    const float margin = game.h * 0.025f;
    float left = game.x + game.w - size - margin, top = game.y + game.h - size - margin;
    if (s.minimap_corner == 0 || s.minimap_corner == 2)
        top = game.y + margin;
    if (s.minimap_corner >= 2)
        left = game.x + margin;
    const ImVec2 lo(left, top), hi(left + size, top + size);
    MapView view{ImGui::GetBackgroundDrawList(), left + size * 0.5f, top + size * 0.5f, map.player_x, map.player_z,
                 size / (2.0f * s.minimap_zoom), static_cast<float>(s.minimap_opacity) / 100.0f};
    view.draw->AddRectFilled(lo, hi, view.colour(10, 9, 8, 0.75f), size * 0.04f);
    view.draw->PushClipRect(lo, hi, true);
    const int radius = s.minimap_zoom + 2;
    const int cx = static_cast<int>(map.player_x), cz = static_cast<int>(map.player_z);
    draw_cells(view, map, cx - radius, cz - radius, cx + radius, cz + radius);
    draw_markers(view, map, false);
    draw_player(view, map);
    view.draw->PopClipRect();
    view.draw->AddRect(lo, hi, view.colour(200, 180, 130, 0.8f), size * 0.04f, 0, 1.5f);
    draw_label(view, ImVec2(lo.x + size * 0.5f, lo.y + size * 0.06f), std::max(12.0f, size * 0.07f), "N", view.colour(230, 60, 50));
}

void draw_legend_entry(ImDrawList *draw, ImVec2 &at, float size, ImU32 colour, const char *text, bool square = false) {
    const ImVec2 centre(at.x + size * 0.5f, at.y + size * 0.55f);
    if (square)
        draw->AddRectFilled(ImVec2(centre.x - size * 0.35f, centre.y - size * 0.3f), ImVec2(centre.x + size * 0.35f, centre.y + size * 0.3f), colour);
    else
        draw->AddCircleFilled(centre, size * 0.33f, colour);
    draw->AddText(ImGui::GetFont(), size, ImVec2(at.x + size * 1.3f, at.y), IM_COL32(230, 220, 200, 255), text);
    at.y += size * 1.35f;
}

void draw_full_map(const AutomapSnapshot &map) {
    const Settings &s = settings();
    const ViewRect game = game_view();
    // Above the game HUD and mod HUDs; the map replaces the picture while open.
    ImDrawList *draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(game.x, game.y), ImVec2(game.x + game.w, game.y + game.h), IM_COL32(6, 5, 4, 255));
    // Fit the shown part of the floor, with a little room around it.
    int min_x = automap_columns, min_z = automap_rows, max_x = -1, max_z = -1;
    for (int z = 0; z < automap_rows; ++z)
        for (int x = 0; x < automap_columns; ++x)
            if (walkable(cell_at(map, x, z)) && cell_shown(map, x, z)) {
                min_x = std::min(min_x, x); max_x = std::max(max_x, x);
                min_z = std::min(min_z, z); max_z = std::max(max_z, z);
            }
    const int px = static_cast<int>(map.player_x), pz = static_cast<int>(map.player_z);
    min_x = std::min(min_x, px) - 2; max_x = std::max(max_x, px) + 3;
    min_z = std::min(min_z, pz) - 2; max_z = std::max(max_z, pz) + 3;
    const float legend_width = game.h * 0.28f;
    const float area_w = game.w - legend_width - game.h * 0.08f, area_h = game.h * 0.86f;
    // Small explored areas are not blown up past a readable size.
    const float fit_scale = std::min({area_w / (max_x - min_x), area_h / (max_z - min_z), game.h / 28.0f});
    const float area_x = game.x + game.h * 0.04f, area_y = game.y + game.h * 0.08f;
    const float text = std::max(14.0f, game.h / 34.0f);
    if (!map_centre_set) {
        map_centre_x = (min_x + max_x) * 0.5f;
        map_centre_z = (min_z + max_z) * 0.5f;
    }
    MapView view{draw, area_x + area_w * 0.5f, area_y + area_h * 0.5f,
                  map_centre_x, map_centre_z, fit_scale * map_zoom, 1.0f};

    // Mouse: wheel zooms towards the cursor, left drag moves, right click resets.
    const ImGuiIO &io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;
    // KF_TEST_MAPZOOM=<zoom>: centre on the player at that zoom. KF_TEST_MAPHOVER=<marker
    // kind number>: point at the first known marker of that kind (automated captures).
    static const char *test_zoom = std::getenv("KF_TEST_MAPZOOM");
    static const char *test_hover = std::getenv("KF_TEST_MAPHOVER");
    if (test_zoom) {
        map_zoom = static_cast<float>(std::atof(test_zoom));
        map_centre_x = map.player_x;
        map_centre_z = map.player_z;
        map_centre_set = true;
        view.scale = fit_scale * map_zoom;
        view.centre_x = map_centre_x;
        view.centre_z = map_centre_z;
    }
    if (test_hover) {
        // Prefers a marker with items (a chest's contents, an enemy's drop).
        const AutomapMarker *chosen = nullptr;
        for (int i = 0; i < map.marker_count; ++i)
            if (static_cast<int>(map.markers[i].kind) == std::atoi(test_hover) && marker_known(map, map.markers[i])
                    && (!chosen || (chosen->items[0] == automap_no_item && map.markers[i].items[0] != automap_no_item)))
                chosen = &map.markers[i];
        if (chosen)
            mouse = view.at(chosen->x, chosen->z);
    }
    const bool inside = mouse.x >= area_x && mouse.x < area_x + area_w && mouse.y >= area_y && mouse.y < area_y + area_h;
    if (inside && io.MouseWheel != 0.0f) {
        const float world_x = view.centre_x + (mouse.x - view.origin_x) / view.scale;
        const float world_z = view.centre_z - (mouse.y - view.origin_y) / view.scale;
        map_zoom = std::clamp(map_zoom * std::pow(1.25f, io.MouseWheel), 1.0f, 16.0f);
        view.scale = fit_scale * map_zoom;
        map_centre_x = world_x - (mouse.x - view.origin_x) / view.scale;
        map_centre_z = world_z + (mouse.y - view.origin_y) / view.scale;
        map_centre_set = true;
    }
    if (inside && ImGui::IsMouseDown(ImGuiMouseButton_Left) && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
        map_centre_x -= io.MouseDelta.x / view.scale;
        map_centre_z += io.MouseDelta.y / view.scale;
        map_centre_set = true;
    }
    if (inside && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        map_zoom = 1.0f;
        map_centre_set = false;
        map_centre_x = (min_x + max_x) * 0.5f;
        map_centre_z = (min_z + max_z) * 0.5f;
        view.scale = fit_scale;
    }
    // Keep some of the floor on screen.
    map_centre_x = std::clamp(map_centre_x, static_cast<float>(min_x), static_cast<float>(max_x));
    map_centre_z = std::clamp(map_centre_z, static_cast<float>(min_z), static_cast<float>(max_z));
    view.centre_x = map_centre_x;
    view.centre_z = map_centre_z;

    draw->PushClipRect(ImVec2(area_x, area_y), ImVec2(area_x + area_w, area_y + area_h), true);
    const float half_w = area_w * 0.5f / view.scale + 1.0f, half_h = area_h * 0.5f / view.scale + 1.0f;
    draw_cells(view, map, static_cast<int>(view.centre_x - half_w), static_cast<int>(view.centre_z - half_h),
               static_cast<int>(view.centre_x + half_w), static_cast<int>(view.centre_z + half_h));
    draw_markers(view, map, true);
    draw_player(view, map);
    draw->PopClipRect();
    if (inside)
        draw_hover(view, map, mouse, text * 0.9f);

    char title[48];
    std::snprintf(title, sizeof title, "Floor %d", map.floor);
    draw->AddText(ImGui::GetFont(), text * 1.4f, ImVec2(game.x + game.h * 0.04f, game.y + game.h * 0.02f), IM_COL32(240, 220, 170, 255), title);
    ImVec2 at(game.x + game.w - legend_width, game.y + game.h * 0.1f);
    draw_legend_entry(draw, at, text, IM_COL32(150, 90, 220, 255), "Warp (number = floor)");
    draw_legend_entry(draw, at, text, IM_COL32(80, 220, 230, 255), "Save point");
    draw_legend_entry(draw, at, text, IM_COL32(70, 140, 255, 255), "Fountain");
    draw_legend_entry(draw, at, text, IM_COL32(190, 120, 50, 255), "Door", true);
    draw_legend_entry(draw, at, text, IM_COL32(210, 60, 50, 255), "Locked door", true);
    draw_legend_entry(draw, at, text, IM_COL32(120, 230, 120, 255), "Person");
    draw_legend_entry(draw, at, text, IM_COL32(120, 60, 40, 255), "Pit / trap", true);
    draw_legend_entry(draw, at, text, IM_COL32(60, 110, 50, 255), "Poison", true);
    draw_legend_entry(draw, at, text, IM_COL32(70, 70, 76, 255), "Chasm (fatal drop)", true);
    draw_legend_entry(draw, at, text, IM_COL32(60, 140, 160, 255), "Harp bridge", true);
    if (s.map_items)
        draw_legend_entry(draw, at, text, IM_COL32(250, 220, 90, 255), "Item");
    if (s.map_chests)
        draw_legend_entry(draw, at, text, IM_COL32(230, 170, 60, 255), "Chest", true);
    if (s.map_enemies) {
        draw_legend_entry(draw, at, text, IM_COL32(235, 50, 50, 255), "Enemy");
        draw_legend_entry(draw, at, text, IM_COL32(255, 205, 60, 255), "Enemy with a drop");
    }
    at.y += text;
    draw->AddText(ImGui::GetFont(), text * 0.85f, at, IM_COL32(170, 160, 140, 255),
                  "Scroll: zoom\nDrag: move\nRight click: reset\nPoint at a marker for details\nM closes the map");
}
}

void automap_toggle_full() {
    if (settings().classic)
        return;   // no automap in Classic mode
    full_map_open = !full_map_open;
    if (full_map_open) {
        map_zoom = 1.0f;
        map_centre_set = false;
    }
}

bool automap_full_open() {
    return full_map_open;
}

void automap_draw(int input_context) {
    const Settings &s = effective_settings();
    const auto &map = automap_snapshot();
    const bool in_game = input_context == gameplay_context || input_context == scripted_context;
    if (!in_game || map.floor == 0 || s.classic) {
        full_map_open = false;
        return;
    }
    // KF_TEST_FULLMAP[=<frames>]: open the full map once in game, as M would,
    // optionally after that many drawn frames (automated captures).
    static const char *test_open = std::getenv("KF_TEST_FULLMAP");
    static int test_frames;
    if (test_open && ++test_frames >= std::atoi(test_open)) {
        full_map_open = true;
        test_open = nullptr;
    }
    if (full_map_open)
        draw_full_map(map);
    else if (s.minimap)
        draw_minimap(map);
}

void automap_settings_page(bool &changed) {
    Settings &s = settings();
    ImGui::SeparatorText("Minimap");
    changed |= ImGui::Checkbox("Show minimap", &s.minimap);
    if (s.minimap) {
        const char *corners[] = {"Top right", "Bottom right", "Top left", "Bottom left"};
        changed |= ImGui::Combo("Corner", &s.minimap_corner, corners, 4);
        changed |= slider_input("Size", &s.minimap_size, 10, 50, "%d%%");
        ImGui::SetItemTooltip("Minimap size as a percentage of the picture's height.");
        changed |= slider_input("Zoom", &s.minimap_zoom, 4, 30, "%d cells");
        ImGui::SetItemTooltip("How many map cells the minimap shows from the centre to its edge.");
        changed |= slider_input("Opacity", &s.minimap_opacity, 20, 100, "%d%%");
    }
    ImGui::TextDisabled("Press M in game for the full floor map.");
    changed |= ImGui::Checkbox("Pause the game while the full map is open", &s.map_pause);
    ImGui::SetItemTooltip("Off: the game keeps running, so you can walk around with the map open.");
    ImGui::BeginDisabled(!s.map_pause);
    ImGui::Indent();
    changed |= ImGui::Checkbox("Keep the music playing", &s.map_keep_music);
    ImGui::SetItemTooltip("While the map pauses the game, the music carries on; sound effects\nwait and continue when you close the map.");
    ImGui::Unindent();
    ImGui::EndDisabled();
    ImGui::SeparatorText("Exploration");
    changed |= ImGui::Checkbox("Fog of war", &s.map_fog);
    ImGui::SetItemTooltip("On: only places you have seen are drawn (the Keeper's or Magician's Map\nreveals the whole floor). Off: the whole floor is always shown.");
    ImGui::SeparatorText("Extra markers (cheats)");
    changed |= ImGui::Checkbox("Items on the ground", &s.map_items);
    changed |= ImGui::Checkbox("Chests with something inside", &s.map_chests);
    changed |= ImGui::Checkbox("Enemies", &s.map_enemies);
    changed |= ImGui::Checkbox("Which key opens locked doors and chests", &s.map_key_hints);
    ImGui::SetItemTooltip("Shown when pointing at a locked door or chest on the full map.");
    ImGui::TextDisabled("Doors, warps, save points, fountains and people always show once seen.");
}
}
