#ifndef KF_PLATFORM_PICKED_FILE_H
#define KF_PLATFORM_PICKED_FILE_H

#include <string>

namespace kf {
// Android's file picker gives a content:// URI, which only SDL's streams open, while the disc
// reader opens paths. Such a file is copied into the user folder (a .cue is not enough there,
// since only the picked file is readable). Returns the path to read: the picked one when it is
// already a path, or the copy; empty when it could not be copied. Slow for a disc, so not on the
// game's update path. One copy at a time.
std::string picked_file_path(const std::string &picked);

// Removes the copy, if there is one.
void picked_file_remove_copy();
} // namespace kf

#endif // KF_PLATFORM_PICKED_FILE_H
