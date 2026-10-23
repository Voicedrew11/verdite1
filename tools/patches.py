#!/usr/bin/env python3
"""Keep Verdite1's edits to the King's Field decompilation's game code as patches.

Adapted from holyland's tools/patches.py. decomp/ is a git submodule holding
sushi-shi's source port (our fork, pinned on `port`). It is never edited.
Verdite's changes to the game code live in patches/ as patch files whose paths
are relative to the decomp root, so `git apply` at the root of a copy of decomp
applies them: one patch per COPIED directory, at patches/<directory>.patch
(patches/src/game.patch, patches/include/kf/game.patch, ...).

The decomp's platform layer, renderer and headers (src/platform, src/renderer,
include/kf/platform, include/kf/renderer) are not copied: Verdite owns its own
in port/.

Commands:

  apply    Build a work copy (work/ by default). It holds COPIED at the
           submodule's commit, committed as "pristine decomp <sha>" and
           tagged `pristine`, then the patches, committed as "patches".
           Edit it freely. An existing work copy with unsaved changes is
           refused unless --force is given; only empty directories and
           work copies made by patches.py are ever replaced.
           If a patch does not apply, the work copy is tagged `incomplete`
           and save refuses it; fix the patch and run apply again.

  save     Regenerate patches/ from the work copy's changes. Each patch is
           the diff of the work copy against the `pristine` tag, so it works
           on any copy made by apply or upgrade. The saved state is
           committed in the work copy as "patches".

  upgrade  Move decomp/ to another commit and carry the patches along.
           Renames of files (from `git diff -M`) and of symbols (matched by
           address in config/symbol_addrs.txt, when the decomp has one; the
           source port did not on 2026-10-22) are applied to the old tree.
           The old tree plus patches plus renames (B) is diffed against the
           old tree plus renames (A), and that diff is replayed with
           `git apply -3` onto the new pristine tree (C). Without conflicts,
           the submodule moves and patches/ is regenerated. With conflicts,
           nothing in the repo changes: the scratch repo is kept, you resolve
           it there, then run `save --from DIR` and check out the new commit
           in decomp/ yourself.

Typical loop:

  python3 tools/patches.py apply
  ... edit work/ (new files and edits are both fine) ...
  python3 tools/patches.py save

Work copies are their own git repos, so always use `git -C work ...`.
"""

import argparse
import io
import re
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import textwrap
from collections import Counter
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
COPIED = ["include/kf/audio", "include/kf/cutscene", "include/kf/game", "include/kf/lib",
          "src/audio", "src/cutscene", "src/game", "src/lib"]
PRISTINE = "pristine"
INCOMPLETE = "incomplete"  # tag on a work copy whose patches did not all apply
SYMBOLS = "config/symbol_addrs.txt"
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
SYMBOL_LINE = re.compile(r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(0x[0-9A-Fa-f]+)\s*;")


class Fail(Exception):
    """A user-facing error: printed without a traceback."""


# --- git and filesystem helpers ---------------------------------------------

def rmtree(path):
    """shutil.rmtree that also removes read-only files (git's objects are read-only on Windows)."""
    def make_writable(func, target, _):
        os.chmod(target, stat.S_IWRITE)
        func(target)
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=make_writable)
    else:
        shutil.rmtree(path, onerror=make_writable)


def git(repo, *args, stdin=None, check=True):
    proc = subprocess.run(["git", "-C", str(repo), *args], input=stdin, capture_output=True)
    if check and proc.returncode:
        err = proc.stderr.decode(errors="replace").strip()
        raise Fail(f"git {args[0]} failed in {repo}:\n{err}")
    return proc


def out_text(proc):
    return proc.stdout.decode(errors="replace").strip()


def head(repo):
    return out_text(git(repo, "rev-parse", "HEAD"))


def commit(repo, message):
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "--allow-empty", "-m", message)


def init_repo(path):
    path.mkdir(parents=True, exist_ok=True)
    git(path, "init", "-q")
    git(path, "config", "user.name", "patches.py")
    git(path, "config", "user.email", "patches.py@localhost")
    git(path, "config", "core.autocrlf", "false")


def has_ref(repo, ref):
    """True if repo is its own git repo (not a directory inside another) and has ref."""
    if not (repo.is_dir() and (repo / ".git").exists()):
        return False
    return git(repo, "rev-parse", "-q", "--verify", ref, check=False).returncode == 0


def clean_dir(path, root):
    """Create an empty directory at path, replacing whatever is there.

    An existing directory is replaced only if it is empty or a work copy made by
    patches.py (a repo tagged `pristine`, or the upgrade scratch with `old-patched`).
    """
    path = path.resolve()
    unsafe = [root / "decomp", root / "patches", root / ".git"]
    if path == root or path in root.parents or any(path.is_relative_to(u) for u in unsafe):
        raise Fail(f"refusing to replace {path}")
    if path.exists():
        is_ours = has_ref(path, f"refs/tags/{PRISTINE}") or has_ref(path, "refs/heads/old-patched")
        if path.is_file() or (path.is_dir() and next(path.iterdir(), None) is not None and not is_ours):
            raise Fail(f"refusing to replace {path}: not a work copy made by patches.py (remove it yourself)")
        rmtree(path)
    path.mkdir(parents=True)


def clear_worktree(work):
    for child in work.iterdir():
        if child.name == ".git":
            continue
        if child.is_dir() and not child.is_symlink():
            rmtree(child)
        else:
            child.unlink()


def resolve_arg(value, root, default=None):
    """An explicit DIR is relative to the current directory; the default is under the repo root."""
    if value is None:
        return root / default
    return Path(value).resolve()


def in_copied(path):
    return any(path == c or path.startswith(c + "/") for c in COPIED)


# --- building a work copy ----------------------------------------------------

def extract_rev(root, rev, dest):
    """Extract COPIED from the submodule at rev (committed content, not its working tree)."""
    decomp = root / "decomp"
    if not (decomp / ".git").exists():
        raise Fail(f"{decomp} is not checked out (git submodule update --init decomp)")
    sha = out_text(git(decomp, "rev-parse", "--verify", f"{rev}^{{commit}}"))
    # Not every commit has every COPIED path (src/nonmatching may be absent), so ask first.
    present = [c for c in COPIED if out_text(git(decomp, "ls-tree", "--name-only", sha, "--", c))]
    if present:
        tar = git(decomp, "-c", "core.autocrlf=false", "archive", "--format=tar", sha, "--", *present).stdout
        with tarfile.open(fileobj=io.BytesIO(tar)) as archive:
            if hasattr(tarfile, "data_filter"):
                archive.extractall(dest, filter="data")
            else:
                archive.extractall(dest)
        # Stage the executable bits from the decomp's tree: on Windows (core.filemode false) git
        # cannot see them on disk, and the patches would flip those files' modes.
        tree = out_text(git(decomp, "ls-tree", "-r", sha, "--", *present)).splitlines()
        executable = [line.split("\t", 1)[1] for line in tree if line.startswith("100755 ")]
        git(dest, "add", "-A")
        if executable:
            git(dest, "update-index", "--chmod=+x", "--", *executable)
    return sha


def make_pristine(work, root, rev):
    """Make work a fresh repo whose only commit is COPIED at rev. Returns the sha of rev."""
    init_repo(work)
    sha = extract_rev(root, rev, work)
    commit(work, f"pristine decomp {sha}")
    return sha


def patch_files(root):
    base = root / "patches"
    if not base.is_dir():
        return []
    return sorted(base.rglob("*.patch"), key=lambda p: p.relative_to(base).as_posix())


def apply_patches(work, root):
    for patch in patch_files(root):
        name = patch.relative_to(root / "patches").as_posix()
        if not patch.read_bytes().strip():
            continue
        proc = git(work, "apply", "--index", "--whitespace=nowarn", str(patch), check=False)
        if proc.returncode:
            err = proc.stderr.decode(errors="replace").strip()
            raise Fail(f"patch {name} does not apply to the pristine copy "
                       f"(work copy left at {work}):\n{err}")
    commit(work, "patches")


def has_unsaved(work):
    """True if work has changes not saved to patches/: uncommitted, or commits not made by apply or save."""
    if out_text(git(work, "status", "--porcelain")):
        return True
    subjects = out_text(git(work, "log", "--format=%s", f"{PRISTINE}..HEAD")).splitlines()
    return any(s != "patches" for s in subjects)


def cmd_apply(args, root):
    work = resolve_arg(args.out, root, "work")
    incomplete = has_ref(work, f"refs/tags/{INCOMPLETE}")
    if not args.force and not incomplete and has_ref(work, f"refs/tags/{PRISTINE}") and has_unsaved(work):
        raise Fail(f"work copy {work} has unsaved changes: run `python3 tools/patches.py save` first, "
                   "or pass --force to discard them")
    clean_dir(work, root)
    sha = make_pristine(work, root, "HEAD")
    git(work, "tag", PRISTINE)
    git(work, "tag", INCOMPLETE)
    apply_patches(work, root)
    git(work, "tag", "-d", INCOMPLETE)
    print(f"work copy: {work}  (pristine decomp {sha[:12]} + patches)")


# --- patches/ from a work copy -----------------------------------------------

def patch_specs():
    """(patch file relative to patches/, git pathspec) for every patch file save manages."""
    return [(f"{c}.patch", c) for c in COPIED]


def save(work, root, base=PRISTINE):
    if git(work, "rev-parse", "-q", "--verify", f"{base}^{{commit}}", check=False).returncode:
        raise Fail(f"{work} has no `{base}` tag: it is not a work copy made by apply or upgrade")
    if has_ref(work, f"refs/tags/{INCOMPLETE}"):
        raise Fail(f"{work} is incomplete: its patches did not all apply, so saving it would drop "
                   "the rest. Fix the patch, then run apply again")
    if out_text(git(work, "diff", "--name-only", "--diff-filter=U")):
        raise Fail(f"{work} has unresolved conflicts: resolve them and `git add` the files first")
    git(work, "add", "-A")

    patches_dir = root / "patches"
    changed = []
    for name, pathspec in patch_specs():
        target = patches_dir / name
        data = git(work, "diff", "--cached", "--binary", "--full-index", "--no-renames",
                   "--no-color", "--no-ext-diff", "--src-prefix=a/", "--dst-prefix=b/",
                   base, "--", pathspec).stdout
        old = target.read_bytes() if target.exists() else None
        new = data if data.strip() else None
        if new == old:
            continue
        if new is None:
            target.unlink()
            changed.append(f"  removed  {name}")
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(new)
            changed.append(f"  {'new' if old is None else 'updated':8} {name}")
    # what is saved is committed, so apply can tell saved work from unsaved
    if git(work, "diff", "--cached", "--quiet", check=False).returncode:
        git(work, "commit", "-q", "-m", "patches")
    return changed


def cmd_save(args, root):
    work = resolve_arg(args.source, root, "work")
    changed = save(work, root)
    print(f"saved from {work}:")
    print("\n".join(changed) if changed else "  no patch files changed")


# --- upgrading decomp/ --------------------------------------------------------

def resolve_commit(decomp, name):
    for candidate in (name, f"origin/{name}"):
        proc = git(decomp, "rev-parse", "-q", "--verify", f"{candidate}^{{commit}}", check=False)
        if proc.returncode == 0:
            return out_text(proc)
    raise Fail(f"cannot resolve {name} in decomp/ (is it fetched?)")


def file_renames(root, old, new):
    """old path -> new path for files renamed between old and new, within COPIED."""
    proc = git(root / "decomp", "diff", "-M", "--name-status", old, new, "--", *COPIED)
    renames = {}
    for line in out_text(proc).splitlines():
        fields = line.split("\t")
        if fields[0].startswith("R") and len(fields) == 3 and in_copied(fields[1]) and in_copied(fields[2]):
            renames[fields[1]] = fields[2]
    return renames


def symbol_table(text):
    """address -> set of names, from lines like `name = 0xADDRESS; // type:func`."""
    table = {}
    for line in text.splitlines():
        m = SYMBOL_LINE.match(line)
        if m:
            table.setdefault(int(m.group(2), 16), set()).add(m.group(1))
    return table


def symbol_renames(root, old, new):
    """Old name -> new name for symbols that moved to a new name at the same address.

    Returns (mapping, skipped). Addresses with several names on either side, names
    still in use at new (ambiguous), and non-identifiers are skipped and counted.
    """
    decomp = root / "decomp"
    old_proc = git(decomp, "show", f"{old}:{SYMBOLS}", check=False)
    new_proc = git(decomp, "show", f"{new}:{SYMBOLS}", check=False)
    if old_proc.returncode or new_proc.returncode:
        return {}, 0  # no symbol table at one end: renames come up as conflicts or compile errors
    old_table = symbol_table(old_proc.stdout.decode(errors="replace"))
    new_table = symbol_table(new_proc.stdout.decode(errors="replace"))
    new_names = set().union(*new_table.values()) if new_table else set()

    candidates = {}
    for addr, names in old_table.items():
        if len(names) != 1:
            continue
        (old_name,) = names
        targets = new_table.get(addr, set())
        if len(targets) != 1 or targets == {old_name}:
            continue
        candidates.setdefault(old_name, set()).update(targets)

    mapping, skipped = {}, 0
    for old_name, targets in candidates.items():
        (target,) = targets if len(targets) == 1 else (None,)
        if target is None or old_name in new_names or not IDENT.match(old_name) or not IDENT.match(target):
            skipped += 1
            continue
        mapping[old_name] = target
    targets_seen = Counter(mapping.values())
    for old_name in [k for k, v in mapping.items() if targets_seen[v] > 1]:
        del mapping[old_name]
        skipped += 1
    return mapping, skipped


def rewrite_tree(work, file_map, sym_map):
    """Move renamed files, then rewrite symbol names in every text file of COPIED."""
    for old, new in file_map.items():
        src = work / old
        if src.is_file():
            (work / new).parent.mkdir(parents=True, exist_ok=True)
            src.rename(work / new)
    if not sym_map:
        return
    pattern = re.compile(r"\b(" + "|".join(re.escape(k) for k in sym_map) + r")\b")
    for c in COPIED:
        base = work / c
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if not path.is_file():
                continue
            data = path.read_bytes()
            if b"\0" in data:
                continue
            try:
                text = data.decode("utf-8")
            except UnicodeDecodeError:
                continue
            new_text = pattern.sub(lambda m: sym_map[m.group(1)], text)
            if new_text != text:
                path.write_bytes(new_text.encode("utf-8"))


def build_upgrade(work, root, old, new, file_map, sym_map):
    """Scratch repo: A = old + renames (on P0), B = old + patches + renames, C = new (on A)."""
    make_pristine(work, root, old)
    p0 = head(work)
    rewrite_tree(work, file_map, sym_map)
    commit(work, "A: old, renamed")
    a = head(work)

    git(work, "checkout", "-q", "-f", "--detach", p0)
    apply_patches(work, root)
    rewrite_tree(work, file_map, sym_map)
    commit(work, "B: old, patches, renamed")
    b = head(work)
    git(work, "branch", "old-renamed", a)  # keep A and B reachable for inspection
    git(work, "branch", "old-patched", b)

    git(work, "checkout", "-q", "-f", "--detach", a)
    clear_worktree(work)
    sha = extract_rev(root, new, work)
    commit(work, f"pristine decomp {sha}")
    git(work, "tag", PRISTINE)
    return a, b


def split_pieces(diff):
    """Split a git diff into (path, chunk) per file."""
    pieces = []
    for chunk in re.split(rb"(?m)^(?=diff --git )", diff):
        if not chunk.strip():
            continue
        m = re.match(rb"diff --git a/(.*) b/(.*)\n", chunk)
        pieces.append((m.group(2).decode(errors="replace"), chunk))
    return pieces


def count_markers(path):
    if not path.is_file():
        return 0
    return sum(1 for line in path.read_bytes().splitlines() if line.startswith(b"<<<<<<<"))


def cmd_upgrade(args, root):
    decomp = root / "decomp"
    old = out_text(git(decomp, "rev-parse", "HEAD"))
    if git(decomp, "fetch", "-q", "origin", check=False).returncode:
        print("warning: fetch from origin failed; resolving from local objects", file=sys.stderr)
    new = resolve_commit(decomp, args.new)
    print(f"upgrading decomp {old[:12]} -> {new[:12]}")

    file_map = file_renames(root, old, new)
    sym_map, sym_skipped = symbol_renames(root, old, new)
    print(f"renames: {len(file_map)} files, {len(sym_map)} symbols "
          f"({sym_skipped} symbol changes skipped as ambiguous or invalid)")

    if args.keep:
        work = resolve_arg(args.keep, root)
        clean_dir(work, root)
    else:
        work = Path(tempfile.mkdtemp(prefix="patches-upgrade-"))
    a, b = build_upgrade(work, root, old, new, file_map, sym_map)

    diff = git(work, "diff", "--binary", "--full-index", "--no-renames", a, b).stdout
    clean, merged, failed = [], [], {}
    for path, chunk in split_pieces(diff):
        # git apply -3 reports "applied cleanly" even when it needed a 3-way merge,
        # so ask first whether the patch applies directly.
        direct = git(work, "apply", "--check", "--index", stdin=chunk, check=False).returncode == 0
        proc = git(work, "apply", "-3", "--index", "--whitespace=nowarn", stdin=chunk, check=False)
        err = proc.stderr.decode(errors="replace")
        if proc.returncode == 0:
            (clean if direct else merged).append(path)
        else:
            lines = [line for line in err.splitlines() if line.strip()]
            failed[path] = lines[0] if lines else "git apply failed"

    conflicts = out_text(git(work, "diff", "--name-only", "--diff-filter=U")).splitlines()
    hunks = {p: count_markers(work / p) for p in conflicts}
    for p in conflicts:
        failed.pop(p, None)

    gone = []
    for line in out_text(git(work, "diff", "--name-status", "--no-renames", a, b)).splitlines():
        status, _, path = line.partition("\t")
        if status in ("M", "D") and git(work, "cat-file", "-e", f"{PRISTINE}:{path}", check=False).returncode:
            gone.append(path)

    print(f"files: {len(clean)} clean, {len(merged)} 3-way merged, "
          f"{len(conflicts)} with conflicts, {len(failed)} failed")
    if clean:
        print(textwrap.fill("clean: " + ", ".join(clean), width=100, subsequent_indent="  "))
    for path in merged:
        print(f"  3-way    {path}")
    for path in conflicts:
        print(f"  CONFLICT {path} ({hunks[path]} hunks)")
    for path, reason in failed.items():
        print(f"  FAILED   {path}: {reason}")
    if gone:
        print("patched files that NEW deleted:")
        for path in gone:
            print(f"  {path}")
    else:
        print("patched files that NEW deleted: none")

    if conflicts or failed:
        print(f"\nnot upgraded: decomp/ is still at {old[:12]} and patches/ is unchanged.")
        print(f"scratch repo kept: {work}")
        print("finish by resolving the conflicts there (edit, then git add), then:")
        print(f"  python3 tools/patches.py save --from {work}")
        print(f"  git -C decomp checkout {new}")
        raise SystemExit(1)

    git(work, "add", "-A")
    git(decomp, "checkout", "-q", new)
    changed = save(work, root)
    print(f"\ndecomp/ is at {new[:12]}; patches/ regenerated:")
    print("\n".join(changed) if changed else "  no patch files changed")
    print("next: git add decomp patches")
    if not args.keep:
        rmtree(work)


# --- command line -------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Maintain the PC port's patches against the decomp submodule. "
                    "See the module docstring (python3 tools/patches.py --help) for the workflow.")
    parser.add_argument("--root", type=Path, default=REPO_ROOT, help=argparse.SUPPRESS)
    sub = parser.add_subparsers(dest="command", required=True, metavar="COMMAND")

    p = sub.add_parser("apply", help="build a work copy: pristine decomp plus the patches")
    p.add_argument("--out", metavar="DIR", help="work copy directory, replaced if it exists "
                                                "(default: work/ in the repo root)")
    p.add_argument("--force", action="store_true", help="replace an existing work copy even if it "
                                                        "has unsaved changes")

    p = sub.add_parser("save", help="regenerate patches/ from a work copy's changes")
    p.add_argument("--from", dest="source", metavar="DIR",
                   help="work copy to save (default: work/ in the repo root)")

    p = sub.add_parser("upgrade", help="move decomp/ to NEW and carry the patches along")
    p.add_argument("new", help="commit to move to: a sha, branch or tag (origin/NAME is tried too)")
    p.add_argument("--keep", metavar="DIR", help="scratch repo directory to keep "
                                                 "(default: a temp dir, removed on success)")

    args = parser.parse_args(argv)
    root = args.root.resolve()
    handlers = {"apply": cmd_apply, "save": cmd_save, "upgrade": cmd_upgrade}
    try:
        handlers[args.command](args, root)
    except Fail as exc:
        print(f"patches.py: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
