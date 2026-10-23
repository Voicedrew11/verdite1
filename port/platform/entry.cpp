#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <kf/platform/host.h>
#include <kf/platform/input.h>
#include <kf/platform/files.h>
#include <kf/platform/disc.h>
#include <kf/platform/language_runtime.h>
#include <kf/platform/translation_ppf.h>
#include <kf/platform/saves.h>
#if KF_EXT
#include <kf/ext/ext.h>
#endif
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#ifdef __EMSCRIPTEN__
#include <kf/platform/assets.h>

#include <emscripten.h>

extern "C" EMSCRIPTEN_KEEPALIVE int kf_extract_disc(const char *source, const char *destination,
                                                  const char *code) {
    kf::Language language;
    return kf::language_parse(code, &language) && kf::disc_extract(source, destination, language);
}
extern "C" EMSCRIPTEN_KEEPALIVE int kf_prepare_resources(const char *source, const char *destination,
                                                       const char *code) {
    kf::Language language;
    return kf::language_parse(code, &language) && kf::disc_prepare_directory(source, destination, language);
}
extern "C" EMSCRIPTEN_KEEPALIVE const char *kf_resource_files_hash(const char *code) {
    kf::Language language;
    return kf::language_parse(code, &language) ? kf::assets_language_hash(language) : "";
}
extern "C" EMSCRIPTEN_KEEPALIVE int kf_request_language(const char *code) {
    kf::Language language;
    return kf::language_parse(code, &language) && kf::language_request(language);
}
extern "C" EMSCRIPTEN_KEEPALIVE const char *kf_current_language() {
    return kf::language_code(kf::game_language());
}
EM_JS(void, browser_game_started, (), { Module['gameStarted'](); });
#endif

#include <kf/cutscene/playback.h>
#include <kf/game/session.h>

namespace {
std::filesystem::path native_path(const char *path) {
    return std::filesystem::path(reinterpret_cast<const char8_t *>(path));
}

std::string utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path executable_directory() {
    const char *base = SDL_GetBasePath();
    return base ? native_path(base) : std::filesystem::path(".");
}

std::string lower_extension(const std::filesystem::path &path) {
    auto extension = path.extension().u8string();
    for (auto &c : extension)
        c = static_cast<char8_t>(std::tolower(static_cast<unsigned char>(c)));
    return std::string(extension.begin(), extension.end());
}

bool is_disc_image(const std::filesystem::path &path) {
    const auto extension = lower_extension(path);
    return extension == ".cue" || extension == ".bin" || extension == ".iso";
}

// The translator's patch, or an archive holding it.
bool is_patch_file(const std::filesystem::path &path) {
    const auto extension = lower_extension(path);
    return extension == ".ppf" || extension == ".rar" || extension == ".zip" || extension == ".7z";
}

void show_error(const char *text) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "King's Field Enhanced", text, nullptr);
}

// Returns the index of the button pressed, or -1 when the box was closed.
int ask(const char *text, std::initializer_list<const char *> labels) {
    std::vector<SDL_MessageBoxButtonData> buttons;
    int id = 0;
    for (const char *label : labels)
        buttons.push_back({id == 0 ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0u, id++, label});
    const SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION, nullptr, "King's Field Enhanced", text,
                                 static_cast<int>(buttons.size()), buttons.data(), nullptr};
    int pressed = -1;
    return SDL_ShowMessageBox(&box, &pressed) ? pressed : -1;
}

std::string choose_file(const char *kind, const char *extensions) {
    SDL_InitSubSystem(SDL_INIT_VIDEO);
    struct Pick { std::string path; bool done = false; } pick;
    const SDL_DialogFileFilter filters[] = {{kind, extensions}};
    SDL_ShowOpenFileDialog([](void *data, const char *const *files, int) {
        auto *result = static_cast<Pick *>(data);
        if (files && files[0])
            result->path = files[0];
        result->done = true;
    }, &pick, nullptr, filters, 1, nullptr, false);
    while (!pick.done) {
        SDL_PumpEvents();
        SDL_Delay(20);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return pick.path;
}

// Builds English from the Japanese files: `source` is a patch file, "download",
// or empty to ask. Returns true when English is ready.
bool add_english(const std::string &source) {
    std::vector<u8> ppf;
    std::string message;
    bool ok;
    if (source == "download") {
        std::printf("Downloading the English patch from %s\n", kf::english_patch_url);
        ok = kf::translation_download(&ppf, &message);
    } else {
        ok = kf::translation_read_patch(source.c_str(), &ppf, &message);
    }
    ok = ok && kf::languages_make_english(ppf, &message);
    std::printf("%s\n", message.c_str());
    return ok;
}

void offer_english() {
    for (;;) {
        const int choice = ask(
            "This is the original Japanese disc. King's Field Enhanced can add John Osborne's English "
            "fan translation (v1.0) to it, keeping Japanese too.\n\n"
            "Download the translator's patch from the Internet Archive (about 6 MB), or choose the patch "
            "file (.rar or .ppf) if you have it. You can also do this later in F1 > Settings > Language.",
            {"Download", "Choose file...", "Play in Japanese"});
        std::string source;
        if (choice == 0)
            source = "download";
        else if (choice == 1)
            source = choose_file("English patch", "rar;ppf;zip;7z");
        else
            return;
        if (source.empty())
            continue;
        std::vector<u8> ppf;
        std::string message;
        const bool ok = (source == "download" ? kf::translation_download(&ppf, &message)
                                              : kf::translation_read_patch(source.c_str(), &ppf, &message)) &&
            kf::languages_make_english(ppf, &message);
        if (ok)
            return;
        show_error(message.c_str());
    }
}

#if defined(_WIN32)
// Release layout on Windows: game files are copied out of the player's disc
// image once, into "data/ja" and "data/en" beside the executable.
std::string disc_beside(const std::filesystem::path &directory) {
    std::error_code error;
    std::string found;
    for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
        if (!entry.is_regular_file(error) || !is_disc_image(entry.path()))
            continue;
        if (lower_extension(entry.path()) == ".cue")
            return utf8(entry.path());
        if (found.empty())
            found = utf8(entry.path());
    }
    return found;
}

std::string ask_for_disc() {
    SDL_InitSubSystem(SDL_INIT_VIDEO);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "King's Field Enhanced",
        "Choose your King's Field (Japan) disc image: the original, or with the English patch applied "
        "(.cue, .bin or .iso).\n\nThe game files are copied out of it once, into the \"data\" folder "
        "beside the game. The image itself is not changed.", nullptr);
    return choose_file("Disc images", "cue;bin;iso");
}

// Earlier versions kept one language directly in "data".
void move_single_language_data(const std::filesystem::path &library) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(library / "GAME.EXE", error))
        return;
    const auto language = kf::language_of_folder(utf8(library));
    auto moving = library;
    moving += ".moving";
    std::filesystem::rename(library, moving, error);
    if (!error)
        std::filesystem::create_directory(library, error);
    if (!error)
        std::filesystem::rename(moving, library / kf::language_code(language), error);
    if (error)
        std::printf("Cannot move the game files into data/%s: %s\n", kf::language_code(language), error.message().c_str());
}

// Finds or creates the game files. Returns false when the player cancelled or
// the disc could not be used (already reported).
bool prepare_release_data(const char *dropped) {
    const auto library = executable_directory() / "data";
    move_single_language_data(library);
    kf::languages_configure(utf8(library));
    std::error_code error;
    for (const char *suffix : {".partial", ".moving"}) {
        auto stale = library / "ja";
        stale += suffix;
        std::filesystem::remove_all(stale, error);
        stale = library / "en";
        stale += suffix;
        std::filesystem::remove_all(stale, error);
    }
    const bool any = kf::language_available(kf::Language::Japanese) || kf::language_available(kf::Language::English);
    // A patch dropped onto the game adds English to the Japanese files.
    if (dropped && is_patch_file(native_path(dropped))) {
        if (!add_english(dropped))
            show_error("That patch could not be applied. Details are in kf.log in %APPDATA%\\KingsField\\SLPS00017.");
        dropped = nullptr;
    }
    std::string disc = dropped ? dropped : "";
    if (!any && disc.empty())
        disc = disc_beside(executable_directory());
    if (!any && disc.empty())
        disc = ask_for_disc();
    if (!any && disc.empty())
        return false;
    if (!disc.empty()) {
        std::printf("Copying game files from %s\n", disc.c_str());
        kf::Language added;
        std::string message;
        if (!kf::languages_add_disc(disc.c_str(), &added, &message)) {
            std::printf("%s\n", message.c_str());
            if (!any) {
                show_error("That disc image could not be read as King's Field (Japan, SLPS-00017).\n\n"
                           "Use the Japanese disc, original or with the English patch applied, as a .cue/.bin "
                           "pair or an .iso. Details are in kf.log in %APPDATA%\\KingsField\\SLPS00017.");
                return false;
            }
            show_error(message.c_str());
        } else if (added == kf::Language::Japanese && !kf::language_available(kf::Language::English)) {
            offer_english();
        }
    }
    return true;
}
#endif
}

int main(int argc, char **argv) {
#if KF_EXT
    kf::ext::console_start();
#endif
    const char *data = nullptr;
    const char *saves = nullptr;
    const char *disc = nullptr;
    const char *extracted = nullptr;
    const char *english_patch = nullptr;
    std::vector<const char *> language_data;
    bool extract_only = false;
    bool skip_intro = false;
    bool language_selected = false;
    kf::Language language = kf::Language::English;
    const char *dropped = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc)
            data = argv[++i];
        else if ((std::strcmp(argv[i], "--data-ja") == 0 || std::strcmp(argv[i], "--data-en") == 0 ||
                  std::strcmp(argv[i], "--japanese-data") == 0) && i + 1 < argc)
            language_data.push_back(argv[++i]);
        else if (std::strcmp(argv[i], "--language") == 0 && i + 1 < argc && kf::language_parse(argv[i + 1], &language)) {
            language_selected = true;
            ++i;
        }
        else if (std::strcmp(argv[i], "--english-patch") == 0 && i + 1 < argc)
            english_patch = argv[++i];
        else if (std::strcmp(argv[i], "--disc") == 0 && i + 1 < argc)
            disc = argv[++i];
        else if (std::strcmp(argv[i], "--extract-to") == 0 && i + 1 < argc)
            extracted = argv[++i];
        else if (std::strcmp(argv[i], "--extract-only") == 0)
            extract_only = true;
        else if (std::strcmp(argv[i], "--saves") == 0 && i + 1 < argc)
            saves = argv[++i];
        else if (std::strcmp(argv[i], "--skip-intro") == 0)
            skip_intro = true;
#if defined(_WIN32)
        // A disc image or the English patch dropped onto the executable.
        else if (!dropped && argv[i][0] != '-' &&
                 (is_disc_image(native_path(argv[i])) || is_patch_file(native_path(argv[i]))))
            dropped = argv[i];
#endif
        else {
            std::fprintf(stderr, "Usage: kings-field [--data DIRECTORY] [--data-ja DIRECTORY] [--data-en DIRECTORY] "
                                 "[--language ja|en] [--english-patch FILE|download] "
                                 "[--disc IMAGE [--extract-to NEW_DIRECTORY] [--extract-only]] [--saves DIRECTORY] [--skip-intro]\n");
            return 1;
        }
    }
    if (!language_selected) {
        const char *code = std::getenv("KF_LANGUAGE");
        language_selected = code && kf::language_parse(code, &language);
    }
    const bool data_selected = data || !language_data.empty();
    if ((disc && data_selected) || (!disc && !data && (extracted || extract_only)) ||
        (data && extract_only && !extracted)) {
        std::fprintf(stderr, "Use --data or --disc. To prepare an existing tree, use --data with --extract-to NEW_DIRECTORY.\n");
        return 1;
    }
    if (disc) {
        data = extracted ? extracted : "data";
        if (!kf::disc_extract(disc, data, language))
            return 1;
        if (extract_only)
            return 0;
    }
    if (data && extracted) {
        if (!kf::disc_prepare_directory(data, extracted, language))
            return 1;
        data = extracted;
        if (extract_only)
            return 0;
    }
    // Explicit folders first; the library beside the game fills in the rest and
    // receives languages added later.
    if (data)
        kf::languages_add_folder(data);
    for (const char *folder : language_data)
        kf::languages_add_folder(folder);
#if defined(_WIN32)
    if (!data_selected && !disc) {
        if (!prepare_release_data(dropped))
            return 1;
    } else {
        kf::languages_configure(utf8(executable_directory() / "data"));
    }
#else
    if (!data_selected && !disc)
        kf::languages_add_folder("data");
#endif
    if (english_patch && !kf::language_available(kf::Language::English) && !add_english(english_patch))
        return 1;
    // Start in English when it is there; the saved preference may change it below.
    std::string root = kf::languages_start(language_selected ? language : kf::Language::English);
    if (root.empty() || !kf::data_files_set_root(root.c_str()))
        return 1;
    // Create browser audio within the launch gesture, before asynchronous storage.
    if (!kf::host_start())
        return 1;
    // Settings are loaded now: honour the player's language unless one was given.
    if (!language_selected && kf::language_preference != kf::game_language() &&
        kf::language_available(kf::language_preference)) {
        kf::language_request(kf::language_preference);
        kf::language_apply_pending();
    }
    std::printf("Starting King's Field (%s).\n", kf::language_name(kf::game_language()));
    if (!kf::save_storage_start(saves))
        kf::host_fail("Cannot initialize save storage. Check browser storage permissions or the selected save directory.");
#ifdef __EMSCRIPTEN__
    browser_game_started();
#endif
    if (!skip_intro)
        cutscene_play(Cutscene::Intro);
    for (;;) {
        const GameResult result = game_play();
        cutscene_play(result == GameResult::Completed ? Cutscene::Ending : Cutscene::Intro);
    }
}
