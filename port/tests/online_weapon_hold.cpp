#include <kf/platform/online_weapon_hold.h>

#include <doctest/doctest.h>

#include <cmath>
#include <numbers>

using namespace kf;

namespace {

constexpr OnlineRotation identity = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};

// A model shaped as the first-person ones are: a forearm from the elbow at the origin along -z, a
// gauntlet at its end, and a blade beyond it, each a closed box.
struct Model {
    std::vector<SVECTOR> vertices;
    std::vector<OnlineModelFace> faces;

    void box(s16 x0, s16 y0, s16 z0, s16 x1, s16 y1, s16 z1) {
        const auto first = static_cast<u16>(vertices.size());
        for (int i = 0; i < 8; ++i)
            vertices.emplace_back((i & 1) ? x1 : x0, (i & 2) ? y1 : y0, (i & 4) ? z1 : z0, s16{0});
        const std::array<std::array<u16, 4>, 6> sides = {{
            {0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 4, 5}, {2, 3, 6, 7}, {0, 2, 4, 6}, {1, 3, 5, 7}}};
        for (const auto &side : sides)
            faces.push_back({{static_cast<u16>(first + side[0]), static_cast<u16>(first + side[1]),
                static_cast<u16>(first + side[2]), static_cast<u16>(first + side[3])}, 4});
    }
};

Model sword() {
    Model model;
    model.box(-60, -60, -1200, 60, 60, -10);      // forearm, the elbow end beside the origin
    model.box(-80, -80, -1400, 80, 80, -1210);    // gauntlet
    model.box(-10, -40, -3000, 10, 40, -1410);    // blade, ahead along -z
    return model;
}

OnlineVector apply(const OnlineRotation &r, OnlineVector v) {
    return {r[0].x * v.x + r[0].y * v.y + r[0].z * v.z, r[1].x * v.x + r[1].y * v.y + r[1].z * v.z,
        r[2].x * v.x + r[2].y * v.y + r[2].z * v.z};
}

OnlineRotation about_y(float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return {{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
}

SVECTOR turned(const OnlineRotation &r, const SVECTOR &v) {
    const auto t = apply(r, {static_cast<float>(v.vx), static_cast<float>(v.vy), static_cast<float>(v.vz)});
    return {static_cast<s16>(std::lround(t.x)), static_cast<s16>(std::lround(t.y)), static_cast<s16>(std::lround(t.z)), 0};
}

// The model's hold; a model that has none fails the test.
OnlineWeaponHold hold_of(const Model &model) {
    auto hold = online_weapon_hold(model.vertices, model.faces, identity);
    REQUIRE(hold.has_value());
    return hold.value_or(OnlineWeaponHold{});
}

std::array<SVECTOR, 3> followed(const OnlineWeaponHold &hold, const std::vector<SVECTOR> &vertices) {
    return {vertices[hold.followed[0]], vertices[hold.followed[1]], vertices[hold.followed[2]]};
}

void check_rotation(const OnlineRotation &a, const OnlineRotation &b) {
    for (std::size_t r = 0; r < 3; ++r) {
        CHECK(a[r].x == doctest::Approx(b[r].x).epsilon(0.01));
        CHECK(a[r].y == doctest::Approx(b[r].y).epsilon(0.01));
        CHECK(a[r].z == doctest::Approx(b[r].z).epsilon(0.01));
    }
}

}

TEST_CASE("the forearm is the piece reaching the elbow; the gauntlet and blade are held") {
    const auto model = sword();
    const auto hold = hold_of(model);
    CHECK(hold.forearm.size() == 8);
    for (const u16 v : hold.forearm)
        CHECK(v < 8);
    for (const u16 v : hold.followed)
        CHECK(v >= 8);
    // The wrist is at the forearm's far end.
    CHECK(hold.wrist.vz == doctest::Approx(-1200).epsilon(0.01));
}

TEST_CASE("at rest the blade points ahead and up") {
    const auto model = sword();
    const auto hold = hold_of(model);
    const auto arm = online_weapon_arm_at_rest(hold, identity, identity);
    const auto blade = apply(arm.held, {0, 0, -1});
    const float n = std::hypot(0.9f, 1.0f);
    CHECK(blade.x == doctest::Approx(0.0f).epsilon(0.02));
    CHECK(blade.y == doctest::Approx(-0.9f / n).epsilon(0.02));
    CHECK(blade.z == doctest::Approx(-1.0f / n).epsilon(0.02));
}

TEST_CASE("through a swing the held piece turns as the clip turns it, the forearm toward the wrist") {
    const auto model = sword();
    const auto hold = hold_of(model);
    const auto rest = online_weapon_arm_at_rest(hold, identity, identity);

    const auto at_hit = followed(hold, model.vertices);
    const auto still = online_weapon_arm_swinging(hold, rest, at_hit, 1.0f);
    check_rotation(still.held, identity);
    // The wrist is straight ahead of the elbow: the forearm points along -z.
    const auto ahead = apply(still.forearm, {0, 1, 0});
    CHECK(ahead.z == doctest::Approx(-1.0f).epsilon(0.01));

    const auto quarter = about_y(std::numbers::pi_v<float> / 2);
    std::vector<SVECTOR> moved;
    moved.reserve(model.vertices.size());
    for (const auto &v : model.vertices)
        moved.push_back(turned(quarter, v));
    const auto swung = online_weapon_arm_swinging(hold, rest, followed(hold, moved), 1.0f);
    check_rotation(swung.held, quarter);
}

TEST_CASE("an attack starts and ends at rest") {
    CHECK(online_attack_weight(-1) == 0.0f);
    CHECK(online_attack_weight(0) == 0.0f);
    CHECK(online_attack_weight(300) == doctest::Approx(0.5f));
    CHECK(online_attack_weight(2000) == 1.0f);
    CHECK(online_attack_weight(4096 - 300) == doctest::Approx(0.5f));

    const auto model = sword();
    const auto hold = hold_of(model);
    const auto rest = online_weapon_arm_at_rest(hold, identity, identity);
    const auto arm = online_weapon_arm_swinging(hold, rest, followed(hold, model.vertices), 0.0f);
    check_rotation(arm.held, rest.held);
    check_rotation(arm.forearm, rest.forearm);
    check_rotation(arm.upper_arm, rest.upper_arm);
}

TEST_CASE("a forearm raised up and back lifts the upper arm past overhead") {
    // The clip's wrist above and behind the elbow.
    Model model;
    model.box(-60, -1200, -60, 60, -10, 60);          // forearm straight up (-y)
    model.box(-80, -1400, -80, 80, -1210, 80);
    model.box(-10, -3000, -40, 10, -1410, 40);
    const auto hold = hold_of(model);
    const auto rest = online_weapon_arm_at_rest(hold, identity, identity);
    const auto arm = online_weapon_arm_swinging(hold, rest, followed(hold, model.vertices), 1.0f);
    // The upper arm takes 0.7 of the forearm's half turn: 126 degrees forward of hanging.
    const auto upper = apply(arm.upper_arm, {0, 1, 0});
    const float angle = std::atan2(-upper.z, upper.y);
    CHECK(angle == doctest::Approx(0.7f * std::numbers::pi_v<float>).epsilon(0.02));
}

TEST_CASE("a model that is not three pieces with a forearm, or names a missing vertex, has no hold") {
    Model forearm_only;
    forearm_only.box(-60, -60, -1200, 60, 60, -10);
    CHECK_FALSE(online_weapon_hold(forearm_only.vertices, forearm_only.faces, identity));

    auto model = sword();
    model.faces.push_back({{0, 1, static_cast<u16>(model.vertices.size()), 0}, 3});
    CHECK_FALSE(online_weapon_hold(model.vertices, model.faces, identity));
    CHECK_FALSE(online_weapon_hold({}, {}, identity));
}

TEST_CASE("rotations go to the game's matrices and back") {
    const auto r = about_y(0.4f);
    check_rotation(online_rotation(online_matrix(r)), r);
    check_rotation(online_compose(about_y(0.3f), about_y(0.1f)), r);
}
