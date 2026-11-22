#include <verdite/mod_archive.h>

#include <cJSON.h>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>

namespace verdite {
namespace {

constexpr std::size_t path_most = 1024;
constexpr std::size_t part_most = 255;
constexpr std::size_t folder_most = 64;

struct JsonFree {
    void operator()(cJSON *json) const { cJSON_Delete(json); }
};

std::vector<std::string_view> split_path(std::string_view path) {
    std::vector<std::string_view> parts;
    std::size_t from = 0;
    for (;;) {
        const std::size_t slash = path.find('/', from);
        parts.push_back(path.substr(from, slash == std::string_view::npos ? std::string_view::npos : slash - from));
        if (slash == std::string_view::npos)
            return parts;
        from = slash + 1;
    }
}

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool same_ignoring_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

// Names Windows will not make a file of, whatever follows the dot.
bool reserved_on_windows(std::string_view part) {
    const std::string_view stem = part.substr(0, part.find('.'));
    constexpr std::array<std::string_view, 4> devices{"con", "prn", "aux", "nul"};
    if (std::ranges::any_of(devices, [&](std::string_view device) { return same_ignoring_case(stem, device); }))
        return true;
    return stem.size() == 4 && (same_ignoring_case(stem.substr(0, 3), "com") || same_ignoring_case(stem.substr(0, 3), "lpt")) &&
           stem[3] >= '1' && stem[3] <= '9';
}

// A part every platform's file system takes as a plain name inside the folder.
bool safe_part(std::string_view part) {
    if (part.empty() || part.size() > part_most || part == "." || part == ".." || reserved_on_windows(part))
        return false;
    if (part.back() == '.' || part.back() == ' ')
        return false;
    return std::ranges::none_of(part, [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte < 0x20 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
    });
}

bool safe_path(std::string_view path) {
    return !path.empty() && path.size() <= path_most && std::ranges::all_of(split_path(path), safe_part);
}

// A folder name the mod host and every file system take: no dot first, so it is no hidden folder.
bool safe_folder(std::string_view name) {
    if (name.empty() || name.size() > folder_most || name.front() == '.' || !safe_part(name))
        return false;
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}

// What a Mac's archiver adds beside the files.
bool mac_litter(std::string_view path) {
    const auto parts = split_path(path);
    return parts.front() == "__MACOSX" || parts.back() == ".DS_Store" || parts.back().starts_with("._");
}

bool marks_a_mod(std::string_view name) {
    return name == "mod.json" || name == "main.lua";
}

// The folder holding the mod's mod.json or main.lua, "" for the top or "x/" for one folder down;
// nothing, with the refusal set, when there is none or more than one.
std::optional<std::string> mod_root(const std::vector<ModArchiveEntry> &entries, std::string &refusal) {
    std::vector<std::string> roots;
    for (const ModArchiveEntry &entry : entries) {
        const auto parts = split_path(entry.path);
        if (parts.size() > 2 || !marks_a_mod(parts.back()))
            continue;
        std::string root = parts.size() == 2 ? std::string(parts.front()) + '/' : std::string();
        if (std::ranges::find(roots, root) == roots.end())
            roots.push_back(std::move(root));
    }
    if (std::ranges::find(roots, std::string()) != roots.end())
        return std::string();
    if (roots.size() == 1)
        return roots.front();
    refusal = roots.empty() ? "That archive holds no mod: there is no mod.json or main.lua in it."
                            : "That archive holds more than one mod. Install them one at a time.";
    return std::nullopt;
}

// mod.json's string field, or empty when there is no such file, no such field, or it is not JSON.
std::string manifest_field(const std::vector<ModInstallFile> &files, const char *key) {
    const auto manifest = std::ranges::find(files, std::string("mod.json"), &ModInstallFile::path);
    if (manifest == files.end())
        return {};
    const std::unique_ptr<cJSON, JsonFree> json(
        cJSON_ParseWithLength(reinterpret_cast<const char *>(manifest->bytes.data()), manifest->bytes.size()));
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json.get(), key);
    return cJSON_IsString(item) ? std::string(item->valuestring) : std::string();
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    c = lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// Android's picker names a file as content://.../primary%3ADownload%2Fmod.zip.
std::string percent_decoded(std::string_view text) {
    std::string decoded;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const int high = i + 2 < text.size() && text[i] == '%' ? hex_digit(text[i + 1]) : -1;
        const int low = high >= 0 ? hex_digit(text[i + 2]) : -1;
        if (low < 0) {
            decoded += text[i];
            continue;
        }
        decoded += static_cast<char>(high * 16 + low);
        i += 2;
    }
    return decoded;
}

std::string archive_stem(std::string_view archive_name) {
    const std::string decoded = percent_decoded(archive_name);
    const std::size_t slash = decoded.find_last_of("/\\:");
    std::string_view name = slash == std::string::npos ? std::string_view(decoded) : std::string_view(decoded).substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    return std::string(dot == std::string_view::npos || dot == 0 ? name : name.substr(0, dot));
}

std::string choose_folder(const std::vector<ModInstallFile> &files, std::string_view root, std::string_view archive_name) {
    std::array<std::string, 3> candidates{manifest_field(files, "id"), std::string(root.substr(0, root.empty() ? 0 : root.size() - 1)),
                                          archive_stem(archive_name)};
    for (std::string &candidate : candidates)
        if (safe_folder(candidate))
            return std::move(candidate);
    return {};
}

ModInstallPlan refused(std::string reason) {
    ModInstallPlan plan;
    plan.refusal = std::move(reason);
    return plan;
}

} // namespace

ModInstallPlan mod_install_plan(std::vector<ModArchiveEntry> entries, std::string_view archive_name) {
    std::erase_if(entries, [](const ModArchiveEntry &entry) { return entry.path.empty() || mac_litter(entry.path); });
    if (entries.size() > mod_archive_files_most)
        return refused("That archive holds too many files to be a mod.");
    std::string refusal;
    const auto root = mod_root(entries, refusal);
    if (!root)
        return refused(refusal);
    ModInstallPlan plan;
    std::size_t total = 0;
    for (ModArchiveEntry &entry : entries) {
        if (!entry.path.starts_with(*root))
            continue;
        std::string path = entry.path.substr(root->size());
        if (!safe_path(path))
            return refused("That archive names a file the game will not write: " + entry.path);
        total += entry.bytes.size();
        if (total > mod_archive_bytes_most)
            return refused("That archive is too large to be a mod.");
        const bool repeated = std::ranges::any_of(plan.files, [&](const ModInstallFile &file) { return same_ignoring_case(file.path, path); });
        if (repeated)
            return refused("That archive holds two files named " + path + ".");
        plan.files.push_back({std::move(path), std::move(entry.bytes)});
    }
    plan.folder = choose_folder(plan.files, *root, archive_name);
    if (plan.folder.empty())
        return refused("That mod has no name the game can use as a folder: give mod.json an \"id\" of letters, digits, - and _.");
    plan.name = manifest_field(plan.files, "name");
    if (plan.name.empty())
        plan.name = plan.folder;
    return plan;
}

} // namespace verdite
