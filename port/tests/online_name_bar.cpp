#include <kf/platform/online_name_bar.h>

#include <doctest/doctest.h>

using namespace kf;

TEST_CASE("a name bar's band is centred over the knight and stands on its anchor") {
    const OnlineNameBarRect band = online_name_bar_band(100);
    CHECK(band.left == -band.right);
    CHECK(band.bottom == 0);
    CHECK(band.top == -(online_name_bar_text_height_px + 2 * online_name_bar_margin_px) * online_name_bar_units_per_px);
    CHECK(band.right - band.left == (100 + 2 * online_name_bar_margin_px) * online_name_bar_units_per_px);
}

TEST_CASE("a name fills its band, a margin in from every side") {
    const OnlineNameBarRect band = online_name_bar_band(100);
    const OnlineNameBarRect whole = online_name_bar_piece(100, 0, 0, 100, online_name_bar_text_height_px);
    const s32 margin = online_name_bar_margin_px * online_name_bar_units_per_px;
    CHECK(whole == OnlineNameBarRect{band.left + margin, band.top + margin, band.right - margin, band.bottom - margin});
}

TEST_CASE("a piece of the text lies where the text says") {
    const OnlineNameBarRect first = online_name_bar_piece(40, 0, 0, 14, 12);
    const OnlineNameBarRect second = online_name_bar_piece(40, 10, 1, 14, 12);
    CHECK(second.left - first.left == 10 * online_name_bar_units_per_px);
    CHECK(second.top - first.top == online_name_bar_units_per_px);
    CHECK(first.right - first.left == 14 * online_name_bar_units_per_px);
}

TEST_CASE("the longest name's corners fit the game's 16-bit model space") {
    const OnlineNameBarRect band = online_name_bar_band(12 * 15);   // name_most capitals, the widest of them
    CHECK(band.right < 32767);
    CHECK(band.top > -32768);
}

TEST_CASE("in sight shows a name only where the cell is in view") {
    CHECK(online_name_bar_shown(OnlineNameBars::InSight, false, true));
    CHECK_FALSE(online_name_bar_shown(OnlineNameBars::InSight, false, false));
}

TEST_CASE("always shows a name whether or not its cell is in view") {
    CHECK(online_name_bar_shown(OnlineNameBars::Always, false, true));
    CHECK(online_name_bar_shown(OnlineNameBars::Always, false, false));
}

TEST_CASE("off shows no name, and no one shows the name of the player whose eyes this is") {
    CHECK_FALSE(online_name_bar_shown(OnlineNameBars::Off, false, true));
    CHECK_FALSE(online_name_bar_shown(OnlineNameBars::Always, true, true));
    CHECK_FALSE(online_name_bar_shown(OnlineNameBars::InSight, true, true));
}

TEST_CASE("only a bar in sight is depth tested; one through walls is drawn over the world") {
    CHECK(online_name_bar_depth_tested(OnlineNameBars::InSight));
    CHECK_FALSE(online_name_bar_depth_tested(OnlineNameBars::Always));
}

TEST_CASE("a bar within reach is drawn at its own size") {
    CHECK(online_name_bar_scale(1000, -20000, 30000) == 1.0);
}

TEST_CASE("a bar beyond reach is scaled so its anchor's largest component is at the reach") {
    const double scale = online_name_bar_scale(-60000, 1000, 40000);
    CHECK(scale == doctest::Approx(0.5));
    CHECK(60000 * scale <= online_name_bar_reach);
}
