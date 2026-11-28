#!/usr/bin/env python3
"""Makes the interface font, libverdite/res/fonts/HolylandMincho.ttf (holyland's menu font, shared), from IPA P Mincho: the
proportional cut of IPA Mincho, the font the game's own letters are drawn again from (tools/make_font_letters.py, which
uses the fixed-width cut for the game's letter cells), so the menus read as the game does.

    python3 tools/make_menu_font.py path/to/ipamp.ttf

Needs fontTools (pip install fonttools). ipamp.ttf is IPA P Mincho 003.03 (Fedora: the ipa-pmincho-fonts package;
elsewhere https://moji.or.jp/ipafont/). What it changes, and why:
- only the Latin letters and punctuation are kept (Basic Latin, Latin-1, General Punctuation, a few symbols the menu
  writes): the whole font is 8 MB of Japanese the menus never show, and every byte is carried inside the game;
- any embedded bitmaps (EBDT, EBLC, EBSC; IPA Mincho has them, IPA P Mincho 003.03 does not) are dropped: FreeType
  draws them in place of the outlines at small sizes, as one-bit pictures with no smoothing; without them every size is
  drawn from the outlines, anti-aliased;
- it is renamed "Holyland Mincho": the IPA Font License (third_party/ipa-fonts/) lets a changed font be shipped under
  its terms, under a name without the original's.

To use the original font instead, put IPA P Mincho's ipamp.ttf in its place under this file name; the menu draws it
the same.
"""
import argparse
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "port", "ui", "res", "fonts", "HolylandMincho.ttf")
FAMILY = "Holyland Mincho"

KEPT = [
    (0x20, 0x7E),  # Basic Latin
    (0xA0, 0xFF),  # Latin-1
    (0x2010, 0x2027),  # dashes, quotes, the ellipsis
    (0x2030, 0x203A),  # per mille, primes, angle quotes
    (0x2190, 0x2193),  # arrows
    (0x2212, 0x2212),  # minus
    (0x00D7, 0x00D7),  # multiplication
]


def renamed(font: TTFont) -> None:
    names = font["name"]
    note = (f"{FAMILY}: IPA P Mincho 003.03 cut down to its Latin letters, without its embedded bitmaps "
            "(tools/make_menu_font.py). Licensed under the IPA Font License Agreement v1.0.")
    for record in list(names.names):
        if record.nameID in (1, 4, 16, 21):
            names.setName(FAMILY, record.nameID, record.platformID, record.platEncID, record.langID)
        elif record.nameID == 6:
            names.setName(FAMILY.replace(" ", ""), record.nameID, record.platformID, record.platEncID, record.langID)
        elif record.nameID == 3:
            names.setName(FAMILY + " 003.03", record.nameID, record.platformID, record.platEncID, record.langID)
        elif record.nameID in (17, 22):
            names.setName("Regular", record.nameID, record.platformID, record.platEncID, record.langID)
        elif record.nameID == 10:
            names.setName(note, record.nameID, record.platformID, record.platEncID, record.langID)
    # (the Japanese names, which hold the original's, go: only English ones are left)
    names.names = [r for r in names.names if r.langID in (0x409, 0) or r.platformID == 1 and r.langID == 0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("ttf")
    args = parser.parse_args()

    options = subset.Options()
    options.drop_tables += ["EBDT", "EBLC", "EBSC", "vhea", "vmtx", "VORG", "GSUB", "GPOS", "morx", "mort"]
    options.name_IDs = ["*"]
    options.name_languages = ["*"]
    options.hinting = True
    options.notdef_outline = True
    font = TTFont(args.ttf)
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=[u for lo, hi in KEPT for u in range(lo, hi + 1)])
    subsetter.subset(font)
    renamed(font)
    font.save(OUT)
    print(f"{OUT}: {os.path.getsize(OUT)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
