# English resource support

The port supports the original Japanese SLPS-00017 resource set and a local
English v1.0 translation delta. John Osborne (weissvulf) announced the translation
in [the Agetec forum](https://www.tapatalk.com/groups/agetec/presenting-the-king-s-field-translation-patch-t1827.html).
The port derives its delta from the translator's PPF release at build time (see
[Translation payload](#translation-payload)). No original disc, translated image,
translation payload or extracted resource tree is stored in this repository.

## Selection and extraction

The user supplies only the original Japanese SLPS-00017 disc. On the first
launch, set `KF_DISC` to its ISO, BIN or CUE; English is selected by default
and prepared automatically. After import, `verdite1` and
`verdite1 --language ja` reuse the cache without the disc. `KF_LANGUAGE`
also selects a default; an explicit option overrides it.

Direct launches with `--data` also accept a verified Japanese tree and prepare
English in a temporary directory, keeping the Japanese files for switching back.

The launcher first imports and verifies all 428 Japanese files in
`$XDG_CACHE_HOME/kings-field/SLPS-00017/resources` (or `~/.cache/...`). It
creates `resources-en` from this base using an embedded translation delta.
The source tree is unchanged. Input and complete output hashes must match;
only verified output is published by renaming a temporary directory under the
import lock. Partial/failed imports never replace a cache. Saves remain shared
between languages: the save file holds no text.

The browser follows the same workflow and keeps Japanese resources in
`kings-field-resources` and English resources in `kings-field-resources-en`
IndexedDB databases. Selecting English can generate its missing cache from
previously imported Japanese files without selecting the disc again.

All files remain ordinary extracted resources. The three PS-X executables are
verified but never executed. Their bytes are retained for resource identity;
gameplay runs the native compiled sources. Already verified English trees remain
accepted for compatibility with earlier local imports.

## Switching during play

**Configuration → Language** sits below Compass and above Return. Left/right
or Confirm toggles Japanese and English while keeping the panel open. The
browser selector requests the same change. Requests made during other menus,
dialogue, or opening/ending scenes wait for gameplay or Configuration, where
no active dialogue/list keeps copied glyph rows from the previous language.

Switching changes the resource root, reloads the menu descriptions and names
from `STAT.DAT`, and updates the translated font and notification texels in
`COM/MIX.TIM`. It does not reload the floor, restart the game, alter player or
actor state, or write a save. Only texels that differ between the two common
TIM streams are replaced; later map and menu texture uploads are preserved.
Subsequent dialogue and image loads use the selected resource root.

The Language row uses the existing menu atlas and text renderer. English
reuses the individual capital letters used by BACK and COMPASS; Japanese
uses the original kana for `げんご` and `にほんご`.
The original four configuration toggles and their save representation remain
unchanged. Language is a session preference; launch options still select the
initial language.

If English is needed during play, the verified Japanese tree is converted into
a private temporary tree, reused for the remainder of that session and removed
on normal shutdown. Failed preparation leaves the active language unchanged
and removes partial temporary output. The launcher retains its Japanese cache;
the browser keeps the verified Japanese tree mounted alongside an English
startup tree. A direct launch from English resources can provide that base with
`--japanese-data DIRECTORY`. Without a Japanese base, switching back is
unavailable; without a translation payload, generating English is unavailable.

Runtime verification is available with `tests/runtime_scenarios.py
--switch-language`: each of six GAME entries switches both ways and checks that
player/actor/object state and the complete live texture store survive the round
trip, before comparing the ordinary saved states with a baseline.

## Translation payload

The English data is the translator's own release: John Osborne's
`Kings_Field_Jap_to_Eng_v1.0.rar` (a PPF 3.0 patch, December 2006). The flake
fetches it from the Wayback Machine copy of the translator's site, pinned by
SHA-256; nothing translated is stored in this repository or re-hosted. The
build converts the PPF into the file-level delta below with
`scripts/english_patch.py from-ppf`, using `scripts/slps-00017-layout.tsv`, the
committed table of the Japanese disc's 428 file extents (path, first
2352-byte sector, size). PPF bytes outside file data (sector headers,
EDC/ECC) are dropped. Applying the translator's PPF to the Japanese BIN
reproduces the English v1.0 image byte for byte, and the converted delta
reproduces all 428 English files.

`nix develop` exports the derived delta as `KF_ENGLISH_PATCH`, and the Nix
package passes it to CMake, so both embed English automatically. Outside Nix,
CMake falls back to
`english-v1.kfdelta` in its build directory, which `english_patch.py create`
or `from-ppf` can produce. A build without any payload still supports Japanese
and reports clearly that English generation is unavailable.

The translation's redistribution terms are not stated, so do not publish
binaries, caches or derived deltas that contain it. `layout` regenerates the
extent table from a Japanese BIN:

```sh
nix develop --command python3 scripts/english_patch.py layout \
  --disc /path/to/Japanese.bin --output scripts/slps-00017-layout.tsv
```

The versioned delta stores sorted resource paths, unchanged file lengths, and
ordered replacement spans. The decoder checks paths, lengths, offsets, ordering,
and complete consumption. It applies only after Japanese identity verification
and validates the full English identity afterward. No patch process or original
MIPS executable runs at game startup.

Direct executable workflows also support conversion:

```sh
build/linux/verdite1 --disc /path/to/Japanese.cue --language en \
  --extract-to /new/english-directory --extract-only
build/linux/verdite1 --data /path/to/japanese/disc --language en \
  --extract-to /new/english-directory --extract-only
```

Unlike the packaged launcher, these explicit extraction destinations must be new.
For subsequent direct launches use `--data /new/english-directory --language en`.

An import must match exactly 428 files and the SHA-256 of sorted normalized paths,
each encoded as LE32 path length, LE32 file length, path bytes, then file bytes:

| Resource set | Aggregate SHA-256 |
| --- | --- |
| Japanese SLPS-00017 | `450b9f09ca34bedc1c8bc150e01b79bfe108c700dad2b2783246b926953deee2` |
| English v1.0 | `697b2b80d13a3e6e54b2d72f90a49e29ae6a31d6d45970b40aee908b94208595` |

The inspected English BIN SHA-256 is
`bbad1218540001ebe8db64191268b14e65c96e3d8a4c55150603e214e329f431`.
Resource identity, rather than a raw BIN digest, also permits equivalent ISO
payloads and CUE-based imports. Unrecognized translation revisions are rejected.

## Where the text lives

The Japanese game already stores most dialogue and labels as graphics. The
translation reuses that system: TIM images, images inside resource containers,
and glyph sequences referencing the font atlas. There is no complete plain-text
dialogue script to extract. No OCR or replacement text renderer is needed.

Comparison with the hash-verified original found no added/removed files or size
changes: 147 files are unchanged; 280 resources and `GAME.EXE` differ.

| Changed resource group | Files |
| --- | ---: |
| Root `E0.`, `E1.`, `E2.` screens | 3 |
| `KF/B0/MIX3.`, `KF/B0/MIX9.` | 2 |
| `KF/COM/MIX.TIM`, `KF/COM/STAT.DAT` | 2 |
| `KF/ENE1`–`KF/ENE5` | 44 |
| `KF/KAN` | 39 |
| `KF/PRSN` | 17 |
| `KF/TALK` | 148 |
| `KF/TIM` | 25 |

Existing loaders retain ownership of all these formats. `STAT.DAT` supplies
window layouts, item/spell names and the font description; `MIX.TIM` supplies
the atlas. The existing renderers display translated dialogue, item descriptions,
plaques and screens without new gameplay logic.

## Executable-owned menu labels

`PSX.EXE` and `OPEN.EXE` are unchanged. `GAME.EXE` has 84 changed bytes in 64
aligned instruction words, all in eight menu functions. The original and
patched headers and program sizes are identical. Instructions, register reuse,
stack stores and original source were inspected using the pinned GAME evidence;
addresses here always refer to **GAME.EXE**, not the other two programs.

| Function | Start | Changed words | Source adaptation |
| --- | --- | ---: | --- |
| `item_pickup_confirm` | `0x80021ffc` | 5 | Pickup/cancel glyph rows |
| `menu_equip_select` | `0x800238d8` | 3 | Unequip label |
| `menu_spell_select` | `0x80023e9c` | 3 | Unequip label |
| `menu_draw_stats_header` | `0x80025f38` | 8 | Level/gold labels and status symbols |
| `menu_draw_status_details` | `0x800264d8` | 24 | Summary/component labels and status symbols |
| `menu_draw_item_detail` | `0x80027b7c` | 2 | Price-unit label |
| `menu_list_interact` | `0x80028380` | 14 | Use/drop/buy/sell/equip/cancel rows |
| `menu_two_option_prompt` | `0x800286d4` | 5 | Confirmation rows; unsafe patch writes excluded |

The port now uses scoped `MenuLabel` identities and complete `MenuGlyphRow`
values in these original callers. The Japanese visible sequences, positions,
selection rules and calculations remain unchanged. English rows apply the
patch's glyph choices, including values reused through saved registers (not
only the assignment nearest each changed instruction). Resource-owned rows
remain loaded directly from `STAT.DAT`; they are not globally remapped.

The patch's `0x41xx` codes select the low twelve-bit atlas glyph, as in the
existing renderer; bit `0x4000` has no rendering effect. No new shader or font
rendering path is needed.

## Deliberate safety corrections

The translation is not an executable-behavior parity target. Its menu text edits
contain defects that must not become native memory behavior:

- At `0x80028780`, a store of `$v1` to the decline row becomes a store of `$at`.
  At `0x80028784`, the next store uses `$t5` as its base instead of `$sp`.
  Neither register is a defined label value/destination in that prompt's local
  setup; the latter no longer initializes the intended stack field. The port
  uses the same valid English `OK` / `NO` glyphs as the other confirmations.
- The generic list's yes/no branch was not translated by the patch. It uses
  those same English confirmation rows in the port, with unchanged choices.
- At `0x80026d94` and `0x80026ed4`, the patch replaces a string terminator with
  glyph `0x79` without adding a new terminator. Native rows explicitly terminate
  after the extended label and initialize their remaining storage. The longer
  rows inherited by the fire labels are handled the same way.

These corrections affect text initialization only. Status effects, combat,
prices, inventory and save operations are not reimplemented or modified.

## Verification

`python3 -m unittest discover -s tests -v` covers the delta decoder (including
all truncations of a small fixture, invalid paths, bounds, overlap and version),
the generator, and first import/cache reuse/language switching/failure handling.
These tests need no retail data and run the C++ decoder under ASan/UBSan.

For supplied local assets, run:

```sh
python3 tests/resource_import.py --binary build/sanitize/verdite1 \
  --disc /path/to/Japanese.cue --english-reference /path/to/english/disc
```

It compares both disc and extracted-tree conversion with every reference file,
checks source preservation and rejects corrupt/symlink inputs and existing
outputs. `tests/runtime_scenarios.py --language en` exercises the same real
floor/save/transition scenarios with English resources. Full English dialogue,
all menus and a full playthrough still need manual review.

Local validation passed 27 regression tests, Linux and WASM builds, Ruff, and
`nix flake check -L`. ASan/UBSan remained enabled; leak detection was disabled
because LeakSanitizer cannot run under the execution environment's ptrace.
The runtime language scenario completed all five floors and six GAME entries,
switched Japanese → English → Japanese in each entry, preserved player/actor/
object state and all live texture words, and produced six saves identical to
the Japanese baseline. Native menu captures checked the added row in both
languages. Headless Chromium verified imports, both cache restores, English
startup with Japanese available, first English generation during Japanese
gameplay, repeated live switches, deferred requests from other menus, and
the Configuration controls. This is not a full playthrough or an audible-audio
check. Earlier disc and directory conversions also matched all 428 English
reference files byte for byte.

The browser resource smoke test can be repeated with:

```sh
node tests/browser_resources.mjs build/wasm /path/to/Japanese.bin
```
