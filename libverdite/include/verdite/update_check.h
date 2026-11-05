#ifndef VERDITE_UPDATE_CHECK_H
#define VERDITE_UPDATE_CHECK_H
// Whether a newer release of the game exists: notify only, nothing is downloaded. The answer of the
// network is kept in a small state file, so it is asked at most once an hour and a player who launches
// twice still hears about the release; a player can skip one release, and a later one is announced
// again.
//
// Game-agnostic: the game hands in how to fetch the answer and where to keep the file, and decides
// when to run a check and how to show what it finds.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace verdite {

struct UpdateRelease {
    std::string tag;        // as the repository names it: v0.2.0
    std::string page_url;   // the release's page, always https
};

// Compares MAJOR.MINOR.PATCH, with an optional leading v. A suffix after the numbers is ignored, and
// neither side unreadable is newer.
bool version_is_newer(std::string_view tag, std::string_view running);

// The release in the answer of GitHub's /releases/latest, if it names a tag and an https page.
std::optional<UpdateRelease> update_release_from_json(std::string_view answer);

enum class UpdatePace : std::uint8_t {
    Hourly,  // the network at most once an hour, the kept answer otherwise
    Now,     // the network whatever the time
};

struct UpdateCheckRequest {
    std::string_view running_version;
    std::filesystem::path state_file;
    std::int64_t now_s = 0;   // seconds since the epoch
    UpdatePace pace = UpdatePace::Hourly;
    // The text of /releases/latest; nullopt when offline, rate-limited or the service is down, and
    // the kept answer then stands.
    std::function<std::optional<std::string>()> fetch_latest;
};

// One whole check, blocking on the fetch: the release to announce, if there is one newer than the
// running version that was not skipped. Failing to read or write the state file costs only the kept
// answer. Not to be run at the same time as update_check_skip on the same file.
std::optional<UpdateRelease> update_check_run(const UpdateCheckRequest &request);

// Stops announcing this tag; a later one is announced again.
void update_check_skip(const std::filesystem::path &state_file, std::string_view tag);

} // namespace verdite

#endif // VERDITE_UPDATE_CHECK_H
