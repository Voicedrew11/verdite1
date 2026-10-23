#ifndef KF_PLATFORM_SAVES_H
#define KF_PLATFORM_SAVES_H

#include <kf/lib/types.h>

#include <cstddef>

namespace kf {
// Quick is the port's F2/F4 quick save, stored beside the game's three slots.
enum class SaveSlot : u8 { First = 1, Second = 2, Third = 3, Quick = 4 };
enum class SaveFileResult : int { Ok = 0, Missing = 1, Invalid = 2, IoError = 3, NoSpace = 4, Unavailable = 5 };
inline constexpr std::size_t save_file_capacity = 16384;
bool save_storage_start(const char *directory = nullptr);
void save_storage_shutdown();
SaveFileResult save_file_read(SaveSlot slot, u8 *data, std::size_t capacity, std::size_t *size);
// Success means the replacement has completed its durable commit, not just a write request.
// An I/O error after native rename may leave the new file visible without proving durability.
SaveFileResult save_file_write(SaveSlot slot, const u8 *data, std::size_t size);
// PC extension data kept beside a slot's file (e.g. "slot1.kfs.map"). Desktop Windows only.
SaveFileResult save_side_read(SaveSlot slot, const char *suffix, u8 *data, std::size_t capacity, std::size_t *size);
SaveFileResult save_side_write(SaveSlot slot, const char *suffix, const u8 *data, std::size_t size);
}

#endif // KF_PLATFORM_SAVES_H
