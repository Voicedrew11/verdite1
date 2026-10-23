#ifndef KF_PLATFORM_LANGUAGE_RUNTIME_H
#define KF_PLATFORM_LANGUAGE_RUNTIME_H

#include <kf/lib/types.h>
#include <kf/platform/language.h>

#include <string>
#include <vector>

namespace kf {
// Each language's game files are an extracted folder; saves are shared.
bool language_resources_start(const char *data, Language language, const char *japanese_data);
void language_resources_stop();
// Queues a change; false (with a status message) when that language is missing.
bool language_request(Language language);
Language language_requested();
bool language_available(Language language);
// Call only where no menu/dialogue holds copies of the old language's labels.
bool language_apply_pending();
void host_language_status(const char *message);

// PC extensions (King's Field Enhanced).
// The player's choice (settings.json), used at start when both are available.
inline Language language_preference = Language::English;
// `library` is where new language folders are written ("<library>/ja" and
// "<library>/en"); existing ones there are picked up.
void languages_configure(const std::string &library);
// The language of an extracted folder, read from its GAME.EXE.
Language language_of_folder(const std::string &directory);
// A folder given explicitly (--data), as the language of its GAME.EXE.
Language languages_add_folder(const std::string &directory);
// Sets `wanted` as the language in use at start and returns its folder, or the
// other language when it is missing. Empty when no game files exist yet.
std::string languages_start(Language wanted);
std::string language_folder(Language language);

// Adding a language. These block; the tasks below run them in the background.
// Imports a disc image (Japanese, or with the English patch applied).
bool languages_add_disc(const char *image, Language *added, std::string *message);
// Builds English from the Japanese files and the translator's patch (a PPF), or
// with an empty patch from the translation embedded in the build, if any.
bool languages_make_english(const std::vector<u8> &ppf, std::string *message);

enum class LanguageTask : u8 { Idle, Working, Done, Failed };
void language_task_download_english();
void language_task_patch_file(const std::string &path);
void language_task_add_disc(const std::string &path);
LanguageTask language_task_state(std::string *message);
void language_task_clear();
}

#endif // KF_PLATFORM_LANGUAGE_RUNTIME_H
