#include "../ext/coop_profile.h"

#include <kf/platform/online_players.h>

#include <doctest/doctest.h>

#include <algorithm>

using namespace kf;

TEST_CASE("the knight's colour offsets each hold the first colour, on a flat-coloured primitive") {
    const auto tmd = online_player_model_tmd();
    const ext::CoopColour &first = ext::coop_colours()[0];
    REQUIRE_FALSE(online_player_model_colour_offsets().empty());
    for (const u32 offset : online_player_model_colour_offsets()) {
        REQUIRE(offset + 4 <= tmd.size());
        CHECK(tmd[offset] == first.r);
        CHECK(tmd[offset + 1] == first.g);
        CHECK(tmd[offset + 2] == first.b);
        CHECK((tmd[offset + 3] == 0x30 || tmd[offset + 3] == 0x38));   // F3, F4: the code repeated
    }
}

TEST_CASE("the knight's ink offsets each hold the first colour's ink, on a flat-coloured primitive") {
    const auto tmd = online_player_model_tmd();
    const ext::CoopInk first = ext::coop_colours()[0].ink;
    REQUIRE_FALSE(online_player_model_ink_offsets().empty());
    for (const u32 offset : online_player_model_ink_offsets()) {
        REQUIRE(offset + 4 <= tmd.size());
        CHECK(tmd[offset] == first.r);
        CHECK(tmd[offset + 1] == first.g);
        CHECK(tmd[offset + 2] == first.b);
        CHECK((tmd[offset + 3] == 0x30 || tmd[offset + 3] == 0x38));   // F3, F4: the code repeated
    }
}

TEST_CASE("a painted knight differs from the model only in its colour and ink bytes") {
    const auto model = online_player_model_tmd();
    const auto offsets = online_player_model_colour_offsets();
    const auto inks = online_player_model_ink_offsets();
    for (u8 colour = 0; colour < ext::coop_colour_count; ++colour) {
        const auto tmd = online_player_tmd_in(colour);
        REQUIRE(tmd.size() == model.size());
        const ext::CoopColour &paint = ext::coop_colours()[colour];
        for (std::size_t i = 0; i < tmd.size(); ++i) {
            const auto in_ink = std::ranges::find_if(inks, [i](u32 o) { return i >= o && i < o + 3; });
            if (in_ink != inks.end()) {
                const std::array<u8, 3> rgb = {paint.ink.r, paint.ink.g, paint.ink.b};
                CHECK(tmd[i] == rgb[i - *in_ink]);
                continue;
            }
            const auto in_colour = std::ranges::find_if(offsets, [i](u32 o) { return i >= o && i < o + 3; });
            if (in_colour == offsets.end()) {
                CHECK(tmd[i] == model[i]);
                continue;
            }
            const std::array<u8, 3> rgb = {paint.r, paint.g, paint.b};
            CHECK(tmd[i] == rgb[i - *in_colour]);
        }
    }
    CHECK(online_player_tmd_in(0).data() == online_player_tmd_in(0).data());
    CHECK(online_player_tmd_in(ext::coop_colour_count).data() == online_player_tmd_in(0).data());
}

TEST_CASE("the title's knight differs from the painted one only in the pages the moves fill") {
    const auto offsets = online_player_model_page_offsets();
    REQUIRE_FALSE(offsets.empty());
    const auto painted = online_player_tmd_in(2);
    const auto title = online_player_title_tmd_in(2);
    REQUIRE(title.size() == painted.size());
    for (const u32 offset : offsets) {
        REQUIRE(offset + 2 <= title.size());
        CHECK(painted[offset + 1] == 0);
        CHECK((painted[offset] == 142 || painted[offset] == 151));
        CHECK(title[offset] == (painted[offset] == 151 ? 130 : painted[offset]));
    }
    for (std::size_t i = 0; i < title.size(); ++i)
        if (title[i] != painted[i])
            CHECK(std::ranges::find_if(offsets, [i](u32 o) { return i == o; }) != offsets.end());
    // An 8-bit page at x 128, y 0: the metal lands inside it, off the logo's area.
    const auto moves = online_player_title_texture_moves();
    CHECK((moves[0].to_x >= 128 && moves[0].to_x + moves[0].from.width <= 256));
}

TEST_CASE("every helmet is an object after the parts") {
    for (u8 helmet = 0; helmet < online_player_helmet_count; ++helmet)
        CHECK(online_player_model_helmet_object(helmet) == online_player_part_count + helmet);
    // The TMD's object count is its third word.
    const auto tmd = online_player_model_tmd();
    CHECK(tmd[8] == online_player_part_count + online_player_helmet_count + online_player_symbol_count);
}

TEST_CASE("every tabard is an object after the helmets, and the plain first one carries no ink") {
    for (u8 symbol = 0; symbol < online_player_symbol_count; ++symbol)
        CHECK(online_player_model_tabard_object(symbol) == online_player_part_count + online_player_helmet_count + symbol);
    CHECK(online_player_model_tabard_depth_bias() < 0);
    // The TMD's object table: each entry's primitives, from the end of the 12-byte header.
    const auto tmd = online_player_model_tmd();
    const auto word = [&](std::size_t at) { return tmd[at] | tmd[at + 1] << 8 | tmd[at + 2] << 16 | tmd[at + 3] << 24; };
    constexpr std::size_t header = 12, entry = 28, primitives_at = 16;
    const auto start_of = [&](std::size_t object) { return header + static_cast<std::size_t>(word(header + object * entry + primitives_at)); };
    for (const u32 offset : online_player_model_ink_offsets())
        CHECK(offset >= start_of(online_player_model_tabard_object(1)));
}
