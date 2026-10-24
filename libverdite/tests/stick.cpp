#include <verdite/stick.h>

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

using namespace verdite;

TEST_CASE("a stick inside its deadzone is centred") {
    const StickAxes shaped = shape_stick({0.1f, -0.05f}, {0.15f, 1.0f});
    CHECK(shaped.x == 0.0f);
    CHECK(shaped.y == 0.0f);
    CHECK(shape_axis(-0.14f, {0.15f, 1.0f}) == 0.0f);
}

TEST_CASE("full deflection is 1 and the direction is kept") {
    const StickAxes shaped = shape_stick({0.6f, 0.8f}, {0.15f, 1.35f});
    CHECK(std::hypot(shaped.x, shaped.y) == doctest::Approx(1.0f));
    CHECK(shaped.x / shaped.y == doctest::Approx(0.75f));
    CHECK(shape_axis(-1.0f, {0.3f, 2.0f}) == doctest::Approx(-1.0f));
}

TEST_CASE("the deadzone's edge is 0 and the curve bends what is past it") {
    // Halfway from the edge to full: 0.5 linear, 0.25 squared.
    CHECK(shape_axis(0.575f, {0.15f, 1.0f}) == doctest::Approx(0.5f));
    CHECK(shape_axis(0.575f, {0.15f, 2.0f}) == doctest::Approx(0.25f));
    CHECK(shape_axis(-0.575f, {0.15f, 2.0f}) == doctest::Approx(-0.25f));
}

TEST_CASE("a stick past full deflection is held at 1") {
    const StickAxes shaped = shape_stick({1.0f, 1.0f}, {0.15f, 1.0f});
    CHECK(std::hypot(shaped.x, shaped.y) == doctest::Approx(1.0f));
}

TEST_CASE("a non-finite axis reads as centred") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const StickAxes shaped = shape_stick({nan, 0.5f}, {});
    CHECK(shaped.x == 0.0f);
    CHECK(shaped.y == 0.0f);
}

TEST_CASE("the look ramp rises while held past the threshold and falls three times as fast") {
    LookAcceleration ramp;
    const LookAccelerationShape shape{3.0f, 0.5f, 0.8f};
    CHECK(ramp.update(1.0f, 0.0f, shape) == doctest::Approx(1.0f));
    CHECK(ramp.update(1.0f, 0.05f, shape) == doctest::Approx(1.2f));
    for (int i = 0; i < 10; ++i)
        ramp.update(1.0f, 0.05f, shape);
    CHECK(ramp.update(1.0f, 0.05f, shape) == doctest::Approx(3.0f));
    CHECK(ramp.update(0.5f, 0.05f, shape) == doctest::Approx(2.4f));
    ramp.reset();
    CHECK(ramp.update(0.5f, 0.05f, shape) == doctest::Approx(1.0f));
}

TEST_CASE("a stall does not jump the look ramp") {
    LookAcceleration ramp;
    CHECK(ramp.update(1.0f, 10.0f, {3.0f, 0.5f, 0.8f}) == doctest::Approx(1.4f));
}

TEST_CASE("a fractional step is carried, not lost") {
    StepCarry carry;
    int total = 0;
    for (int i = 0; i < 10; ++i)
        total += carry.take(8.6f, 28);
    CHECK(total == 86);
    carry.reset();
    CHECK(carry.take(-0.6f, 28) == 0);
    CHECK(carry.take(-0.6f, 28) == -1);
}

TEST_CASE("a step is held at the limit and the excess is not saved up") {
    StepCarry carry;
    CHECK(carry.take(100.0f, 28) == 28);
    CHECK(carry.take(0.0f, 28) == 0);
}
