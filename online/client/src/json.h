#pragma once
// cJSON objects read as the protocol wants them: a field is there with the right type, or nothing.
// Internal to the client; the frames' parsers (signaling.cpp, floor_frames.cpp) check each field
// once.

#include <cJSON.h>

#include <cmath>
#include <memory>
#include <optional>
#include <string>

namespace vo::json {

struct Delete {
    void operator()(cJSON *json) const {
        cJSON_Delete(json);
    }
};
using Object = std::unique_ptr<cJSON, Delete>;

// The field as a string, or nothing when it is missing or not a string.
inline std::optional<std::string> string_field(const cJSON *object, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring == nullptr)
        return std::nullopt;
    return std::string(item->valuestring);
}

// A whole number in [low, high], or nothing.
inline std::optional<int> int_field(const cJSON *object, const char *name, int low, int high) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item))
        return std::nullopt;
    const double value = item->valuedouble;
    if (value < low || value > high || std::floor(value) != value)
        return std::nullopt;
    return static_cast<int>(value);
}

} // namespace vo::json
