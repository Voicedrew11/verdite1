#!/usr/bin/env python3
"""The menus' pictures (port/ui/art/*.png, white on clear) as the alpha masks the game carries (port/ui/res/*.alpha),
so the game needs no PNG decoder: holyland's format (verdite/ui.h: alpha_mask). The style sheet colours them
(image-color). Run after changing a picture; the masks are committed. Needs Pillow.

The pictures: logo.png is "Verdite" cut from Verdite2's wordmark (logo-japanese/assets/verdite2whi.png) and
logo_one.png its superscript 1, drawn to match the wordmark's 2 (its height and stroke, at the logo's scale); the
corners are holyland's (its tools/ui_ornaments.py)."""
import struct
import sys
from pathlib import Path

from PIL import Image

# (picture in port/ui/art/, mask in port/ui/res/). About 3 pixels a dp, so sharp up to 4K.
MASKS = [
    ("logo.png", "logo.alpha"),          # the sidebar's logo, about 210 dp wide
    ("logo_one.png", "logo_one.alpha"),  # its 1, at the same scale
    ("corner_tl.png", "corner_tl.alpha"),
    ("corner_tr.png", "corner_tr.alpha"),
    ("corner_bl.png", "corner_bl.alpha"),
    ("corner_br.png", "corner_br.alpha"),
]


def make_mask(source: Path, target: Path) -> None:
    alpha = Image.open(source).convert("RGBA").getchannel("A")
    target.write_bytes(b"KF4A" + struct.pack("<II", alpha.width, alpha.height) + alpha.tobytes())
    print(f"{target}: {alpha.width} x {alpha.height}")


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    for picture, mask in MASKS:
        make_mask(root / "port/ui/art" / picture, root / "port/ui/res" / mask)
    return 0


if __name__ == "__main__":
    sys.exit(main())
