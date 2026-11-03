#ifndef VERDITE_ONLINE_DURABLE_FILE_H
#define VERDITE_ONLINE_DURABLE_FILE_H
// What keeping a small file safe needs that POSIX and Windows spell differently. Windows opens in
// text mode unless told otherwise, which would change a key's bytes.
#include <fcntl.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#endif

namespace vo {
#ifdef _WIN32
inline constexpr int open_flags = _O_BINARY | _O_NOINHERIT;
inline int sync_file(int fd) { return ::_commit(fd); }
#else
inline constexpr int open_flags = O_CLOEXEC;
inline int sync_file(int fd) { return ::fsync(fd); }
#endif
} // namespace vo

#endif // VERDITE_ONLINE_DURABLE_FILE_H
