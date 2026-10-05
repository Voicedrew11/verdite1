#!/usr/bin/env python3
"""Locate a Verdite game repository and load its config/verdite.json.

Shared scripts carry no game values of their own: the disc image, the
recompiler config and the function-map directory all come from the game.
VERDITE_GAME_ROOT names the game repo when a script is not run from inside it;
otherwise the first directory at or above the current one holding
config/verdite.json is the repo root.
"""

import json
import os
from pathlib import Path


def find_root(start=None) -> Path:
    env = os.environ.get("VERDITE_GAME_ROOT")
    if env:
        return Path(env).resolve()
    here = Path(start or Path.cwd()).resolve()
    for candidate in [here, *here.parents]:
        if (candidate / "config" / "verdite.json").is_file():
            return candidate
    raise SystemExit(
        "config/verdite.json not found; set VERDITE_GAME_ROOT to the game repo")


def get(key, default):
    """One value from config/verdite.json, or default when the game has none yet."""
    try:
        return load()[1].get(key, default)
    except SystemExit:
        return default


def load():
    """Return (repo root, the parsed config/verdite.json)."""
    root = find_root()
    return root, json.loads((root / "config" / "verdite.json").read_text())
