#include <kf/platform/assets.h>
#include <kf/platform/disc.h>
#include <kf/platform/disc_layout.h>
#include <kf/platform/translation.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#ifdef _WIN32
#include <filesystem>
#else
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace kf {
static constexpr unsigned cue_line_capacity = 1024;
static constexpr unsigned disc_path_capacity = 4096;
static constexpr std::size_t cue_file_capacity = 65536;
static constexpr unsigned ascii_first_printable = 32;
bool disc_cue_image(const char *text, std::span<char> filename) {
    bool file = false, track = false, index = false;
    while (*text) {
        const char *end = std::strchr(text, '\n');
        if (!end)
            end = text + std::strlen(text);
        while (text < end && std::isspace(static_cast<unsigned char>(*text)))
            ++text;
        const char *tail = end;
        while (tail > text && std::isspace(static_cast<unsigned char>(tail[-1])))
            --tail;
        std::array<char, cue_line_capacity> line;
        const auto length = static_cast<std::size_t>(tail - text);
        if (length >= line.size())
            return false;
        std::memcpy(line.data(), text, length);
        line[length] = 0;
        text = *end ? end + 1 : end;
        if (!length || std::strncmp(line.data(), "REM ", 4) == 0)
            continue;
        if (std::strncmp(line.data(), "FILE ", 5) == 0) {
            if (file || track || index)
                return false;
            const char *name = line.data() + 5;
            while (*name == ' ' || *name == '\t')
                ++name;
            const bool quoted = *name == '"';
            if (quoted)
                ++name;
            const char *name_end = quoted ? std::strchr(name, '"') : std::strpbrk(name, " \t");
            if (!name_end || name_end == name)
                return false;
            const auto name_size = static_cast<std::size_t>(name_end - name);
            const char *kind = name_end + (quoted ? 1 : 0);
            if (*kind != ' ' && *kind != '\t')
                return false;
            while (*kind == ' ' || *kind == '\t')
                ++kind;
            if (std::strcmp(kind, "BINARY") != 0 || name_size >= filename.size())
                return false;
            // Only a sibling file: never resolve absolute paths or traversal.
            for (const char *at = name; at != name_end; ++at)
                if (*at == '/' || *at == '\\' || *at == ':' ||
                    static_cast<unsigned char>(*at) < ascii_first_printable)
                    return false;
            std::memcpy(filename.data(), name, name_size);
            filename[name_size] = 0;
            if (std::strcmp(filename.data(), ".") == 0 || std::strcmp(filename.data(), "..") == 0)
                return false;
            file = true;
        } else if (std::strcmp(line.data(), "TRACK 01 MODE2/2352") == 0) {
            if (!file || track || index)
                return false;
            track = true;
        } else if (std::strcmp(line.data(), "INDEX 01 00:00:00") == 0) {
            if (!track || index)
                return false;
            index = true;
        } else {
            return false;
        }
    }
    return file && track && index;
}

#ifdef _WIN32
static std::filesystem::path native_path(const char *path) {
    return std::filesystem::path(reinterpret_cast<const char8_t *>(path));
}

static FILE *open_image(const char *path, std::size_t *size) {
    std::error_code error;
    const auto file_path = native_path(path);
    if (!std::filesystem::is_regular_file(file_path, error))
        return nullptr;
    const auto length = std::filesystem::file_size(file_path, error);
    if (error || length == 0 || length > disc_import_limit)
        return nullptr;
    FILE *file = ::_wfopen(file_path.c_str(), L"rb");
    if (!file)
        return nullptr;
    *size = static_cast<std::size_t>(length);
    return file;
}

static int seek_file(FILE *file, std::uint64_t offset) {
    return ::_fseeki64(file, static_cast<long long>(offset), SEEK_SET);
}
#else
static int seek_file(FILE *file, std::uint64_t offset) {
    return ::fseeko(file, static_cast<off_t>(offset), SEEK_SET);
}

static FILE *open_image(const char *path, std::size_t *size) {
    const int descriptor = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0)
        return nullptr;
    struct stat info{};
    if (::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        static_cast<std::uint64_t>(info.st_size) > disc_import_limit) {
        ::close(descriptor);
        return nullptr;
    }
    FILE *file = ::fdopen(descriptor, "rb");
    if (!file) {
        ::close(descriptor);
        return nullptr;
    }
    *size = static_cast<std::size_t>(info.st_size);
    return file;
}
#endif

static bool ends_with_cue(const char *path) {
    const auto size = std::strlen(path);
    return size >= 4 && path[size - 4] == '.' &&
        std::tolower(static_cast<unsigned char>(path[size - 3])) == 'c' &&
        std::tolower(static_cast<unsigned char>(path[size - 2])) == 'u' &&
        std::tolower(static_cast<unsigned char>(path[size - 1])) == 'e';
}

static FILE *open_disc(const char *source, std::size_t *size) {
    FILE *file = open_image(source, size);
    if (!file || !ends_with_cue(source))
        return file;
    if (*size > cue_file_capacity) {
        std::fclose(file);
        return nullptr;
    }
    ByteBuffer cue{};
    bool ok = true;
    try {
        cue.resize(*size + 1);
        ok = std::fread(cue.data(), 1, *size, file) == *size;
    } catch (const std::bad_alloc &) {
        ok = false;
    }
    if (std::fclose(file) != 0)
        ok = false;
    std::array<char, cue_line_capacity> name;
    if (ok) {
        cue[*size] = 0;
        ok = !std::memchr(cue.data(), 0, *size) &&
            disc_cue_image(reinterpret_cast<const char *>(cue.data()), name);
    }
    if (!ok) {
        std::fprintf(stderr, "CUE must describe one MODE2/2352 track with INDEX 01 at 00:00:00.\n");
        return nullptr;
    }
    const char *slash = std::strrchr(source, '/');
#ifdef _WIN32
    if (const char *backslash = std::strrchr(source, '\\'); backslash && (!slash || backslash > slash))
        slash = backslash;
#endif
    const auto prefix = slash ? static_cast<std::size_t>(slash + 1 - source) : 0;
    std::array<char, disc_path_capacity> path;
    if (prefix + std::strlen(name.data()) >= path.size())
        return nullptr;
    std::memcpy(path.data(), source, prefix);
    std::strcpy(path.data() + prefix, name.data());
    return open_image(path.data(), size);
}

#ifdef _WIN32
static bool write_asset(const std::filesystem::path &root, const Asset &asset) {
    std::error_code error;
    const auto path = root / native_path(asset.path.data());
    std::filesystem::create_directories(path.parent_path(), error);
    if (error || std::filesystem::exists(path, error))
        return false;
    FILE *file = ::_wfopen(path.c_str(), L"wb");
    if (!file)
        return false;
    const bool written = std::fwrite(asset.bytes.data(), 1, asset.bytes.size(), file) == asset.bytes.size();
    return std::fclose(file) == 0 && written;
}
#else
static bool write_asset(int root, const Asset &asset) {
    auto path = asset.path;
    int directory = ::dup(root);
    if (directory < 0)
        return false;
    char *name = path.data();
    while (char *slash = std::strchr(name, '/')) {
        *slash = 0;
        if (::mkdirat(directory, name, S_IRWXU) != 0 && errno != EEXIST) {
            ::close(directory);
            return false;
        }
        const int next = ::openat(directory, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(directory);
        if (next < 0)
            return false;
        directory = next;
        name = slash + 1;
    }
    const int file = ::openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
    ::close(directory);
    if (file < 0)
        return false;
    std::size_t written = 0;
    while (written < asset.bytes.size()) {
        const auto count = ::write(file, asset.bytes.data() + written, asset.bytes.size() - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        written += static_cast<std::size_t>(count);
    }
    const bool closed = ::close(file) == 0;
    return closed && written == asset.bytes.size();
}

#endif

bool resource_tree_write(const AssetTable &assets, const char *destination) {
#ifdef _WIN32
    // Written beside the destination first and renamed when complete, so an
    // interrupted write never leaves a partial tree that looks usable.
    std::error_code error;
    const auto root = native_path(destination);
    auto partial = root;
    partial += ".partial";
    std::filesystem::remove_all(partial, error);
    if (std::filesystem::exists(root, error)) {
        std::fprintf(stderr, "Cannot write %s: it already exists.\n", destination);
        return false;
    }
    std::filesystem::create_directories(partial, error);
    bool ok = !error;
    for (std::size_t i = 0; ok && i < assets.size(); ++i)
        ok = write_asset(partial, assets[i]);
    if (ok) {
        std::filesystem::rename(partial, root, error);
        ok = !error;
    }
    if (!ok) {
        std::fprintf(stderr, "Cannot write the game files to %s.\n", destination);
        std::filesystem::remove_all(partial, error);
        return false;
    }
#else
    if (::mkdir(destination, S_IRWXU) != 0) {
        std::fprintf(stderr, "Cannot create %s: %s. Extraction requires a new directory; use --data to reuse one.\n",
                     destination, std::strerror(errno));
        return false;
    }
    const int root = ::open(destination, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    bool ok = root >= 0;
    for (std::size_t i = 0; ok && i < assets.size(); ++i)
        ok = write_asset(root, assets[i]);
    if (root >= 0 && ::close(root) != 0)
        ok = false;
    if (!ok) {
        std::fprintf(stderr, "Extraction failed; partial files remain in %s. No existing directory was replaced.\n", destination);
        return false;
    }
#endif
    return true;
}

static bool write_resource_tree(const char *destination, const AssetTable &assets, Language language) {
    if (!resource_tree_write(assets, destination))
        return false;
    std::printf("Extracted %zu verified %s files to %s\n", assets.size(), language_name(language), destination);
    return true;
}

bool resource_tree_read(const char *directory, AssetTable &assets) {
    // Exactly the retail file list, so nothing else in the folder is read.
    for (const auto &entry : slps00017_layout) {
        std::string path = directory;
        path += '/';
        path += entry.path;
        // Windows drops a trailing dot from file names ("E0." is stored as "E0").
        if (!path.empty() && path.back() == '.')
            path.pop_back();
        std::size_t size;
        FILE *file = open_image(path.c_str(), &size);
        if (!file)
            return false;
        ByteBuffer bytes;
        bool ok = size == entry.size;
        if (ok) {
            try {
                bytes.resize(size);
            } catch (const std::bad_alloc &) {
                ok = false;
            }
        }
        ok = ok && std::fread(bytes.data(), 1, size, file) == size;
        std::fclose(file);
        if (!ok || !assets_append(assets, entry.path, std::move(bytes)))
            return false;
    }
    return true;
}

// any_release: accept every release with the retail file set (the Windows
// flow sorts them by language afterwards); otherwise require `language`.
static bool import_image(const char *source, Language language, bool any_release, AssetTable &assets) {
    std::size_t size;
    FILE *file = open_disc(source, &size);
    if (!file) {
        std::fprintf(stderr, "Cannot open disc image: %s\n", source);
        return false;
    }
    std::unique_ptr<DiscImporter> importer;
    try {
        importer = std::make_unique<DiscImporter>();
    } catch (const std::bad_alloc &) {
        std::fclose(file);
        return false;
    }
    disc_import_start(importer.get(), size, language);
    importer->any_release = any_release;
    ByteBuffer read{};
    while (disc_import_waiting(importer.get())) {
        const auto request = importer->request;
        try {
            read.resize(request.length);
        } catch (const std::bad_alloc &) {
            disc_import_fail(importer.get(), "Cannot allocate disc read buffer.");
            break;
        }
        if (seek_file(file, request.offset) != 0 ||
            std::fread(read.data(), 1, read.size(), file) != read.size()) {
            disc_import_fail(importer.get(), "Cannot read the selected disc image.");
            break;
        }
        disc_import_supply(importer.get(), read);
    }
    bool ok = importer->state == ImportState::complete;
    if (std::fclose(file) != 0)
        ok = false;
    if (!ok)
        std::fprintf(stderr, "%s\n", importer->message.data());
    if (ok)
        assets = std::move(importer->assets);
    return ok;
}

bool disc_import_image(const char *source, AssetTable &assets, DiscMatch *match) {
    if (!import_image(source, Language::Japanese, true, assets))
        return false;
    *match = assets_classify(assets);
    return true;
}

bool disc_extract(const char *source, const char *destination, Language language) {
    AssetTable assets;
    return import_image(source, language, false, assets) && write_resource_tree(destination, assets, language);
}

#ifdef _WIN32
bool disc_verify_directory(const char *directory, Language language, Language *actual) {
    AssetTable assets;
    bool ok = resource_tree_read(directory, assets);
    Language detected = language;
    if (ok && !assets_match_language(assets, language)) {
        ok = actual && language == Language::English && assets_match_language(assets, Language::Japanese);
        detected = Language::Japanese;
    }
    if (ok && actual)
        *actual = detected;
    if (!ok)
        std::fprintf(stderr, "Resource tree does not match supported %s SLPS-00017 files: %s\n",
                     language_name(language), directory);
    return ok;
}

bool disc_prepare_directory(const char *source, const char *destination, Language language) {
    AssetTable assets;
    bool ok = resource_tree_read(source, assets);
    if (!ok)
        std::fprintf(stderr, "Cannot read resource tree: %s\n", source);
    if (ok) {
        if (const char *error = assets_prepare_language(assets, language)) {
            std::fprintf(stderr, "%s\n", error);
            ok = false;
        }
    }
    return ok && write_resource_tree(destination, assets, language);
}
#else
static bool read_resource_tree(int root, const char *prefix, AssetTable &assets,
                               std::size_t *total, unsigned *directories) {
    if (++*directories > 128)
        return false;
    const int descriptor = ::dup(root);
    if (descriptor < 0)
        return false;
    DIR *directory = ::fdopendir(descriptor);
    if (!directory) {
        ::close(descriptor);
        return false;
    }
    bool ok = true;
    while (ok) {
        errno = 0;
        const auto *entry = ::readdir(directory);
        if (!entry) {
            ok = errno == 0;
            break;
        }
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
            continue;
        std::array<char, asset_path_capacity> path, normalized;
        const int length = std::snprintf(path.data(), path.size(), "%s%s", prefix, entry->d_name);
        if (length < 0 || static_cast<std::size_t>(length) >= path.size() - 1 ||
            !asset_path(normalized, path.data()) || std::strcmp(path.data(), normalized.data()) != 0) {
            ok = false;
            break;
        }
        // Reject links and special files before reading. O_NONBLOCK also avoids FIFO waits.
        const int child = ::openat(root, entry->d_name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        struct stat info{};
        if (child < 0) {
            ok = false;
            break;
        }
        ok = ::fstat(child, &info) == 0;
        if (ok && S_ISDIR(info.st_mode)) {
            path[length] = '/';
            path[length + 1] = 0;
            ok = read_resource_tree(child, path.data(), assets, total, directories);
        } else if (ok && S_ISREG(info.st_mode) && info.st_size >= 0 &&
                   info.st_size <= 16 * 1024 * 1024 && assets.size() < 428 &&
                   static_cast<std::uint64_t>(info.st_size) <= disc_import_limit - *total) {
            ByteBuffer bytes{};
            try {
                bytes.resize(static_cast<std::size_t>(info.st_size));
            } catch (const std::bad_alloc &) {
                ok = false;
            }
            std::size_t at = 0;
            while (ok && at < bytes.size()) {
                const auto count = ::read(child, bytes.data() + at, bytes.size() - at);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0) { ok = false; break; }
                at += static_cast<std::size_t>(count);
            }
            u8 extra;
            if (ok)
                ok = ::read(child, &extra, 1) == 0;
            *total += bytes.size();
            if (ok)
                ok = assets_append(assets, path.data(), std::move(bytes));
        } else {
            ok = false;
        }
        if (::close(child) != 0)
            ok = false;
    }
    if (::closedir(directory) != 0)
        ok = false;
    return ok;
}

bool disc_verify_directory(const char *directory, Language language, Language *actual) {
    const int root = ::open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    AssetTable assets{};
    std::size_t total = 0;
    unsigned directories = 0;
    bool ok = root >= 0 && read_resource_tree(root, "", assets, &total, &directories);
    Language detected = language;
    if (ok && !assets_match_language(assets, language)) {
        ok = actual && language == Language::English && assets_match_language(assets, Language::Japanese);
        detected = Language::Japanese;
    }
    if (root >= 0 && ::close(root) != 0)
        ok = false;
    if (ok && actual)
        *actual = detected;
    if (!ok)
        std::fprintf(stderr, "Resource tree does not match supported %s SLPS-00017 files: %s. Select the matching --language ja|en and disc tree.\n",
                     language_name(language), directory);
    return ok;
}
bool disc_prepare_directory(const char *source, const char *destination, Language language) {
    const int root = ::open(source, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    AssetTable assets{};
    std::size_t total = 0;
    unsigned directories = 0;
    bool ok = root >= 0 && read_resource_tree(root, "", assets, &total, &directories);
    if (root >= 0 && ::close(root) != 0)
        ok = false;
    if (!ok)
        std::fprintf(stderr, "Cannot read resource tree: %s\n", source);
    if (ok) {
        if (const char *error = assets_prepare_language(assets, language)) {
            std::fprintf(stderr, "%s\n", error);
            ok = false;
        }
    }
    if (ok)
        ok = write_resource_tree(destination, assets, language);
    return ok;
}
#endif
}
