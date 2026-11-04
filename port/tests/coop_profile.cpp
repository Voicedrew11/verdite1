#include "../ext/coop_profile.h"

#include <doctest/doctest.h>

using namespace kf::ext;

TEST_CASE("an install's first profile is PLAYER and four digits of its id") {
    const CoopProfile profile = coop_profile_default(0x1234567890ULL);
    CHECK(profile.name == "PLAYER" + std::to_string(0x1234567890ULL % 10000));
    CHECK(profile.colour == 0);
    CHECK(profile.helmet == 0);
    CHECK(profile.symbol == 0);
    CHECK(coop_profile_valid(profile));
    CHECK(coop_profile_default(7).name == "PLAYER0007");
}

TEST_CASE("a profile is valid with a drawable name and a colour, helmet and symbol in their lists") {
    CHECK(coop_profile_valid({"ANDREW", coop_colour_count - 1, 0}));
    CHECK_FALSE(coop_profile_valid({"Andrew", 0, 0}));
    CHECK_FALSE(coop_profile_valid({"", 0, 0}));
    CHECK_FALSE(coop_profile_valid({"ANDREW", coop_colour_count, 0}));
    CHECK_FALSE(coop_profile_valid({"ANDREW", 0, kf::online_player_helmet_count}));
    CHECK(coop_profile_valid({"ANDREW", 0, 0, kf::online_player_symbol_count - 1}));
    CHECK_FALSE(coop_profile_valid({"ANDREW", 0, 0, kf::online_player_symbol_count}));
}

TEST_CASE("the profile's file reads back as written") {
    const CoopProfile profile{"ANDREW", 3, 0};
    CHECK(coop_profile_text(profile) == "name=ANDREW\ncolour=3\nhelmet=0\nsymbol=0\n");
    CHECK(coop_profile_read(coop_profile_text(profile), coop_profile_default(1)) == profile);
    const CoopProfile fluted{"ANDREW", 3, 1};
    CHECK(coop_profile_read(coop_profile_text(fluted), coop_profile_default(1)) == fluted);
    const CoopProfile marked{"ANDREW", 3, 1, 1};
    CHECK(coop_profile_read(coop_profile_text(marked), coop_profile_default(1)) == marked);
}

TEST_CASE("a part missing or bad in the file is the fallback's") {
    const CoopProfile fallback{"PLAYER0001", 2, 0};
    CHECK(coop_profile_read("", fallback) == fallback);
    CHECK(coop_profile_read("colour=5\r\n", fallback) == CoopProfile{"PLAYER0001", 5, 0});
    CHECK(coop_profile_read("name=bad name\ncolour=8\nhelmet=x\nsymbol=2\n", fallback) == fallback);
    CHECK(coop_profile_read("name=PLAYER0001\ncolour=2\nhelmet=0\n", CoopProfile{"X", 0, 0, 1}).symbol == 1);
    CHECK(coop_profile_read("name=\ncolour=-1\ncolour=3x\nnamex=AB\n", fallback) == fallback);
    CHECK(coop_profile_read("name=THIRTEENCHARS\n", fallback) == fallback);
    CHECK(coop_profile_read("name=NEW", fallback).name == "NEW");
}

TEST_CASE("the symbol is white, but black on the white tabard, where white would not show") {
    for (const CoopColour &colour : coop_colours()) {
        const bool white_tabard = colour.name.english == "WHITE";
        CHECK(colour.ink.r == (white_tabard ? 24 : 232));
        CHECK(colour.ink.g == (white_tabard ? 22 : 228));
        CHECK(colour.ink.b == (white_tabard ? 20 : 216));
    }
}

TEST_CASE("every colour, helmet and symbol has a name in both languages") {
    for (const CoopColour &colour : coop_colours())
        CHECK((!colour.name.english.empty() && !colour.name.japanese.empty()));
    for (const CoopProfileWords &helmet : coop_helmet_names())
        CHECK((!helmet.english.empty() && !helmet.japanese.empty()));
    for (const CoopProfileWords &symbol : coop_symbol_names())
        CHECK((!symbol.english.empty() && !symbol.japanese.empty()));
}
