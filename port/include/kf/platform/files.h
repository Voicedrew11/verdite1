#ifndef KF_PLATFORM_FILES_H
#define KF_PLATFORM_FILES_H

#include <kf/lib/types.h>

#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace kf {
enum class FileResult { Ok, NotFound, InvalidPath, IoError, TooLarge, OutOfMemory };

// An open file: the disc's, or the bytes a mod replaces it with (stream is then null).
struct DataFile {
    std::FILE *stream;
    std::size_t size;
    std::shared_ptr<const std::vector<u8>> replaced;
    std::string replaced_path;   // the path a replacement was opened for: a fuzz note names it at the read
};

// A mod's replacement for a path as the game names it ("KF/B1/SND0.SEQ"), or null for the disc's.
using DataReplacer = std::shared_ptr<const std::vector<u8>> (*)(const char *path);
// Set by the mods at start and cleared at shutdown; null: no replacements.
void data_files_set_replacer(DataReplacer replacer);

// Paths are ordinary, case-sensitive paths relative to the extracted disc root.
bool data_files_set_root(const char *directory);
// The replacement for path, if a mod gives one; false (file untouched) otherwise.
bool data_file_open_replaced(DataFile *file, const char *path);
// The disc's file at path, or the replacement for it; the game's own opening.
FileResult data_file_open(DataFile *file, const char *path);
// The disc's file at path, never a replacement (the disc's size for mods, and the other folders).
FileResult data_file_open_disc(DataFile *file, const char *path);
FileResult data_file_open_at(DataFile *file, const char *directory, const char *path);
FileResult data_file_read(DataFile *file, void *destination, std::size_t capacity);
FileResult data_file_read(DataFile *file, std::vector<u8> &destination, std::size_t capacity);
void data_file_close(DataFile *file);
FileResult data_file_read_into(const char *path, void *destination, std::size_t capacity,
                              std::size_t *loaded_size = nullptr);
void data_file_report_error(FileResult result, const char *path);
}

#endif // KF_PLATFORM_FILES_H
