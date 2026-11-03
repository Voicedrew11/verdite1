// The mods a save was made with (verdite/mod_saved.h). The text is read from a file the player can
// edit, so decoding checks every shape; the cJSON nodes are only walked, never indexed.
#include <verdite/mod_saved.h>

#include <cJSON.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

namespace verdite {
namespace {

struct JsonDelete {
    void operator()(cJSON *json) const { cJSON_Delete(json); }
};
struct JsonTextFree {
    void operator()(char *text) const { cJSON_free(text); }
};
using Json = std::unique_ptr<cJSON, JsonDelete>;

// The string under key, or fallback when it is missing, not a string, or empty.
std::string string_or(const cJSON *object, const char *key, std::string fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring && *item->valuestring)
        return item->valuestring;
    return fallback;
}

// cJSON owns item once it is added; when the add fails, item is freed here. A null item fails.
bool add_member(cJSON *object, const char *name, Json item) {
    cJSON *raw = item.release();
    if (cJSON_AddItemToObject(object, name, raw))
        return true;
    cJSON_Delete(raw);
    return false;
}

bool add_element(cJSON *array, Json item) {
    cJSON *raw = item.release();
    if (cJSON_AddItemToArray(array, raw))
        return true;
    cJSON_Delete(raw);
    return false;
}

bool add_string(cJSON *object, const char *name, const std::string &text) {
    return add_member(object, name, Json(cJSON_CreateString(text.c_str())));
}

Json encode_mod(const SavedMod &mod) {
    Json object(cJSON_CreateObject());
    if (!object || !add_string(object.get(), "id", mod.id) || !add_string(object.get(), "name", mod.name) ||
        !add_string(object.get(), "version", mod.version))
        return nullptr;
    return object;
}

// The mod an entry of "mods" holds, or nothing if it is not an object with a non-empty id.
std::optional<SavedMod> decode_mod(const cJSON *item) {
    if (!cJSON_IsObject(item))
        return std::nullopt;
    SavedMod mod;
    mod.id = string_or(item, "id", "");
    if (mod.id.empty())
        return std::nullopt;
    mod.name = string_or(item, "name", mod.id);
    mod.version = string_or(item, "version", "");
    return mod;
}

bool listed(std::span<const SavedMod> mods, std::string_view id) {
    return std::ranges::any_of(mods, [&](const SavedMod &mod) { return mod.id == id; });
}

const SavedMod *find_by_id(std::span<const SavedMod> mods, std::string_view id) {
    const auto found = std::ranges::find(mods, id, &SavedMod::id);
    return found == mods.end() ? nullptr : &*found;
}

// Walks the entries of a save's "mods" array: at most saved_mods_most, each a mod with a distinct id.
std::optional<std::vector<SavedMod>> decode_list(const cJSON *list) {
    std::vector<SavedMod> mods;
    for (const cJSON *item = list->child; item; item = item->next) {
        if (mods.size() == saved_mods_most)
            return std::nullopt;
        std::optional<SavedMod> mod = decode_mod(item);
        if (!mod || listed(mods, mod->id))
            return std::nullopt;
        mods.push_back(std::move(*mod));
    }
    return mods;
}

} // namespace

std::string saved_mods_encode(std::span<const SavedMod> mods) {
    Json root(cJSON_CreateObject());
    Json list(cJSON_CreateArray());
    if (!root || !list)
        return {};
    for (const SavedMod &mod : mods) {
        if (!add_element(list.get(), encode_mod(mod)))
            return {};
    }
    if (!add_member(root.get(), "mods", std::move(list)))
        return {};
    const std::unique_ptr<char, JsonTextFree> text(cJSON_PrintUnformatted(root.get()));
    return text ? std::string(text.get()) : std::string();
}

std::optional<std::vector<SavedMod>> saved_mods_decode(std::string_view json) {
    const Json root(cJSON_ParseWithLength(json.data(), json.size()));
    if (!cJSON_IsObject(root.get()))
        return std::nullopt;
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root.get(), "mods");
    if (!cJSON_IsArray(list))
        return std::nullopt;
    return decode_list(list);
}

std::vector<SavedModDifference> saved_mods_compare(std::span<const SavedMod> in_save, std::span<const SavedMod> running) {
    std::vector<SavedModDifference> differences;
    for (const SavedMod &saved : in_save) {
        const SavedMod *now = find_by_id(running, saved.id);
        if (!now)
            differences.push_back(SavedModDifference{saved, SavedModChange::Missing});
        else if (now->version != saved.version)
            differences.push_back(SavedModDifference{*now, SavedModChange::OtherVersion});
    }
    for (const SavedMod &mod : running) {
        if (!find_by_id(in_save, mod.id))
            differences.push_back(SavedModDifference{mod, SavedModChange::Added});
    }
    std::stable_sort(differences.begin(), differences.end(), [](const SavedModDifference &a, const SavedModDifference &b) {
        if (a.change != b.change)
            return a.change < b.change;
        return a.mod.name < b.mod.name;
    });
    return differences;
}

} // namespace verdite
