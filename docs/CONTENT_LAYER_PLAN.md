# The content layer (modding API step 10): plan and record

Written and built 2026-11-02 (API 0.16). Step 10 of [MODDING.md](MODDING.md#order), the last: new items, monster
kinds and floors beside the disc's. The surface is in [MODDING.md](MODDING.md#kfcontent).

## What makes this harder than raising a number

- **An item id is not only an item id.** Items and map objects share one byte: 0 to 79 are items, 81 to 139 fittings,
  255 none. The game tells them apart by `id < 80` in many places. So a mod's ids start above the objects' table, at 160.
- **The card is the PlayStation's bytes,** with 80 bytes a stock bank and five floors, kept so a save stays a real save.
- **Coop sends 80-byte packs,** and a stranger's item must not reach a room.
- **A floor's files are trusted.** The game stops with `host_fail` on much that is wrong in them, and the rest it
  indexes with. A mod comes from a stranger, and a floor is two megabytes of data the game reads as tables.
- **Numbers must outlive mods.** A save holds a byte. If an id were handed out by load order, turning a mod off would
  turn one item into another.

## Decided

The user asked for the step and left the design open, so these are this author's choices.

1. **Old names keep the disc's meaning.** `KF_ITEM_COUNT` (80), `KF_MAP_OBJECT_DEFINITION_COUNT` (160) and
   `KF_ACTOR_DEFINITION_COUNT` (12) still say what the disc, the card and coop hold; every check using them stays
   as strict as it was. The tables grew under new names (`KF_ITEM_ID_COUNT`, `KF_MAP_OBJECT_ID_COUNT`,
   `KF_ACTOR_DEFINITION_CAPACITY`), and only the sites that must take a mod's id changed, to `kf_item_id_valid`. `kf.call`
   and `kf.hook` ranges are unchanged, so they do not take a mod's ids.
2. **A mod's item is an extra item.** The Message Stone's plumbing already spelled a name and borrowed a model, so
   the menus changed little. On the ground and in the menus a mod's item uses a disc item's model.
3. **Ids are kept by key on this machine** (`verdite/content_ids.h`, `mods/.content_ids`), never reused. The other
   choice, a table in each save and rewriting ids on load, has to reach into every floor's records. The cost: a save
   copied to another machine may name other items.
4. **A mod that is gone leaves its things.** Stock stays hidden, the ground still shows something, and a floor's files
   stay held until the game exits, so a player standing on one can reload it.
5. **Side file, not a bigger card.** `slot1.content` holds stock past id 79 and the floors' records.
6. **A kind copies a kind.** Its model and clips are its base's, which holds the rule the raw layer found: a clip must
   be its model's. Numbers go through `kf.rules`, so they are the fields already checked and fuzzed.
7. **Floors have no people or signs yet.** Their pictures and pages are disc files named by numbers in the floor's
   data, which fail the game when missing. `tools/mod_floor_from_disc.py` strips them.
8. **The highest floor stays the disc's.** It drives dialogue stages; a side trip is no progress in the story.

## How a floor is checked

`port/ext/mod_floor_check.cpp` runs at `kf.content.floor`, before a byte is in use, walking the files as
`map_resources_load` does, with the same decoders where they throw and the same tests where the game stops:

- sizes no larger than the disc's largest, since the game's buffers are made for those;
- the two `.DAT`s' chunks, the sound bank, every model (the `.TMD` check) and animation;
- grid values, sprites, object and monster placements, links as `map_saved_link_valid` tests them, and how many
  links a save's 1,690 bytes can hold;
- each kind in use: its model is on the floor and each clip is its model's;
- at least the effect models and clips the disc's floors have, read from the disc: the game asks for those by number.

What cannot be read from the bytes is held by the rules above, and by the fuzzer below.

## Where the code goes

- **`verdite/content_ids.h`**: keys to numbers, kept from run to run. Game-agnostic.
- **`port/ext/mod_content.cpp`**: `kf.content.item` and `floor`, the rows, the side file's reading and writing.
  `port/ext/mod_content_save.cpp`: its pure codec. `port/ext/mod_floor_check.cpp`: the check.
- **`port/ext/mods.cpp`**: `kf.content.monster`, beside `kf.actors`, and `on_use_item`.
- **`kf/platform/content.h`, `kf/platform/extra_items.h`**: what game code asks. The rest is in `patches/`.

## Checks

- Unit tests of the ids and the side file's codec (`libverdite/tests/content_ids.cpp`, `port/tests/mod_content_save.cpp`)
  and of the check's refusals on made-up files (`port/tests/mod_floor_check.cpp`), under ASan and UBSan.
- `scripts/check.sh` in full.
- A probe mod, headless on the plain build: an item given, dropped and picked up, a kind spawned, a floor made by the tool
  entered, quick saved and quick loaded (the stock came back from the side file), left and entered again, the highest
  floor unchanged at 1, bad rows refused with their reasons, a clean exit.
- `fuzz_mod_floor`, the libFuzzer target over the check and the side file's decoder: 300 s, no finding.
- `--preset fuzz --content 6 --mix --vary --ticks 20000`, under ASan, UBSan and checked containers, on the final build. Each run's
  mod adds six items, four kinds a floor, a floor from the tool and six mutants of it, which the check accepts or refuses:

  | with | seeds | floors accepted | refused | loads of a mod's floor | mod items used in menus | result |
  |---|---|---:|---:|---:|---:|---|
  | nothing more | 20000-20095 | 432 | 240 | 364 | 31 | 96 clean |
  | `--raw 4000` | 21000-21095 | 443 | 229 | 362 | 53 | 94 clean, 2 stalls |

  The 2 stalls came under random raw writes, with the game in its `opening` context on floor 1, as the hook layer's campaigns saw too; they were not traced further.

Found on the way: `map_object_pool_find_interaction_from` read past the pool when the object in its last slot matched, since
its callers resume from the next slot; a mod's drops reach that slot, and any game could. A game patch stops it.

Not checked: how any of it looks or sounds in play, a build for Android or Windows, and every path through a mutant: a run plays a
few minutes of each floor, so what the fuzzer never touches is not proved safe. Menu use was reached 84 times, and a shop's list of a
mod's items was not confirmed reached. The disc's floors were not compared with the build before; the smoke runs and campaigns
show only that the game still plays.

## Left for later

- **Items to equip.** Weapon and armour records are indexed by id, a weapon's model is a `.MIM` with clips asked for
  by number, and the coop and save checks name their ranges.
- **A model of an item's own.** On the ground an item is an object in the floor's model, so it needs a second path.
- **Monster models from elsewhere,** and kinds from another floor: a floor loads its own models and textures.
- **People, events and signs on a mod's floor.** A mod can script without them, using `kf.warp` and `on_update`.
- **Taking a mod's id in `kf.call`,** which means reading what each function does with an item.
- **An editor for floors.** There is none; the tool copies the disc's.
