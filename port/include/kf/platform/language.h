#ifndef KF_PLATFORM_LANGUAGE_H
#define KF_PLATFORM_LANGUAGE_H

#include <kf/lib/types.h>

namespace kf {
enum class Language : u8 { Japanese, English };
bool language_parse(const char *code, Language *language);
const char *language_code(Language language);
const char *language_name(Language language);
Language game_language();
void game_set_language(Language language);
}

#endif // KF_PLATFORM_LANGUAGE_H
