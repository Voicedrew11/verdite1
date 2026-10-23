#include <kf/platform/language.h>

#include <cstring>

namespace kf {
static Language selected_language = Language::Japanese;

bool language_parse(const char *code, Language *language) {
    if (std::strcmp(code, "ja") == 0)
        *language = Language::Japanese;
    else if (std::strcmp(code, "en") == 0)
        *language = Language::English;
    else
        return false;
    return true;
}
const char *language_code(Language language) {
    return language == Language::English ? "en" : "ja";
}
const char *language_name(Language language) {
    return language == Language::English ? "English v1.0" : "Japanese";
}
Language game_language() { return selected_language; }
void game_set_language(Language language) { selected_language = language; }
}
