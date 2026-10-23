#include <kf/platform/saves.h>

#include <SDL3/SDL.h>

#include <array>
#include <cstdio>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_ASYNC_JS(int, browser_saves_open, (), {
    const saveSchemaVersion = 1;
    try {
        const db = await new Promise((resolve, reject) => {
            const request = indexedDB.open('kings-field-saves', saveSchemaVersion);
            let failed = false;
            request.onupgradeneeded = () => request.result.createObjectStore('files');
            request.onblocked = () => { failed = true; reject(new Error('Save database upgrade is blocked')); };
            request.onerror = () => { failed = true; reject(request.error); };
            request.onsuccess = () => {
                if (failed) request.result.close();
                else resolve(request.result);
            };
        });
        db.onversionchange = () => { db.close(); Module['kfSaveDb'] = null; };
        Module['kfSaveDb'] = db;
        return 1;
    } catch (error) {
        console.error('Save storage:', error);
        return 0;
    }
});

EM_JS(void, browser_saves_close, (), {
    if (Module['kfSaveDb']) Module['kfSaveDb'].close();
    Module['kfSaveDb'] = null;
});

EM_ASYNC_JS(int, browser_save_read, (int slot, u8 *output, unsigned capacity, unsigned *size), {
    // Wire values mirror kf::SaveFileResult.
    const Result = {Ok: 0, Missing: 1, Invalid: 2, IoError: 3, NoSpace: 4, Unavailable: 5};
    const db = Module['kfSaveDb'];
    if (!db) return Result.Unavailable;
    try {
        const data = await new Promise((resolve, reject) => {
            const transaction = db.transaction('files', 'readonly');
            const request = transaction.objectStore('files').get('slot' + slot + '.kfs');
            transaction.onabort = () => reject(transaction.error);
            transaction.oncomplete = () => resolve(request.result);
        });
        if (data === undefined) return Result.Missing;
        if (!(data instanceof Uint8Array) || !data.length || data.length > capacity) return Result.Invalid;
        HEAPU8.set(data, output);
        HEAPU32[size >> 2] = data.length;
        return Result.Ok;
    } catch (error) {
        console.error('Read save:', error);
        return Result.IoError;
    }
});

EM_ASYNC_JS(int, browser_save_write, (int slot, const u8 *input, unsigned size), {
    // Wire values mirror kf::SaveFileResult.
    const Result = {Ok: 0, Missing: 1, Invalid: 2, IoError: 3, NoSpace: 4, Unavailable: 5};
    const db = Module['kfSaveDb'];
    if (!db) return Result.Unavailable;
    try {
        const data = HEAPU8.slice(input, input + size);
        await new Promise((resolve, reject) => {
            const transaction = db.transaction('files', 'readwrite', {durability: 'strict'});
            transaction.onabort = () => reject(transaction.error || new Error('Save transaction aborted'));
            transaction.oncomplete = () => resolve();
            // Do not silently downgrade the requested persistence guarantee.
            if (transaction.durability !== 'strict') {
                transaction.abort();
                return;
            }
            transaction.objectStore('files').put(data, 'slot' + slot + '.kfs');
        });
        return Result.Ok;
    } catch (error) {
        console.error('Write save:', error);
        return error && error.name === 'QuotaExceededError' ? Result.NoSpace : Result.IoError;
    }
});
#elif defined(_WIN32)
#include <filesystem>
#include <windows.h>
#else
#include <sys/stat.h>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#endif

namespace kf {
#if defined(_WIN32)
static constexpr unsigned save_name_capacity = 16;
static constexpr unsigned pending_name_capacity = 64;
static std::filesystem::path *save_directory;
static unsigned pending_serial;
static std::filesystem::path save_path(SaveSlot slot) {
    char name[save_name_capacity];
    if (slot == SaveSlot::Quick)
        std::snprintf(name, sizeof name, "quicksave.kfs");
    else
        std::snprintf(name, sizeof name, "slot%u.kfs", static_cast<unsigned>(slot));
    return *save_directory / name;
}
#elif !defined(__EMSCRIPTEN__)
static constexpr unsigned save_name_capacity = 16;
static constexpr unsigned pending_name_capacity = 64;
static constexpr unsigned pending_name_attempts = 8;
static int save_directory = -1;
static unsigned pending_serial;
static SaveFileResult io_error() {
    return errno == ENOSPC || errno == EDQUOT ? SaveFileResult::NoSpace : SaveFileResult::IoError;
}
static bool sync_file(int descriptor) {
    int result;
    do { result = ::fsync(descriptor); } while (result != 0 && errno == EINTR);
    return result == 0;
}
static bool sync_directory_path(int directory) {
    int child = ::dup(directory);
    if (child < 0)
        return false;
    for (;;) {
        const int parent = ::openat(child, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        struct stat child_info{}, parent_info{};
        const bool ok = parent >= 0 && ::fstat(child, &child_info) == 0
            && ::fstat(parent, &parent_info) == 0 && sync_file(child);
        ::close(child);
        if (!ok) {
            if (parent >= 0)
                ::close(parent);
            return false;
        }
        if (child_info.st_dev == parent_info.st_dev && child_info.st_ino == parent_info.st_ino) {
            ::close(parent);
            return true;
        }
        child = parent;
    }
}
#endif

static bool valid_slot(SaveSlot slot) {
    return slot >= SaveSlot::First && slot <= SaveSlot::Quick;
}

bool save_storage_start(const char *directory) {
#ifdef __EMSCRIPTEN__
    // Browser slots are file byte streams in a dedicated IndexedDB transaction.
    // Resource extraction/cache uses a separate store and cannot delay their commit.
    return !directory && browser_saves_open() != 0;
#elif defined(_WIN32)
    char *default_path = directory ? nullptr : SDL_GetPrefPath("KingsField", "SLPS00017");
    const char *path = directory ? directory : default_path;
    std::error_code error;
    if (path && std::filesystem::is_directory(std::filesystem::path(reinterpret_cast<const char8_t *>(path)), error))
        save_directory = new std::filesystem::path(std::filesystem::path(reinterpret_cast<const char8_t *>(path)));
    SDL_free(default_path);
    if (!save_directory)
        std::fprintf(stderr, "Cannot open save directory: %s\n", directory ? directory : "default user storage");
    return save_directory != nullptr;
#else
    char *default_path = directory ? nullptr : SDL_GetPrefPath("KingsField", "SLPS00017");
    const char *path = directory ? directory : default_path;
    if (path)
        save_directory = ::open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    SDL_free(default_path);
    // SDL may have created several preference directories. Publish their parent
    // entries before claiming that a first save in the leaf can be durable.
    if (!directory && save_directory >= 0 && !sync_directory_path(save_directory)) {
        ::close(save_directory);
        save_directory = -1;
    }
    if (save_directory < 0)
        std::fprintf(stderr, "Cannot open save directory: %s\n", directory ? directory : "default user storage");
    return save_directory >= 0;
#endif
}

void save_storage_shutdown() {
#ifdef __EMSCRIPTEN__
    browser_saves_close();
#elif defined(_WIN32)
    delete save_directory;
    save_directory = nullptr;
#else
    if (save_directory >= 0)
        ::close(save_directory);
    save_directory = -1;
#endif
}

SaveFileResult save_file_read(SaveSlot slot, u8 *data, std::size_t capacity, std::size_t *size) {
    if (size)
        *size = 0;
    if (!valid_slot(slot) || !data || !size || !capacity || capacity > save_file_capacity)
        return SaveFileResult::Invalid;
#ifdef __EMSCRIPTEN__
    unsigned loaded = 0;
    const auto result = static_cast<SaveFileResult>(browser_save_read(static_cast<int>(slot), data, capacity, &loaded));
    *size = loaded;
    return result;
#elif defined(_WIN32)
    if (!save_directory)
        return SaveFileResult::Unavailable;
    const auto path = save_path(slot);
    std::error_code error;
    if (!std::filesystem::exists(path, error))
        return error ? SaveFileResult::IoError : SaveFileResult::Missing;
    if (!std::filesystem::is_regular_file(path, error))
        return SaveFileResult::Invalid;
    const auto length = std::filesystem::file_size(path, error);
    if (error)
        return SaveFileResult::IoError;
    if (length == 0 || length > capacity)
        return SaveFileResult::Invalid;
    FILE *file = ::_wfopen(path.c_str(), L"rb");
    if (!file)
        return SaveFileResult::IoError;
    const auto loaded = std::fread(data, 1, static_cast<std::size_t>(length), file);
    const bool at_end = std::fgetc(file) == EOF && !std::ferror(file);
    if (std::fclose(file) != 0 || loaded != length || !at_end)
        return SaveFileResult::IoError;
    *size = loaded;
    return SaveFileResult::Ok;
#else
    if (save_directory < 0)
        return SaveFileResult::Unavailable;
    std::array<char, save_name_capacity> name;
    std::snprintf(name.data(), name.size(), "slot%u.kfs", static_cast<unsigned>(slot));
    const int file = ::openat(save_directory, name.data(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (file < 0)
        return errno == ENOENT ? SaveFileResult::Missing : io_error();
    struct stat info{};
    auto result = SaveFileResult::Ok;
    if (::fstat(file, &info) != 0)
        result = io_error();
    else if (!S_ISREG(info.st_mode) || info.st_size <= 0 || static_cast<unsigned long long>(info.st_size) > capacity)
        result = SaveFileResult::Invalid;
    std::size_t loaded = 0;
    while (result == SaveFileResult::Ok && loaded < static_cast<std::size_t>(info.st_size)) {
        const auto count = ::read(file, data + loaded, info.st_size - loaded);
        if (count > 0)
            loaded += count;
        else if (count == 0 || errno != EINTR)
            result = SaveFileResult::IoError;
    }
    if (result == SaveFileResult::Ok) {
        u8 extra;
        ssize_t count;
        do { count = ::read(file, &extra, 1); } while (count < 0 && errno == EINTR);
        if (count != 0)
            result = SaveFileResult::IoError;
    }
    if (::close(file) != 0)
        result = io_error();
    if (result == SaveFileResult::Ok)
        *size = loaded;
    return result;
#endif
}

SaveFileResult save_file_write(SaveSlot slot, const u8 *data, std::size_t size) {
    if (!valid_slot(slot) || !data || !size || size > save_file_capacity)
        return SaveFileResult::Invalid;
#ifdef __EMSCRIPTEN__
    return static_cast<SaveFileResult>(browser_save_write(static_cast<int>(slot), data, size));
#elif defined(_WIN32)
    if (!save_directory)
        return SaveFileResult::Unavailable;
    const auto path = save_path(slot);
    char pending_name[pending_name_capacity];
    std::snprintf(pending_name, sizeof pending_name, ".slot%u.%llx.%u.tmp", static_cast<unsigned>(slot),
        static_cast<unsigned long long>(SDL_GetTicksNS()), ++pending_serial);
    const auto pending = *save_directory / pending_name;
    if (slot == SaveSlot::Quick) {
        // Keep the previous quick save, so one mistimed F2 is recoverable.
        auto backup = path;
        backup += ".bak";
        ::CopyFileW(path.c_str(), backup.c_str(), FALSE);
    }
    FILE *file = ::_wfopen(pending.c_str(), L"wbx");
    if (!file)
        return SaveFileResult::IoError;
    bool ok = std::fwrite(data, 1, size, file) == size && std::fflush(file) == 0;
    ok = std::fclose(file) == 0 && ok;
    // Atomic replacement with write-through: the Windows analogue of fsync + renameat.
    if (ok && !::MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        ok = false;
    if (!ok) {
        const bool full = ::GetLastError() == ERROR_DISK_FULL;
        std::error_code error;
        std::filesystem::remove(pending, error);
        return full ? SaveFileResult::NoSpace : SaveFileResult::IoError;
    }
    return SaveFileResult::Ok;
#else
    if (save_directory < 0)
        return SaveFileResult::Unavailable;
    std::array<char, save_name_capacity> name;
    std::array<char, pending_name_capacity> pending;
    std::snprintf(name.data(), name.size(), "slot%u.kfs", static_cast<unsigned>(slot));
    int file = -1;
    for (unsigned attempt = 0; attempt < pending_name_attempts && file < 0; ++attempt) {
        std::snprintf(pending.data(), pending.size(), ".slot%u.%llx.%u.tmp", static_cast<unsigned>(slot),
            static_cast<unsigned long long>(SDL_GetTicksNS()), ++pending_serial);
        file = ::openat(save_directory, pending.data(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
        if (file < 0 && errno != EEXIST)
            return io_error();
    }
    if (file < 0)
        return SaveFileResult::IoError;
    auto result = SaveFileResult::Ok;
    std::size_t written = 0;
    while (written < size) {
        const auto count = ::write(file, data + written, size - written);
        if (count > 0)
            written += count;
        else if (count == 0 || errno != EINTR) {
            result = io_error();
            break;
        }
    }
    if (result == SaveFileResult::Ok && !sync_file(file))
        result = io_error();
    if (::close(file) != 0)
        result = io_error();
    if (result == SaveFileResult::Ok && ::renameat(save_directory, pending.data(), save_directory, name.data()) != 0)
        result = io_error();
    if (result != SaveFileResult::Ok) {
        // Only the uniquely created, unpublished temporary file is removed.
        ::unlinkat(save_directory, pending.data(), 0);
        return result;
    }
    // The rename is atomic; synchronizing its directory makes it durable too.
    return sync_file(save_directory) ? SaveFileResult::Ok : io_error();
#endif
}
#if defined(_WIN32)
static std::filesystem::path side_path(SaveSlot slot, const char *suffix) {
    auto path = save_path(slot);
    path += suffix;
    return path;
}
#endif

SaveFileResult save_side_read(SaveSlot slot, const char *suffix, u8 *data, std::size_t capacity, std::size_t *size) {
    *size = 0;
#if defined(_WIN32)
    if (!save_directory || !valid_slot(slot))
        return SaveFileResult::Unavailable;
    FILE *file = ::_wfopen(side_path(slot, suffix).c_str(), L"rb");
    if (!file)
        return SaveFileResult::Missing;
    *size = std::fread(data, 1, capacity, file);
    std::fclose(file);
    return SaveFileResult::Ok;
#else
    (void)slot; (void)suffix; (void)data; (void)capacity;
    return SaveFileResult::Unavailable;
#endif
}

SaveFileResult save_side_write(SaveSlot slot, const char *suffix, const u8 *data, std::size_t size) {
#if defined(_WIN32)
    if (!save_directory || !valid_slot(slot))
        return SaveFileResult::Unavailable;
    const auto path = side_path(slot, suffix);
    auto pending = path;
    pending += ".tmp";
    FILE *file = ::_wfopen(pending.c_str(), L"wb");
    if (!file)
        return SaveFileResult::IoError;
    bool ok = std::fwrite(data, 1, size, file) == size;
    ok = std::fclose(file) == 0 && ok;
    if (ok && !::MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        ok = false;
    return ok ? SaveFileResult::Ok : SaveFileResult::IoError;
#else
    (void)slot; (void)suffix; (void)data; (void)size;
    return SaveFileResult::Unavailable;
#endif
}
}
