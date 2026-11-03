#ifndef KF_PLATFORM_SAVES_H
#define KF_PLATFORM_SAVES_H

#include <kf/lib/types.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kf {
// The game's three slots, the quick save (F2/F4), and the player's own game set aside in coop.
enum class SaveSlot : u8 { First = 1, Second = 2, Third = 3, Quick = 4, Aside = 5 };
enum class SaveFileResult : int { Ok = 0, Missing = 1, Invalid = 2, IoError = 3, NoSpace = 4, Unavailable = 5 };

// The game's save files, each the bytes the PlayStation keeps on a memory card as
// BISLPS-00017KF: its header, directory and four entries. Game holds the three slots; Quick is
// a second file of the same format holding the quick save as its first slot; Aside a third,
// holding as its first slot the player's own game while they play a coop world.
enum class SaveCard : u8 { Game, Quick, Aside };
inline constexpr std::size_t save_card_bytes = 40960;

bool save_storage_start(const char *directory = nullptr);
void save_storage_shutdown();
SaveFileResult save_card_read(SaveCard card, std::span<u8, save_card_bytes> data);
// Success means the replacement has completed its durable commit, not just a write request.
// An I/O error after native rename may leave the new file visible without proving durability.
SaveFileResult save_card_write(SaveCard card, std::span<const u8, save_card_bytes> data);
// Ok when the file is gone, whether or not it was there.
SaveFileResult save_card_remove(SaveCard card);

// A room's coop world, kept by whoever hosts it (docs/COOP_PLAN.md), in coop-<room>.kfcoop.
// The room is a room code: 1 to 64 letters, digits, - or _.
inline constexpr std::size_t coop_world_capacity = 1 << 20;
SaveFileResult coop_world_read(std::string_view room, std::vector<u8> &data);
SaveFileResult coop_world_write(std::string_view room, std::span<const u8> data);
// The rooms whose worlds are kept here, in no order; empty where files cannot be listed.
std::vector<std::string> coop_world_rooms();

// A slot's file from the builds before the card format (slot1.kfs ..., the quick save), read
// once to bring it into the card files. Desktop only.
inline constexpr std::size_t earlier_save_capacity = 16384;
SaveFileResult save_earlier_read(SaveSlot slot, std::vector<u8> &data);

// The mods' save data kept beside a slot (slot1.mods.json ..., quick.mods.json, aside.mods.json),
// as JSON text (docs/MODDING.md, "save"). Every platform with a save directory.
inline constexpr std::size_t save_mods_capacity = 1 << 20;
SaveFileResult save_mods_read(SaveSlot slot, std::string &json);
SaveFileResult save_mods_write(SaveSlot slot, std::string_view json);

// Which mods a slot's save was made with, beside it (slot1.modlist.json ...; verdite/mod_saved.h), as
// JSON text. Written with every save, so an empty list says "no mods"; a save with no file does not say.
SaveFileResult save_mod_list_read(SaveSlot slot, std::string &json);
SaveFileResult save_mod_list_write(SaveSlot slot, std::string_view json);

// The stock of mods' items and the state of their floors, kept beside a slot (slot1.content ...;
// port/ext/mod_content_save.h). Every platform with a save directory.
inline constexpr std::size_t save_content_capacity = 1 << 17;
SaveFileResult save_content_read(SaveSlot slot, std::vector<u8> &data);
SaveFileResult save_content_write(SaveSlot slot, std::span<const u8> data);

// PC extension data kept beside a slot (e.g. "slot1.kfs.map"). Desktop Windows only.
SaveFileResult save_side_read(SaveSlot slot, const char *suffix, u8 *data, std::size_t capacity, std::size_t *size);
SaveFileResult save_side_write(SaveSlot slot, const char *suffix, const u8 *data, std::size_t size);
}

#endif // KF_PLATFORM_SAVES_H
