#!/usr/bin/env bash
# Build RecompOne, and manage the two shared subtrees.
#
# tools/RecompOne is a git SUBTREE (taken with --squash) of the fork
# Voicedrew11/verdite-recompone, and tools/verdite-core of Voicedrew11/verdite-core.
# Their sources are tracked here, so a fresh clone builds with nothing fetched.
# A commit touching either prefix touches nothing else, and this repository's
# copy must always equal some commit of the shared repo. Ported from Verdite2's
# script of the same name.
#
# Nothing goes upstream: no pull requests and no issues against RecompOne.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS="$ROOT/tools/RecompOne"
# Defined once; override with VERDITE_FORK_URL / VERDITE_FORK_BRANCH (a local
# path is handy while testing).
FORK_URL="${VERDITE_FORK_URL:-https://github.com/Voicedrew11/verdite-recompone.git}"
FORK_BRANCH="${VERDITE_FORK_BRANCH:-main}"
# Verdite Core, the game-agnostic tooling, is the second subtree; same rules.
CORE_URL="${VERDITE_CORE_URL:-https://github.com/Voicedrew11/verdite-core.git}"
CORE_BRANCH="${VERDITE_CORE_BRANCH:-main}"
SIGS="$TOOLS/RecompOne.Recompiler/AutoConfigure/signatures/psyq.json"

usage() {
    cat <<'USAGE'
usage: setup_tools.sh [--signatures] [--pull-fork [ref]] [--push-fork]
                      [--pull-core [ref]] [--push-core] [--no-build]

  (no flags)        build the recompiler
  --signatures      fetch AutoConfigure/signatures/psyq.json (15.7 MB, gitignored;
                    only the standalone --autoconfigure command reads it)
  --pull-fork [ref] pull the RecompOne fork into tools/RecompOne as one squash
                    subtree commit; ref defaults to main
  --push-fork       push tools/RecompOne's subtree commits to the RecompOne fork;
                    refuses if one of them also touches files outside it
  --pull-core [ref] the same for Verdite Core into tools/verdite-core
  --push-core       the same for Verdite Core
  --sync-upstream   removed; prints where harvesting moved and exits 2
  --no-build        skip the build
USAGE
}

# `git subtree` ships as a separate package on some distros (Fedora:
# git-subtree), and it prints usage to stderr with exit 129 when present, so
# test the output rather than the status.
require_subtree() {
    help="$(git subtree -h 2>&1 || true)"
    case "$help" in
        *"usage: git subtree"*) ;;
        *) echo "git subtree is not installed. On Fedora install git-subtree." >&2
           exit 1 ;;
    esac
}

SYNC=0 SIGNATURES=0 BUILD=1 PULL=0 PUSH=0 PULL_REF="$FORK_BRANCH"
CORE_PULL=0 CORE_PUSH=0 CORE_REF="$CORE_BRANCH"
while [ $# -gt 0 ]; do
    case "$1" in
        --signatures)    SIGNATURES=1 ;;
        --pull-fork)
            PULL=1
            case "${2:-}" in
                ""|-*) ;;
                *) PULL_REF="$2"; shift ;;
            esac
            ;;
        --push-fork)     PUSH=1 ;;
        --pull-core)
            CORE_PULL=1
            case "${2:-}" in
                ""|-*) ;;
                *) CORE_REF="$2"; shift ;;
            esac
            ;;
        --push-core)     CORE_PUSH=1 ;;
        --sync-upstream) SYNC=1 ;;
        --no-build)      BUILD=0 ;;
        -h|--help)       usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

if [ ! -d "$TOOLS" ]; then
    echo "tools/RecompOne is missing. It is a subtree of $FORK_URL -- restore it" >&2
    echo "with 'git subtree pull --prefix=tools/RecompOne --squash $FORK_URL $FORK_BRANCH'." >&2
    exit 1
fi

if [ "$SYNC" = 1 ]; then
    cat >&2 <<EOF
--sync-upstream has moved. Upstream harvesting happens in a working clone of the
fork ($FORK_URL), not in this checkout:

  1. clone $FORK_URL and add upstream BlackLabelHQ/RecompOne in it
  2. run its harvest_upstream.sh, resolve the merge, push the fork
  3. back here, run: bash scripts/setup_tools.sh --pull-fork
EOF
    exit 2
fi

if [ "$SIGNATURES" = 1 ]; then
    upstream="$(cat "$TOOLS/UPSTREAM")"
    url="https://raw.githubusercontent.com/BlackLabelHQ/RecompOne/$upstream/RecompOne.Recompiler/AutoConfigure/signatures/psyq.json"
    echo "==> fetching PSY-Q signatures (upstream $upstream)"
    mkdir -p "$(dirname "$SIGS")"
    curl -fL "$url" -o "$SIGS.part" || { rm -f "$SIGS.part"; exit 1; }
    mv "$SIGS.part" "$SIGS"
    echo "    $(du -h "$SIGS" | cut -f1) -> ${SIGS#$ROOT/}"
fi

# pull_subtree PREFIX URL REF NAME
pull_subtree() {
    require_subtree
    if [ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]; then
        echo "working tree is dirty; commit or stash before pulling $1." >&2
        exit 1
    fi
    echo "==> pulling $2 $3 into $1 (squash)"
    git -C "$ROOT" subtree pull --prefix="$1" --squash "$2" "$3" \
        -m "Pull $1 from $4 at $3"
}

# push_subtree PREFIX URL BRANCH
push_subtree() {
    require_subtree
    # A commit that touches the prefix must touch nothing else, or the split
    # carries a half-commit to the shared repo. Check every one since the last
    # join. The last join is the newest add/pull merge on the first-parent line:
    # its second parent is a squash commit naming the prefix.
    join=""
    while read -r c _ p2; do
        if git -C "$ROOT" log -1 --format=%B "$p2" | grep -q "^git-subtree-dir: $1/*\$"; then
            join="$c"; break
        fi
    done < <(git -C "$ROOT" log --first-parent --merges --format='%H %P' HEAD)
    if [ -z "$join" ]; then
        echo "no $1 subtree join found on the first-parent line; refusing to push." >&2
        exit 1
    fi
    mixed=0
    for c in $(git -C "$ROOT" rev-list --no-merges "$join..HEAD" -- "$1"); do
        if git -C "$ROOT" diff-tree --no-commit-id --name-only -r "$c" | grep -qv "^$1/"; then
            echo "mixed commit: $(git -C "$ROOT" log -1 --format='%h %s' "$c")" >&2
            mixed=1
        fi
    done
    if [ "$mixed" = 1 ]; then
        echo "split those into a $1 commit and a game commit, then push." >&2
        exit 1
    fi
    echo "==> pushing $1 to $2 $3 (this publishes to the shared repo)"
    git -C "$ROOT" subtree push --prefix="$1" "$2" "$3"
}

[ "$PULL" = 1 ] && pull_subtree tools/RecompOne "$FORK_URL" "$PULL_REF" "the fork"
[ "$PUSH" = 1 ] && push_subtree tools/RecompOne "$FORK_URL" "$FORK_BRANCH"
[ "$CORE_PULL" = 1 ] && pull_subtree tools/verdite-core "$CORE_URL" "$CORE_REF" "Verdite Core"
[ "$CORE_PUSH" = 1 ] && push_subtree tools/verdite-core "$CORE_URL" "$CORE_BRANCH"

if [ "$BUILD" = 1 ]; then
    echo "==> building recompiler"
    dotnet build "$TOOLS/RecompOne.Recompiler" -c Release
fi

echo "done."
