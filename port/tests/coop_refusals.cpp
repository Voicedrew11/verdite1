#include "../ext/coop_refusals.h"

#include <doctest/doctest.h>

using namespace kf::ext;

TEST_CASE("every refusal is counted, and a kind is logged at most once a second") {
    coop_refused_forget();
    CHECK(coop_refused(CoopRefusal::Frame, 7, 5000));
    CHECK_FALSE(coop_refused(CoopRefusal::Frame, 7, 5999));
    CHECK(coop_refused_count(CoopRefusal::Frame) == 2);
    CHECK(coop_refused(CoopRefusal::Frame, 7, 6000));
    CHECK(coop_refused_count(CoopRefusal::Frame) == 3);
}

TEST_CASE("each kind of refusal is logged and counted on its own") {
    coop_refused_forget();
    CHECK(coop_refused(CoopRefusal::Frame, 7, 5000));
    CHECK(coop_refused(CoopRefusal::Character, 7, 5000));
    CHECK(coop_refused_count(CoopRefusal::Frame) == 1);
    CHECK(coop_refused_count(CoopRefusal::Pose) == 0);
}

TEST_CASE("the first refusal is logged whatever the clock says") {
    coop_refused_forget();
    CHECK(coop_refused(CoopRefusal::Pose, 7, 0));
}

TEST_CASE("the counts read as JSON and start again in a new room") {
    coop_refused_forget();
    CHECK(coop_refused_json() ==
        "\"refused\":{\"message\":0,\"frame\":0,\"pose\":0,\"floorWrites\":0,\"floorState\":0,\"character\":0}");
    coop_refused(CoopRefusal::FloorState, 7, 5000);
    CHECK(coop_refused_json().find("\"floorState\":1") != std::string::npos);
    coop_refused_forget();
    CHECK(coop_refused_count(CoopRefusal::FloorState) == 0);
}
