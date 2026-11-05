#include <verdite/update_check.h>

#include <cJSON.h>

#include <array>
#include <charconv>
#include <fstream>
#include <memory>
#include <sstream>

namespace verdite {
namespace {
constexpr std::int64_t check_interval_s = std::int64_t{60} * 60;
constexpr std::string_view secure_scheme = "https://";

using Numbers = std::array<unsigned, 3>;

// A missing part reads as 0 (v1.2 is 1.2.0); no number at all, or one too large, reads as nothing.
std::optional<Numbers> read_numbers(std::string_view text) {
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V'))
        text.remove_prefix(1);
    Numbers numbers{};
    const char *at = text.data();
    const char *const end = text.data() + text.size();
    for (std::size_t part = 0; part < numbers.size(); ++part) {
        const auto [next, error] = std::from_chars(at, end, numbers[part]);
        if (error != std::errc{}) {
            if (part == 0 || error == std::errc::result_out_of_range)
                return std::nullopt;
            return numbers;
        }
        at = next;
        if (at == end || *at != '.')
            return numbers;
        ++at;
    }
    return numbers;
}

struct DocumentFree {
    void operator()(cJSON *document) const { cJSON_Delete(document); }
};
using Document = std::unique_ptr<cJSON, DocumentFree>;

std::string text_of(const cJSON *object, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

bool is_secure(std::string_view url) { return url.starts_with(secure_scheme) && url.size() > secure_scheme.size(); }

// update.json. A cache anyone can edit, so a value of the wrong kind reads as absent.
struct UpdateState {
    std::int64_t checked_s = 0;
    std::string tag, page_url, skipped;
};

UpdateState load_state(const std::filesystem::path &file) {
    std::ifstream stream(file, std::ios::binary);
    std::stringstream text;
    text << stream.rdbuf();
    const std::string json = text.str();
    const Document document(cJSON_ParseWithLength(json.data(), json.size()));
    UpdateState state;
    if (!document)
        return state;
    const cJSON *checked = cJSON_GetObjectItemCaseSensitive(document.get(), "checked_s");
    if (cJSON_IsNumber(checked))
        state.checked_s = static_cast<std::int64_t>(checked->valuedouble);
    state.tag = text_of(document.get(), "tag");
    state.page_url = text_of(document.get(), "url");
    state.skipped = text_of(document.get(), "skipped");
    return state;
}

void save_state(const std::filesystem::path &file, const UpdateState &state) {
    const Document document(cJSON_CreateObject());
    if (!document)
        return;
    cJSON_AddNumberToObject(document.get(), "checked_s", static_cast<double>(state.checked_s));
    cJSON_AddStringToObject(document.get(), "tag", state.tag.c_str());
    cJSON_AddStringToObject(document.get(), "url", state.page_url.c_str());
    cJSON_AddStringToObject(document.get(), "skipped", state.skipped.c_str());
    const std::unique_ptr<char, decltype(&cJSON_free)> json(cJSON_Print(document.get()), cJSON_free);
    if (!json)
        return;
    std::ofstream(file, std::ios::binary) << json.get();
}

bool network_due(const UpdateCheckRequest &request, const UpdateState &state) {
    if (request.pace == UpdatePace::Now)
        return true;
    const std::int64_t age_s = request.now_s - state.checked_s;
    // A time in the future is a clock that was wrong: ask again.
    return age_s < 0 || age_s > check_interval_s;
}
} // namespace

bool version_is_newer(std::string_view tag, std::string_view running) {
    const auto latest = read_numbers(tag);
    const auto current = read_numbers(running);
    return latest && current && *latest > *current;
}

std::optional<UpdateRelease> update_release_from_json(std::string_view answer) {
    const Document document(cJSON_ParseWithLength(answer.data(), answer.size()));
    if (!document)
        return std::nullopt;
    UpdateRelease release{text_of(document.get(), "tag_name"), text_of(document.get(), "html_url")};
    if (release.tag.empty() || !is_secure(release.page_url))
        return std::nullopt;
    return release;
}

std::optional<UpdateRelease> update_check_run(const UpdateCheckRequest &request) {
    UpdateState state = load_state(request.state_file);
    if (network_due(request, state)) {
        if (const auto answer = request.fetch_latest()) {
            if (const auto release = update_release_from_json(*answer)) {
                state.checked_s = request.now_s;
                state.tag = release->tag;
                state.page_url = release->page_url;
                save_state(request.state_file, state);
            }
        }
    }
    if (state.tag.empty() || !is_secure(state.page_url) || state.tag == state.skipped ||
        !version_is_newer(state.tag, request.running_version))
        return std::nullopt;
    return UpdateRelease{state.tag, state.page_url};
}

void update_check_skip(const std::filesystem::path &state_file, std::string_view tag) {
    UpdateState state = load_state(state_file);
    state.skipped = tag;
    save_state(state_file, state);
}

} // namespace verdite
