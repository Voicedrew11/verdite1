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

TEST_CASE("a painted knight differs from the model only in its player's-colour bytes") {
    const auto model = online_player_model_tmd();
    const auto offsets = online_player_model_colour_offsets();
    for (u8 colour = 0; colour < ext::coop_colour_count; ++colour) {
        const auto tmd = online_player_tmd_in(colour);
        REQUIRE(tmd.size() == model.size());
        const ext::CoopColour &paint = ext::coop_colours()[colour];
        for (std::size_t i = 0; i < tmd.size(); ++i) {
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

TEST_CASE("every helmet is an object after the parts") {
    for (u8 helmet = 0; helmet < online_player_helmet_count; ++helmet)
        CHECK(online_player_model_helmet_object(helmet) == online_player_part_count + helmet);
    // The TMD's object count is its third word.
    const auto tmd = online_player_model_tmd();
    CHECK(tmd[8] == online_player_part_count + online_player_helmet_count);
}
