#!/usr/bin/env python3
"""Write the PSY-Q names a signature match found into the game's function maps.

The sweep names everything positionally (`func_800xxxxx`), which is why
`SdkPatches` reports `applied 0 reimplementations` and every SDK entry point has
to be bound by address in the recompiler config's `patches[]`. Upstream's
`--autoconfigure` matches a signature bank against the same functions and hands
back real names for about half of each overlay.

**This script deliberately refuses the names that would bind.** `SdkPatches`
matches by name, so naming a function it knows reroutes that function to the
runtime's HLE -- a behaviour change, not a rename. Everything in HLE_NAMES is
therefore skipped and stays `func_`-named, so `patches[]` keeps its 63 entries
and the CD, GPU and VRAM paths are untouched. What lands is the rest: libgte,
libsnd, libspu, libc and the libgpu routines that are not entry points, which
nothing binds and which only ever appear as identifiers in `generated/`.

  python3 scripts/merge_sdk_names.py --auto <dir> [--dry-run]

<dir> holds the funcmaps `--autoconfigure` wrote, one per SDK overlay.
"""
import argparse, json, os, re, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import verdite_game

# Every name SdkPatches binds on the pinned recompiler, plus the aliases that
# name the same routines. Naming any of these would reroute it to HLE.
HLE_NAMES = set("""
CdInit CdReset CdControl CdControlF CdControlB CdSync CdReady CdRead CdReadSync
CdGetSector CdDataSync CdSearchFile CdSyncCallback CdReadyCallback CdReadCallback
CdDataCallback CdStatus CdMode CdLastCom CdMix CdFlush
CdGetSector2 CD_getsector2
DecDCTin DecDCTout DecDCTinSync DecDCToutSync DecDCToutCallback
VSync
DrawOTag DrawSync PutDrawEnv PutDispEnv LoadImage StoreImage MoveImage ClearImage
StSetRing StClearRing StUnSetRing StSetStream StSetMask StGetNext StFreeRing
StGetBackloc
PadInitDirect PadStartCom PadStopCom PadEnableCom PadChkVsync PadChkMtap
PadGetState PadInfoMode PadInfoAct PadInfoComb PadSetMainMode PadSetActAlign
PadSetAct
CD_sync CD_ready CD_datasync CD_getsector
MemCardInit MemCardEnd MemCardStart MemCardStop MemCardExist MemCardAccept
MemCardOpen MemCardClose MemCardReadData MemCardWriteData MemCardReadFile
MemCardWriteFile MemCardCreateFile MemCardDeleteFile MemCardFormat
MemCardUnformat MemCardGetDirentry MemCardSync MemCardCallback
_patch_card _patch_card2 _patch_pad _patch_bios
""".split())

IDENT = re.compile(r'^[A-Za-z_][A-Za-z0-9_]*$')

SDK_PATCHES = "tools/RecompOne/RecompOne.Recompiler/CodeGen/SdkPatches.cs"


def bound_names(path=SDK_PATCHES):
    """The names SdkPatches binds, read out of the patched checkout.

    The list above is a copy, and a copy drifts: it was first written from the
    pristine pin and so missed `DMACallback`, which `0004-libapi-dma-callbacks`
    adds -- with the result that the matcher's name for it bound a *second*
    address alongside the one `patches[]` already binds by hand. Reading the
    table the recompiler will actually use cannot drift; the copy stays as the
    fallback for a tree with no checkout, and as cover for names a later
    upstream adds.
    """
    try:
        with open(path) as fh:
            src = fh.read()
    except OSError:
        print(f"note: no {path}, falling back to the copied name list")
        return set(HLE_NAMES)

    block = re.search(r'Libraries\s*=\s*\{(.*?)\n    \};', src, re.S)
    if not block:
        print(f"note: could not read the bind table in {path}, using the copied list")
        return set(HLE_NAMES)

    found = {n for n in re.findall(r'"([A-Za-z_][A-Za-z0-9_]*)"', block.group(1))
             if not n.startswith("RecompOne")}
    missing = found - HLE_NAMES
    if missing:
        print(f"note: {path} also binds {', '.join(sorted(missing))} -- refusing those too")
    return found | HLE_NAMES


def load(path):
    with open(path) as fh:
        d = json.load(fh)
    return d if isinstance(d, dict) else {"functions": d}


def addr(entry):
    a = entry["address"]
    return int(a, 16) if isinstance(a, str) else int(a)


def main():
    root, game = verdite_game.load()
    ap = argparse.ArgumentParser()
    ap.add_argument("--auto", required=True, help="directory of autoconfigure funcmaps")
    ap.add_argument("--maps", default=os.path.join(root, game["funcmaps"]))
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    hle_names = bound_names()
    total = skipped_hle = 0
    for ov in game["sdkOverlays"]:
        src = os.path.join(args.auto, ov + ".json")
        dst = os.path.join(args.maps, ov + ".json")
        if not os.path.exists(src):
            print(f"{ov}: no {src}, skipped")
            continue

        matched = load(src)["functions"]
        names = {addr(f): f["name"] for f in matched}

        # A signature that matches twice in one overlay has matched something
        # wrong at least once, and there is nothing here that says which. A name
        # is only worth having if it is right -- the whole point is that a
        # stack trace can be read at face value -- so an ambiguous one is
        # refused at both addresses rather than awarded to whichever came first.
        seen = {}
        for f in matched:
            if not f["name"].startswith("func_"):
                seen[f["name"]] = seen.get(f["name"], 0) + 1
        ambiguous = {n for n, k in seen.items() if k > 1}

        doc = load(dst)

        renamed = hle = collide = 0
        # A name has to be unique inside the overlay: it becomes a C# method.
        taken = {f["name"] for f in doc["functions"]}
        for f in doc["functions"]:
            want = names.get(addr(f))
            if want is None or want.startswith("func_"):
                continue
            if not f["name"].startswith("func_"):
                continue                      # a hand-given name outranks the match
            if want in hle_names:
                hle += 1
                continue
            if want in ambiguous or not IDENT.match(want) or want in taken:
                collide += 1
                continue
            taken.discard(f["name"])
            taken.add(want)
            f["name"] = want
            renamed += 1

        total += renamed
        skipped_hle += hle
        print(f"{ov}: {renamed} renamed, {hle} left func_-named because SdkPatches "
              f"binds them, {collide} refused (matched twice, or not an identifier)")

        if not args.dry_run:
            with open(dst, "w") as fh:
                json.dump(doc, fh, indent=2)
                fh.write("\n")

    print(f"total: {total} renamed, {skipped_hle} deliberately not")
    return 0


if __name__ == "__main__":
    sys.exit(main())
