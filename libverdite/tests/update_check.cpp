#include <verdite/update_check.h>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace verdite;

namespace {
constexpr std::int64_t day_s = std::int64_t{24} * 60 * 60;
const std::string release_020 = R"({"tag_name":"v0.2.0","html_url":"https://github.com/o/r/releases/tag/v0.2.0"})";

// A state file of its own, gone afterwards.
struct StateFile {
    std::filesystem::path path;
    explicit StateFile(const char *name) : path(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove(path);
    }
    ~StateFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

struct Run {
    const StateFile &file;
    int fetches = 0;
    std::optional<std::string> answer = release_020;

    std::optional<UpdateRelease> at(std::int64_t now_s, UpdatePace pace = UpdatePace::Daily) {
        return update_check_run({"0.1.0", file.path, now_s, pace, [this] {
                                     ++fetches;
                                     return answer;
                                 }});
    }
};
} // namespace

TEST_CASE("versions compare as numbers") {
    CHECK(version_is_newer("v0.2.0", "0.1.9"));
    CHECK(version_is_newer("v0.10.0", "0.9.0"));
    CHECK(version_is_newer("1.0.0", "0.99.99"));
    CHECK(version_is_newer("V1.2", "1.1.9"));
    CHECK_FALSE(version_is_newer("v0.1.0", "0.1.0"));
    CHECK_FALSE(version_is_newer("v0.1.0", "0.2.0"));
    CHECK(version_is_newer("v0.4.1-hotfix", "0.4.0"));
    CHECK_FALSE(version_is_newer("v0.4.0-hotfix", "0.4.0"));
}

TEST_CASE("an unreadable version is never newer") {
    CHECK_FALSE(version_is_newer("preview", "0.1.0"));
    CHECK_FALSE(version_is_newer("", "0.1.0"));
    CHECK_FALSE(version_is_newer("v", "0.1.0"));
    CHECK_FALSE(version_is_newer("v0.2.0", "dev"));
    CHECK_FALSE(version_is_newer("v99999999999999999999.0.0", "0.1.0"));
}

TEST_CASE("the release comes from GitHub's answer") {
    const auto release = update_release_from_json(release_020);
    REQUIRE(release);
    CHECK(release.value_or(UpdateRelease{}).tag == "v0.2.0");
    CHECK(release.value_or(UpdateRelease{}).page_url == "https://github.com/o/r/releases/tag/v0.2.0");
    CHECK_FALSE(update_release_from_json("not json"));
    CHECK_FALSE(update_release_from_json(""));
    CHECK_FALSE(update_release_from_json("[]"));
    CHECK_FALSE(update_release_from_json(R"({"tag_name":"v1.0.0"})"));
    CHECK_FALSE(update_release_from_json(R"({"tag_name":3,"html_url":"https://a"})"));
    // Only a web page may reach the system's opener.
    CHECK_FALSE(update_release_from_json(R"({"tag_name":"v1.0.0","html_url":"file:///etc/passwd"})"));
}

TEST_CASE("the network is asked at most once a day, and the answer is kept") {
    const StateFile file("verdite-update-daily.json");
    Run run{file};
    CHECK(run.at(100'000));
    CHECK(run.fetches == 1);
    CHECK(run.at(100'000 + day_s - 1));   // a second launch still hears about it
    CHECK(run.fetches == 1);
    CHECK(run.at(100'000 + day_s + 1));
    CHECK(run.fetches == 2);
    CHECK(run.at(100'000 + day_s + 2, UpdatePace::Now));
    CHECK(run.fetches == 3);
}

TEST_CASE("a clock that was wrong asks again") {
    const StateFile file("verdite-update-clock.json");
    Run run{file};
    run.at(100'000);
    run.at(50'000);
    CHECK(run.fetches == 2);
}

TEST_CASE("a failed fetch leaves the kept answer and asks again next time") {
    const StateFile file("verdite-update-offline.json");
    Run run{file};
    run.answer.reset();
    CHECK_FALSE(run.at(100'000));
    CHECK_FALSE(run.at(100'001));
    CHECK(run.fetches == 2);
    run.answer = release_020;
    CHECK(run.at(100'002));
    run.answer = "garbage";
    CHECK(run.at(100'002 + day_s + 1));
}

TEST_CASE("a release no newer than the running one is not announced") {
    const StateFile file("verdite-update-same.json");
    Run run{file};
    run.answer = R"({"tag_name":"v0.1.0","html_url":"https://github.com/o/r/releases/tag/v0.1.0"})";
    CHECK_FALSE(run.at(100'000));
}

TEST_CASE("a skipped release stays quiet until a later one") {
    const StateFile file("verdite-update-skip.json");
    Run run{file};
    REQUIRE(run.at(100'000));
    update_check_skip(file.path, "v0.2.0");
    CHECK_FALSE(run.at(100'001));
    CHECK_FALSE(run.at(100'002, UpdatePace::Now));
    run.answer = R"({"tag_name":"v0.3.0","html_url":"https://github.com/o/r/releases/tag/v0.3.0"})";
    const auto later = run.at(100'003, UpdatePace::Now);
    REQUIRE(later);
    CHECK(later.value_or(UpdateRelease{}).tag == "v0.3.0");
}

TEST_CASE("a damaged state file is only a lost cache") {
    const StateFile file("verdite-update-damaged.json");
    for (const char *damaged : {"{", "[1,2]", R"({"checked_s":"soon","tag":7,"url":[]})",
                                R"({"checked_s":9e18,"tag":"v9.0.0","url":"file:///etc/passwd"})"}) {
        std::ofstream(file.path, std::ios::binary) << damaged;
        Run run{file};
        run.answer.reset();
        CHECK_FALSE(run.at(100'000));
    }
}
