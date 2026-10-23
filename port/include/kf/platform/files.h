#ifndef KF_PLATFORM_FILES_H
#define KF_PLATFORM_FILES_H

#include <kf/lib/types.h>

#include <cstddef>
#include <cstdio>
#include <vector>

namespace kf {
enum class FileResult { Ok, NotFound, InvalidPath, IoError, TooLarge, OutOfMemory };

struct DataFile {
    std::FILE *stream;
    std::size_t size;
};

// Paths are ordinary, case-sensitive paths relative to the extracted disc root.
bool data_files_set_root(const char *directory);
FileResult data_file_open(DataFile *file, const char *path);
FileResult data_file_open_at(DataFile *file, const char *directory, const char *path);
FileResult data_file_read(DataFile *file, void *destination, std::size_t capacity);
FileResult data_file_read(DataFile *file, std::vector<u8> &destination, std::size_t capacity);
void data_file_close(DataFile *file);
FileResult data_file_read_into(const char *path, void *destination, std::size_t capacity,
                              std::size_t *loaded_size = nullptr);
void data_file_report_error(FileResult result, const char *path);
}

#endif // KF_PLATFORM_FILES_H
