#ifndef KF_PLATFORM_SAVES_H
#define KF_PLATFORM_SAVES_H

#include <kf/lib/types.h>

#include <cstddef>
#include <span>
#include <vector>

namespace kf {
// The game's three slots, and the quick save (F2/F4).
enum class SaveSlot : u8 { First = 1, Second = 2, Third = 3, Quick = 4 };
enum class SaveFileResult : int { Ok = 0, Missing = 1, Invalid = 2, IoError = 3, NoSpace = 4, Unavailable = 5 };

// The game's save files, each the bytes the PlayStation keeps on a memory card as
// BISLPS-00017KF: its header, directory and four entries. Game holds the three slots; Quick is
// a second file of the same format holding the quick save as its first slot.
enum class SaveCard : u8 { Game, Quick };
inline constexpr std::size_t save_card_bytes = 40960;

bool save_storage_start(const char *directory = nullptr);
void save_storage_shutdown();
SaveFileResult save_card_read(SaveCard card, std::span<u8, save_card_bytes> data);
// Success means the replacement has completed its durable commit, not just a write request.
// An I/O error after native rename may leave the new file visible without proving durability.
SaveFileResult save_card_write(SaveCard card, std::span<const u8, save_card_bytes> data);

// A slot's file from the builds before the card format (slot1.kfs ..., the quick save), read
// once to bring it into the card files. Desktop only.
inline constexpr std::size_t earlier_save_capacity = 16384;
SaveFileResult save_earlier_read(SaveSlot slot, std::vector<u8> &data);

// PC extension data kept beside a slot (e.g. "slot1.kfs.map"). Desktop Windows only.
SaveFileResult save_side_read(SaveSlot slot, const char *suffix, u8 *data, std::size_t capacity, std::size_t *size);
SaveFileResult save_side_write(SaveSlot slot, const char *suffix, const u8 *data, std::size_t size);
}

#endif // KF_PLATFORM_SAVES_H
